#!/usr/bin/env python3
"""
WebSocket Audio Stream Receiver for M5-Audio-Recorder (WiFi mode).

Receives IMA ADPCM compressed audio over WebSocket, decodes to 16-bit PCM,
and saves the full session as a WAV file when streaming stops.

Install:  pip3 install websockets
Usage:    python3 ws_receiver.py
Then press Button A on M5Stick to start/stop streaming.
"""

import asyncio
import struct
import wave
import sys
import json
from datetime import datetime
from urllib.parse import parse_qs, urlparse
import websockets

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


def save_wav(audio_pcm: bytearray, sample_rate: int):
    """Save accumulated PCM audio to a timestamped WAV file."""
    if len(audio_pcm) == 0:
        print("No audio data to save.")
        return
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    filename = f"stream_ws_{timestamp}.wav"
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


async def handle_device(websocket):
    """Handle a WebSocket connection from the M5StickC device."""
    params = parse_qs(urlparse(str(websocket.path)).query)
    token = params.get("token", ["none"])[0]
    mac = params.get("mac", ["unknown"])[0]
    print(f"Device connected: MAC={mac}, token={token[:8]}...")

    await websocket.send(json.dumps({"type": "auth_ok"}))

    decoder = AdpcmDecoder()
    audio_pcm = bytearray()
    sample_rate = 16000
    streaming = False
    chunk_count = 0

    try:
        async for message in websocket:
            if isinstance(message, str):
                continue

            data = message

            # Stop sentinel: 0xFFFFFFFF
            if len(data) == 4 and data == b'\xff\xff\xff\xff':
                if streaming:
                    duration = len(audio_pcm) / 2 / sample_rate
                    print(f"\nStream ended: {len(audio_pcm)} bytes "
                          f"({duration:.1f}s), {chunk_count} chunks")
                    streaming = False
                    save_wav(audio_pcm, sample_rate)
                    audio_pcm = bytearray()
                    print("\nWaiting for next stream...")
                continue

            # Start marker: 8 bytes [sample_rate:u32, 0x00000001:u32]
            if len(data) == 8:
                sr, marker = struct.unpack('<II', data)
                if marker == 1:
                    sample_rate = sr
                    decoder.reset()
                    audio_pcm = bytearray()
                    chunk_count = 0
                    streaming = True
                    print(f"Stream started: {sample_rate} Hz, ADPCM compressed")
                    continue

            if not streaming:
                continue

            # Audio data: decode ADPCM -> PCM
            pcm_chunk = decoder.decode(bytes(data))
            audio_pcm.extend(pcm_chunk)
            chunk_count += 1

            total_sec = len(audio_pcm) / 2 / sample_rate
            sys.stdout.write(f"\rReceiving... {total_sec:.1f}s  "
                             f"({len(audio_pcm)} bytes, chunk #{chunk_count})")
            sys.stdout.flush()

    except websockets.exceptions.ConnectionClosed:
        print(f"\nDevice disconnected (MAC={mac})")
        if len(audio_pcm) > 0:
            print("Saving buffered audio...")
            save_wav(audio_pcm, sample_rate)


async def handle_http(path, request_headers):
    """Handle plain HTTP requests (device registration, pairing status)."""
    # This is a stub for Layer 1 testing -- the full FastAPI backend handles these later
    return None


async def handler(websocket):
    """Route connections based on path."""
    path = str(websocket.path)

    if path.startswith("/ws/device"):
        await handle_device(websocket)
    else:
        await websocket.send(json.dumps({"type": "error", "message": "unknown path"}))
        await websocket.close()


async def main():
    host = "0.0.0.0"
    port = 8000
    print(f"WebSocket Audio Receiver starting on ws://{host}:{port}")
    print("Waiting for M5StickC to connect...\n")

    async with websockets.serve(handler, host, port):
        await asyncio.Future()  # run forever


if __name__ == "__main__":
    asyncio.run(main())
