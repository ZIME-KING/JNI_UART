package com.carserial.sdk;

public interface VehicleCallback {
  void onAccStateChanged(boolean on);

  void onReverseStateChanged(boolean on);

  void onCarSpeed(int speedKmh);

  void onBatteryVoltage(int mv);

  void onMcuVersion(String version);
}

