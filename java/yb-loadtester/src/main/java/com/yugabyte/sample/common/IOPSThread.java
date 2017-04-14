// Copyright (c) YugaByte, Inc.

package com.yugabyte.sample.common;

import org.apache.log4j.Logger;

import com.yugabyte.sample.apps.AppBase;

/**
 * A class that encapsulates a single IO thread. The thread has an index (which is an integer),
 * models an OLTP app and an IO type (read or write). It performs the required IO as long as
 * the app has not completed all its IO.
 */
public class IOPSThread extends Thread {
  private static final Logger LOG = Logger.getLogger(IOPSThread.class);

  // The thread id.
  protected int threadIdx;

  /**
   * The IO types supported by this class.
   */
  public static enum IOType {
    Write,
    Read,
  }
  // The io type this thread performs.
  IOType ioType;

  // The app that is being run.
  protected AppBase app;

  public IOPSThread(int threadIdx, AppBase app, IOType ioType) {
    this.threadIdx = threadIdx;
    this.app = app;
    this.ioType = ioType;
  }

  /**
   * Method that performs the desired type of IO in the IOPS thread.
   */
  @Override
  public void run() {
    try {
      LOG.debug("Starting " + ioType.toString() + " IOPS thread #" + threadIdx);
      int numConsecutiveExceptions = 0;
      while(!app.hasFinished()) {
        try {
          switch (ioType) {
            case Write: app.performWrite(); break;
            case Read: app.performRead(); break;
          }
          numConsecutiveExceptions = 0;
        } catch (RuntimeException e) {
          if (numConsecutiveExceptions++ % 10 == 0) {
            LOG.info("Caught Exception ", e);
          }
          if (numConsecutiveExceptions > 500) {
            LOG.error("Had more than " + numConsecutiveExceptions
                      + " consecutive exceptions. Exiting.", e);
            return;
          }
          try {
            Thread.sleep(1000);
          } catch (InterruptedException ie) {
            LOG.error("Sleep interrupted.", ie);
            return;
          }
        }
      }
    } finally {
      LOG.debug("IOPS thread #" + threadIdx + " finished");
      app.terminate();
    }
  }
}