//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "yb/rocksdb/table/block_based_table_reader.h"

#include <string>
#include <utility>
#include <cinttypes>

#include "yb/rocksdb/db/dbformat.h"

#include "yb/rocksdb/cache.h"
#include "yb/rocksdb/comparator.h"
#include "yb/rocksdb/env.h"
#include "yb/rocksdb/filter_policy.h"
#include "yb/rocksdb/iterator.h"
#include "yb/rocksdb/options.h"
#include "yb/rocksdb/statistics.h"
#include "yb/rocksdb/table.h"
#include "yb/rocksdb/table_properties.h"
#include "yb/rocksdb/table/block.h"
#include "yb/rocksdb/table/block_based_filter_block.h"
#include "yb/rocksdb/table/block_based_table_factory.h"
#include "yb/rocksdb/table/block_hash_index.h"
#include "yb/rocksdb/table/block_prefix_index.h"
#include "yb/rocksdb/table/filter_block.h"
#include "yb/rocksdb/table/format.h"
#include "yb/rocksdb/table/forwarding_iterator.h"
#include "yb/rocksdb/table/fixed_size_filter_block.h"
#include "yb/rocksdb/table/full_filter_block.h"
#include "yb/rocksdb/table/get_context.h"
#include "yb/rocksdb/table/internal_iterator.h"
#include "yb/rocksdb/table/meta_blocks.h"
#include "yb/rocksdb/table/two_level_iterator.h"

#include "yb/rocksdb/util/coding.h"
#include "yb/rocksdb/util/file_reader_writer.h"
#include "yb/rocksdb/util/perf_context_imp.h"
#include "yb/rocksdb/util/stop_watch.h"
#include "yb/rocksdb/util/string_util.h"

#include "yb/gutil/macros.h"

namespace rocksdb {

extern const uint64_t kBlockBasedTableMagicNumber;
extern const char kHashIndexPrefixesBlock[];
extern const char kHashIndexPrefixesMetadataBlock[];
using std::unique_ptr;

typedef BlockBasedTable::IndexReader IndexReader;
typedef FilterPolicy::FilterType FilterType;

namespace {
// The longest the prefix of the cache key used to identify blocks can be.
// We are using the fact that we know for Posix files the unique ID is three
// varints.
// For some reason, compiling for iOS complains that this variable is unused
const size_t kMaxCacheKeyPrefixSize __attribute__((unused)) =
    kMaxVarint64Length * 3 + 1;

// Read the block identified by "handle" from "file".
// The only relevant option is options.verify_checksums for now.
// On failure return non-OK.
// On success fill *result and return OK - caller owns *result
Status ReadBlockFromFile(RandomAccessFileReader* file, const Footer& footer,
                         const ReadOptions& options, const BlockHandle& handle,
                         std::unique_ptr<Block>* result, Env* env,
                         bool do_uncompress = true) {
  BlockContents contents;
  Status s = ReadBlockContents(file, footer, options, handle, &contents, env,
                               do_uncompress);
  if (s.ok()) {
    result->reset(new Block(std::move(contents)));
  }

  return s;
}

// Delete the resource that is held by the iterator.
template <class ResourceType>
void DeleteHeldResource(void* arg, void* ignored) {
  delete reinterpret_cast<ResourceType*>(arg);
}

// Delete the entry resided in the cache.
template <class Entry>
void DeleteCachedEntry(const Slice& key, void* value) {
  auto entry = reinterpret_cast<Entry*>(value);
  delete entry;
}

// Release the cached entry and decrement its ref count.
void ReleaseCachedEntry(void* arg, void* h) {
  Cache* cache = reinterpret_cast<Cache*>(arg);
  Cache::Handle* handle = reinterpret_cast<Cache::Handle*>(h);
  cache->Release(handle);
}

Cache::Handle* GetEntryFromCache(Cache* block_cache, const Slice& key,
                                 Tickers block_cache_miss_ticker,
                                 Tickers block_cache_hit_ticker,
                                 Statistics* statistics,
                                 const QueryId query_id) {
  auto cache_handle = block_cache->Lookup(key, query_id, statistics);
  if (cache_handle != nullptr) {
    PERF_COUNTER_ADD(block_cache_hit_count, 1);
    // block-type specific cache hit
    RecordTick(statistics, block_cache_hit_ticker);
  } else {
    // block-type specific cache miss
    RecordTick(statistics, block_cache_miss_ticker);
  }

  return cache_handle;
}

class NotMatchingFilterBlockReader : public FilterBlockReader {
 public:
  NotMatchingFilterBlockReader() {}
  NotMatchingFilterBlockReader(const NotMatchingFilterBlockReader&) = delete;
  void operator=(const NotMatchingFilterBlockReader&) = delete;
  virtual bool KeyMayMatch(const Slice& key, uint64_t block_offset = 0) override {
    return false; }
  virtual bool PrefixMayMatch(const Slice& prefix, uint64_t block_offset = 0) override {
    return false; }
  virtual size_t ApproximateMemoryUsage() const override { return 0; }
};

}  // namespace

// -- IndexReader and its subclasses
// IndexReader is the interface that provide the functionality for index access.
class BlockBasedTable::IndexReader {
 public:
  explicit IndexReader(const Comparator* comparator)
      : comparator_(comparator) {}

  virtual ~IndexReader() {}

  // Create an iterator for index access.
  // An iter is passed in, if it is not null, update this one and return it
  // If it is null, create a new Iterator
  virtual InternalIterator* NewIterator(BlockIter* iter = nullptr,
                                        bool total_order_seek = true) = 0;

  // The size of the index.
  virtual size_t size() const = 0;
  // Memory usage of the index block
  virtual size_t usable_size() const = 0;

  // Report an approximation of how much memory has been used other than memory
  // that was allocated in block cache.
  virtual size_t ApproximateMemoryUsage() const = 0;

 protected:
  const Comparator* comparator_;
};

// Index that allows binary search lookup for the first key of each block.
// This class can be viewed as a thin wrapper for `Block` class which already
// supports binary search.
class BinarySearchIndexReader : public IndexReader {
 public:
  // Read index from the file and create an instance for
  // `BinarySearchIndexReader`.
  // On success, index_reader will be populated; otherwise it will remain
  // unmodified.
  static Status Create(RandomAccessFileReader* file, const Footer& footer,
                       const BlockHandle& index_handle, Env* env,
                       const Comparator* comparator,
                       std::unique_ptr<IndexReader>* index_reader) {
    std::unique_ptr<Block> index_block;
    auto s = ReadBlockFromFile(file, footer, ReadOptions::kDefault, index_handle,
                               &index_block, env);

    if (s.ok()) {
      index_reader->reset(new BinarySearchIndexReader(comparator, std::move(index_block)));
    }

    return s;
  }

  virtual InternalIterator* NewIterator(BlockIter* iter = nullptr,
                                        bool dont_care = true) override {
    return index_block_->NewIterator(comparator_, iter, true);
  }

  size_t size() const override { return index_block_->size(); }
  size_t usable_size() const override {
    return index_block_->usable_size();
  }

  size_t ApproximateMemoryUsage() const override {
    assert(index_block_);
    return index_block_->ApproximateMemoryUsage();
  }

 private:
  BinarySearchIndexReader(const Comparator* comparator,
                          std::unique_ptr<Block>&& index_block)
      : IndexReader(comparator), index_block_(std::move(index_block)) {
    assert(index_block_ != nullptr);
  }
  std::unique_ptr<Block> index_block_;
};

// Index that leverages an internal hash table to quicken the lookup for a given
// key.
class HashIndexReader : public IndexReader {
 public:
  static Status Create(const SliceTransform* hash_key_extractor,
                       const Footer& footer, RandomAccessFileReader* file,
                       Env* env, const Comparator* comparator,
                       const BlockHandle& index_handle,
                       InternalIterator* meta_index_iter,
                       std::unique_ptr<IndexReader>* index_reader,
                       bool hash_index_allow_collision) {
    std::unique_ptr<Block> index_block;
    auto s = ReadBlockFromFile(file, footer, ReadOptions::kDefault, index_handle,
                               &index_block, env);

    if (!s.ok()) {
      return s;
    }

    // Note, failure to create prefix hash index does not need to be a
    // hard error. We can still fall back to the original binary search index.
    // So, Create will succeed regardless, from this point on.
    auto new_index_reader =
        new HashIndexReader(comparator, std::move(index_block));
    index_reader->reset(new_index_reader);

    // Get prefixes block
    BlockHandle prefixes_handle;
    s = FindMetaBlock(meta_index_iter, kHashIndexPrefixesBlock,
                      &prefixes_handle);
    if (!s.ok()) {
      // TODO: log error
      return Status::OK();
    }

    // Get index metadata block
    BlockHandle prefixes_meta_handle;
    s = FindMetaBlock(meta_index_iter, kHashIndexPrefixesMetadataBlock,
                      &prefixes_meta_handle);
    if (!s.ok()) {
      // TODO: log error
      return Status::OK();
    }

    // Read contents for the blocks
    BlockContents prefixes_contents;
    s = ReadBlockContents(file, footer, ReadOptions::kDefault, prefixes_handle,
                          &prefixes_contents, env, true /* do decompression */);
    if (!s.ok()) {
      return s;
    }
    BlockContents prefixes_meta_contents;
    s = ReadBlockContents(file, footer, ReadOptions::kDefault, prefixes_meta_handle,
                          &prefixes_meta_contents, env,
                          true /* do decompression */);
    if (!s.ok()) {
      // TODO: log error
      return Status::OK();
    }

    if (!hash_index_allow_collision) {
      // TODO: deprecate once hash_index_allow_collision proves to be stable.
      BlockHashIndex* hash_index = nullptr;
      s = CreateBlockHashIndex(hash_key_extractor,
                               prefixes_contents.data,
                               prefixes_meta_contents.data,
                               &hash_index);
      // TODO: log error
      if (s.ok()) {
        new_index_reader->index_block_->SetBlockHashIndex(hash_index);
        new_index_reader->OwnPrefixesContents(std::move(prefixes_contents));
      }
    } else {
      BlockPrefixIndex* prefix_index = nullptr;
      s = BlockPrefixIndex::Create(hash_key_extractor,
                                   prefixes_contents.data,
                                   prefixes_meta_contents.data,
                                   &prefix_index);
      // TODO: log error
      if (s.ok()) {
        new_index_reader->index_block_->SetBlockPrefixIndex(prefix_index);
      }
    }

    return Status::OK();
  }

  virtual InternalIterator* NewIterator(BlockIter* iter = nullptr,
                                        bool total_order_seek = true) override {
    return index_block_->NewIterator(comparator_, iter, total_order_seek);
  }

  size_t size() const override { return index_block_->size(); }
  size_t usable_size() const override {
    return index_block_->usable_size();
  }

  size_t ApproximateMemoryUsage() const override {
    assert(index_block_);
    return index_block_->ApproximateMemoryUsage() +
           prefixes_contents_.data.size();
  }

 private:
  HashIndexReader(const Comparator* comparator,
                  std::unique_ptr<Block>&& index_block)
      : IndexReader(comparator), index_block_(std::move(index_block)) {
    assert(index_block_ != nullptr);
  }

  ~HashIndexReader() {
  }

  void OwnPrefixesContents(BlockContents&& prefixes_contents) {
    prefixes_contents_ = std::move(prefixes_contents);
  }

  std::unique_ptr<Block> index_block_;
  BlockContents prefixes_contents_;
};

// Originally following data was stored in BlockBasedTable::Rep and related to a single SST file.
// Since SST file is now split into two files - data file and metadata file, all file-related data
// was moved into dedicated structure for each file.
struct BlockBasedTable::FileReaderWithCachePrefix {
  // Pointer to file reader.
  unique_ptr<RandomAccessFileReader> reader;

  // BlockBasedTableReader uses the block cache passed to BlockBasedTableReader::Open inside
  // a BlockBasedTableOptions instance to reduce the number of file read requests. If block cache
  // pointer in options is nullptr, cache is not used. File blocks are referred in cache by keys,
  // which are composed from the following data (see GetCacheKey helper function):
  // - cache key prefix (unique for each file), generated by BlockBasedTable::GenerateCachePrefix
  // - block offset within a file.
  CacheKeyBuffer cache_key_prefix;

  // Similar prefix, but for compressed blocks cache:
  CacheKeyBuffer compressed_cache_key_prefix;

  explicit FileReaderWithCachePrefix(unique_ptr<RandomAccessFileReader>&& _reader) :
      reader(std::move(_reader)) {}
};

// CachableEntry represents the entries that *may* be fetched from block cache.
//  field `value` is the item we want to get.
//  field `cache_handle` is the cache handle to the block cache. If the value
//    was not read from cache, `cache_handle` will be nullptr.
template <class TValue>
struct BlockBasedTable::CachableEntry {
  CachableEntry(TValue* _value, Cache::Handle* _cache_handle)
      : value(_value), cache_handle(_cache_handle) {}
  CachableEntry() : CachableEntry(nullptr, nullptr) {}
  void Release(Cache* cache) {
    if (cache_handle) {
      cache->Release(cache_handle);
      value = nullptr;
      cache_handle = nullptr;
    }
  }

  TValue* value = nullptr;
  // if the entry is from the cache, cache_handle will be populated.
  Cache::Handle* cache_handle = nullptr;
};

struct BlockBasedTable::Rep {
  struct NotMatchingFilterEntry : public CachableEntry<FilterBlockReader> {
    NotMatchingFilterEntry() : CachableEntry(&filter, nullptr) {}
    NotMatchingFilterBlockReader filter;
  };

  Rep(const ImmutableCFOptions& _ioptions, const EnvOptions& _env_options,
      const BlockBasedTableOptions& _table_opt,
      const InternalKeyComparator& _internal_comparator, bool skip_filters)
      : ioptions(_ioptions),
        env_options(_env_options),
        table_options(_table_opt),
        filter_policy(skip_filters ? nullptr : _table_opt.filter_policy.get()),
        filter_key_transformer(filter_policy ? filter_policy->GetKeyTransformer() : nullptr),
        internal_comparator(_internal_comparator),
        filter_type(FilterType::kNoFilter),
        whole_key_filtering(_table_opt.whole_key_filtering),
        prefix_filtering(true) {}

  const ImmutableCFOptions& ioptions;
  const EnvOptions& env_options;
  const BlockBasedTableOptions& table_options;
  const FilterPolicy* const filter_policy;
  const FilterPolicy::KeyTransformer* const filter_key_transformer;
  const InternalKeyComparator& internal_comparator;
  const NotMatchingFilterEntry not_matching_filter_entry;
  Status status;
  std::shared_ptr<FileReaderWithCachePrefix> base_reader_with_cache_prefix;
  std::shared_ptr<FileReaderWithCachePrefix> data_reader_with_cache_prefix;

  // Footer contains the fixed table information
  Footer footer;
  // data_index_reader and filter will be populated and used only when
  // options.block_cache is nullptr; otherwise we will get the index block via
  // the block cache.
  unique_ptr<IndexReader> data_index_reader;
  unique_ptr<IndexReader> filter_index_reader;
  unique_ptr<FilterBlockReader> filter;

  FilterType filter_type;

  // Handle of fixed-size bloom filter index block or simply filter block for filters of other
  // types.
  BlockHandle filter_handle;

  std::shared_ptr<const TableProperties> table_properties;
  BlockBasedTableOptions::IndexType index_type;
  bool hash_index_allow_collision;
  bool whole_key_filtering;
  bool prefix_filtering;
  // TODO(kailiu) It is very ugly to use internal key in table, since table
  // module should not be relying on db module. However to make things easier
  // and compatible with existing code, we introduce a wrapper that allows
  // block to extract prefix without knowing if a key is internal or not.
  unique_ptr<SliceTransform> internal_prefix_transform;
};

BlockBasedTable::~BlockBasedTable() {
  delete rep_;
}

void BlockBasedTable::GenerateCachePrefix(Cache* cc, File* file, CacheKeyBuffer* prefix) {
  // generate an id from the file
  prefix->size = file->GetUniqueId(prefix->data, kMaxCacheKeyPrefixSize);

  // If the prefix wasn't generated or was too long,
  // create one from the cache.
  if (prefix->size == 0) {
    char* end = EncodeVarint64(prefix->data, cc->NewId());
    prefix->size = static_cast<size_t>(end - prefix->data);
  }
}

Slice BlockBasedTable::GetCacheKey(const CacheKeyBuffer& cache_key_prefix,
    const BlockHandle& handle, char* cache_key) {
  assert(cache_key != nullptr);
  assert(cache_key_prefix.size != 0);
  assert(cache_key_prefix.size <= kMaxCacheKeyPrefixSize);
  memcpy(cache_key, cache_key_prefix.data, cache_key_prefix.size);
  char* end = EncodeVarint64(cache_key + cache_key_prefix.size, handle.offset());
  return Slice(cache_key, static_cast<size_t>(end - cache_key));
}

void BlockBasedTable::SetupCacheKeyPrefix(Rep* rep,
    FileReaderWithCachePrefix* reader_with_cache_prefix) {
  reader_with_cache_prefix->cache_key_prefix.size = 0;
  reader_with_cache_prefix->compressed_cache_key_prefix.size = 0;
  if (rep->table_options.block_cache != nullptr) {
    GenerateCachePrefix(rep->table_options.block_cache.get(),
        reader_with_cache_prefix->reader->file(),
        &reader_with_cache_prefix->cache_key_prefix);
  }
  if (rep->table_options.block_cache_compressed != nullptr) {
    GenerateCachePrefix(rep->table_options.block_cache_compressed.get(),
        reader_with_cache_prefix->reader->file(),
        &reader_with_cache_prefix->compressed_cache_key_prefix);
  }
}

// Indirection to TwoLevelIterator as it's a private class we cannot inherit from.
// BloomFilterAwareIterator should only be used when scanning within the same hashed components of
// the key and it should be used together with DocDbAwareFilterPolicy which only takes into account
// hashed components of key for filtering.
// BloomFilterAwareIterator ignores an SST file completely if there are no keys with the same hashed
// components as the key specified for the seek operation in that file.
class BloomFilterAwareIterator : public ForwardingIterator {
 public:
  BloomFilterAwareIterator(
      BlockBasedTable* table, const ReadOptions& ro, bool skip_filters,
      InternalIterator* internal_iter, bool need_free_iter)
      : ForwardingIterator(internal_iter, need_free_iter),
        table_(table),
        read_options_(ro),
        skip_filters_(skip_filters) {}

  virtual ~BloomFilterAwareIterator() {}

  void Seek(const Slice& internal_key) override {
    if (skip_filters_) {
      InternalSeek(internal_key);
    } else {
      if (table_->rep_->filter_type == FilterType::kFixedSizeFilter) {
        auto filter_key = table_->GetFilterKey(internal_key);
        auto filter_entry =
            table_->GetFilter(read_options_.query_id,
                              read_options_.read_tier == kBlockCacheTier /* no_io */, &filter_key);
        FilterBlockReader* filter = filter_entry.value;
        if (table_->NonBlockBasedFilterKeyMayMatch(filter, filter_key)) {
          // If bloom filter was not useful, then take this file into account.
          InternalSeek(internal_key);
        } else {
          // Else, record that the bloom filter was useful.
          RecordTick(table_->rep_->ioptions.statistics, BLOOM_FILTER_USEFUL);
          // Since BloomFilterAwareIterator should only be used when scanning within the same hashed
          // components of the key and it is used together with DocDbAwareFilterPolicy, we don't
          // need to seek to next key, because DocDbAwareFilterPolicy uses bloom filters only for
          // hashed components of the key. So, in this else-branch we know that there are no keys
          // in this SST with required hashed components.
          valid_ = false;
        }
        filter_entry.Release(table_->rep_->table_options.block_cache.get());
      } else {
        // For non fixed-size filters - just seek. We are only using fixed-size bloom filters for
        // DocDB, so not need to support others.
        InternalSeek(internal_key);
      }
    }
  }

  bool Valid() const override {
    return valid_;
  }

  void InternalSeek(const Slice& internal_key) {
    ForwardingIterator::Seek(internal_key);
    valid_ = internal_iter_.Valid();
  }

  void SeekToFirst() override {
    ForwardingIterator::SeekToFirst();
    valid_ = internal_iter_.Valid();
  }

  void SeekToLast() override {
    ForwardingIterator::SeekToLast();
    valid_ = internal_iter_.Valid();
  }

  void Next() override {
    ForwardingIterator::Next();
    valid_ = internal_iter_.Valid();
  }

  void Prev() override {
    ForwardingIterator::Prev();
    valid_ = internal_iter_.Valid();
  }

 private:
  // No ownership.
  BlockBasedTable* table_;
  const ReadOptions read_options_;
  const bool skip_filters_;
  bool valid_ = false;
};

namespace {
// Return True if table_properties has `user_prop_name` has a `true` value
// or it doesn't contain this property (for backward compatible).
bool IsFeatureSupported(const TableProperties& table_properties,
                        const std::string& user_prop_name, Logger* info_log) {
  auto& props = table_properties.user_collected_properties;
  auto pos = props.find(user_prop_name);
  // Older version doesn't have this value set. Skip this check.
  if (pos != props.end()) {
    if (pos->second == kPropFalse) {
      return false;
    } else if (pos->second != kPropTrue) {
      RLOG(InfoLogLevel::WARN_LEVEL, info_log,
          "Property %s has invalidate value %s", user_prop_name.c_str(),
          pos->second.c_str());
    }
  }
  return true;
}
}  // namespace

Status BlockBasedTable::Open(const ImmutableCFOptions& ioptions,
                             const EnvOptions& env_options,
                             const BlockBasedTableOptions& table_options,
                             const InternalKeyComparator& internal_comparator,
                             unique_ptr<RandomAccessFileReader>&& base_file,
                             uint64_t base_file_size,
                             unique_ptr<TableReader>* table_reader,
                             const bool prefetch_index_and_filter,
                             const bool skip_filters) {
  table_reader->reset();

  Footer footer;
  auto s = ReadFooterFromFile(base_file.get(), base_file_size, &footer,
                              kBlockBasedTableMagicNumber);
  if (!s.ok()) {
    return s;
  }
  if (!BlockBasedTableSupportedVersion(footer.version())) {
    return STATUS(Corruption,
        "Unknown Footer version. Maybe this file was created with newer "
        "version of RocksDB?");
  }

  // We've successfully read the footer and the index block: we're
  // ready to serve requests.
  Rep* rep = new BlockBasedTable::Rep(ioptions, env_options, table_options,
                                      internal_comparator, skip_filters);
  rep->base_reader_with_cache_prefix =
      std::make_shared<FileReaderWithCachePrefix>(std::move(base_file));
  rep->data_reader_with_cache_prefix = rep->base_reader_with_cache_prefix;
  rep->footer = footer;
  rep->index_type = table_options.index_type;
  rep->hash_index_allow_collision = table_options.hash_index_allow_collision;
  SetupCacheKeyPrefix(rep, rep->base_reader_with_cache_prefix.get());
  unique_ptr<BlockBasedTable> new_table(new BlockBasedTable(rep));

  // Read meta index
  std::unique_ptr<Block> meta;
  std::unique_ptr<InternalIterator> meta_iter;
  s = ReadMetaBlock(rep, &meta, &meta_iter);
  if (!s.ok()) {
    return s;
  }

  // Find filter handle and filter type.
  if (rep->filter_policy) {
    for (const auto& prefix : {kFullFilterBlockPrefix,
                               kFilterBlockPrefix,
                               kFixedSizeFilterBlockPrefix}) {
      // Unsuccessful read implies we should not use filter.
      std::string filter_block_key = prefix;
      filter_block_key.append(rep->filter_policy->Name());
      if (FindMetaBlock(meta_iter.get(), filter_block_key, &rep->filter_handle).ok()) {
        if (prefix == kFullFilterBlockPrefix) {
          rep->filter_type = FilterType::kFullFilter;
        } else if (prefix == kFilterBlockPrefix) {
          rep->filter_type = FilterType::kBlockBasedFilter;
        } else if (prefix == kFixedSizeFilterBlockPrefix) {
          rep->filter_type = FilterType::kFixedSizeFilter;
        } else {
          // That means we have memory corruption, so we should fail.
          RLOG(InfoLogLevel::FATAL_LEVEL, rep->ioptions.info_log, "Invalid filter block prefix: %s",
              prefix);
          assert(false);
          return STATUS(Corruption, "Invalid filter block prefix", prefix);
        }
        break;
      }
    }
  }

  // Read the properties
  bool found_properties_block = true;
  s = SeekToPropertiesBlock(meta_iter.get(), &found_properties_block);

  if (!s.ok()) {
    RLOG(InfoLogLevel::WARN_LEVEL, rep->ioptions.info_log,
        "Cannot seek to properties block from file: %s",
        s.ToString().c_str());
  } else if (found_properties_block) {
    s = meta_iter->status();
    TableProperties* table_properties = nullptr;
    if (s.ok()) {
      s = ReadProperties(meta_iter->value(), rep->base_reader_with_cache_prefix->reader.get(),
            rep->footer, rep->ioptions.env, rep->ioptions.info_log, &table_properties);
    }

    if (!s.ok()) {
      RLOG(InfoLogLevel::WARN_LEVEL, rep->ioptions.info_log,
        "Encountered error while reading data from properties "
        "block %s", s.ToString().c_str());
    } else {
      rep->table_properties.reset(table_properties);
    }
  } else {
    RLOG(InfoLogLevel::ERROR_LEVEL, rep->ioptions.info_log,
        "Cannot find Properties block from file.");
  }

  // Determine whether whole key filtering is supported.
  if (rep->table_properties) {
    rep->whole_key_filtering &=
        IsFeatureSupported(*(rep->table_properties),
                           BlockBasedTablePropertyNames::kWholeKeyFiltering,
                           rep->ioptions.info_log);
    rep->prefix_filtering &= IsFeatureSupported(
        *(rep->table_properties),
        BlockBasedTablePropertyNames::kPrefixFiltering, rep->ioptions.info_log);
  }

  if (prefetch_index_and_filter) {
    // pre-fetching of blocks is turned on
    // TODO: may be put it in block cache instead of table reader in case
    // table_options.cache_index_and_filter_blocks is set?
    // NOTE: Table reader objects are cached in table cache (table_cache.cc).
    if (rep->filter_policy && rep->filter_type == FilterType::kFixedSizeFilter) {
      s = new_table->CreateFilterIndexReader(&rep->filter_index_reader);
    }

    // Will use block cache for index/filter blocks access?
    if (table_options.cache_index_and_filter_blocks) {
      assert(table_options.block_cache != nullptr);
      // Hack: Call NewIndexIterator() to implicitly add index to the
      // block_cache
      unique_ptr<InternalIterator> iter(
          new_table->NewIndexIterator(ReadOptions::kDefault));
      s = iter->status();

      if (s.ok()) {
        bool corrupted_filter_type = true;
        switch (rep->filter_type) {
          case FilterType::kFullFilter:
            FALLTHROUGH_INTENDED;
          case FilterType::kBlockBasedFilter: {
            // Hack: Call GetFilter() to implicitly add filter to the block_cache
            auto filter_entry = new_table->GetFilter(kDefaultQueryId);
            filter_entry.Release(table_options.block_cache.get());
            corrupted_filter_type = false;
            break;
          }
          case FilterType::kFixedSizeFilter:
            // We never pre-cache fixed-size bloom filters.
            FALLTHROUGH_INTENDED;
          case FilterType::kNoFilter:
            corrupted_filter_type = false;
            break;
        }
        if (corrupted_filter_type) {
          RLOG(InfoLogLevel::FATAL_LEVEL, rep->ioptions.info_log, "Corrupted bloom filter type: %d",
              rep->filter_type);
          assert(false);
          return STATUS_SUBSTITUTE(Corruption, "Corrupted bloom filter type: $0", rep->filter_type);
        }
      }
    } else {
      // If we don't use block cache for index/filter blocks access, we'll
      // pre-load these blocks, which will kept in member variables in Rep
      // and with a same life-time as this table object.

      s = new_table->CreateDataBlockIndexReader(&rep->data_index_reader, meta_iter.get());

      if (s.ok()) {
        bool corrupted_filter_type = true;
        switch (rep->filter_type) {
          case FilterType::kFullFilter:
            FALLTHROUGH_INTENDED;
          case FilterType::kBlockBasedFilter:
            rep->filter.reset(ReadFilterBlock(rep->filter_handle, rep, nullptr));
            corrupted_filter_type = false;
            break;
          case FilterType::kFixedSizeFilter:
            // We never pre-load fixed-size bloom filters.
            FALLTHROUGH_INTENDED;
          case FilterType::kNoFilter:
            corrupted_filter_type = false;
            break;
        }
        if (corrupted_filter_type) {
          RLOG(InfoLogLevel::FATAL_LEVEL, rep->ioptions.info_log, "Corrupted bloom filter type: %d",
              rep->filter_type);
          assert(false);
          return STATUS_SUBSTITUTE(Corruption, "Corrupted bloom filter type: $0", rep->filter_type);
        }
      }
    }
  }

  if (s.ok()) {
    *table_reader = std::move(new_table);
  }

  return s;
}

void BlockBasedTable::SetDataFileReader(unique_ptr<RandomAccessFileReader> &&data_file) {
  rep_->data_reader_with_cache_prefix =
      std::make_shared<FileReaderWithCachePrefix>(std::move(data_file));
  SetupCacheKeyPrefix(rep_, rep_->data_reader_with_cache_prefix.get());
}

namespace {
void SetupFileReaderForCompaction(const Options::AccessHint &access_hint,
    RandomAccessFileReader *reader) {
  if (reader != nullptr) {
    switch (access_hint) {
      case Options::NONE:
        break;
      case Options::NORMAL:
        reader->file()->Hint(RandomAccessFile::NORMAL);
        break;
      case Options::SEQUENTIAL:
        reader->file()->Hint(RandomAccessFile::SEQUENTIAL);
        break;
      case Options::WILLNEED:
        reader->file()->Hint(RandomAccessFile::WILLNEED);
        break;
      default:
        assert(false);
    }
  }
}
} // anonymous namespace

void BlockBasedTable::SetupForCompaction() {
  auto access_hint = rep_->ioptions.access_hint_on_compaction_start;
  ::rocksdb::SetupFileReaderForCompaction(access_hint,
      rep_->base_reader_with_cache_prefix->reader.get());
  ::rocksdb::SetupFileReaderForCompaction(access_hint,
      rep_->data_reader_with_cache_prefix->reader.get());
}

std::shared_ptr<const TableProperties> BlockBasedTable::GetTableProperties()
    const {
  return rep_->table_properties;
}

size_t BlockBasedTable::ApproximateMemoryUsage() const {
  size_t usage = 0;
  if (rep_->filter) {
    usage += rep_->filter->ApproximateMemoryUsage();
  }
  if (rep_->filter_index_reader) {
    usage += rep_->filter_index_reader->ApproximateMemoryUsage();
  }
  if (rep_->data_index_reader) {
    usage += rep_->data_index_reader->ApproximateMemoryUsage();
  }
  return usage;
}

// Load the meta-block from the file. On success, return the loaded meta block
// and its iterator.
Status BlockBasedTable::ReadMetaBlock(Rep* rep,
                                      std::unique_ptr<Block>* meta_block,
                                      std::unique_ptr<InternalIterator>* iter) {
  // TODO(sanjay): Skip this if footer.metaindex_handle() size indicates
  // it is an empty block.
  // TODO: we never really verify check sum for meta index block
  std::unique_ptr<Block> meta;
  Status s = ReadBlockFromFile(
      rep->base_reader_with_cache_prefix->reader.get(),
      rep->footer,
      ReadOptions::kDefault,
      rep->footer.metaindex_handle(),
      &meta,
      rep->ioptions.env);

  if (!s.ok()) {
    RLOG(InfoLogLevel::ERROR_LEVEL, rep->ioptions.info_log,
        "Encountered error while reading data from properties"
        " block %s", s.ToString().c_str());
    return s;
  }

  *meta_block = std::move(meta);
  // meta block uses bytewise comparator.
  iter->reset(meta_block->get()->NewIterator(BytewiseComparator()));
  return Status::OK();
}

Status BlockBasedTable::GetDataBlockFromCache(
    const Slice& block_cache_key, const Slice& compressed_block_cache_key,
    Cache* block_cache, Cache* block_cache_compressed, Statistics* statistics,
    const ReadOptions& read_options,
    BlockBasedTable::CachableEntry<Block>* block, uint32_t format_version) {
  Status s;
  Block* compressed_block = nullptr;
  Cache::Handle* block_cache_compressed_handle = nullptr;

  // Lookup uncompressed cache first
  if (block_cache != nullptr) {
    block->cache_handle =
        GetEntryFromCache(block_cache, block_cache_key, BLOCK_CACHE_DATA_MISS,
                          BLOCK_CACHE_DATA_HIT, statistics, read_options.query_id);
    if (block->cache_handle != nullptr) {
      block->value =
          static_cast<Block*>(block_cache->Value(block->cache_handle));
      return s;
    }
  }

  // If not found, search from the compressed block cache.
  assert(block->cache_handle == nullptr && block->value == nullptr);

  if (block_cache_compressed == nullptr) {
    return s;
  }

  assert(!compressed_block_cache_key.empty());
  block_cache_compressed_handle =
      block_cache_compressed->Lookup(compressed_block_cache_key, read_options.query_id);
  // if we found in the compressed cache, then uncompress and insert into
  // uncompressed cache
  if (block_cache_compressed_handle == nullptr) {
    RecordTick(statistics, BLOCK_CACHE_COMPRESSED_MISS);
    return s;
  }

  // found compressed block
  RecordTick(statistics, BLOCK_CACHE_COMPRESSED_HIT);
  compressed_block = static_cast<Block*>(
      block_cache_compressed->Value(block_cache_compressed_handle));
  assert(compressed_block->compression_type() != kNoCompression);

  // Retrieve the uncompressed contents into a new buffer
  BlockContents contents;
  s = UncompressBlockContents(compressed_block->data(),
                              compressed_block->size(), &contents,
                              format_version);

  // Insert uncompressed block into block cache
  if (s.ok()) {
    block->value = new Block(std::move(contents));  // uncompressed block
    assert(block->value->compression_type() == kNoCompression);
    if (block_cache != nullptr && block->value->cachable() &&
        read_options.fill_cache) {
      s = block_cache->Insert(block_cache_key, read_options.query_id, block->value,
                              block->value->usable_size(), &DeleteCachedEntry<Block>,
                              &block->cache_handle, statistics);
      if (!s.ok()) {
        delete block->value;
        block->value = nullptr;
      }
    }
  }

  // Release hold on compressed cache entry
  block_cache_compressed->Release(block_cache_compressed_handle);
  return s;
}

Status BlockBasedTable::PutDataBlockToCache(
    const Slice& block_cache_key, const Slice& compressed_block_cache_key,
    Cache* block_cache, Cache* block_cache_compressed,
    const ReadOptions& read_options, Statistics* statistics,
    CachableEntry<Block>* block, Block* raw_block, uint32_t format_version) {
  assert(raw_block->compression_type() == kNoCompression ||
         block_cache_compressed != nullptr);

  Status s;
  // Retrieve the uncompressed contents into a new buffer
  BlockContents contents;
  if (raw_block->compression_type() != kNoCompression) {
    s = UncompressBlockContents(raw_block->data(), raw_block->size(), &contents,
                                format_version);
  }
  if (!s.ok()) {
    delete raw_block;
    return s;
  }

  if (raw_block->compression_type() != kNoCompression) {
    block->value = new Block(std::move(contents));  // uncompressed block
  } else {
    block->value = raw_block;
    raw_block = nullptr;
  }

  // Insert compressed block into compressed block cache.
  // Release the hold on the compressed cache entry immediately.
  if (block_cache_compressed != nullptr && raw_block != nullptr &&
      raw_block->cachable()) {
    s = block_cache_compressed->Insert(compressed_block_cache_key, read_options.query_id, raw_block,
                                       raw_block->usable_size(), &DeleteCachedEntry<Block>);
    if (s.ok()) {
      // Avoid the following code to delete this cached block.
      raw_block = nullptr;
      RecordTick(statistics, BLOCK_CACHE_COMPRESSED_ADD);
    } else {
      RecordTick(statistics, BLOCK_CACHE_COMPRESSED_ADD_FAILURES);
    }
  }
  delete raw_block;

  // insert into uncompressed block cache
  assert((block->value->compression_type() == kNoCompression));
  if (block_cache != nullptr && block->value->cachable()) {
    s = block_cache->Insert(block_cache_key, read_options.query_id, block->value,
                            block->value->usable_size(),
                            &DeleteCachedEntry<Block>, &block->cache_handle, statistics);
    if (!s.ok()) {
      delete block->value;
      block->value = nullptr;
    }
  }

  return s;
}

Status BlockBasedTable::CreateFilterIndexReader(std::unique_ptr<IndexReader>* filter_index_reader) {
  auto base_file_reader = rep_->base_reader_with_cache_prefix->reader.get();
  auto env = rep_->ioptions.env;
  auto footer = rep_->footer;
  return BinarySearchIndexReader::Create(base_file_reader, footer, rep_->filter_handle, env,
      BytewiseComparator(), filter_index_reader);
}

FilterBlockReader* BlockBasedTable::ReadFilterBlock(const BlockHandle& filter_handle, Rep* rep,
    size_t* filter_size) {
  // TODO: We might want to unify with ReadBlockFromFile() if we start
  // requiring checksum verification in Table::Open.
  if (rep->filter_type == FilterType::kNoFilter) {
    return nullptr;
  }
  BlockContents block;
  if (!ReadBlockContents(rep->base_reader_with_cache_prefix->reader.get(), rep->footer,
      ReadOptions::kDefault, filter_handle, &block, rep->ioptions.env, false).ok()) {
    // Error reading the block
    return nullptr;
  }

  if (filter_size) {
    *filter_size = block.data.size();
  }

  assert(rep->filter_policy);

  switch (rep->filter_type) {
    case FilterType::kNoFilter:
      // Shouldn't happen, since we already checked for that above. In case of memory corruption
      // will be caught after switch statement.
      break;
    case FilterType::kBlockBasedFilter:
      return new BlockBasedFilterBlockReader(
          rep->prefix_filtering ? rep->ioptions.prefix_extractor : nullptr,
          rep->table_options, rep->whole_key_filtering, std::move(block));
    case FilterType::kFullFilter: {
      auto filter_bits_reader = rep->filter_policy->GetFilterBitsReader(block.data);
      assert(filter_bits_reader);
      return new FullFilterBlockReader(
          rep->prefix_filtering ? rep->ioptions.prefix_extractor : nullptr,
          rep->whole_key_filtering, std::move(block), filter_bits_reader);
    }
    case FilterType::kFixedSizeFilter:
      return new FixedSizeFilterBlockReader(
          rep->prefix_filtering ? rep->ioptions.prefix_extractor : nullptr,
          rep->table_options, rep->whole_key_filtering, std::move(block));
      break;
  }
  RLOG(InfoLogLevel::FATAL_LEVEL, rep->ioptions.info_log, "Corrupted filter_type: %d",
      rep->filter_type);
  return nullptr;
}

Status BlockBasedTable::GetFixedSizeFilterBlockHandle(const Slice& filter_key,
    BlockHandle* filter_block_handle) const {
  // Determine block of fixed-size bloom filter using filter index.
  BlockIter fiter;
  rep_->filter_index_reader->NewIterator(&fiter,
      true /* ignored by BinarySearchIndexReader which we use as filter_index_reader */);
  fiter.Seek(filter_key);
  if (fiter.Valid()) {
    Slice filter_block_handle_encoded = fiter.value();
    return filter_block_handle->DecodeFrom(&filter_block_handle_encoded);
  } else {
    // We are beyond the index, that means key is absent in filter, we use null block handle
    // stub to indicate that.
    filter_block_handle->set_offset(0);
    filter_block_handle->set_size(0);
    return Status::OK();
  }
}

Slice BlockBasedTable::GetFilterKey(const Slice &internal_key) const {
  const Slice user_key = ExtractUserKey(internal_key);
  return rep_->filter_key_transformer ?
      rep_->filter_key_transformer->Transform(user_key) : user_key;
}

BlockBasedTable::CachableEntry<FilterBlockReader> BlockBasedTable::GetFilter(
    const QueryId query_id,
    bool no_io,
    const Slice* filter_key) const {
  const bool is_fixed_size_filter = rep_->filter_type == FilterType::kFixedSizeFilter;

  // Key is required for fixed size filter.
  assert(!is_fixed_size_filter || filter_key != nullptr);

  // If cache_index_and_filter_blocks is false, filter (except fixed-size filter) should be
  // pre-populated.
  // We will return rep_->filter anyway. rep_->filter can be nullptr if filter
  // read fails at Open() time. We don't want to reload again since it will
  // most probably fail again.
  // Note: rep_->filter can be nullptr also if Open was called with
  // prefetch_index_and_filter == false. That means bloom filters are not be used if
  // both prefetch_index_and_filter and table_options.cache_index_and_filter_blocks are false.
  if (!rep_->table_options.cache_index_and_filter_blocks && !is_fixed_size_filter) {
    return {rep_->filter.get(), nullptr /* cache handle */};
  }

  PERF_TIMER_GUARD(read_filter_block_nanos);

  Cache* block_cache = rep_->table_options.block_cache.get();
  if (rep_->filter_policy == nullptr /* do not use filter */ ||
      block_cache == nullptr /* no block cache at all */) {
    // If we get here, we have:
    // table_options.cache_index_and_filter_blocks || is_fixed_size_filter
    // table_options.block_cache == nullptr
    return {nullptr /* filter */, nullptr /* cache handle */};
  }

  const BlockHandle* filter_block_handle;
  // Determine filter block handle
  BlockHandle fixed_size_filter_block_handle;
  if (is_fixed_size_filter) {
    Status s = GetFixedSizeFilterBlockHandle(*filter_key, &fixed_size_filter_block_handle);
    if (s.ok()) {
      if (fixed_size_filter_block_handle.IsNull()) {
        // Key is beyond filter index - return stub filter.
        return rep_->not_matching_filter_entry;
      }
      filter_block_handle = &fixed_size_filter_block_handle;
    } else {
      // If we failed to decode filter block handle from filter index we will just log error in
      // production to continue operation in case of just filter corruption,
      // but we should fail in debug and under tests to be able to catch possible bugs.
      RLOG(InfoLogLevel::ERROR_LEVEL, rep_->ioptions.info_log,
          "Failed to decode fixed-size filter block handle from filter index.");
      FAIL_IF_NOT_PRODUCTION();
      return {nullptr /* filter */, nullptr /* cache handle */};
    }
  } else {
    filter_block_handle = &rep_->filter_handle;
  }

  // Fetching from the cache
  char cache_key_buffer[kMaxCacheKeyPrefixSize + kMaxVarint64Length];
  auto filter_block_cache_key = GetCacheKey(rep_->base_reader_with_cache_prefix->cache_key_prefix,
      *filter_block_handle, cache_key_buffer);

  Statistics* statistics = rep_->ioptions.statistics;
  auto cache_handle = GetEntryFromCache(block_cache, filter_block_cache_key,
      BLOCK_CACHE_FILTER_MISS, BLOCK_CACHE_FILTER_HIT, statistics, query_id);

  FilterBlockReader* filter = nullptr;
  if (cache_handle != nullptr) {
    filter = static_cast<FilterBlockReader*>(block_cache->Value(cache_handle));
  } else if (no_io && rep_->filter_type != FilterType::kFixedSizeFilter) {
    // Do not invoke any io.
    return CachableEntry<FilterBlockReader>();
  } else {
    // For fixed-size filter we don't prefetch all filter blocks and ignore no_io parameter always
    // loading necessary filter block through block cache.
    size_t filter_size = 0;
    filter = ReadFilterBlock(*filter_block_handle, rep_, &filter_size);
    if (filter != nullptr) {
      assert(filter_size > 0);
      Status s = block_cache->Insert(filter_block_cache_key, query_id,
                                     filter, filter_size,
                                     &DeleteCachedEntry<FilterBlockReader>, &cache_handle,
                                     statistics);
      if (!s.ok()) {
        delete filter;
        return CachableEntry<FilterBlockReader>();
      }
    }
  }

  return { filter, cache_handle };
}

InternalIterator* BlockBasedTable::NewIndexIterator(
    const ReadOptions& read_options, BlockIter* input_iter) {
  // index reader has already been pre-populated.
  if (rep_->data_index_reader) {
    return rep_->data_index_reader->NewIterator(
        input_iter, read_options.total_order_seek);
  }
  PERF_TIMER_GUARD(read_index_block_nanos);

  bool no_io = read_options.read_tier == kBlockCacheTier;
  Cache* block_cache = rep_->table_options.block_cache.get();
  char cache_key[kMaxCacheKeyPrefixSize + kMaxVarint64Length];
  auto key = GetCacheKey(rep_->base_reader_with_cache_prefix->cache_key_prefix,
                         rep_->footer.index_handle(), cache_key);
  Statistics* statistics = rep_->ioptions.statistics;
  auto cache_handle =
      GetEntryFromCache(block_cache, key, BLOCK_CACHE_INDEX_MISS,
          BLOCK_CACHE_INDEX_HIT, statistics, read_options.query_id);

  if (cache_handle == nullptr && no_io) {
    if (input_iter != nullptr) {
      input_iter->SetStatus(STATUS(Incomplete, "no blocking io"));
      return input_iter;
    } else {
      return NewErrorInternalIterator(STATUS(Incomplete, "no blocking io"));
    }
  }

  IndexReader* index_reader;
  if (cache_handle != nullptr) {
    index_reader = static_cast<IndexReader*>(block_cache->Value(cache_handle));
  } else {
    // Create index reader and put it in the cache.
    std::unique_ptr<IndexReader> index_reader_unique;
    Status s = CreateDataBlockIndexReader(&index_reader_unique);
    if (s.ok()) {
      s = block_cache->Insert(key, read_options.query_id, index_reader_unique.get(),
                              index_reader_unique->usable_size(),
                              &DeleteCachedEntry<IndexReader>, &cache_handle, statistics);
    }

    if (s.ok()) {
      assert(cache_handle != nullptr);
      index_reader = index_reader_unique.release();
    } else {
      // make sure if something goes wrong, data_index_reader shall remain intact.
      if (input_iter != nullptr) {
        input_iter->SetStatus(s);
        return input_iter;
      } else {
        return NewErrorInternalIterator(s);
      }
    }
  }

  assert(cache_handle);
  auto* iter = index_reader->NewIterator(
      input_iter, read_options.total_order_seek);
  iter->RegisterCleanup(&ReleaseCachedEntry, block_cache, cache_handle);
  return iter;
}

// Convert an index iterator value (i.e., an encoded BlockHandle)
// into an iterator over the contents of the corresponding block.
// If input_iter is null, new a iterator
// If input_iter is not null, update this iter and return it
InternalIterator* BlockBasedTable::NewDataBlockIterator(
    Rep* rep, const ReadOptions& ro, const Slice& index_value,
    BlockIter* input_iter) {
  PERF_TIMER_GUARD(new_table_block_iter_nanos);

  const bool no_io = (ro.read_tier == kBlockCacheTier);
  Cache* block_cache = rep->table_options.block_cache.get();
  Cache* block_cache_compressed =
      rep->table_options.block_cache_compressed.get();
  CachableEntry<Block> block;

  BlockHandle handle;
  Slice input = index_value;
  // We intentionally allow extra stuff in index_value so that we
  // can add more features in the future.
  Status s = handle.DecodeFrom(&input);

  if (!s.ok()) {
    if (input_iter != nullptr) {
      input_iter->SetStatus(s);
      return input_iter;
    } else {
      return NewErrorInternalIterator(s);
    }
  }

  // If either block cache is enabled, we'll try to read from it.
  if (block_cache != nullptr || block_cache_compressed != nullptr) {
    Statistics* statistics = rep->ioptions.statistics;
    char cache_key[kMaxCacheKeyPrefixSize + kMaxVarint64Length];
    char compressed_cache_key[kMaxCacheKeyPrefixSize + kMaxVarint64Length];
    Slice key, /* key to the block cache */
        ckey /* key to the compressed block cache */;

    // create key for block cache
    if (block_cache != nullptr) {
      key = GetCacheKey(rep->data_reader_with_cache_prefix->cache_key_prefix, handle, cache_key);
    }

    if (block_cache_compressed != nullptr) {
      ckey = GetCacheKey(rep->data_reader_with_cache_prefix->compressed_cache_key_prefix, handle,
          compressed_cache_key);
    }

    s = GetDataBlockFromCache(key, ckey, block_cache, block_cache_compressed,
                              statistics, ro, &block,
                              rep->table_options.format_version);

    if (block.value == nullptr && !no_io && ro.fill_cache) {
      std::unique_ptr<Block> raw_block;
      {
        StopWatch sw(rep->ioptions.env, statistics, READ_BLOCK_GET_MICROS);
        s = ReadBlockFromFile(rep->data_reader_with_cache_prefix->reader.get(),
            rep->footer, ro, handle, &raw_block, rep->ioptions.env,
            block_cache_compressed == nullptr);
      }

      if (s.ok()) {
        s = PutDataBlockToCache(key, ckey, block_cache, block_cache_compressed,
                                ro, statistics, &block, raw_block.release(),
                                rep->table_options.format_version);
      }
    }
  }

  // Didn't get any data from block caches.
  if (s.ok() && block.value == nullptr) {
    if (no_io) {
      // Could not read from block_cache and can't do IO
      if (input_iter != nullptr) {
        input_iter->SetStatus(STATUS(Incomplete, "no blocking io"));
        return input_iter;
      } else {
        return NewErrorInternalIterator(STATUS(Incomplete, "no blocking io"));
      }
    }
    std::unique_ptr<Block> block_value;
    s = ReadBlockFromFile(rep->data_reader_with_cache_prefix->reader.get(), rep->footer, ro, handle,
                          &block_value, rep->ioptions.env);
    if (s.ok()) {
      block.value = block_value.release();
    }
  }

  InternalIterator* iter;
  if (s.ok() && block.value != nullptr) {
    iter = block.value->NewIterator(&rep->internal_comparator, input_iter);
    if (block.cache_handle != nullptr) {
      iter->RegisterCleanup(&ReleaseCachedEntry, block_cache,
          block.cache_handle);
    } else {
      iter->RegisterCleanup(&DeleteHeldResource<Block>, block.value, nullptr);
    }
  } else {
    if (input_iter != nullptr) {
      input_iter->SetStatus(s);
      iter = input_iter;
    } else {
      iter = NewErrorInternalIterator(s);
    }
  }
  return iter;
}

class BlockBasedTable::BlockEntryIteratorState : public TwoLevelIteratorState {
 public:
  BlockEntryIteratorState(BlockBasedTable* table,
                          const ReadOptions& read_options, bool skip_filters)
      : TwoLevelIteratorState(table->rep_->ioptions.prefix_extractor !=
                              nullptr),
        table_(table),
        read_options_(read_options),
        skip_filters_(skip_filters) {}

  InternalIterator* NewSecondaryIterator(const Slice& index_value) override {
    return NewDataBlockIterator(table_->rep_, read_options_, index_value);
  }

  bool PrefixMayMatch(const Slice& internal_key) override {
    if (read_options_.total_order_seek || skip_filters_) {
      return true;
    }
    return table_->PrefixMayMatch(internal_key);
  }

 private:
  // Don't own table_
  BlockBasedTable* table_;
  const ReadOptions read_options_;
  bool skip_filters_;
};

// This will be broken if the user specifies an unusual implementation
// of Options.comparator, or if the user specifies an unusual
// definition of prefixes in BlockBasedTableOptions.filter_policy.
// In particular, we require the following three properties:
//
// 1) key.starts_with(prefix(key))
// 2) Compare(prefix(key), key) <= 0.
// 3) If Compare(key1, key2) <= 0, then Compare(prefix(key1), prefix(key2)) <= 0
//
// Otherwise, this method guarantees no I/O will be incurred.
//
// REQUIRES: this method shouldn't be called while the DB lock is held.
bool BlockBasedTable::PrefixMayMatch(const Slice& internal_key) {
  if (!rep_->filter_policy) {
    return true;
  }

  assert(rep_->ioptions.prefix_extractor != nullptr);
  auto user_key = ExtractUserKey(internal_key);
  auto filter_key = rep_->filter_key_transformer ?
      rep_->filter_key_transformer->Transform(user_key) : user_key;
  if (!rep_->ioptions.prefix_extractor->InDomain(filter_key) ||
      !rep_->ioptions.prefix_extractor->InDomain(user_key)) {
    return true;
  }
  auto user_key_prefix = rep_->ioptions.prefix_extractor->Transform(user_key);
  auto filter_key_prefix = rep_->ioptions.prefix_extractor->Transform(filter_key);
  InternalKey internal_key_prefix(user_key_prefix, kMaxSequenceNumber, kTypeValue);
  auto internal_prefix = internal_key_prefix.Encode();

  bool may_match = true;
  Status s;

  // To prevent any io operation in this method, we set `read_tier` to make
  // sure we always read index or filter only when they have already been
  // loaded to memory.
  ReadOptions no_io_read_options;
  no_io_read_options.read_tier = kBlockCacheTier;

  // First check non block-based filter.
  auto filter_entry = GetFilter(no_io_read_options.query_id, true /* no io */, &filter_key);
  FilterBlockReader* filter = filter_entry.value;
  const bool is_block_based_filter = rep_->filter_type == FilterType::kBlockBasedFilter;
  if (filter != nullptr && !is_block_based_filter) {
    may_match = filter->PrefixMayMatch(filter_key_prefix);
  }

  // If filter is block-based or checking filter was not successful we need to get data block
  // offset. For block-based filter we need to know offset of data block to get and check
  // corresponding filter block. For non block-based filter we just need offset to try to get data
  // for the key.
  if (may_match) {
    unique_ptr<InternalIterator> iiter(NewIndexIterator(no_io_read_options));
    iiter->Seek(internal_prefix);

    if (!iiter->Valid()) {
      // we're past end of file
      // if it's incomplete, it means that we avoided I/O
      // and we're not really sure that we're past the end
      // of the file
      may_match = iiter->status().IsIncomplete();
    } else if (ExtractUserKey(iiter->key()).starts_with(
                ExtractUserKey(internal_prefix))) {
      // we need to check for this subtle case because our only
      // guarantee is that "the key is a string >= last key in that data
      // block" according to the doc/table_format.txt spec.
      //
      // Suppose iiter->key() starts with the desired prefix; it is not
      // necessarily the case that the corresponding data block will
      // contain the prefix, since iiter->key() need not be in the
      // block.  However, the next data block may contain the prefix, so
      // we return true to play it safe.
      may_match = true;
    } else if (filter != nullptr && is_block_based_filter) {
      // iiter->key() does NOT start with the desired prefix.  Because
      // Seek() finds the first key that is >= the seek target, this
      // means that iiter->key() > prefix.  Thus, any data blocks coming
      // after the data block corresponding to iiter->key() cannot
      // possibly contain the key.  Thus, the corresponding data block
      // is the only on could potentially contain the prefix.
      Slice handle_value = iiter->value();
      BlockHandle handle;
      s = handle.DecodeFrom(&handle_value);
      assert(s.ok());
      may_match = filter->PrefixMayMatch(filter_key_prefix, handle.offset());
    }
  }

  Statistics* statistics = rep_->ioptions.statistics;
  RecordTick(statistics, BLOOM_FILTER_PREFIX_CHECKED);
  if (!may_match) {
    RecordTick(statistics, BLOOM_FILTER_PREFIX_USEFUL);
  }

  filter_entry.Release(rep_->table_options.block_cache.get());
  return may_match;
}

InternalIterator* BlockBasedTable::NewIterator(const ReadOptions& read_options,
                                               Arena* arena,
                                               bool skip_filters) {
  BlockEntryIteratorState* state = new BlockEntryIteratorState(this, read_options, skip_filters);

  // TODO: unify the semantics across NewIterator callsites, so that we can pass an arena across
  // them, and decide the free / no free based on that. This callsite, for example, allows us to
  // put the top level iterator on the arena and potentially even the State object, however, not
  // the IndexIterator, as that does not expose arena allocation semantics...
  const bool need_free_iter = arena == nullptr;
  unique_ptr<InternalIterator> internal_iterator(NewTwoLevelIterator(
      state, NewIndexIterator(read_options), arena, true /* need_free_iter_and_state */));

  if (!read_options.use_bloom_on_scan) {
    return internal_iterator.release();
  }

  if (arena == nullptr) {
    return new BloomFilterAwareIterator(
        this, read_options, skip_filters, internal_iterator.release(), need_free_iter);
  } else {
    auto mem = arena->AllocateAligned(sizeof(BloomFilterAwareIterator));
    return new (mem) BloomFilterAwareIterator(
        this, read_options, skip_filters, internal_iterator.release(), need_free_iter);
  }
}

bool BlockBasedTable::NonBlockBasedFilterKeyMayMatch(FilterBlockReader* filter,
    const Slice& filter_key) const {
  assert(rep_->filter_type != FilterType::kBlockBasedFilter);
  if (filter == nullptr) {
    return true;
  }
  RecordTick(rep_->ioptions.statistics, BLOOM_FILTER_CHECKED);
  if (!filter->KeyMayMatch(filter_key)) {
    return false;
  }
  if (rep_->ioptions.prefix_extractor &&
      rep_->ioptions.prefix_extractor->InDomain(filter_key) &&
      !filter->PrefixMayMatch(
          rep_->ioptions.prefix_extractor->Transform(filter_key))) {
    return false;
  }
  return true;
}

Status BlockBasedTable::Get(const ReadOptions& read_options, const Slice& internal_key,
                            GetContext* get_context, bool skip_filters) {
  Status s;
  CachableEntry<FilterBlockReader> filter_entry;
  Slice filter_key;
  if (!skip_filters) {
    filter_key = GetFilterKey(internal_key);
    filter_entry = GetFilter(read_options.query_id,
                             read_options.read_tier == kBlockCacheTier,
                             &filter_key);
  }
  FilterBlockReader* filter = filter_entry.value;

  const bool is_block_based_filter = rep_->filter_type == FilterType::kBlockBasedFilter;

  // First check non block-based filter.
  if (!is_block_based_filter && !NonBlockBasedFilterKeyMayMatch(filter, filter_key)) {
    RecordTick(rep_->ioptions.statistics, BLOOM_FILTER_USEFUL);
  } else {
    // Either filter is block-based or key may match.
    BlockIter iiter;
    NewIndexIterator(read_options, &iiter);

    bool done = false;
    for (iiter.Seek(internal_key); iiter.Valid() && !done; iiter.Next()) {
      {
        Slice data_block_handle_encoded = iiter.value();

        if (!skip_filters && is_block_based_filter) {
          RecordTick(rep_->ioptions.statistics, BLOOM_FILTER_CHECKED);
          BlockHandle data_block_handle;
          const bool absent_from_filter =
              data_block_handle.DecodeFrom(&data_block_handle_encoded).ok()
              && !filter->KeyMayMatch(filter_key, data_block_handle.offset());

          if (absent_from_filter) {
            // Not found
            // TODO: think about interaction with Merge. If a user key cannot
            // cross one data block, we should be fine.
            RecordTick(rep_->ioptions.statistics, BLOOM_FILTER_USEFUL);
            break;
          }
        }
      }

      BlockIter biter;
      NewDataBlockIterator(rep_, read_options, iiter.value(), &biter);

      if (read_options.read_tier == kBlockCacheTier &&
          biter.status().IsIncomplete()) {
        // couldn't get block from block_cache
        // Update Saver.state to Found because we are only looking for whether
        // we can guarantee the key is not there when "no_io" is set
        get_context->MarkKeyMayExist();
        break;
      }
      if (!biter.status().ok()) {
        s = biter.status();
        break;
      }

      // Call the *saver function on each entry/block until it returns false
      for (biter.Seek(internal_key); biter.Valid(); biter.Next()) {
        ParsedInternalKey parsed_key;
        if (!ParseInternalKey(biter.key(), &parsed_key)) {
          s = STATUS(Corruption, Slice());
        }

        if (!get_context->SaveValue(parsed_key, biter.value())) {
          done = true;
          break;
        }
      }
      s = biter.status();
    }
    if (s.ok()) {
      s = iiter.status();
    }
  }

  filter_entry.Release(rep_->table_options.block_cache.get());
  return s;
}

Status BlockBasedTable::Prefetch(const Slice* const begin,
                                 const Slice* const end) {
  auto& comparator = rep_->internal_comparator;
  // pre-condition
  if (begin && end && comparator.Compare(*begin, *end) > 0) {
    return STATUS(InvalidArgument, *begin, *end);
  }

  BlockIter iiter;
  NewIndexIterator(ReadOptions::kDefault, &iiter);

  if (!iiter.status().ok()) {
    // error opening index iterator
    return iiter.status();
  }

  // indicates if we are on the last page that need to be pre-fetched
  bool prefetching_boundary_page = false;

  for (begin ? iiter.Seek(*begin) : iiter.SeekToFirst(); iiter.Valid();
       iiter.Next()) {
    Slice block_handle = iiter.value();

    if (end && comparator.Compare(iiter.key(), *end) >= 0) {
      if (prefetching_boundary_page) {
        break;
      }

      // The index entry represents the last key in the data block.
      // We should load this page into memory as well, but no more
      prefetching_boundary_page = true;
    }

    // Load the block specified by the block_handle into the block cache
    BlockIter biter;
    NewDataBlockIterator(rep_, ReadOptions::kDefault, block_handle, &biter);

    if (!biter.status().ok()) {
      // there was an unexpected error while pre-fetching
      return biter.status();
    }
  }

  return Status::OK();
}

bool BlockBasedTable::TEST_KeyInCache(const ReadOptions& options,
                                      const Slice& key) {
  std::unique_ptr<InternalIterator> iiter(NewIndexIterator(options));
  iiter->Seek(key);
  assert(iiter->Valid());
  CachableEntry<Block> block;

  BlockHandle handle;
  Slice input = iiter->value();
  Status s = handle.DecodeFrom(&input);
  assert(s.ok());
  Cache* block_cache = rep_->table_options.block_cache.get();
  assert(block_cache != nullptr);

  char cache_key_storage[kMaxCacheKeyPrefixSize + kMaxVarint64Length];
  Slice cache_key =
      GetCacheKey(rep_->data_reader_with_cache_prefix->cache_key_prefix, handle, cache_key_storage);
  Slice ckey;

  s = GetDataBlockFromCache(cache_key, ckey, block_cache, nullptr, nullptr, options, &block,
      rep_->table_options.format_version);
  assert(s.ok());
  bool in_cache = block.value != nullptr;
  if (in_cache) {
    ReleaseCachedEntry(block_cache, block.cache_handle);
  }
  return in_cache;
}

// REQUIRES: The following fields of rep_ should have already been populated:
//  1. file
//  2. index_handle,
//  3. options
//  4. internal_comparator
//  5. index_type
Status BlockBasedTable::CreateDataBlockIndexReader(
    std::unique_ptr<IndexReader>* index_reader, InternalIterator* preloaded_meta_index_iter) {
  // Some old version of block-based tables don't have index type present in
  // table properties. If that's the case we can safely use the kBinarySearch.
  auto index_type_on_file = BlockBasedTableOptions::kBinarySearch;
  if (rep_->table_properties) {
    auto& props = rep_->table_properties->user_collected_properties;
    auto pos = props.find(BlockBasedTablePropertyNames::kIndexType);
    if (pos != props.end()) {
      index_type_on_file = static_cast<BlockBasedTableOptions::IndexType>(
          DecodeFixed32(pos->second.c_str()));
    }
  }

  auto file = rep_->base_reader_with_cache_prefix->reader.get();
  auto env = rep_->ioptions.env;
  auto comparator = &rep_->internal_comparator;
  const Footer& footer = rep_->footer;

  if (index_type_on_file == BlockBasedTableOptions::kHashSearch &&
      rep_->ioptions.prefix_extractor == nullptr) {
    RLOG(InfoLogLevel::WARN_LEVEL, rep_->ioptions.info_log,
        "BlockBasedTableOptions::kHashSearch requires "
        "options.prefix_extractor to be set."
        " Fall back to binary search index.");
    index_type_on_file = BlockBasedTableOptions::kBinarySearch;
  }

  switch (index_type_on_file) {
    case BlockBasedTableOptions::kBinarySearch: {
      return BinarySearchIndexReader::Create(
          file, footer, footer.index_handle(), env, comparator, index_reader);
    }
    case BlockBasedTableOptions::kHashSearch: {
      std::unique_ptr<Block> meta_guard;
      std::unique_ptr<InternalIterator> meta_iter_guard;
      auto meta_index_iter = preloaded_meta_index_iter;
      if (meta_index_iter == nullptr) {
        auto s = ReadMetaBlock(rep_, &meta_guard, &meta_iter_guard);
        if (!s.ok()) {
          // we simply fall back to binary search in case there is any
          // problem with prefix hash index loading.
          RLOG(InfoLogLevel::WARN_LEVEL, rep_->ioptions.info_log,
              "Unable to read the metaindex block."
              " Fall back to binary search index.");
          return BinarySearchIndexReader::Create(
            file, footer, footer.index_handle(), env, comparator, index_reader);
        }
        meta_index_iter = meta_iter_guard.get();
      }

      // We need to wrap data with internal_prefix_transform to make sure it can
      // handle prefix correctly.
      rep_->internal_prefix_transform.reset(
          new InternalKeySliceTransform(rep_->ioptions.prefix_extractor));
      return HashIndexReader::Create(
          rep_->internal_prefix_transform.get(), footer, file, env, comparator,
          footer.index_handle(), meta_index_iter, index_reader,
          rep_->hash_index_allow_collision);
    }
    default: {
      std::string error_message =
          "Unrecognized index type: " + ToString(rep_->index_type);
      return STATUS(InvalidArgument, error_message.c_str());
    }
  }
}

uint64_t BlockBasedTable::ApproximateOffsetOf(const Slice& key) {
  unique_ptr<InternalIterator> index_iter(NewIndexIterator(ReadOptions::kDefault));

  index_iter->Seek(key);
  uint64_t result;
  if (index_iter->Valid()) {
    BlockHandle handle;
    Slice input = index_iter->value();
    Status s = handle.DecodeFrom(&input);
    if (s.ok()) {
      result = handle.offset();
    } else {
      // Strange: we can't decode the block handle in the index block.
      // We'll just return the offset of the metaindex block, which is
      // close to the whole file size for this case.
      result = rep_->footer.metaindex_handle().offset();
    }
  } else {
    // key is past the last key in the file. If table_properties is not
    // available, approximate the offset by returning the offset of the
    // metaindex block (which is right near the end of the file).
    result = 0;
    if (rep_->table_properties) {
      result = rep_->table_properties->data_size;
    }
    // table_properties is not present in the table.
    if (result == 0) {
      result = rep_->footer.metaindex_handle().offset();
    }
  }
  return result;
}

bool BlockBasedTable::TEST_filter_block_preloaded() const {
  return rep_->filter != nullptr;
}

bool BlockBasedTable::TEST_index_reader_preloaded() const {
  return rep_->data_index_reader != nullptr;
}

Status BlockBasedTable::DumpTable(WritableFile* out_file) {
  // Output Footer
  out_file->Append(
      "Footer Details:\n"
      "--------------------------------------\n"
      "  ");
  out_file->Append(rep_->footer.ToString().c_str());
  out_file->Append("\n");

  // Output MetaIndex
  out_file->Append(
      "Metaindex Details:\n"
      "--------------------------------------\n");
  std::unique_ptr<Block> meta;
  std::unique_ptr<InternalIterator> meta_iter;
  Status s = ReadMetaBlock(rep_, &meta, &meta_iter);
  if (s.ok()) {
    for (meta_iter->SeekToFirst(); meta_iter->Valid(); meta_iter->Next()) {
      s = meta_iter->status();
      if (!s.ok()) {
        return s;
      }
      if (meta_iter->key() == rocksdb::kPropertiesBlock) {
        out_file->Append("  Properties block handle: ");
        out_file->Append(meta_iter->value().ToString(true).c_str());
        out_file->Append("\n");
      } else if (strstr(meta_iter->key().ToString().c_str(),
                        "filter.rocksdb.") != nullptr) {
        out_file->Append("  Filter block handle: ");
        out_file->Append(meta_iter->value().ToString(true).c_str());
        out_file->Append("\n");
      }
    }
    out_file->Append("\n");
  } else {
    return s;
  }

  // Output TableProperties
  const rocksdb::TableProperties* table_properties;
  table_properties = rep_->table_properties.get();

  if (table_properties != nullptr) {
    out_file->Append(
        "Table Properties:\n"
        "--------------------------------------\n"
        "  ");
    out_file->Append(table_properties->ToString("\n  ", ": ").c_str());
    out_file->Append("\n");
  }

  // Output Filter blocks
  if (!rep_->filter && !table_properties->filter_policy_name.empty()) {
    // Support only BloomFilter as off now
    rocksdb::BlockBasedTableOptions table_options;
    table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(1));
    if (table_properties->filter_policy_name.compare(
            table_options.filter_policy->Name()) == 0) {
      std::string filter_block_key = kFilterBlockPrefix;
      filter_block_key.append(table_properties->filter_policy_name);
      BlockHandle handle;
      if (FindMetaBlock(meta_iter.get(), filter_block_key, &handle).ok()) {
        BlockContents block;
        if (ReadBlockContents(rep_->base_reader_with_cache_prefix->reader.get(), rep_->footer,
            ReadOptions::kDefault, handle, &block, rep_->ioptions.env, false).ok()) {
          rep_->filter.reset(new BlockBasedFilterBlockReader(
              rep_->ioptions.prefix_extractor, table_options,
              table_options.whole_key_filtering, std::move(block)));
        }
      }
    }
  }
  if (rep_->filter) {
    out_file->Append(
        "Filter Details:\n"
        "--------------------------------------\n"
        "  ");
    out_file->Append(rep_->filter->ToString().c_str());
    out_file->Append("\n");
  }

  // Output Index block
  s = DumpIndexBlock(out_file);
  if (!s.ok()) {
    return s;
  }
  // Output Data blocks
  s = DumpDataBlocks(out_file);

  return s;
}

Status BlockBasedTable::DumpIndexBlock(WritableFile* out_file) {
  out_file->Append(
      "Index Details:\n"
      "--------------------------------------\n");

  std::unique_ptr<InternalIterator> blockhandles_iter(
      NewIndexIterator(ReadOptions::kDefault));
  Status s = blockhandles_iter->status();
  if (!s.ok()) {
    out_file->Append("Can not read Index Block \n\n");
    return s;
  }

  out_file->Append("  Block key hex dump: Data block handle\n");
  out_file->Append("  Block key ascii\n\n");
  for (blockhandles_iter->SeekToFirst(); blockhandles_iter->Valid();
       blockhandles_iter->Next()) {
    s = blockhandles_iter->status();
    if (!s.ok()) {
      break;
    }
    Slice key = blockhandles_iter->key();
    InternalKey ikey = InternalKey::DecodeFrom(key);

    out_file->Append("  HEX    ");
    out_file->Append(ikey.user_key().ToString(true).c_str());
    out_file->Append(": ");
    out_file->Append(blockhandles_iter->value().ToString(true).c_str());
    out_file->Append("\n");

    std::string str_key = ikey.user_key().ToString();
    std::string res_key("");
    char cspace = ' ';
    for (size_t i = 0; i < str_key.size(); i++) {
      res_key.append(&str_key[i], 1);
      res_key.append(1, cspace);
    }
    out_file->Append("  ASCII  ");
    out_file->Append(res_key.c_str());
    out_file->Append("\n  ------\n");
  }
  out_file->Append("\n");
  return Status::OK();
}

Status BlockBasedTable::DumpDataBlocks(WritableFile* out_file) {
  std::unique_ptr<InternalIterator> blockhandles_iter(
      NewIndexIterator(ReadOptions::kDefault));
  Status s = blockhandles_iter->status();
  if (!s.ok()) {
    out_file->Append("Can not read Index Block \n\n");
    return s;
  }

  size_t block_id = 1;
  for (blockhandles_iter->SeekToFirst(); blockhandles_iter->Valid();
       block_id++, blockhandles_iter->Next()) {
    s = blockhandles_iter->status();
    if (!s.ok()) {
      break;
    }

    out_file->Append("Data Block # ");
    out_file->Append(rocksdb::ToString(block_id));
    out_file->Append(" @ ");
    out_file->Append(blockhandles_iter->value().ToString(true).c_str());
    out_file->Append("\n");
    out_file->Append("--------------------------------------\n");

    std::unique_ptr<InternalIterator> datablock_iter;
    datablock_iter.reset(
        NewDataBlockIterator(rep_, ReadOptions::kDefault, blockhandles_iter->value()));
    s = datablock_iter->status();

    if (!s.ok()) {
      out_file->Append("Error reading the block - Skipped \n\n");
      continue;
    }

    for (datablock_iter->SeekToFirst(); datablock_iter->Valid();
         datablock_iter->Next()) {
      s = datablock_iter->status();
      if (!s.ok()) {
        out_file->Append("Error reading the block - Skipped \n");
        break;
      }
      Slice key = datablock_iter->key();
      Slice value = datablock_iter->value();
      InternalKey ikey = InternalKey::DecodeFrom(key);
      InternalKey iValue = InternalKey::DecodeFrom(value);

      out_file->Append("  HEX    ");
      out_file->Append(ikey.user_key().ToString(true).c_str());
      out_file->Append(": ");
      out_file->Append(iValue.user_key().ToString(true).c_str());
      out_file->Append("\n");

      std::string str_key = ikey.user_key().ToString();
      std::string str_value = iValue.user_key().ToString();
      std::string res_key(""), res_value("");
      char cspace = ' ';
      for (size_t i = 0; i < str_key.size(); i++) {
        res_key.append(&str_key[i], 1);
        res_key.append(1, cspace);
      }
      for (size_t i = 0; i < str_value.size(); i++) {
        res_value.append(&str_value[i], 1);
        res_value.append(1, cspace);
      }

      out_file->Append("  ASCII  ");
      out_file->Append(res_key.c_str());
      out_file->Append(": ");
      out_file->Append(res_value.c_str());
      out_file->Append("\n  ------\n");
    }
    out_file->Append("\n");
  }
  return Status::OK();
}

}  // namespace rocksdb