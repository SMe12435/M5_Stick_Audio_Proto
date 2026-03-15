#include <M5Unified.h>
#include "BluetoothA2DPSink.h"
#include "esp_hf_client_api.h"
#include "esp_log.h"

static const char* TAG = "M5HFP";

// ── Serial Protocol ─────────────────────────────────────
//   Frame: [0xAA][0x55][TYPE:1][LEN:2 LE][PAYLOAD:LEN bytes]
//   TYPE 0x01 = BT audio PCM  (A2DP or HFP — mutually exclusive)
//   TYPE 0x02 = MIC audio PCM
//   TYPE 0x80 = Session start (16-byte payload: BT sr/ch/bps + MIC sr/ch/bps)
//   TYPE 0xFF = Session end (0-byte payload)
static constexpr uint32_t SERIAL_BAUD = 3000000;

// ── Audio Formats ───────────────────────────────────────
static constexpr uint32_t A2DP_SAMPLE_RATE = 44100;
static constexpr uint16_t A2DP_CHANNELS   = 2;
static constexpr uint16_t A2DP_BPS        = 16;

static constexpr uint32_t HFP_SAMPLE_RATE_NB = 8000;   // CVSD narrowband
static constexpr uint32_t HFP_SAMPLE_RATE_WB = 16000;  // mSBC wideband
static constexpr uint16_t HFP_CHANNELS       = 1;
static constexpr uint16_t HFP_BPS            = 16;

static constexpr uint32_t MIC_SAMPLE_RATE = 16000;
static constexpr uint16_t MIC_CHANNELS    = 1;
static constexpr uint16_t MIC_BPS         = 16;
static constexpr size_t   MIC_CHUNK       = 512;

// ── Frame Types ─────────────────────────────────────────
static constexpr uint8_t FRAME_BT_DATA      = 0x01;
static constexpr uint8_t FRAME_MIC_DATA     = 0x02;
static constexpr uint8_t FRAME_SESSION_START = 0x80;
static constexpr uint8_t FRAME_SESSION_END   = 0xFF;

// ── PSRAM Ring Buffer ───────────────────────────────────
struct RingBuf {
    uint8_t* buf;
    size_t   cap;
    volatile size_t head;
    volatile size_t tail;

    void init(size_t capacity) {
        buf = (uint8_t*)ps_malloc(capacity);
        cap = buf ? capacity : 0;
        head = tail = 0;
    }

    size_t available() const {
        size_t h = head, t = tail;
        return (h >= t) ? (h - t) : (cap - t + h);
    }

    size_t freeSpace() const {
        return cap - available() - 1;
    }

    bool write(const uint8_t* data, size_t len) {
        if (len > freeSpace()) return false;
        size_t h = head;
        for (size_t i = 0; i < len; i++) {
            buf[h] = data[i];
            h = (h + 1) % cap;
        }
        head = h;
        return true;
    }

    size_t read(uint8_t* dst, size_t maxLen) {
        size_t avail = available();
        size_t toRead = (maxLen < avail) ? maxLen : avail;
        size_t t = tail;
        for (size_t i = 0; i < toRead; i++) {
            dst[i] = buf[t];
            t = (t + 1) % cap;
        }
        tail = t;
        return toRead;
    }

    void flush() { tail = head; }
};

static RingBuf btRing;
static RingBuf micRing;

static constexpr size_t BT_RING_SIZE  = 65536;
static constexpr size_t MIC_RING_SIZE = 16384;

// ── HFP outgoing mic ring (separate from serial-capture mic ring) ──
static RingBuf hfpMicRing;
static constexpr size_t HFP_MIC_RING_SIZE = 4096;

// ── A2DP ────────────────────────────────────────────────
static BluetoothA2DPSink a2dpSink;
static const char* BT_DEVICE_NAME = "M5Audio";

// ── State ───────────────────────────────────────────────
static volatile bool btConnected     = false;
static volatile bool audioStreaming   = false;
static volatile bool sessionStarted  = false;
static volatile uint32_t btTotalBytes  = 0;
static volatile uint32_t micTotalBytes = 0;
static unsigned long streamStartMs   = 0;

// ── HFP State ───────────────────────────────────────────
static volatile bool hfpConnected    = false;
static volatile bool hfpCallActive   = false;
static volatile bool hfpAudioActive  = false;
static volatile uint32_t hfpSampleRate = HFP_SAMPLE_RATE_NB;
static esp_bd_addr_t hfpPeerAddr;

// Active BT audio format (changes between A2DP and HFP sessions)
static volatile uint32_t activeBtSampleRate = A2DP_SAMPLE_RATE;
static volatile uint16_t activeBtChannels   = A2DP_CHANNELS;

// ── Screen ──────────────────────────────────────────────
static bool screenOn = true;
static unsigned long lastDisplayUpdate = 0;
static constexpr unsigned long DISPLAY_INTERVAL = 500;

// ── Audio levels (for display) ──────────────────────────
static volatile int16_t btPeakLevel  = 0;
static volatile int16_t micPeakLevel = 0;

// ── Mic buffer ──────────────────────────────────────────
static int16_t micPcmBuf[MIC_CHUNK];

// ── Serial drain buffer ─────────────────────────────────
static constexpr size_t DRAIN_CHUNK = 1024;
static uint8_t drainBuf[DRAIN_CHUNK];

// ── Display Helpers ─────────────────────────────────────
static void showCentered(const char* msg, uint16_t color, uint8_t size = 3) {
    M5.Display.fillScreen(BLACK);
    M5.Display.setTextSize(size);
    M5.Display.setTextColor(color, BLACK);
    int16_t x = (M5.Display.width()  - M5.Display.textWidth(msg)) / 2;
    int16_t y = (M5.Display.height() - size * 8) / 2;
    M5.Display.setCursor(x, y);
    M5.Display.print(msg);
}

static void drawBattery() {
    int pct = M5.Power.getBatteryLevel();
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(pct > 20 ? GREEN : RED, BLACK);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    int16_t x = M5.Display.width() - M5.Display.textWidth(buf) - 4;
    M5.Display.setCursor(x, 2);
    M5.Display.print(buf);
}

static void showWaiting() {
    M5.Display.fillScreen(BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(CYAN, BLACK);
    int16_t x = (M5.Display.width() - M5.Display.textWidth("BT WAITING")) / 2;
    M5.Display.setCursor(x, 15);
    M5.Display.print("BT WAITING");

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(DARKGREY, BLACK);
    const char* hint = "Pair phone to \"M5Audio\"";
    int16_t x2 = (M5.Display.width() - M5.Display.textWidth(hint)) / 2;
    M5.Display.setCursor(x2, 55);
    M5.Display.print(hint);

    drawBattery();
}

static void showConnected() {
    M5.Display.fillScreen(BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(GREEN, BLACK);

    const char* label = hfpConnected ? "A2DP+HFP" : "CONNECTED";
    int16_t x = (M5.Display.width() - M5.Display.textWidth(label)) / 2;
    M5.Display.setCursor(x, 10);
    M5.Display.print(label);

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(YELLOW, BLACK);
    const char* hint = hfpConnected
        ? "Play music or make a call"
        : "Play audio to start capture";
    int16_t x2 = (M5.Display.width() - M5.Display.textWidth(hint)) / 2;
    M5.Display.setCursor(x2, 45);
    M5.Display.print(hint);

    if (hfpConnected) {
        M5.Display.setTextColor(DARKGREY, BLACK);
        const char* hfpHint = "HFP ready for calls";
        int16_t x3 = (M5.Display.width() - M5.Display.textWidth(hfpHint)) / 2;
        M5.Display.setCursor(x3, 58);
        M5.Display.print(hfpHint);
    }

    drawBattery();
}

static void showRecording() {
    M5.Display.fillScreen(BLACK);

    unsigned long elapsed = (millis() - streamStartMs) / 1000;

    M5.Display.setTextSize(2);
    bool isCall = hfpAudioActive;
    M5.Display.setTextColor(isCall ? MAGENTA : RED, BLACK);
    M5.Display.setCursor(4, 4);
    if (isCall) {
        M5.Display.printf("CALL %lum%02lus", elapsed / 60, elapsed % 60);
    } else {
        M5.Display.printf("REC %lum%02lus", elapsed / 60, elapsed % 60);
    }

    int dispW = M5.Display.width() - 20;

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(CYAN, BLACK);
    M5.Display.setCursor(10, 30);
    M5.Display.print("BT");
    int btBar = constrain(btPeakLevel / 100, 0, dispW - 20);
    M5.Display.fillRect(30, 30, btBar, 8, CYAN);
    M5.Display.fillRect(30 + btBar, 30, dispW - 20 - btBar, 8, DARKGREY);

    M5.Display.setTextColor(GREEN, BLACK);
    M5.Display.setCursor(10, 44);
    M5.Display.print("MC");
    int micBar = constrain(micPeakLevel / 100, 0, dispW - 20);
    M5.Display.fillRect(30, 44, micBar, 8, GREEN);
    M5.Display.fillRect(30 + micBar, 44, dispW - 20 - micBar, 8, DARKGREY);

    M5.Display.setTextColor(DARKGREY, BLACK);
    float btMb  = btTotalBytes  / (1024.0f * 1024.0f);
    float micMb = micTotalBytes / (1024.0f * 1024.0f);
    M5.Display.setCursor(10, 60);
    M5.Display.printf("BT:%.1fMB  MIC:%.1fMB", btMb, micMb);

    drawBattery();
}

// ── Serial Frame Helpers ────────────────────────────────
static void sendFrame(uint8_t type, const uint8_t* payload, uint16_t len) {
    uint8_t hdr[5];
    hdr[0] = 0xAA;
    hdr[1] = 0x55;
    hdr[2] = type;
    hdr[3] = (uint8_t)(len & 0xFF);
    hdr[4] = (uint8_t)(len >> 8);
    Serial.write(hdr, 5);
    if (len > 0 && payload) {
        Serial.write(payload, len);
    }
}

static void sendSessionStart(uint32_t btSr, uint16_t btCh, uint16_t btBps) {
    uint8_t pay[16];
    uint32_t sr; uint16_t ch, bps;

    sr = btSr; ch = btCh; bps = btBps;
    memcpy(pay + 0, &sr, 4);
    memcpy(pay + 4, &ch, 2);
    memcpy(pay + 6, &bps, 2);

    sr = MIC_SAMPLE_RATE; ch = MIC_CHANNELS; bps = MIC_BPS;
    memcpy(pay + 8,  &sr, 4);
    memcpy(pay + 12, &ch, 2);
    memcpy(pay + 14, &bps, 2);

    sendFrame(FRAME_SESSION_START, pay, 16);
    Serial.flush();
}

static void sendSessionEnd() {
    sendFrame(FRAME_SESSION_END, nullptr, 0);
    Serial.flush();
}

// ── Drain ring buffers to Serial ────────────────────────
static void drainRingToSerial(RingBuf& ring, uint8_t frameType, volatile uint32_t& totalCounter) {
    while (ring.available() > 0) {
        size_t n = ring.read(drainBuf, DRAIN_CHUNK);
        if (n == 0) break;
        sendFrame(frameType, drainBuf, (uint16_t)n);
        totalCounter += n;
    }
}

// ── Compute peak from int16 samples ─────────────────────
static int16_t computePeak(const int16_t* samples, size_t count) {
    int16_t peak = 0;
    size_t step = count > 64 ? count / 32 : 1;
    for (size_t i = 0; i < count; i += step) {
        int16_t v = samples[i] < 0 ? -samples[i] : samples[i];
        if (v > peak) peak = v;
    }
    return peak;
}

// ── Session management helpers ──────────────────────────
static void beginSession(uint32_t btSr, uint16_t btCh) {
    if (sessionStarted) return;
    activeBtSampleRate = btSr;
    activeBtChannels   = btCh;
    audioStreaming = true;
    sessionStarted = true;
    streamStartMs = millis();
    btTotalBytes = 0;
    micTotalBytes = 0;
    btRing.flush();
    micRing.flush();
    M5.Mic.begin();
    delay(50);
    sendSessionStart(btSr, btCh, 16);
}

static void endSession() {
    if (!sessionStarted) return;
    drainRingToSerial(btRing, FRAME_BT_DATA, btTotalBytes);
    drainRingToSerial(micRing, FRAME_MIC_DATA, micTotalBytes);
    sendSessionEnd();
    M5.Mic.end();
    audioStreaming = false;
    sessionStarted = false;
}

// ── A2DP Callbacks ──────────────────────────────────────
static void onAudioData(const uint8_t* data, uint32_t length) {
    if (hfpAudioActive) return;  // HFP takes priority; ignore stale A2DP data

    if (!audioStreaming) {
        beginSession(A2DP_SAMPLE_RATE, A2DP_CHANNELS);
    }

    btRing.write(data, length);

    const int16_t* samples = (const int16_t*)data;
    btPeakLevel = computePeak(samples, length / 2);
}

static void onBtStateChanged(esp_a2d_connection_state_t state, void*) {
    if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
        btConnected = true;
    } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
        btConnected = false;
        if (!hfpAudioActive) {
            endSession();
        }
        btPeakLevel = 0;
        micPeakLevel = 0;
    }
}

// ── HFP Callbacks ───────────────────────────────────────
static void hfpClientCb(esp_hf_client_cb_event_t event, esp_hf_client_cb_param_t* param) {
    switch (event) {
    case ESP_HF_CLIENT_CONNECTION_STATE_EVT: {
        auto& cs = param->conn_stat;
        if (cs.state == ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED) {
            hfpConnected = true;
            memcpy(hfpPeerAddr, cs.remote_bda, sizeof(esp_bd_addr_t));
        } else if (cs.state == ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED) {
            hfpConnected = false;
            hfpCallActive = false;
            if (hfpAudioActive) {
                hfpAudioActive = false;
                endSession();
            }
        }
        break;
    }
    case ESP_HF_CLIENT_AUDIO_STATE_EVT: {
        auto& as = param->audio_stat;
        if (as.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED) {
            hfpSampleRate = HFP_SAMPLE_RATE_NB;
            hfpAudioActive = true;
            if (sessionStarted) endSession();  // end any A2DP session
            beginSession(hfpSampleRate, HFP_CHANNELS);
        } else if (as.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC) {
            hfpSampleRate = HFP_SAMPLE_RATE_WB;
            hfpAudioActive = true;
            if (sessionStarted) endSession();
            beginSession(hfpSampleRate, HFP_CHANNELS);
        } else if (as.state == ESP_HF_CLIENT_AUDIO_STATE_DISCONNECTED) {
            if (hfpAudioActive) {
                hfpAudioActive = false;
                endSession();
            }
        }
        break;
    }
    case ESP_HF_CLIENT_CIND_CALL_EVT:
        hfpCallActive = (param->call.status == ESP_HF_CALL_STATUS_CALL_IN_PROGRESS);
        break;
    case ESP_HF_CLIENT_VOLUME_CONTROL_EVT:
        break;
    default:
        break;
    }
}

// HFP incoming audio: remote party's voice (PCM from Bluedroid via HCI)
static void hfpIncomingDataCb(const uint8_t* buf, uint32_t len) {
    if (!hfpAudioActive) return;
    btRing.write(buf, len);

    const int16_t* samples = (const int16_t*)buf;
    btPeakLevel = computePeak(samples, len / 2);
}

// HFP outgoing audio: send mic data to the phone
// Bluedroid calls this to pull PCM for the SCO uplink.
// Must be non-blocking; return 0 if no data ready.
static uint32_t hfpOutgoingDataCb(uint8_t* buf, uint32_t len) {
    if (!hfpAudioActive) return 0;
    size_t avail = hfpMicRing.available();
    if (avail == 0) return 0;
    size_t toRead = (len < avail) ? len : avail;
    return (uint32_t)hfpMicRing.read(buf, toRead);
}

// ── Downsample 16kHz -> 8kHz (simple 2:1 decimation) ───
static size_t downsample16to8(const int16_t* src, size_t srcCount,
                              int16_t* dst, size_t dstCap) {
    size_t out = 0;
    for (size_t i = 0; i < srcCount && out < dstCap; i += 2) {
        dst[out++] = src[i];
    }
    return out;
}

// ── Setup ───────────────────────────────────────────────
void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);
    Serial.begin(SERIAL_BAUD);
    delay(100);

    M5.Display.setRotation(1);
    M5.Display.setBrightness(80);
    M5.Speaker.end();

    showCentered("INIT...", YELLOW);

    btRing.init(BT_RING_SIZE);
    micRing.init(MIC_RING_SIZE);
    hfpMicRing.init(HFP_MIC_RING_SIZE);

    if (!btRing.buf || !micRing.buf || !hfpMicRing.buf) {
        showCentered("PSRAM!", RED);
        while (true) delay(1000);
    }

    auto mic_cfg          = M5.Mic.config();
    mic_cfg.sample_rate   = MIC_SAMPLE_RATE;
    mic_cfg.dma_buf_count = 8;
    mic_cfg.dma_buf_len   = 256;
    M5.Mic.config(mic_cfg);

    // Start A2DP sink
    a2dpSink.set_stream_reader(onAudioData, false);
    a2dpSink.set_on_connection_state_changed(onBtStateChanged);
    a2dpSink.set_auto_reconnect(true);
    a2dpSink.start(BT_DEVICE_NAME);

    // Init HFP client on the same Bluedroid stack
    ESP_LOGI(TAG, "Registering HFP client callback...");
    esp_err_t err = esp_hf_client_register_callback(hfpClientCb);
    ESP_LOGI(TAG, "esp_hf_client_register_callback: %s", esp_err_to_name(err));

    ESP_LOGI(TAG, "Initializing HFP client...");
    err = esp_hf_client_init();
    ESP_LOGI(TAG, "esp_hf_client_init: %s", esp_err_to_name(err));

    err = esp_hf_client_register_data_callback(hfpIncomingDataCb, hfpOutgoingDataCb);
    ESP_LOGI(TAG, "esp_hf_client_register_data_callback: %s", esp_err_to_name(err));

    showWaiting();
    ESP_LOGI(TAG, "Setup complete, entering loop");
}

// ── Main Loop ───────────────────────────────────────────
void loop() {
    M5.update();

    // BtnA: toggle screen on/off
    if (M5.BtnA.wasPressed()) {
        screenOn = !screenOn;
        if (screenOn) {
            M5.Display.setBrightness(80);
            lastDisplayUpdate = 0;
        } else {
            M5.Display.setBrightness(0);
        }
    }

    if (audioStreaming && sessionStarted) {
        // Record mic chunk
        if (M5.Mic.record(micPcmBuf, MIC_CHUNK, MIC_SAMPLE_RATE)) {
            // Always write to serial-capture ring
            micRing.write((const uint8_t*)micPcmBuf, MIC_CHUNK * sizeof(int16_t));
            micPeakLevel = computePeak(micPcmBuf, MIC_CHUNK);

            // If HFP call active, also feed mic to HFP outgoing ring
            if (hfpAudioActive) {
                if (hfpSampleRate == HFP_SAMPLE_RATE_NB) {
                    // Downsample 16kHz -> 8kHz for CVSD
                    int16_t downBuf[MIC_CHUNK / 2];
                    size_t n = downsample16to8(micPcmBuf, MIC_CHUNK,
                                               downBuf, MIC_CHUNK / 2);
                    hfpMicRing.write((const uint8_t*)downBuf, n * sizeof(int16_t));
                } else {
                    // 16kHz matches mic rate — pass through
                    hfpMicRing.write((const uint8_t*)micPcmBuf,
                                    MIC_CHUNK * sizeof(int16_t));
                }
                esp_hf_client_outgoing_data_ready();
            }
        }

        // Drain both ring buffers to serial with framing
        drainRingToSerial(btRing, FRAME_BT_DATA, btTotalBytes);
        drainRingToSerial(micRing, FRAME_MIC_DATA, micTotalBytes);
    }

    // Update display periodically
    if (screenOn && millis() - lastDisplayUpdate >= DISPLAY_INTERVAL) {
        lastDisplayUpdate = millis();

        if (audioStreaming && sessionStarted) {
            showRecording();
        } else if (btConnected || hfpConnected) {
            showConnected();
        } else {
            showWaiting();
        }
    }

    delay(1);  // yield to RTOS IDLE task (prevents WDT under ESP-IDF)
}
