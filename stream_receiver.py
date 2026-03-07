#!/usr/bin/env python3
"""
BLE Live Audio Stream Receiver for M5-Audio-Recorder.

Connects via BLE, receives IMA ADPCM compressed audio in real-time,
decodes to 16-bit PCM, calls transcribe() on each chunk, and saves
the full session as a WAV file when streaming stops.

Install:  pip3 install bleak
Usage:    python3 stream_receiver.py
Then press Button A on M5Stick to start/stop streaming.
"""

import asyncio
import struct
import wave
import sys
from datetime import datetime
from bleak import BleakClient, BleakScanner

SERVICE_UUID = "19b10000-e8f2-537e-4f6c-d104768a1214"
CHAR_UUID    = "19b10001-e8f2-537e-4f6c-d104768a1214"

# ── IMA ADPCM Tables (must match encoder) ────────────────────

ADPCM_STEP_TABLE = [
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,
    50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,
    230,253,279,307,337,371,408,449,494,544,598,658,724,796,
    876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,
    2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,
    7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,
    20350,22385,24623,27086,29794,32767
]

ADPCM_INDEX_TABLE = [
    -1,-1,-1,-1, 2,4,6,8,
    -1,-1,-1,-1, 2,4,6,8
]


class AdpcmDecoder:
    def __init__(self):
        self.predictor = 0
        self.index = 0

    def reset(self):
        self.predictor = 0
        self.index = 0

    def _decode_nibble(self, nibble: int) -> int:
        step = ADPCM_STEP_TABLE[self.index]

        delta = step >> 3
        if nibble & 4: delta += step
        if nibble & 2: delta += step >> 1
        if nibble & 1: delta += step >> 2

        if nibble & 8:
            self.predictor -= delta
        else:
            self.predictor += delta

        self.predictor = max(-32768, min(32767, self.predictor))

        self.index += ADPCM_INDEX_TABLE[nibble]
        self.index = max(0, min(88, self.index))

        return self.predictor

    def decode(self, adpcm_bytes: bytes) -> bytes:
        """Decode ADPCM bytes to 16-bit PCM (little-endian)."""
        samples = []
        for byte in adpcm_bytes:
            lo = byte & 0x0F
            hi = (byte >> 4) & 0x0F
            samples.append(self._decode_nibble(lo))
            samples.append(self._decode_nibble(hi))
        return struct.pack(f"<{len(samples)}h", *samples)


# ── Transcription Placeholder ────────────────────────────────

def transcribe(audio_chunk: bytes, sample_rate: int) -> None:
    """
    Placeholder for speech-to-text processing.

    Args:
        audio_chunk: Raw 16-bit signed PCM bytes (little-endian).
        sample_rate: Sample rate in Hz (e.g. 16000).

    Replace this with Whisper, Deepgram, Google STT, etc.
    """
    num_samples = len(audio_chunk) // 2
    duration_ms = num_samples * 1000 // sample_rate
    rms = 0
    if num_samples > 0:
        pcm = struct.unpack(f"<{num_samples}h", audio_chunk)
        rms = int((sum(s * s for s in pcm) / num_samples) ** 0.5)
    print(f"  [transcribe] {num_samples} samples, {duration_ms}ms, RMS={rms}")


# ── Stream State ─────────────────────────────────────────────

sample_rate = 16000
decoder = AdpcmDecoder()
audio_pcm = bytearray()
streaming = False
stream_done = asyncio.Event()
chunk_count = 0


def notification_handler(sender, data: bytearray):
    global sample_rate, streaming, audio_pcm, chunk_count

    # Stop sentinel: 0xFFFFFFFF
    if len(data) == 4 and data == b'\xff\xff\xff\xff':
        if streaming:
            duration = len(audio_pcm) / 2 / sample_rate
            print(f"\nStream ended: {len(audio_pcm)} bytes "
                  f"({duration:.1f}s), {chunk_count} chunks")
            streaming = False
            stream_done.set()
        return

    # Start marker: 8 bytes [sample_rate:u32, 0x00000001:u32]
    if len(data) == 8:
        sr, marker = struct.unpack('<II', data)
        if marker == 1:
            sample_rate = sr
            decoder.reset()
            audio_pcm = bytearray()
            chunk_count = 0
            streaming = True
            stream_done.clear()
            print(f"Stream started: {sample_rate} Hz, ADPCM compressed")
            return

    if not streaming:
        return

    # Audio data: decode ADPCM → PCM
    pcm_chunk = decoder.decode(bytes(data))
    audio_pcm.extend(pcm_chunk)
    chunk_count += 1

    num_samples = len(pcm_chunk) // 2
    total_sec = len(audio_pcm) / 2 / sample_rate
    sys.stdout.write(f"\rReceiving... {total_sec:.1f}s  "
                     f"({len(audio_pcm)} bytes, chunk #{chunk_count})")
    sys.stdout.flush()

    transcribe(pcm_chunk, sample_rate)


def save_wav():
    """Save accumulated PCM audio to a timestamped WAV file."""
    if len(audio_pcm) == 0:
        print("No audio data to save.")
        return
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    filename = f"stream_{timestamp}.wav"
    with wave.open(filename, 'wb') as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(audio_pcm)
    actual_samples = len(audio_pcm) // 2
    duration = actual_samples / sample_rate
    print(f"\nSaved: {filename} "
          f"({sample_rate} Hz, {duration:.1f}s, "
          f"{actual_samples} samples)")


async def main():
    print("Scanning for M5-Audio-Recorder...")
    device = await BleakScanner.find_device_by_name(
        "M5-Audio-Recorder", timeout=15)
    if not device:
        print("Device not found. Make sure M5Stick is on and showing READY.")
        return

    print(f"Found! Connecting to {device.address}...")

    async with BleakClient(device, timeout=20) as client:
        mtu = client.mtu_size
        print(f"Connected! MTU: {mtu}")
        print("Press Button A on M5Stick to start streaming.\n")

        await client.start_notify(CHAR_UUID, notification_handler)

        # Keep running until Ctrl+C. Saves WAV each time streaming stops.
        try:
            while True:
                await stream_done.wait()
                stream_done.clear()
                save_wav()
                print("\nPress Button A again to start a new stream, "
                      "or Ctrl+C to quit.\n")

        except KeyboardInterrupt:
            print("\nInterrupted -- saving any buffered audio...")
            save_wav()

        try:
            await client.stop_notify(CHAR_UUID)
        except Exception:
            pass


if __name__ == "__main__":
    asyncio.run(main())
