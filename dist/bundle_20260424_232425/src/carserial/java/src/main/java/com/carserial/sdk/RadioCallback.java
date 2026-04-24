package com.carserial.sdk;

public interface RadioCallback {
  void onFreqChanged(int band, int freq, boolean stereo, boolean valid);

  void onRdsPs(String ps);

  void onRdsRt(String rt);

  void onSignalStrength(int strength);
}

