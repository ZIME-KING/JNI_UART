#!/usr/bin/env python3

import argparse
import json
import threading
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Optional

import serial


FRAME_HEAD0 = 0xFF
FRAME_HEAD1 = 0xAA

FRAME_TYPE_ACK = 0x02


def to_hex(data: bytes) -> str:
    return " ".join(f"{x:02X}" for x in data)


def checksum(data: bytes) -> int:
    s = sum(data) & 0xFF
    return (~s) & 0xFF


def encode_frame(seq: int, frame_type: int, payload: bytes) -> bytes:
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


@dataclass(frozen=True)
class DecodedFrame:
    raw: bytes
    seq: int
    frame_type: int
    payload: bytes


def try_decode_one(buf: bytearray) -> Optional[DecodedFrame]:
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

    raw = bytes(buf[:total_len])
    if checksum(raw[2:-1]) != raw[-1]:
        del buf[0]
        return None

    del buf[:total_len]
    return DecodedFrame(raw=raw, seq=raw[4], frame_type=raw[5], payload=raw[6:-1])


def parse_int(s: str) -> int:
    return int(s, 0)


def parse_hex_bytes(s: str) -> bytes:
    txt = s.strip()
    if not txt:
        return b""
    txt = txt.replace(",", " ").replace("\t", " ").replace("0x", "0X")
    parts = [p for p in txt.split(" ") if p]
    out = bytearray()
    for p in parts:
        if p.startswith("0X"):
            v = int(p, 16)
        else:
            v = int(p, 16)
        if v < 0 or v > 255:
            raise ValueError(f"hex byte out of range: {p}")
        out.append(v)
    return bytes(out)


class JsonlLogger:
    def __init__(self, path: Path, echo: bool):
        self.path = path
        self.echo = echo
        self._lock = threading.Lock()
        self._fp = path.open("w", encoding="utf-8", newline="\n")

    def close(self) -> None:
        with self._lock:
            self._fp.close()

    def write(self, event: dict) -> None:
        now = time.time()
        event = dict(event)
        event["ts"] = now
        event["iso"] = datetime.fromtimestamp(now).isoformat(timespec="milliseconds")
        line = json.dumps(event, ensure_ascii=False, separators=(",", ":"))
        with self._lock:
            self._fp.write(line + "\n")
            self._fp.flush()
        if self.echo:
            if event.get("dir") in ("TX", "RX") and event.get("kind") == "chunk":
                print(f'[{event["iso"]}] {event["dir"]} len={event.get("len", 0)} hex={event.get("hex", "")}', flush=True)
            elif event.get("kind") == "frame":
                print(
                    f'[{event["iso"]}] {event.get("dir","")} frame seq={event.get("seq")} type=0x{event.get("frameType",0):02X} payloadLen={event.get("payloadLen",0)}',
                    flush=True,
                )


class SerialReader(threading.Thread):
    def __init__(self, ser: serial.Serial, logger: JsonlLogger):
        super().__init__(daemon=True)
        self.ser = ser
        self.logger = logger
        self._running = threading.Event()
        self._running.set()
        self._buf = bytearray()
        self._ack_cv = threading.Condition()
        self._acked = set()

    def stop(self) -> None:
        self._running.clear()

    def wait_ack(self, seq: int, timeout_s: float) -> bool:
        deadline = time.time() + timeout_s
        with self._ack_cv:
            while True:
                if seq in self._acked:
                    return True
                remain = deadline - time.time()
                if remain <= 0:
                    return False
                self._ack_cv.wait(timeout=remain)

    def run(self) -> None:
        while self._running.is_set():
            try:
                chunk = self.ser.read(self.ser.in_waiting or 1)
            except Exception as exc:
                self.logger.write({"kind": "rx_error", "error": str(exc)})
                time.sleep(0.05)
                continue
            if not chunk:
                continue
            self.logger.write({"dir": "RX", "kind": "chunk", "len": len(chunk), "hex": to_hex(chunk)})
            self._buf.extend(chunk)
            while True:
                f = try_decode_one(self._buf)
                if f is None:
                    break
                self.logger.write(
                    {
                        "dir": "RX",
                        "kind": "frame",
                        "seq": f.seq,
                        "frameType": f.frame_type,
                        "payloadLen": len(f.payload),
                        "payloadHex": to_hex(f.payload),
                        "rawHex": to_hex(f.raw),
                    }
                )
                if f.frame_type == FRAME_TYPE_ACK:
                    with self._ack_cv:
                        self._acked.add(f.seq)
                        self._ack_cv.notify_all()


def next_seq(seq: int) -> int:
    seq = (seq + 1) & 0xFF
    if seq == 0:
        seq = 1
    return seq


def load_frames(path: Path):
    items = []
    for idx, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = raw_line.strip()
        if not line:
            continue
        if line.startswith("#") or line.startswith("//"):
            continue

        if line.lower().startswith("raw "):
            b = parse_hex_bytes(line[4:])
            items.append(("raw", idx, b))
            continue

        if line.lower().startswith("frame "):
            parts = [p for p in line.split(" ") if p]
            if len(parts) < 3:
                raise ValueError(f"{path}:{idx}: invalid frame line: {raw_line}")
            seq = parse_int(parts[1])
            frame_type = parse_int(parts[2])
            payload = parse_hex_bytes(" ".join(parts[3:]))
            b = encode_frame(seq, frame_type, payload)
            items.append(("frame", idx, b))
            continue

        if line.lower().startswith("msg "):
            parts = [p for p in line.split(" ") if p]
            if len(parts) < 3:
                raise ValueError(f"{path}:{idx}: invalid msg line: {raw_line}")
            msg_type = parse_int(parts[1])
            cmd = parse_int(parts[2])
            data = parse_hex_bytes(" ".join(parts[3:]))
            payload = bytes([(msg_type & 0xFF), (cmd & 0xFF)]) + data
            items.append(("msg", idx, payload))
            continue

        b = parse_hex_bytes(line)
        if len(b) >= 2 and b[0] == FRAME_HEAD0 and b[1] == FRAME_HEAD1:
            items.append(("raw", idx, b))
            continue
        raise ValueError(f"{path}:{idx}: line must be raw/frame/msg or start with FF AA: {raw_line}")
    return items


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True, help="e.g. COM5")
    parser.add_argument("--baud", type=int, default=460800)
    parser.add_argument("--frames", required=True, help="frames file path")
    parser.add_argument("--log", default="", help="log path (jsonl)")
    parser.add_argument("--echo", action="store_true")
    parser.add_argument("--delay-ms", type=int, default=50)
    parser.add_argument("--read-after-ms", type=int, default=200)
    parser.add_argument("--wait-ack", action="store_true")
    parser.add_argument("--ack-timeout-ms", type=int, default=300)
    parser.add_argument("--retry", type=int, default=0)
    args = parser.parse_args()

    frames_path = Path(args.frames).expanduser()
    items = load_frames(frames_path)

    if args.log:
        log_path = Path(args.log).expanduser()
    else:
        log_path = Path.cwd() / f"serial_log_{datetime.now().strftime('%Y%m%d_%H%M%S')}.jsonl"

    ser = serial.Serial(args.port, args.baud, timeout=0.02)
    logger = JsonlLogger(log_path, echo=args.echo)
    reader = SerialReader(ser, logger)
    reader.start()

    seq_auto = 1
    try:
        logger.write(
            {
                "kind": "session",
                "port": args.port,
                "baud": args.baud,
                "framesFile": str(frames_path),
                "frameCount": len(items),
            }
        )
        for kind, line_no, content in items:
            if kind == "msg":
                seq_auto = next_seq(seq_auto)
                raw = encode_frame(seq_auto, 0x06, content)
                target_seq = seq_auto
            else:
                raw = content
                target_seq = raw[4] if len(raw) >= 6 and raw[0] == FRAME_HEAD0 and raw[1] == FRAME_HEAD1 else None

            logger.write({"dir": "TX", "kind": "chunk", "len": len(raw), "hex": to_hex(raw), "line": line_no})

            tx_buf = bytearray(raw)
            decoded = try_decode_one(tx_buf)
            if decoded is not None:
                logger.write(
                    {
                        "dir": "TX",
                        "kind": "frame",
                        "seq": decoded.seq,
                        "frameType": decoded.frame_type,
                        "payloadLen": len(decoded.payload),
                        "payloadHex": to_hex(decoded.payload),
                        "rawHex": to_hex(decoded.raw),
                        "line": line_no,
                    }
                )

            attempt = 0
            while True:
                ser.write(raw)
                if not args.wait_ack or target_seq is None:
                    break
                ok = reader.wait_ack(target_seq, timeout_s=args.ack_timeout_ms / 1000.0)
                if ok:
                    break
                if attempt >= args.retry:
                    logger.write({"kind": "ack_timeout", "seq": int(target_seq), "line": line_no})
                    break
                attempt += 1
                logger.write({"kind": "retry", "seq": int(target_seq), "attempt": attempt, "line": line_no})

            time.sleep(args.delay_ms / 1000.0)
            time.sleep(args.read_after_ms / 1000.0)
    finally:
        reader.stop()
        time.sleep(0.05)
        ser.close()
        logger.close()

    print(f"saved log: {log_path}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

