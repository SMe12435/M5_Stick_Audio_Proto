#!/usr/bin/env python3
"""
Convert a LightBlue BLE log into a playable WAV file.

Protocol (from M5-Audio-Recorder):
  Packet 1:   8-byte header  →  sampleRate(u32 LE) + sampleCount(u32 LE)
  Packet 2…N: raw int16 PCM audio bytes
  Last packet: 0xFFFFFFFF end-of-transfer sentinel

LightBlue log lines look like:
  04:07:37.315 - Characteristic (...) notified: <803e0000 80bb0000>

Usage:
  python3 hex_to_wav.py sound.txt [output.wav]
"""

import sys
import re
import struct
import wave
import argparse

SENTINEL = b"\xff\xff\xff\xff"


def extract_payloads(text: str) -> list[bytes]:
    """Pull every <hex ...> payload from LightBlue notification lines."""
    payloads: list[bytes] = []
    for match in re.finditer(r"notified:\s*<([^>]+)>", text):
        hex_str = re.sub(r"[^0-9a-fA-F]", "", match.group(1))
        if len(hex_str) % 2:
            hex_str = hex_str[:-1]
        payloads.append(bytes.fromhex(hex_str))
    return payloads


def main():
    ap = argparse.ArgumentParser(
        description="Convert a LightBlue BLE log to a WAV file.")
    ap.add_argument("input", help="LightBlue log file")
    ap.add_argument("output", nargs="?", default="output.wav",
                    help="Output WAV path (default: output.wav)")
    args = ap.parse_args()

    with open(args.input) as f:
        text = f.read()

    payloads = extract_payloads(text)
    if not payloads:
        sys.exit("Error: no BLE notification payloads found.")

    print(f"Found {len(payloads)} notification packets")

    # First packet is the header
    hdr = payloads[0]
    if len(hdr) == 8:
        sample_rate, sample_count = struct.unpack("<II", hdr)
        print(f"Header:  sample_rate={sample_rate} Hz, "
              f"sample_count={sample_count} "
              f"({sample_count * 2} bytes expected)")
        data_payloads = payloads[1:]
    else:
        print(f"Warning: first packet is {len(hdr)} bytes, not 8 — "
              "treating all packets as raw PCM @ 16000 Hz")
        sample_rate = 16000
        data_payloads = payloads

    pcm = b"".join(data_payloads)

    # Strip end-of-transfer sentinel if present
    if pcm.endswith(SENTINEL):
        pcm = pcm[:-len(SENTINEL)]
        print("Stripped end-of-transfer sentinel")

    num_samples = len(pcm) // 2
    duration = num_samples / sample_rate

    print(f"PCM data: {len(pcm)} bytes → {num_samples} samples, "
          f"{duration:.2f}s")

    with wave.open(args.output, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm)

    print(f"Wrote {args.output}")


if __name__ == "__main__":
    main()
