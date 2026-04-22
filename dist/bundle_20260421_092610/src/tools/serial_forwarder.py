#!/usr/bin/env python3
"""
串口透明转发工具（带十六进制日志）

用途：
- 将 A 端口收到的数据原样转发到 B 端口，同时把 A->B / B->A 的数据以 hex 打印出来；
- 常用于：
  1) Windows 虚拟串口对（COM5<->COM6）联调；
  2) 在 PC 侧抓包定位协议问题（粘包/丢包/ACK/重试等）。

示例：
  python tools/serial_forwarder.py --a COM5 --b COM6 --baud 460800
"""

import argparse
import sys
import threading
import time

import serial


def to_hex(data: bytes) -> str:
    # 以 “AA BB CC …” 的格式打印字节数组
    return " ".join(f"{x:02X}" for x in data)


def pipe(src: serial.Serial, dst: serial.Serial, tag: str) -> None:
    # 从 src 读数据并写入 dst，持续循环；tag 用于区分方向日志。
    while True:
        try:
            # in_waiting 为接收缓冲区可读字节数；为 0 时至少读 1 字节避免忙等
            read_len = src.in_waiting or 1
            chunk = src.read(read_len)
            if not chunk:
                continue
            dst.write(chunk)
            print(f"[{tag}] len={len(chunk)} hex={to_hex(chunk)}", flush=True)
        except Exception as exc:  # pragma: no cover
            print(f"[{tag}] error: {exc}", file=sys.stderr, flush=True)
            time.sleep(0.2)


def main() -> int:
    parser = argparse.ArgumentParser(description="Transparent COM<->COM forwarder with hex logs")
    parser.add_argument("--a", required=True, help="Port A, e.g. COM5")
    parser.add_argument("--b", required=True, help="Port B, e.g. COM6")
    parser.add_argument("--baud", type=int, default=460800, help="Baudrate (default: 460800)")
    args = parser.parse_args()

    # 打开两个串口（timeout 设短一些，避免 read 阻塞太久）
    sa = serial.Serial(args.a, args.baud, timeout=0.05)
    sb = serial.Serial(args.b, args.baud, timeout=0.05)
    print(f"forwarding {args.a} <-> {args.b} @ {args.baud}", flush=True)

    # 两个方向各起一个线程
    ta = threading.Thread(target=pipe, args=(sa, sb, "A->B"), daemon=True)
    tb = threading.Thread(target=pipe, args=(sb, sa, "B->A"), daemon=True)
    ta.start()
    tb.start()

    try:
        # 主线程保持存活，Ctrl+C 退出
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("stopped", flush=True)
    finally:
        sa.close()
        sb.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
