package com.example.carserialdemo;

import android.os.Bundle;
import android.widget.Button;
import android.widget.TextView;

import androidx.appcompat.app.AppCompatActivity;

import com.carserial.sdk.CallbackThreadMode;
import com.carserial.sdk.CarSerialSDK;
import com.carserial.sdk.RadioCallback;
import com.carserial.sdk.UpdateCallback;
import com.carserial.sdk.VehicleCallback;

public class MainActivity extends AppCompatActivity {
  private TextView statusView;
  private CarSerialSDK sdk;

  @Override
  protected void onCreate(Bundle savedInstanceState) {
    super.onCreate(savedInstanceState);
    setContentView(R.layout.activity_main);

    statusView = findViewById(R.id.statusText);
    Button initBtn = findViewById(R.id.btnInit);
    Button freqBtn = findViewById(R.id.btnSetFreq);
    Button deinitBtn = findViewById(R.id.btnDeinit);

    sdk = CarSerialSDK.getInstance();
    sdk.setCallbackThreadMode(CallbackThreadMode.MAIN);
    sdk.setVehicleCallback(
        new VehicleCallback() {
          @Override
          public void onAccStateChanged(boolean on) {
            statusView.setText("ACC=" + on);
          }

          @Override
          public void onReverseStateChanged(boolean on) {
            statusView.setText("Reverse=" + on);
          }

          @Override
          public void onCarSpeed(int speedKmh) {
            statusView.setText("Speed=" + speedKmh);
          }

          @Override
          public void onBatteryVoltage(int mv) {
            statusView.setText("Voltage=" + mv + "mV");
          }

          @Override
          public void onMcuVersion(String version) {
            statusView.setText("MCU=" + version);
          }
        });
    sdk.setRadioCallback(
        new RadioCallback() {
          @Override
          public void onFreqChanged(int band, int freq, boolean stereo, boolean valid) {
            statusView.setText(
                "Freq band=" + band + " freq=" + freq + " stereo=" + stereo + " valid=" + valid);
          }

          @Override
          public void onRdsPs(String ps) {
            statusView.setText("RDS PS=" + ps);
          }

          @Override
          public void onRdsRt(String rt) {
            statusView.setText("RDS RT=" + rt);
          }

          @Override
          public void onSignalStrength(int strength) {
            statusView.setText("Signal=" + strength);
          }
        });
    sdk.setUpdateCallback(
        new UpdateCallback() {
          @Override
          public void onUpgradeStarted() {
            statusView.setText("Upgrade started");
          }

          @Override
          public void onUpgradeProgress(int sentBytes, int totalBytes) {
            statusView.setText("Upgrade " + sentBytes + "/" + totalBytes);
          }

          @Override
          public void onUpgradeFinished(boolean success, int errorCode) {
            statusView.setText("Upgrade finished success=" + success + " err=" + errorCode);
          }
        });
    sdk.setRawMessageListener((seq, frameType, msgType, cmd, data) -> {
      statusView.setText(
          "msg seq="
              + seq
              + " type=0x"
              + Integer.toHexString(frameType)
              + " msgType=0x"
              + Integer.toHexString(msgType)
              + " cmd=0x"
              + Integer.toHexString(cmd)
              + " dataLen="
              + (data == null ? 0 : data.length));
    });

    initBtn.setOnClickListener(v -> {
      boolean ok = sdk.init("/dev/ttyS7");
      statusView.setText(ok ? "init success" : "init failed");
    });

    freqBtn.setOnClickListener(v -> {
      int seq = sdk.setRadioFreq(0, 10170);
      statusView.setText("setRadioFreq seq=" + seq);
    });

    deinitBtn.setOnClickListener(v -> {
      sdk.deinit();
      statusView.setText("deinit done");
    });
  }

  @Override
  protected void onDestroy() {
    super.onDestroy();
    if (sdk != null) {
      sdk.deinit();
    }
  }
}

