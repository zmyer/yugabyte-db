//--------------------------------------------------------------------------------------------------
// Copyright (c) YugaByte, Inc.
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
//--------------------------------------------------------------------------------------------------

#include "yb/yql/pgsql/processor/statement.h"
#include "yb/yql/pgsql/ql_processor.h"

namespace yb {
namespace ql {

using std::list;
using std::shared_ptr;
using std::string;
using std::unique_ptr;

Statement::Statement(const string& keyspace, const string& text)
    : keyspace_(keyspace), text_(text) {
}

Statement::~Statement() {
}

Status Statement::Prepare(
    QLProcessor *processor, shared_ptr<MemTracker> mem_tracker, PreparedResult::UniPtr *result) {
  // Prepare the statement (parse and semantically analysis). Do so within an exclusive lock.
  if (!prepared_.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> guard(parse_tree_mutex_);

    if (parse_tree_ == nullptr) {
      ParseTree::UniPtr parse_tree;
      RETURN_NOT_OK(processor->Prepare(text_, &parse_tree, false /* reparsed */, mem_tracker));
      parse_tree_ = std::move(parse_tree);
      prepared_.store(true, std::memory_order_release);
    }
  }

  // Return prepared result if requested and the statement is a DML. Do not need a lock here
  // because we have verified that the parse tree is either present already or we have successfully
  // prepared the statement above. The parse tree is guaranteed read-only afterwards.
  if (result != nullptr && parse_tree_->root() != nullptr) {
    const TreeNode& stmt = *parse_tree_->root();
    switch (stmt.opcode()) {
      case TreeNodeOpcode::kPTSelectStmt: FALLTHROUGH_INTENDED;
      case TreeNodeOpcode::kPTInsertStmt: FALLTHROUGH_INTENDED;
      case TreeNodeOpcode::kPTUpdateStmt: FALLTHROUGH_INTENDED;
      case TreeNodeOpcode::kPTDeleteStmt:
        result->reset(new PreparedResult(static_cast<const PTDmlStmt&>(stmt)));
        break;
      default:
        break;
    }
  }

  return Status::OK();
}

Status Statement::Validate() const {
  if (!prepared_.load(std::memory_order_acquire)) {
    return ErrorStatus(ErrorCode::UNPREPARED_STATEMENT);
  }
  DCHECK(parse_tree_ != nullptr) << "Parse tree missing";
  if (parse_tree_->stale()) {
    return ErrorStatus(ErrorCode::STALE_METADATA);
  }
  return Status::OK();
}

Status Statement::ExecuteAsync(
    QLProcessor* processor, const StatementParameters& params, StatementExecutedCallback cb)
    const {
  RETURN_NOT_OK(Validate());
  processor->ExecuteAsync(text_, *parse_tree_, params, std::move(cb));
  return Status::OK();
}

Status Statement::ExecuteBatch(QLProcessor* processor, const StatementParameters& params) const {
  RETURN_NOT_OK(Validate());
  processor->ExecuteBatch(text_, *parse_tree_, params);
  return Status::OK();
}

}  // namespace ql
}  // namespace yb
