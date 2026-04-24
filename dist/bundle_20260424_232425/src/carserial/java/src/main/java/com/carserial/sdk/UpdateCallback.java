package com.carserial.sdk;

public interface UpdateCallback {
  void onUpgradeStarted();

  void onUpgradeProgress(int sentBytes, int totalBytes);

  void onUpgradeFinished(boolean success, int errorCode);
}

