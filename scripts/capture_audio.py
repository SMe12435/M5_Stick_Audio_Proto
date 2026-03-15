#!/usr/bin/env python3
"""
Dual-Stream Bluetooth Audio Capture — reads framed BT + MIC PCM from
M5StickC Plus over serial and writes two separate WAV files.

Usage:
    python capture_audio.py --port /dev/cu.usbserial-XXXX
    python capture_audio.py --port COM3              # Windows

Press Ctrl+C to stop and finalize both WAV files.
"""

import argparse
import struct
import sys
import time
import wave
from datetime import datetime
from pathlib import Path

import serial

# Frame format: [0xAA][0x55][TYPE:1][LEN:2 LE][PAYLOAD:LEN bytes]
SYNC = b"\xAA\x55"
FRAME_BT_DATA      = 0x01
FRAME_MIC_DATA     = 0x02
FRAME_SESSION_START = 0x80
FRAME_SESSION_END   = 0xFF

HEADER_OVERHEAD = 5  # sync(2) + type(1) + len(2)
READ_SIZE = 8192
OUTPUT_DIR = Path(__file__).parent.parent / "recordings"


class StreamState:
    def __init__(self, label: str):
        self.label = label
        self.wav: wave.Wave_write | None = None
        self.sample_rate = 0
        self.channels = 0
        self.bps = 0
        self.total_bytes = 0

    def open_wav(self, filename: Path):
        self.wav = wave.open(str(filename), "wb")
        self.wav.setnchannels(self.channels)
        self.wav.setsampwidth(self.bps // 8)
        self.wav.setframerate(self.sample_rate)
        self.total_bytes = 0

    def write(self, data: bytes):
        if self.wav:
            self.wav.writeframes(data)
            self.total_bytes += len(data)

    def close(self):
        if self.wav:
            self.wav.close()
            self.wav = None

    def duration(self) -> float:
        bps_bytes = self.sample_rate * self.channels * (self.bps // 8)
        return self.total_bytes / bps_bytes if bps_bytes else 0.0


def print_status(wall: float, bt: StreamState, mic: StreamState):
    def fmt(s: StreamState) -> str:
        d = s.duration()
        m, sec = divmod(int(d), 60)
        mb = s.total_bytes / (1024 * 1024)
        return f"{s.label} {m:02d}:{sec:02d} {mb:5.2f}MB"

    sys.stdout.write(f"\r  {fmt(bt)}  |  {fmt(mic)}  |  wall {wall:.0f}s")
    sys.stdout.flush()


def capture(port: str, baud: int):
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    print(f"Opening {port} at {baud} baud...")
    ser = serial.Serial(port, baud, timeout=0.1)
    ser.reset_input_buffer()

    print("Waiting for session start frame...")

    buf = b""
    bt = StreamState("BT")
    mic = StreamState("MIC")
    session_active = False
    wall_start = 0.0
    last_status = 0.0

    try:
        while True:
            data = ser.read(READ_SIZE)
            if data:
                buf += data

            # Print status once per second during active session
            if session_active and time.time() - last_status >= 1.0:
                print_status(time.time() - wall_start, bt, mic)
                last_status = time.time()

            # Process all complete frames in the buffer
            while True:
                # Find sync bytes
                idx = buf.find(SYNC)
                if idx < 0:
                    # Keep last byte in case it's the start of a sync
                    if len(buf) > 1:
                        buf = buf[-1:]
                    break

                # Discard any junk before sync
                if idx > 0:
                    buf = buf[idx:]

                # Need at least the full header
                if len(buf) < HEADER_OVERHEAD:
                    break

                frame_type = buf[2]
                payload_len = struct.unpack_from("<H", buf, 3)[0]

                # Need full payload
                if len(buf) < HEADER_OVERHEAD + payload_len:
                    break

                payload = buf[HEADER_OVERHEAD:HEADER_OVERHEAD + payload_len]
                buf = buf[HEADER_OVERHEAD + payload_len:]

                # ── Handle frame types ───────────────────
                if frame_type == FRAME_SESSION_START and payload_len >= 16:
                    bt.sample_rate  = struct.unpack_from("<I", payload, 0)[0]
                    bt.channels     = struct.unpack_from("<H", payload, 4)[0]
                    bt.bps          = struct.unpack_from("<H", payload, 6)[0]
                    mic.sample_rate = struct.unpack_from("<I", payload, 8)[0]
                    mic.channels    = struct.unpack_from("<H", payload, 12)[0]
                    mic.bps         = struct.unpack_from("<H", payload, 14)[0]

                    is_hfp = bt.sample_rate <= 16000 and bt.channels == 1
                    mode = "hfp" if is_hfp else "a2dp"
                    bt.label = "HFP" if is_hfp else "A2DP"

                    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
                    bt_file  = OUTPUT_DIR / f"{mode}_{ts}.wav"
                    mic_file = OUTPUT_DIR / f"mic_{ts}.wav"

                    bt.open_wav(bt_file)
                    mic.open_wav(mic_file)

                    session_active = True
                    wall_start = time.time()
                    last_status = 0.0

                    mode_label = f"HFP call ({bt.sample_rate} Hz)" if is_hfp else "A2DP music"
                    print(f"\n  Session started! [{mode_label}]")
                    print(f"    BT:  {bt.sample_rate} Hz, {bt.channels}ch, {bt.bps}-bit -> {bt_file.name}")
                    print(f"    MIC: {mic.sample_rate} Hz, {mic.channels}ch, {mic.bps}-bit -> {mic_file.name}")

                elif frame_type == FRAME_BT_DATA and session_active:
                    bt.write(payload)

                elif frame_type == FRAME_MIC_DATA and session_active:
                    mic.write(payload)

                elif frame_type == FRAME_SESSION_END and session_active:
                    bt.close()
                    mic.close()
                    print_status(time.time() - wall_start, bt, mic)
                    print(f"\n  Session ended.")
                    print(f"    BT:  {bt.total_bytes:,} bytes")
                    print(f"    MIC: {mic.total_bytes:,} bytes")
                    session_active = False
                    print("\nWaiting for next session...")

    except KeyboardInterrupt:
        print()
        if session_active:
            bt.close()
            mic.close()
            print(f"  Interrupted. Files finalized.")
            print(f"    BT:  {bt.total_bytes:,} bytes")
            print(f"    MIC: {mic.total_bytes:,} bytes")
        else:
            print("  No active session. Exiting.")
    finally:
        ser.close()
        print("Serial port closed.")


def main():
    parser = argparse.ArgumentParser(
        description="Capture dual-stream BT + MIC audio from M5StickC Plus to WAV"
    )
    parser.add_argument("--port", required=True, help="Serial port (e.g. /dev/cu.usbserial-XXXX or COM3)")
    parser.add_argument("--baud", type=int, default=3000000, help="Baud rate (default: 3000000)")
    args = parser.parse_args()
    capture(args.port, args.baud)


if __name__ == "__main__":
    main()
