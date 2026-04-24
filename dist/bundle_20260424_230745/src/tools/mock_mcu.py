#!/usr/bin/env python3
"""
MCU 模拟器（用于 PC/Windows 联调）

用途：
- 在没有真实 MCU 的情况下，模拟 MCU 侧协议行为：
  - 收到 Setup 帧后回 SetupAck；
  - 收到 Heartbeat 回 ACK；
  - 收到 Update 流程命令后回 UpdateAck / Len / Index；
  - 定时上报 Public/Radio 示例数据（车速、电压、版本、频率、RDS）。

依赖：
  pyserial（安装：py -m pip install pyserial）

示例：
  py tools/mock_mcu.py --port COM6 --baud 460800
"""

import argparse
import random
import struct
import time

import serial

FRAME_HEAD0 = 0xFF
FRAME_HEAD1 = 0xAA

FRAME_TYPE_ACK = 0x02
FRAME_TYPE_HEARTBEAT = 0x05
FRAME_TYPE_DATA = 0x06
FRAME_TYPE_SETUP = 0x10
FRAME_TYPE_SETUP_ACK = 0x11

MSG_TYPE_PUBLIC = 0x01
MSG_TYPE_RADIO = 0x05
MSG_TYPE_UPDATE = 0x07

PUBLIC_M2A_LINE_DETECT = 0x01
PUBLIC_M2A_MCU_VERSION = 0x02
PUBLIC_M2A_CAR_SPEED = 0x0A
PUBLIC_M2A_VOLTAGE = 0x0D

RADIO_M2A_FREQ = 0x01
RADIO_M2A_PS_PTY = 0x02
RADIO_M2A_RT = 0x03

UPDATE_M2A_UPDATE_ACK = 0x01
UPDATE_M2A_UPDATE_LEN = 0x02
UPDATE_M2A_UPDATE_IND_AQUIRE = 0x03

UPDATE_A2M_UPDATE_REQ = 0x80
UPDATE_A2M_UPDATE_BEGIN = 0x81
UPDATE_A2M_UPDATE_DATA = 0x82
UPDATE_A2M_UPDATE_END = 0x83


def checksum(data: bytes) -> int:
    # 与 native FrameCodec 一致：
    # 对 [lenH..payload] 求和，取低 8 位后按位取反。
    s = sum(data) & 0xFF
    return (~s) & 0xFF


def encode_frame(seq: int, frame_type: int, payload: bytes) -> bytes:
    # 编码为线协议帧：
    # [FF AA][len][seq][frameType][payload][checksum]
    total_len = 2 + 2 + 1 + 1 + len(payload) + 1
    out = bytearray(total_len)
    out[0] = FRAME_HEAD0
    out[1] = FRAME_HEAD1
    out[2] = (total_len >> 8) & 0xFF
    out[3] = total_len & 0xFF
    out[4] = seq & 0xFF
    out[5] = frame_type & 0xFF
    out[6 : 6 + len(payload)] = payload
    out[-1] = checksum(out[2:-1])
    return bytes(out)


def decode_one(buf: bytearray):
    # 从字节流缓冲区中尝试解出 1 帧：
    # - 支持丢弃无效头部字节
    # - 支持粘包/分包
    while len(buf) >= 2 and not (buf[0] == FRAME_HEAD0 and buf[1] == FRAME_HEAD1):
        del buf[0]
    if len(buf) < 7:
        return None
    total_len = (buf[2] << 8) | buf[3]
    if total_len < 7:
        del buf[0]
        return None
    if len(buf) < total_len:
        return None
    frame = bytes(buf[:total_len])
    del buf[:total_len]
    if checksum(frame[2:-1]) != frame[-1]:
        return None
    seq = frame[4]
    frame_type = frame[5]
    payload = frame[6:-1]
    return seq, frame_type, payload


class MockMcu:
    def __init__(self, ser: serial.Serial):
        # ser：打开后的串口对象
        self.ser = ser
        self.running = True
        self.seq = 1
        self.connected = False
        self.update_total = 8192
        self.update_index = 0

    def next_seq(self) -> int:
        # 模拟 MCU 自己发包使用的 seq（避免为 0）
        self.seq = (self.seq + 1) & 0xFF
        if self.seq == 0:
            self.seq = 1
        return self.seq

    def send(self, frame_type: int, payload: bytes, seq: int = None):
        # 发送一帧 frameType 的协议帧；seq 若不传则自增生成
        if seq is None:
            seq = self.next_seq()
        frame = encode_frame(seq, frame_type, payload)
        self.ser.write(frame)

    def send_message(self, msg_type: int, cmd: int, data: bytes):
        # 发送一条“数据帧”消息：payload = [msgType][cmd][data...]
        self.send(FRAME_TYPE_DATA, bytes([msg_type & 0xFF, cmd & 0xFF]) + data)

    def handle_frame(self, seq: int, frame_type: int, payload: bytes):
        # 处理来自对端（ARM/PC/Android）的帧
        if frame_type == FRAME_TYPE_SETUP:
            # 建链：收到 Setup 后回 SetupAck
            self.connected = True
            self.send(FRAME_TYPE_SETUP_ACK, b"", seq=self.next_seq())
            return
        if frame_type == FRAME_TYPE_HEARTBEAT:
            # 心跳：收到 Heartbeat 回 ACK（使用对端 seq）
            self.send(FRAME_TYPE_ACK, b"", seq=seq)
            return
        if frame_type != FRAME_TYPE_DATA or len(payload) < 2:
            return
        msg_type, cmd = payload[0], payload[1]
        data = payload[2:]

        if msg_type == MSG_TYPE_UPDATE:
            # 升级流程（简化版）：只为联调演示提供最小闭环
            if cmd == UPDATE_A2M_UPDATE_REQ:
                self.send_message(MSG_TYPE_UPDATE, UPDATE_M2A_UPDATE_ACK, bytes([0x00]))
                self.send_message(MSG_TYPE_UPDATE, UPDATE_M2A_UPDATE_LEN, struct.pack(">H", self.update_total))
                self.update_index = 0
            elif cmd == UPDATE_A2M_UPDATE_BEGIN:
                self.send_message(MSG_TYPE_UPDATE, UPDATE_M2A_UPDATE_ACK, bytes([0x00]))
            elif cmd == UPDATE_A2M_UPDATE_DATA:
                self.update_index += 1
                self.send_message(MSG_TYPE_UPDATE, UPDATE_M2A_UPDATE_IND_AQUIRE, struct.pack(">H", self.update_index))
            elif cmd == UPDATE_A2M_UPDATE_END:
                self.send_message(MSG_TYPE_UPDATE, UPDATE_M2A_UPDATE_ACK, bytes([0x00]))
            return

        # 其他数据命令：统一回一个 ACK（使用对端 seq）
        self.send(FRAME_TYPE_ACK, b"", seq=seq)

    def periodic_push(self):
        # 定时上报示例数据，便于上位机验证解析与回调分发
        speed = random.randint(0, 120)
        ratio = 100
        speed_raw = speed
        self.send_message(MSG_TYPE_PUBLIC, PUBLIC_M2A_CAR_SPEED, struct.pack(">HB", speed_raw, ratio))

        # bit1=ACC, bit3=Reverse（这里固定为 1，便于观察）
        line_p0 = 0b00001010
        self.send_message(MSG_TYPE_PUBLIC, PUBLIC_M2A_LINE_DETECT, bytes([line_p0]))
        # 电压 12.8V（0.1V 单位 -> 128）
        self.send_message(MSG_TYPE_PUBLIC, PUBLIC_M2A_VOLTAGE, struct.pack(">H", 128))
        self.send_message(MSG_TYPE_PUBLIC, PUBLIC_M2A_MCU_VERSION, b"MCU-20260416-001\x00")

        # FM 101.7 -> 10170（按协议示例字段组包）
        freq_payload = bytearray(7)
        freq_payload[0:2] = struct.pack(">H", 10170)
        freq_payload[2] = 0  # FM
        freq_payload[4] = 0x81  # stereo=1, valid=1
        freq_payload[6] = random.randint(20, 90)
        self.send_message(MSG_TYPE_RADIO, RADIO_M2A_FREQ, bytes(freq_payload))
        self.send_message(MSG_TYPE_RADIO, RADIO_M2A_PS_PTY, bytes([10]) + b"TESTFM01")
        rt = b"RDS RT FROM MOCK MCU"
        self.send_message(MSG_TYPE_RADIO, RADIO_M2A_RT, rt + b"\x00")

    def run(self):
        # 主循环：读串口 -> 解帧 -> 处理；连接建立后定时上报
        rx_buf = bytearray()
        next_push = time.time() + 1.0
        while self.running:
            chunk = self.ser.read(self.ser.in_waiting or 1)
            if chunk:
                rx_buf.extend(chunk)
                while True:
                    item = decode_one(rx_buf)
                    if item is None:
                        break
                    self.handle_frame(*item)

            now = time.time()
            if self.connected and now >= next_push:
                self.periodic_push()
                next_push = now + 1.0


def main():
    parser = argparse.ArgumentParser(description="Mock MCU for carserial protocol")
    parser.add_argument("--port", required=True, help="Serial port, e.g. COM6")
    parser.add_argument("--baud", type=int, default=460800, help="Baudrate")
    args = parser.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=0.02)
    print(f"mock_mcu listening on {args.port} @ {args.baud}")
    mcu = MockMcu(ser)
    try:
        mcu.run()
    except KeyboardInterrupt:
        pass
    finally:
        mcu.running = False
        ser.close()


if __name__ == "__main__":
    main()
