#!/usr/bin/env python3
"""
BLE Audio Receiver for M5-Audio-Recorder
Connects via BLE, listens for notifications, and saves a .wav file.

Install dependency:  pip3 install bleak
Usage:               python3 receive_audio.py
Then press Button A on the M5Stick to record & sync.
"""

import asyncio
import struct
import wave
import sys
from datetime import datetime
from bleak import BleakClient, BleakScanner

SERVICE_UUID = "19b10000-e8f2-537e-4f6c-d104768a1214"
CHAR_UUID    = "19b10001-e8f2-537e-4f6c-d104768a1214"

# Received data
sample_rate = 16000
sample_count = 0
expected_bytes = 0
audio_bytes = bytearray()
header_received = False
transfer_done = asyncio.Event()


def notification_handler(sender, data: bytearray):
    global sample_rate, sample_count, expected_bytes, audio_bytes, header_received

    # End-of-transfer sentinel: 0xFFFFFFFF
    if len(data) == 4 and data == b'\xff\xff\xff\xff':
        print(f"\n✅ Sentinel received! Total: {len(audio_bytes)} bytes")
        transfer_done.set()
        return

    # First packet is the 8-byte header
    if not header_received and len(data) == 8:
        sample_rate, sample_count = struct.unpack('<II', data)
        expected_bytes = sample_count * 2
        print(f"📋 Header: {sample_rate} Hz, {sample_count} samples "
              f"({sample_count / sample_rate:.1f}s, {expected_bytes} bytes expected)")
        header_received = True
        return

    # Audio data packet
    audio_bytes.extend(data)
    pct = min(100, len(audio_bytes) * 100 // expected_bytes) if expected_bytes else 0
    sys.stdout.write(f"\r🎙  Receiving... {pct}%  ({len(audio_bytes)} / {expected_bytes} bytes)")
    sys.stdout.flush()

    # Complete when we have all expected bytes (don't wait for sentinel)
    if expected_bytes > 0 and len(audio_bytes) >= expected_bytes:
        print(f"\n✅ All {len(audio_bytes)} bytes received!")
        transfer_done.set()


async def main():
    global header_received, audio_bytes, expected_bytes, sample_count

    print("🔍 Scanning for M5-Audio-Recorder...")
    device = await BleakScanner.find_device_by_name("M5-Audio-Recorder", timeout=15)
    if not device:
        print("❌ Device not found. Make sure M5Stick is on and showing READY.")
        return

    print(f"📡 Found! Connecting to {device.address}...")

    async with BleakClient(device, timeout=20) as client:
        mtu = client.mtu_size
        print(f"🔗 Connected! MTU: {mtu}")
        print("⏳ Waiting... Press Button A on M5Stick to record.\n")

        # Subscribe to notifications
        await client.start_notify(CHAR_UUID, notification_handler)

        # Wait for transfer to complete (generous 120s timeout)
        try:
            await asyncio.wait_for(transfer_done.wait(), timeout=120)
        except asyncio.TimeoutError:
            print(f"\n⏰ Timeout — received {len(audio_bytes)} / {expected_bytes} bytes.")

        try:
            await client.stop_notify(CHAR_UUID)
        except Exception:
            pass  # connection may already be gone

    # Save as .wav (even if partial — save whatever we got)
    if len(audio_bytes) > 0:
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        filename = f"recording_{timestamp}.wav"
        with wave.open(filename, 'wb') as wf:
            wf.setnchannels(1)
            wf.setsampwidth(2)  # 16-bit
            wf.setframerate(sample_rate)
            wf.writeframes(audio_bytes)
        actual_samples = len(audio_bytes) // 2
        completeness = len(audio_bytes) * 100 // expected_bytes if expected_bytes else 0
        print(f"💾 Saved: {filename}")
        print(f"   Format: {sample_rate} Hz, 16-bit mono, "
              f"{actual_samples} samples, "
              f"{actual_samples / sample_rate:.1f}s "
              f"({completeness}% of expected)")
    else:
        print("⚠️  No audio data received.")


if __name__ == "__main__":
    asyncio.run(main())
