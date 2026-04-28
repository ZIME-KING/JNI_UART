package com.carserial.sdk;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.atomic.AtomicBoolean;

public final class CarSerialSDK {
  private static final int EVENT_LINK_STATE = 1;
  private static final int EVENT_RAW_FRAME = 2;
  private static final int EVENT_MESSAGE = 3;

  private static final int FRAME_TYPE_APP_COMMAND = 0x80;
  private static final int FRAME_TYPE_DATA = 0x06;

  private static final int MSG_TYPE_PUBLIC = 0x01;
  private static final int MSG_TYPE_RADIO = 0x05;
  private static final int MSG_TYPE_UPDATE = 0x07;

  private static final int PUBLIC_M2A_LINE_DETECT = 0x01;
  private static final int PUBLIC_M2A_MCU_VERSION = 0x02;
  private static final int PUBLIC_M2A_CAR_SPEED = 0x0A;
  private static final int PUBLIC_M2A_VOLTAGE = 0x0D;

  private static final int RADIO_M2A_FREQ = 0x01;
  private static final int RADIO_M2A_PS_PTY = 0x02;
  private static final int RADIO_M2A_RT = 0x03;

  private static final int UPDATE_M2A_UPDATE_ACK = 0x01;
  private static final int UPDATE_M2A_UPDATE_LEN = 0x02;
  private static final int UPDATE_M2A_UPDATE_IND_AQUIRE = 0x03;

  private static final int UPDATE_A2M_UPDATE_REQ = 0x80;
  private static final int UPDATE_A2M_UPDATE_BEGIN = 0x81;
  private static final int UPDATE_A2M_UPDATE_DATA = 0x82;
  private static final int UPDATE_A2M_UPDATE_END = 0x83;

  private static final int CMD_SET_RADIO_FREQ = 1;
  private static final int CMD_SET_RADIO_SEEK = 2;
  private static final int CMD_SET_RADIO_SWITCH = 3;
  private static final int CMD_SET_BACKLIGHT = 4;
  private static final int CMD_SET_AMP_POWER = 5;
  private static final int CMD_SEND_UPGRADE_DATA = 6;

  private static final CarSerialSDK INSTANCE = new CarSerialSDK();

  public static CarSerialSDK getInstance() {
    return INSTANCE;
  }

  private final CallbackDispatcher dispatcher = new CallbackDispatcher();
  private final AtomicBoolean inited = new AtomicBoolean(false);
  private final Object updateLock = new Object();

  private volatile boolean connected;
  private volatile VehicleCallback vehicleCallback;
  private volatile RadioCallback radioCallback;
  private volatile UpdateCallback updateCallback;
  private volatile RawMessageListener rawMessageListener;

  private int updateTotalBytes;
  private int updateLastSentBytes;
  private boolean updateStarted;
  private boolean updateEndRequested;

  private final Object eventSink =
      new Object() {
        @SuppressWarnings("unused")
        public void onNativeEvent(int eventId, int p1, int p2, int p3, String s, byte[] data) {
          handleNativeEvent(eventId, p1, p2, p3, s, data);
        }
      };

  private CarSerialSDK() {}

  public void setCallbackThreadMode(CallbackThreadMode mode) {
    dispatcher.setMode(mode);
  }

  public boolean init(String ttyPath) {
    if (ttyPath == null || ttyPath.isEmpty()) ttyPath = "/dev/ttyAS0";
    if (!inited.compareAndSet(false, true)) return true;
    boolean ok = NativeBridge.nativeInit(ttyPath, eventSink);
    if (!ok) {
      inited.set(false);
      dispatcher.shutdown();
    }
    return ok;
  }

  public void deinit() {
    if (!inited.compareAndSet(true, false)) return;
    NativeBridge.nativeDeinit();
    dispatcher.shutdown();
    vehicleCallback = null;
    radioCallback = null;
    updateCallback = null;
    connected = false;
  }

  public boolean isConnected() {
    return connected;
  }

  public void setVehicleCallback(VehicleCallback callback) {
    vehicleCallback = callback;
  }

  public void setRadioCallback(RadioCallback callback) {
    radioCallback = callback;
  }

  public void setUpdateCallback(UpdateCallback callback) {
    updateCallback = callback;
  }

  public interface RawMessageListener {
    void onMessage(int seq, int frameType, int msgType, int cmd, byte[] data);
  }

  public void setRawMessageListener(RawMessageListener listener) {
    rawMessageListener = listener;
  }

  public int sendRaw(int frameType, byte[] payload, boolean needAck) {
    if (!inited.get()) return -1;
    return NativeBridge.nativeSendRaw(frameType, payload, needAck);
  }

  public int setRadioFreq(int band, int freq) {
    return sendCommand(CMD_SET_RADIO_FREQ, new int[] {band, freq}, null, true);
  }

  public int setRadioSeek(int seekMode, int ptyType) {
    return sendCommand(CMD_SET_RADIO_SEEK, new int[] {seekMode, ptyType}, null, true);
  }

  public int setRadioSwitch(int switchType, boolean on) {
    return sendCommand(CMD_SET_RADIO_SWITCH, new int[] {switchType, on ? 1 : 0}, null, true);
  }

  public int setBacklight(int percent) {
    return sendCommand(CMD_SET_BACKLIGHT, new int[] {percent}, null, true);
  }

  public int setAmpPower(boolean on) {
    return sendCommand(CMD_SET_AMP_POWER, new int[] {on ? 1 : 0}, null, true);
  }

  public int sendMcuUpgradeData(byte[] data, int offset, int len) {
    if (data == null) return -1;
    if (offset < 0) offset = 0;
    if (len < 0) len = 0;
    if (offset + len > data.length) len = data.length - offset;
    byte[] slice = new byte[len];
    System.arraycopy(data, offset, slice, 0, len);
    return sendCommand(CMD_SEND_UPGRADE_DATA, new int[] {offset, len}, slice, true);
  }

  public int sendProtocolMessage(int msgType, int cmd, byte[] data, boolean needAck) {
    if (data == null) data = new byte[0];
    byte[] payload = new byte[data.length + 2];
    payload[0] = (byte) (msgType & 0xFF);
    payload[1] = (byte) (cmd & 0xFF);
    if (data.length > 0) System.arraycopy(data, 0, payload, 2, data.length);
    return sendRaw(FRAME_TYPE_DATA, payload, needAck);
  }

  public int updateRequest(byte[] reqPayload) {
    synchronized (updateLock) {
      updateTotalBytes = 0;
      updateLastSentBytes = 0;
      updateStarted = false;
      updateEndRequested = false;
    }
    return sendProtocolMessage(MSG_TYPE_UPDATE, UPDATE_A2M_UPDATE_REQ, reqPayload, true);
  }

  public int updateBegin(byte[] beginPayload) {
    synchronized (updateLock) {
      updateStarted = true;
      updateEndRequested = false;
      updateLastSentBytes = 0;
    }
    return sendProtocolMessage(MSG_TYPE_UPDATE, UPDATE_A2M_UPDATE_BEGIN, beginPayload, true);
  }

  public int updateSendData(byte[] dataPayload) {
    return sendProtocolMessage(MSG_TYPE_UPDATE, UPDATE_A2M_UPDATE_DATA, dataPayload, true);
  }

  public int updateEnd(byte[] endPayload) {
    synchronized (updateLock) {
      updateEndRequested = true;
    }
    return sendProtocolMessage(MSG_TYPE_UPDATE, UPDATE_A2M_UPDATE_END, endPayload, true);
  }

  private int sendCommand(int cmd, int[] ints, byte[] extra, boolean needAck) {
    int intCount = ints == null ? 0 : ints.length;
    int extraLen = extra == null ? 0 : extra.length;
    ByteBuffer bb = ByteBuffer.allocate(4 + 4 * intCount + extraLen).order(ByteOrder.BIG_ENDIAN);
    bb.putInt(cmd);
    for (int i = 0; i < intCount; i++) bb.putInt(ints[i]);
    if (extraLen > 0) bb.put(extra);
    return sendRaw(FRAME_TYPE_APP_COMMAND, bb.array(), needAck);
  }

  private void handleNativeEvent(int eventId, int p1, int p2, int p3, String s, byte[] data) {
    if (eventId == EVENT_LINK_STATE) {
      boolean c = p1 == 1;
      connected = c;
      return;
    }
    if (eventId == EVENT_RAW_FRAME) {
      return;
    }
    if (eventId == EVENT_MESSAGE) {
      RawMessageListener listener = rawMessageListener;
      int msgType = (p3 >> 8) & 0xFF;
      int cmd = p3 & 0xFF;
      byte[] payload = data == null ? new byte[0] : data;

      VehicleCallback vc = vehicleCallback;
      RadioCallback rc = radioCallback;
      UpdateCallback uc = updateCallback;

      if (listener == null && vc == null && rc == null && uc == null) return;

      dispatcher.post(
          () -> {
            if (listener != null) listener.onMessage(p1, p2, msgType, cmd, payload);
            dispatchMessage(vc, rc, uc, msgType, cmd, payload);
          });
    }
  }

  private void dispatchMessage(
      VehicleCallback vc, RadioCallback rc, UpdateCallback uc, int msgType, int cmd, byte[] payload) {
    if (msgType == MSG_TYPE_PUBLIC && vc != null) {
      dispatchPublicMessage(vc, cmd, payload);
      return;
    }
    if (msgType == MSG_TYPE_RADIO && rc != null) {
      dispatchRadioMessage(rc, cmd, payload);
      return;
    }
    if (msgType == MSG_TYPE_UPDATE && uc != null) {
      dispatchUpdateMessage(uc, cmd, payload);
      return;
    }
  }

  private void dispatchPublicMessage(VehicleCallback vc, int cmd, byte[] payload) {
    if (cmd == PUBLIC_M2A_LINE_DETECT) {
      if (payload.length < 1) return;
      int p0 = payload[0] & 0xFF;
      boolean accHigh = (p0 & (1 << 1)) != 0;
      boolean reverseHigh = (p0 & (1 << 3)) != 0;
      vc.onAccStateChanged(accHigh);
      vc.onReverseStateChanged(reverseHigh);
      return;
    }
    if (cmd == PUBLIC_M2A_MCU_VERSION) {
      if (payload.length <= 0) return;
      String version = asciiTrimmed(payload);
      if (!version.isEmpty()) vc.onMcuVersion(version);
      return;
    }
    if (cmd == PUBLIC_M2A_CAR_SPEED) {
      if (payload.length < 2) return;
      int raw = u16be(payload, 0);
      int speed = raw;
      if (payload.length >= 3) {
        int ratio = payload[2] & 0xFF;
        if (ratio >= 50 && ratio <= 200) {
          speed = (raw * ratio) / 100;
        }
      }
      vc.onCarSpeed(speed);
      return;
    }
    if (cmd == PUBLIC_M2A_VOLTAGE) {
      if (payload.length < 2) return;
      int v = u16be(payload, 0);
      int mv = v * 100;
      vc.onBatteryVoltage(mv);
    }
  }

  private void dispatchRadioMessage(RadioCallback rc, int cmd, byte[] payload) {
    if (cmd == RADIO_M2A_FREQ) {
      if (payload.length < 3) return;
      int freq = u16be(payload, 0);
      int band = payload[2] & 0x0F;
      boolean stereo = false;
      boolean valid = true;
      if (payload.length >= 5) {
        int p4 = payload[4] & 0xFF;
        stereo = (p4 & 0x01) != 0;
        valid = (p4 & 0x80) != 0;
      }
      rc.onFreqChanged(band, freq, stereo, valid);
      if (payload.length >= 7) {
        int strength = payload[6] & 0xFF;
        rc.onSignalStrength(strength);
      }
      return;
    }
    if (cmd == RADIO_M2A_PS_PTY) {
      if (payload.length < 9) return;
      String ps = asciiTrimmed(payload, 1, 8);
      if (!ps.isEmpty()) rc.onRdsPs(ps);
      return;
    }
    if (cmd == RADIO_M2A_RT) {
      if (payload.length < 1) return;
      String rt = asciiTrimmed(payload, 0, Math.min(64, payload.length));
      if (!rt.isEmpty()) rc.onRdsRt(rt);
    }
  }

  private void dispatchUpdateMessage(UpdateCallback uc, int cmd, byte[] payload) {
    if (cmd == UPDATE_M2A_UPDATE_LEN) {
      if (payload.length < 2) return;
      int total = u16be(payload, 0);
      synchronized (updateLock) {
        updateTotalBytes = total;
        updateLastSentBytes = 0;
        updateStarted = true;
      }
      uc.onUpgradeStarted();
      uc.onUpgradeProgress(0, total);
      return;
    }

    if (cmd == UPDATE_M2A_UPDATE_IND_AQUIRE) {
      if (payload.length < 2) return;
      int index = u16be(payload, 0);
      int sent = index * 1024;
      int total;
      boolean started;
      synchronized (updateLock) {
        updateLastSentBytes = sent;
        total = updateTotalBytes;
        started = updateStarted;
        if (!started) updateStarted = true;
      }
      if (!started) uc.onUpgradeStarted();
      int reportedTotal = total > 0 ? total : 0;
      int reportedSent = total > 0 ? Math.min(sent, total) : sent;
      uc.onUpgradeProgress(reportedSent, reportedTotal);
      return;
    }

    if (cmd == UPDATE_M2A_UPDATE_ACK) {
      int status = payload.length > 0 ? (payload[0] & 0xFF) : 0;
      boolean endReq;
      boolean started;
      synchronized (updateLock) {
        endReq = updateEndRequested;
        started = updateStarted;
        if (!started) updateStarted = true;
      }
      if (!started) uc.onUpgradeStarted();
      if (status != 0) {
        resetUpdateState();
        uc.onUpgradeFinished(false, status);
        return;
      }
      if (endReq) {
        resetUpdateState();
        uc.onUpgradeFinished(true, 0);
      }
    }
  }

  private void resetUpdateState() {
    synchronized (updateLock) {
      updateTotalBytes = 0;
      updateLastSentBytes = 0;
      updateStarted = false;
      updateEndRequested = false;
    }
  }

  private static int u16be(byte[] data, int offset) {
    if (offset + 2 > data.length) return 0;
    return ((data[offset] & 0xFF) << 8) | (data[offset + 1] & 0xFF);
  }

  private static String asciiTrimmed(byte[] data) {
    return asciiTrimmed(data, 0, data.length);
  }

  private static String asciiTrimmed(byte[] data, int offset, int len) {
    if (data == null || len <= 0 || offset < 0 || offset >= data.length) return "";
    int max = Math.min(data.length, offset + len);
    int end = offset;
    while (end < max && data[end] != 0) end++;
    if (end <= offset) return "";
    return new String(data, offset, end - offset, StandardCharsets.US_ASCII).trim();
  }
}

