package com.carserial.sdk;

final class NativeBridge {
  static {
    System.loadLibrary("carserial");
  }

  private NativeBridge() {}

  static native boolean nativeInit(String ttyPath, Object eventSink);

  static native boolean nativeInit2(String ttyPath, int baudrate, Object eventSink);

  static native void nativeDeinit();

  static native int nativeSendRaw(int frameType, byte[] payload, boolean needAck);
}

