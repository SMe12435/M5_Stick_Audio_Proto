# XIAO ESP32-C3 Audio Device -- Firmware Spec

## What We're Building

A battery-powered audio recorder on the Seeed XIAO ESP32-C3 that captures microphone audio, compresses it, and sends it to our cloud server for transcription and AI note extraction. It has three modes:

| WiFi Status | Recording? | Behavior |
|---|---|---|
| Connected | Yes | Stream audio live to server via WebSocket |
| Not connected | Yes | Record to SD card as WAV file |
| Connected | No (idle) | Upload any saved WAV files from SD card to server |

---

## Hardware & Wiring

**Board**: Seeed Studio XIAO ESP32-C3 (single-core RISC-V 160MHz, 400KB SRAM, 4MB flash, no PSRAM)

```
Component        Pin      GPIO
────────────────────────────────
I2S Mic WS       D0       2
I2S Mic SCK      D1       3
I2S Mic DATA     D2       4
Button           D3       5        (to GND, use INPUT_PULLUP)
SD Card CS       D5       7
SD Card SCK      D8       8
SD Card MISO     D9       9
SD Card MOSI     D10      10
LED (optional)   D4       6        (or use spare GPIO)
```

- Mic: INMP441 MEMS I2S microphone. Tie its L/R pin to GND (left channel).
- Button: Momentary pushbutton. One leg to GPIO 5, other to GND.
- SD Card: Any 3.3V-compatible micro-SD breakout module.

---

## Arduino IDE Setup

- **Board package**: "esp32 by Espressif Systems" (install via Board Manager)
- **Board**: Tools > Board > "XIAO_ESP32C3"
- **Partition**: Minimal SPIFFS (for OTA support)
- **Library to install**: "ArduinoWebsockets" by Gil Maimon (via Library Manager)
- All other libraries (`WiFi`, `SD`, `SPI`, `WebServer`, `DNSServer`, `Preferences`, `driver/i2s.h`, `esp_ota_ops.h`, `esp_http_client.h`) are built-in

---

## State Machine

The firmware runs a state machine in `loop()`:

```
STATE_SETUP              Captive portal mode -- serving WiFi config page
STATE_WIFI_CONNECTING    Trying to join saved WiFi network
STATE_OTA_CHECK          Checking server for firmware update
STATE_CHECK_PAIRED       Checking if device has a pairing token
STATE_PAIRING            Showing pairing code, waiting for user to pair via web portal
STATE_WS_CONNECTING      Opening WebSocket to server
STATE_READY              Idle -- connected, waiting for button press
STATE_STREAMING          Recording + sending audio over WebSocket
STATE_RECORDING_OFFLINE  Recording to SD card (no WiFi)
STATE_UPLOADING_PENDING  Uploading saved WAV files from SD to server
STATE_OTA_UPDATING       Downloading + flashing firmware update
```

### Boot sequence

```
Power on
  → Load WiFi SSID from NVS
  → If no SSID saved → STATE_SETUP (captive portal)
  → If SSID saved → STATE_WIFI_CONNECTING
    → If WiFi fails 3 times → STATE_SETUP
    → If WiFi connects → STATE_OTA_CHECK
      → If no token → STATE_CHECK_PAIRED → STATE_PAIRING
      → If token exists → STATE_WS_CONNECTING → STATE_READY
```

### Button behavior

| Current State | Button Press | Action |
|---|---|---|
| `STATE_READY` + WebSocket connected | Press | Start live streaming (`STATE_STREAMING`) |
| `STATE_READY` + no WebSocket | Press | Start offline recording (`STATE_RECORDING_OFFLINE`) |
| `STATE_STREAMING` | Press | Stop streaming, return to `STATE_READY` |
| `STATE_RECORDING_OFFLINE` | Press | Stop recording, attempt WiFi reconnect |
| Boot (held 5 sec) | Long press | Factory reset -- clear all NVS data, reboot |

---

## Audio Pipeline

### Capture

Read from I2S mic at **16kHz mono**. The I2S peripheral returns 32-bit samples; shift right to fit 16-bit:

```
I2S config:
  mode = MASTER | RX
  sample_rate = 16000
  bits_per_sample = 32BIT
  channel_format = ONLY_LEFT
  dma_buf_count = 8
  dma_buf_len = 128

Read 512 samples per cycle (32ms of audio).
Convert: int16_t sample = (int16_t)(raw32[i] >> 11)
```

Use `pdMS_TO_TICKS(100)` timeout on `i2s_read()`, **never** `portMAX_DELAY` (would starve WiFi on this single-core chip).

### DC Offset Removal

Apply before encoding/recording. Simple IIR high-pass filter:

```cpp
static int32_t dcOffset = 0;
for each sample:
    dcOffset += (sample - dcOffset) >> 8
    sample = clamp(sample - dcOffset, -32768, 32767)
```

### IMA ADPCM Compression (for live streaming only)

4:1 compression. 512 PCM samples (1024 bytes) → 256 bytes ADPCM.

Uses two standard lookup tables:
- **Step table**: 89 entries (7, 8, 9, ... 32767)
- **Index table**: 16 entries (-1,-1,-1,-1, 2,4,6,8, -1,-1,-1,-1, 2,4,6,8)

Encoder state: `int16_t predictor`, `int8_t index` (reset to 0 at start of each stream).

Each input sample produces a 4-bit nibble. Two nibbles are packed per byte: low nibble first, then high nibble in the upper 4 bits.

The server has a matching decoder. The tables and algorithm are the IMA ADPCM standard -- easily found in reference implementations.

---

## Server Communication

**Server address** (default): `ws://15.206.232.216:8888`

### WiFi Setup -- Captive Portal

When no WiFi is configured, the device:
1. Starts a soft AP named `"XiaoAudio-Setup"` (open, no password)
2. Runs a DNS server redirecting all lookups to itself (triggers phone captive portal popup)
3. Serves an HTML page on port 80 with:
   - A list of scanned nearby WiFi networks (tap to select)
   - SSID text input
   - Password input
   - Advanced: server URL input (pre-filled with default)
   - Save button
4. On save: stores SSID, password, server URL to NVS (`Preferences` library, namespace `"audio"`) and reboots

### Device Pairing

First-time setup after WiFi connects. The device needs to be associated with a user account.

1. Generate a random 6-character pairing code (characters: `ABCDEFGHJKMNPQRSTUVWXYZ23456789`)
2. Print it to Serial: `Serial.printf("PAIRING CODE: %s\n", code)`
3. POST to server:
   ```
   POST /api/devices/register
   Content-Type: application/json
   {"mac": "<MAC_NO_COLONS>", "code": "<6_CHAR_CODE>"}
   ```
4. Poll every 3 seconds:
   ```
   GET /api/devices/status?mac=<MAC>
   ```
   Response when paired: `{"paired": true, "token": "<64_char_hex>"}`
5. Save token to NVS key `"token"`. This token is used for all future server communication.

### WebSocket Connection

Connect to: `ws://<server>/ws/device?token=<TOKEN>&mac=<MAC>&fw=<FW_VERSION>`

Server responds with JSON text: `{"type": "auth_ok"}` or `{"type": "auth_error", ...}`.

Handle incoming JSON messages:
- `"auth_ok"` -- authenticated successfully
- `"wifi_update"` -- contains `ssid` and `password` fields; save to NVS and reboot
- `"paired"` -- contains `token` field; save to NVS

Send a WebSocket ping every 15 seconds to keep the connection alive.

### Live Audio Streaming Protocol

All audio is sent as **binary WebSocket frames**. The server identifies message types by length:

**1. Start marker (8 bytes)** -- send when recording begins:
```
Bytes 0-3: sample_rate as uint32 little-endian (16000)
Bytes 4-7: 0x00000001 as uint32 little-endian (marker)
```

**2. Audio chunks (256 bytes each)** -- send continuously while recording:
```
256 bytes of ADPCM data (encodes 512 PCM samples = 32ms of audio)
```

**3. Stop sentinel (4 bytes)** -- send when recording stops:
```
0xFF 0xFF 0xFF 0xFF
```

### Offline WAV Upload (new endpoint)

Upload saved recordings via HTTP:

```
POST /api/upload HTTP/1.1
Host: <server>:<port>
Content-Type: application/octet-stream
X-Device-Token: <token>
X-Device-MAC: <mac>
Content-Length: <file_size>
Connection: close

<raw WAV file bytes>
```

Server responds `201` on success. Stream the file in 1KB chunks from SD -- do **not** load the whole file into memory.

---

## SD Card Operations

### Directory structure

```
/pending/           ← recordings waiting to be uploaded
  rec_0001A2B3.wav
  rec_0004F1C0.wav
```

### Recording to SD (offline mode)

1. Create `/pending/` directory if it doesn't exist
2. Open `/pending/rec_<millis_hex>.wav` for writing
3. Write 44 bytes of zeroes (WAV header placeholder)
4. In the loop: read I2S → DC offset removal → write 16-bit PCM samples to file
5. On stop: seek to byte 0, write correct WAV header, close file

WAV format: 16kHz, 16-bit, mono, PCM (format code 1). Standard 44-byte RIFF header.

### Upload pending files (idle + connected)

Every 10 seconds while in `STATE_READY`:
1. Open `/pending/` directory
2. For each `.wav` file:
   - HTTP POST to `/api/upload` (stream in 1KB chunks)
   - On `201` response: delete the file
   - On failure: stop, retry next cycle
   - Call `wsClient.poll()` between files to keep WebSocket alive
3. If WebSocket drops during upload, abort and return to reconnect flow

### Edge cases

- Files smaller than 44 bytes → delete (corrupt/empty)
- SD card not present → skip all SD operations, live streaming still works
- Check free space before recording (~32KB per second of audio)

---

## OTA Firmware Updates

### Check for update

```
GET /api/ota/check?mac=<MAC>&version=<FW_VERSION>&token=<TOKEN>
```

Response if update available:
```json
{"update": true, "version": "1.1.0", "firmware_id": 5, "sha256": "abc...", "size": 512000}
```

Check on boot (after pairing) and every 1 hour while idle.

### Download and flash

```
GET /api/ota/firmware/<firmware_id>?token=<TOKEN>&mac=<MAC>
```

Use `esp_http_client` to download, `esp_ota_begin/write/end` to flash, `esp_ota_set_boot_partition` to activate. Read in 4KB chunks. Abort if WiFi RSSI < -80 or free heap < 50KB.

After OTA reboot, call `esp_ota_mark_app_valid_cancel_rollback()` to confirm the new firmware works.

---

## Persistent Storage (NVS)

Using `Preferences` library, namespace `"audio"`:

| Key | Type | Description |
|---|---|---|
| `ssid` | String | WiFi network name |
| `pass` | String | WiFi password |
| `server` | String | Server WebSocket URL |
| `token` | String | Device API token (64-char hex, set after pairing) |

**Factory reset**: Hold button during boot for 5 seconds → clear all NVS → reboot.

---

## LED Feedback

Since there's no display, use an LED for status:

| State | Pattern |
|---|---|
| Captive portal | Slow blink (1s on/off) |
| WiFi connecting | Fast blink (200ms) |
| Pairing | Double blink |
| WebSocket connecting | Triple blink |
| Ready (idle) | Solid on |
| Live streaming | Rapid pulse (50ms) |
| Recording to SD | Alternating (500ms on, 200ms off) |
| Uploading files | Slow pulse |

Use non-blocking `millis()`-based toggling in `loop()`.

---

## Important Constraints (Single-Core ESP32-C3)

- WiFi and your code share **one CPU core**. Never block for long.
- Use 100ms timeout on `i2s_read()`, not `portMAX_DELAY`.
- Call `wsClient.poll()` at least every 100ms during idle/streaming.
- Never do SD writes during live WebSocket streaming (choose one or the other per session).
- Keep total buffer usage small (~3KB for audio buffers). No dynamic allocation during streaming.
- Avoid `delay()` > 10ms during streaming.

---

## Testing Checklist

1. **Hardware**: Button, SD card mount, I2S mic reads non-zero samples
2. **Offline recording**: Button starts/stops, WAV plays correctly on PC, ~32KB/sec file growth
3. **Captive portal**: AP visible, phone shows config page, credentials saved, reboots
4. **WiFi + Pairing**: Connects to WiFi, registers pairing code, polls for token, saves token
5. **Live streaming**: WebSocket connects, start marker sent, audio chunks flow, stop sentinel sent, server logs show decoded audio
6. **Offline upload**: Record with WiFi off → reconnect → pending files upload → files deleted from SD
7. **OTA**: Update detected, firmware downloaded and flashed, device boots new version
8. **Edge cases**: WiFi drop mid-stream, rapid button presses, SD card missing, long recording (5+ min), power loss during recording (stale files upload on next boot)
