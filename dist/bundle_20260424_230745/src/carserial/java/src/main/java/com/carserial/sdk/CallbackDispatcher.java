package com.carserial.sdk;

import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;

final class CallbackDispatcher {
  private final Object lock = new Object();

  private volatile CallbackThreadMode mode = CallbackThreadMode.MAIN;
  private HandlerThread thread;
  private Handler handler;

  void setMode(CallbackThreadMode mode) {
    if (mode == null) mode = CallbackThreadMode.MAIN;
    synchronized (lock) {
      this.mode = mode;
      if (mode != CallbackThreadMode.BACKGROUND) {
        stopThreadLocked();
      } else {
        ensureThreadLocked();
      }
    }
  }

  void post(Runnable r) {
    if (r == null) return;
    CallbackThreadMode m = mode;
    if (m == CallbackThreadMode.DIRECT) {
      r.run();
      return;
    }
    if (m == CallbackThreadMode.MAIN) {
      new Handler(Looper.getMainLooper()).post(r);
      return;
    }
    synchronized (lock) {
      ensureThreadLocked();
      handler.post(r);
    }
  }

  void shutdown() {
    synchronized (lock) {
      stopThreadLocked();
    }
  }

  private void ensureThreadLocked() {
    if (thread != null) return;
    thread = new HandlerThread("carserial-callback");
    thread.start();
    handler = new Handler(thread.getLooper());
  }

  private void stopThreadLocked() {
    if (thread == null) return;
    thread.quitSafely();
    thread = null;
    handler = null;
  }
}

