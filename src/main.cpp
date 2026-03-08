#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ArduinoWebsockets.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <esp_system.h>

using namespace websockets;

// ── Defaults ─────────────────────────────────────────────
static const char* DEFAULT_SERVER = "ws://15.206.232.216:8888/ws/device";
static const char* AP_SSID        = "M5Audio-Setup";

// ── Audio Settings ──────────────────────────────────────
static constexpr uint32_t SAMPLE_RATE = 16000;
static constexpr size_t   MIC_CHUNK  = 512;

// ── IMA ADPCM Tables ───────────────────────────────────
static const int16_t ADPCM_STEP_TABLE[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,
    50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,
    230,253,279,307,337,371,408,449,494,544,598,658,724,796,
    876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,
    2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,
    7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,
    20350,22385,24623,27086,29794,32767
};

static const int8_t ADPCM_INDEX_TABLE[16] = {
    -1,-1,-1,-1, 2,4,6,8,
    -1,-1,-1,-1, 2,4,6,8
};

// ── ADPCM Encoder State ────────────────────────────────
struct AdpcmState {
    int16_t predictor;
    int8_t  index;
};

static AdpcmState adpcmState;

static uint8_t adpcm_encode_sample(int16_t sample) {
    int step = ADPCM_STEP_TABLE[adpcmState.index];
    int diff = sample - adpcmState.predictor;
    uint8_t nibble = 0;

    if (diff < 0) {
        nibble = 8;
        diff = -diff;
    }

    if (diff >= step) { nibble |= 4; diff -= step; }
    if (diff >= (step >> 1)) { nibble |= 2; diff -= (step >> 1); }
    if (diff >= (step >> 2)) { nibble |= 1; }

    int delta = (step >> 3);
    if (nibble & 4) delta += step;
    if (nibble & 2) delta += (step >> 1);
    if (nibble & 1) delta += (step >> 2);

    if (nibble & 8)
        adpcmState.predictor -= delta;
    else
        adpcmState.predictor += delta;

    if (adpcmState.predictor > 32767)  adpcmState.predictor = 32767;
    if (adpcmState.predictor < -32768) adpcmState.predictor = -32768;

    adpcmState.index += ADPCM_INDEX_TABLE[nibble];
    if (adpcmState.index < 0)  adpcmState.index = 0;
    if (adpcmState.index > 88) adpcmState.index = 88;

    return nibble;
}

static void adpcm_encode_block(const int16_t* pcm, uint8_t* out, size_t numSamples) {
    for (size_t i = 0; i < numSamples; i += 2) {
        uint8_t lo = adpcm_encode_sample(pcm[i]);
        uint8_t hi = adpcm_encode_sample(pcm[i + 1]);
        out[i / 2] = (hi << 4) | (lo & 0x0F);
    }
}

// ── State Machine ───────────────────────────────────────
enum AppState {
    STATE_SETUP,
    STATE_WIFI_CONNECTING,
    STATE_CHECK_PAIRED,
    STATE_PAIRING,
    STATE_WS_CONNECTING,
    STATE_READY,
    STATE_STREAMING
};
static volatile AppState appState = STATE_SETUP;

// ── Globals ─────────────────────────────────────────────
static int16_t  pcmChunk[MIC_CHUNK];
static uint8_t  adpcmBuf[MIC_CHUNK / 2];
static int32_t  dcOffset       = 0;
static unsigned long streamStartMs = 0;

static WebsocketsClient wsClient;
static bool wsConnected = false;
static Preferences prefs;

static String cfgSsid;
static String cfgPassword;
static String cfgServerUrl;

static String deviceToken;
static String pairingCode;
static String deviceMac;

static unsigned long lastWsReconnect = 0;
static constexpr unsigned long WS_RECONNECT_INTERVAL = 5000;

static unsigned long lastPairingPoll = 0;
static constexpr unsigned long PAIRING_POLL_INTERVAL = 3000;

// ── Screen Power Management ────────────────────────────
static bool screenOn = true;
static unsigned long screenOnMs = 0;
static constexpr unsigned long SCREEN_TIMEOUT = 5000;

// ── WebSocket Keepalive ────────────────────────────────
static unsigned long lastWsPing = 0;
static constexpr unsigned long WS_PING_INTERVAL = 15000;

// ── Captive Portal Globals ──────────────────────────────
static WebServer*  portalServer = nullptr;
static DNSServer*  portalDns    = nullptr;
static String      portalHtml;

// ── Pairing Code Generation ─────────────────────────────
static const char PAIRING_CHARS[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";

static String generatePairingCode() {
    String code = "";
    for (int i = 0; i < 6; i++) {
        code += PAIRING_CHARS[esp_random() % (sizeof(PAIRING_CHARS) - 1)];
    }
    return code;
}

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

static void showTwoLines(const char* line1, const char* line2, uint16_t color) {
    M5.Display.fillScreen(BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(color, BLACK);
    int16_t x1 = (M5.Display.width() - M5.Display.textWidth(line1)) / 2;
    M5.Display.setCursor(x1, 20);
    M5.Display.print(line1);
    M5.Display.setTextSize(3);
    int16_t x2 = (M5.Display.width() - M5.Display.textWidth(line2)) / 2;
    M5.Display.setCursor(x2, 48);
    M5.Display.print(line2);
}

static void showStreaming(int seconds, const int16_t* samples, size_t count) {
    M5.Display.fillScreen(BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(RED, BLACK);
    M5.Display.setCursor(20, 10);
    M5.Display.printf("STREAM  %ds", seconds);

    if (count > 0) {
        int32_t sum = 0;
        for (size_t i = 0; i < count; i++)
            sum += abs(samples[i]);
        int avg = sum / count;
        int barW = constrain(avg / 6, 0, M5.Display.width() - 20);
        M5.Display.fillRect(10, 55, barW, 14, GREEN);
        M5.Display.fillRect(10 + barW, 55, M5.Display.width() - 20 - barW, 14, DARKGREY);
    }
}

static void showSetupScreen() {
    M5.Display.fillScreen(BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(CYAN, BLACK);
    M5.Display.setCursor(30, 8);
    M5.Display.print("SETUP MODE");

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(WHITE, BLACK);
    M5.Display.setCursor(10, 40);
    M5.Display.print("Connect phone WiFi to:");

    M5.Display.setTextSize(2);
    M5.Display.setTextColor(GREEN, BLACK);
    int16_t x = (M5.Display.width() - M5.Display.textWidth(AP_SSID)) / 2;
    M5.Display.setCursor(x, 58);
    M5.Display.print(AP_SSID);

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(DARKGREY, BLACK);
    M5.Display.setCursor(10, 85);
    M5.Display.printf("Open http://%s", WiFi.softAPIP().toString().c_str());
}

// ── Screen Power Helpers ────────────────────────────────
static void screenWake() {
    M5.Display.setBrightness(80);
    screenOn = true;
    screenOnMs = millis();
}

static void screenSleep() {
    M5.Display.setBrightness(0);
    screenOn = false;
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

// ── DC Offset Removal ───────────────────────────────────
static void removeDcInPlace(int16_t* buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        dcOffset += ((int32_t)buf[i] - dcOffset) >> 8;
        int32_t val = (int32_t)buf[i] - dcOffset;
        if (val > 32767)  val = 32767;
        if (val < -32768) val = -32768;
        buf[i] = (int16_t)val;
    }
}

// ── WebSocket Callbacks ─────────────────────────────────
static void onWsMessage(WebsocketsMessage msg) {
    String data = msg.data();
    Serial.printf("WS recv: %s\n", data.c_str());

    if (data.indexOf("\"auth_ok\"") >= 0) {
        Serial.println("WebSocket authenticated");
    } else if (data.indexOf("\"wifi_update\"") >= 0) {
        // Extract ssid field
        int ssidStart = data.indexOf("\"ssid\":\"") + 8;
        int ssidEnd = data.indexOf("\"", ssidStart);
        // Extract password field
        int passStart = data.indexOf("\"password\":\"") + 12;
        int passEnd = data.indexOf("\"", passStart);

        if (ssidStart > 7 && ssidEnd > ssidStart && passStart > 11 && passEnd > passStart) {
            String newSsid = data.substring(ssidStart, ssidEnd);
            String newPass = data.substring(passStart, passEnd);

            Serial.printf("WiFi update received: ssid='%s'\n", newSsid.c_str());

            prefs.begin("audio", false);
            prefs.putString("ssid", newSsid);
            prefs.putString("pass", newPass);
            prefs.end();

            showCentered("WiFi OK!", GREEN);
            delay(1500);
            showCentered("Reboot..", YELLOW);
            delay(500);
            ESP.restart();
        }
    } else if (data.indexOf("\"paired\"") >= 0) {
        int tokenStart = data.indexOf("\"token\":\"") + 9;
        int tokenEnd = data.indexOf("\"", tokenStart);
        if (tokenStart > 8 && tokenEnd > tokenStart) {
            deviceToken = data.substring(tokenStart, tokenEnd);
            prefs.begin("audio", false);
            prefs.putString("token", deviceToken);
            prefs.end();
            Serial.printf("Paired! Token saved: %s...\n", deviceToken.substring(0, 8).c_str());
            showCentered("PAIRED!", GREEN);
            delay(1500);
            appState = STATE_WS_CONNECTING;
        }
    }
}

static void onWsEvent(WebsocketsEvent event, String data) {
    switch (event) {
    case WebsocketsEvent::ConnectionOpened:
        Serial.println("WS: connected");
        wsConnected = true;
        break;
    case WebsocketsEvent::ConnectionClosed:
        Serial.printf("WS: disconnected (state=%d, heap=%u, RSSI=%d)\n",
            (int)appState, (unsigned)ESP.getFreeHeap(), WiFi.RSSI());
        wsConnected = false;
        break;
    case WebsocketsEvent::GotPing:
        break;
    case WebsocketsEvent::GotPong:
        break;
    }
}

// ── Captive Portal ──────────────────────────────────────
static String buildSetupPage(const String& networkItems) {
    String html = R"rawliteral(<!DOCTYPE html><html><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>M5 Audio Setup</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,system-ui,sans-serif;background:#0f1117;color:#e4e6ef;
min-height:100vh;display:flex;justify-content:center;padding:20px}
.card{background:#1a1d27;border:1px solid #2e3144;border-radius:12px;padding:28px 24px;
width:100%;max-width:400px;margin-top:10px}
h2{text-align:center;margin-bottom:6px;font-size:22px}
.sub{text-align:center;color:#8b8fa3;font-size:14px;margin-bottom:20px}
label{display:block;font-size:13px;color:#8b8fa3;margin:14px 0 6px;font-weight:500}
input,select{width:100%;padding:10px 14px;background:#242736;border:1px solid #2e3144;
border-radius:8px;color:#e4e6ef;font-size:15px;outline:none}
input:focus{border-color:#6c63ff}
.nets{display:flex;flex-wrap:wrap;gap:6px;margin-top:8px}
.net{padding:6px 12px;background:#242736;border:1px solid #2e3144;border-radius:6px;
font-size:13px;cursor:pointer;transition:border-color .2s}
.net:hover,.net:active{border-color:#6c63ff;color:#6c63ff}
.rssi{color:#8b8fa3;font-size:11px;margin-left:4px}
details{margin-top:14px}
summary{color:#8b8fa3;font-size:13px;cursor:pointer}
button{width:100%;padding:12px;margin-top:20px;background:#6c63ff;color:#fff;border:none;
border-radius:8px;font-size:16px;font-weight:500;cursor:pointer}
button:hover{background:#5a52e0}
</style></head><body>
<div class="card">
<h2>M5 Audio Setup</h2>
<p class="sub">Connect your device to a WiFi hotspot</p>
<form method="POST" action="/save">
<label>WiFi Network</label>
<input type="text" name="ssid" id="ssid" placeholder="Type or tap a network below" required>
<div class="nets">)rawliteral";

    html += networkItems;

    html += R"rawliteral(</div>
<label>Password</label>
<input type="password" name="password" placeholder="Hotspot password">
<details><summary>Advanced Settings</summary>
<label>Server URL</label>
<input name="server" value=")rawliteral";

    html += cfgServerUrl.length() > 0 ? cfgServerUrl : String(DEFAULT_SERVER);

    html += R"rawliteral(">
</details>
<button type="submit">Save &amp; Connect</button>
</form></div>
<script>
document.querySelectorAll('.net').forEach(function(el){
el.onclick=function(){document.getElementById('ssid').value=this.dataset.ssid;};
});
</script>
</body></html>)rawliteral";

    return html;
}

static void handlePortalRoot() {
    portalServer->send(200, "text/html", portalHtml);
}

static void handlePortalSave() {
    String ssid = portalServer->arg("ssid");
    String pass = portalServer->arg("password");
    String server = portalServer->arg("server");

    Serial.printf("Portal save: ssid=%s, server=%s\n", ssid.c_str(), server.c_str());

    prefs.begin("audio", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    if (server.length() > 0) {
        prefs.putString("server", server);
    }
    prefs.end();

    portalServer->send(200, "text/html",
        "<!DOCTYPE html><html><head>"
        "<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<style>body{font-family:-apple-system,sans-serif;background:#0f1117;color:#e4e6ef;"
        "display:flex;justify-content:center;align-items:center;min-height:100vh;text-align:center}"
        ".ok{font-size:48px;margin-bottom:16px}</style></head><body>"
        "<div><div class='ok'>&#10003;</div><h2>Saved!</h2><p style='color:#8b8fa3;margin-top:8px'>"
        "Rebooting and connecting...</p></div></body></html>");

    delay(1500);
    ESP.restart();
}

static void startCaptivePortal() {
    Serial.println("Starting captive portal...");

    WiFi.mode(WIFI_AP_STA);
    delay(200);

    // Scan for nearby networks
    Serial.println("Scanning WiFi networks...");
    int n = WiFi.scanNetworks();
    String netItems = "";
    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        int rssi = WiFi.RSSI(i);
        if (ssid.length() == 0) continue;
        netItems += "<div class='net' data-ssid='" + ssid + "'>"
                  + ssid + "<span class='rssi'>" + String(rssi) + "dB</span></div>";
    }
    WiFi.scanDelete();
    if (n == 0) {
        netItems = "<div style='color:#8b8fa3;font-size:13px'>No networks found. Type name manually.</div>";
    }

    portalHtml = buildSetupPage(netItems);

    // Start soft AP (open network for easy phone connection)
    WiFi.softAP(AP_SSID);
    delay(500);
    Serial.printf("AP started: %s, IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());

    // DNS server: redirect all lookups to our IP (triggers captive portal on iOS/Android)
    portalDns = new DNSServer();
    portalDns->start(53, "*", WiFi.softAPIP());

    // Web server
    portalServer = new WebServer(80);
    portalServer->on("/", HTTP_GET, handlePortalRoot);
    portalServer->on("/save", HTTP_POST, handlePortalSave);
    portalServer->onNotFound(handlePortalRoot);
    portalServer->begin();

    showSetupScreen();
}

// ── WiFi Connection ─────────────────────────────────────
static bool connectWiFi() {
    Serial.printf("WiFi: connecting to %s...\n", cfgSsid.c_str());
    showCentered("WiFi...", YELLOW);

    WiFi.mode(WIFI_STA);
    delay(500);

    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);

    wifi_config_t wifi_cfg = {};
    strncpy((char*)wifi_cfg.sta.ssid, cfgSsid.c_str(), sizeof(wifi_cfg.sta.ssid));
    strncpy((char*)wifi_cfg.sta.password, cfgPassword.c_str(), sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    wifi_cfg.sta.pmf_cfg.capable = false;
    wifi_cfg.sta.pmf_cfg.required = false;

    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_connect();

    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 40) {
        delay(500);
        Serial.print(".");
        tries++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("WiFi: connected, IP: %s\n", WiFi.localIP().toString().c_str());
        deviceMac = WiFi.macAddress();
        deviceMac.replace(":", "");
        Serial.printf("Device MAC: %s\n", deviceMac.c_str());
        return true;
    }
    Serial.println("WiFi: connection failed");
    return false;
}

// ── WebSocket Connection ────────────────────────────────
static bool connectWebSocket() {
    String url = cfgServerUrl + "?token=" + deviceToken + "&mac=" + deviceMac;
    Serial.printf("WS: connecting to %s\n", url.c_str());
    return wsClient.connect(url);
}

// ── Parse host and port from server URL ─────────────────
static void parseServerUrl(String& outHost, int& outPort) {
    String url = cfgServerUrl;
    int hostStart = url.indexOf("://") + 3;
    int portStart = url.indexOf(":", hostStart);
    int pathStart = url.indexOf("/", hostStart);
    outHost = url.substring(hostStart, portStart > 0 ? portStart : pathStart);
    outPort = 8888;
    if (portStart > 0 && pathStart > portStart) {
        outPort = url.substring(portStart + 1, pathStart).toInt();
    }
}

// ── HTTP Helper for Pairing ─────────────────────────────
static bool registerForPairing() {
    WiFiClient http;
    String serverHost;
    int serverPort;
    parseServerUrl(serverHost, serverPort);

    if (!http.connect(serverHost.c_str(), serverPort)) {
        Serial.println("HTTP: connection failed");
        return false;
    }

    String body = "{\"mac\":\"" + deviceMac + "\",\"code\":\"" + pairingCode + "\"}";
    http.printf("POST /api/devices/register HTTP/1.1\r\n"
                "Host: %s:%d\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %d\r\n"
                "Connection: close\r\n\r\n%s",
                serverHost.c_str(), serverPort, body.length(), body.c_str());

    unsigned long start = millis();
    while (!http.available() && millis() - start < 5000) delay(10);

    String response = "";
    while (http.available()) response += (char)http.read();
    http.stop();

    Serial.printf("Register response: %s\n", response.c_str());
    return response.indexOf("200") >= 0 || response.indexOf("201") >= 0;
}

static String pollPairingStatus() {
    WiFiClient http;
    String serverHost;
    int serverPort;
    parseServerUrl(serverHost, serverPort);

    if (!http.connect(serverHost.c_str(), serverPort)) return "";

    String path = "/api/devices/status?mac=" + deviceMac;
    http.printf("GET %s HTTP/1.1\r\n"
                "Host: %s:%d\r\n"
                "Connection: close\r\n\r\n",
                path.c_str(), serverHost.c_str(), serverPort);

    unsigned long start = millis();
    while (!http.available() && millis() - start < 5000) delay(10);

    String response = "";
    while (http.available()) response += (char)http.read();
    http.stop();

    int tokenStart = response.indexOf("\"token\":\"") + 9;
    int tokenEnd = response.indexOf("\"", tokenStart);
    if (tokenStart > 8 && tokenEnd > tokenStart) {
        return response.substring(tokenStart, tokenEnd);
    }
    return "";
}

// ── Setup ───────────────────────────────────────────────
void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);
    Serial.begin(115200);
    delay(100);

    M5.Display.setRotation(1);
    M5.Display.setBrightness(80);
    showCentered("INIT...", YELLOW);

    M5.Speaker.end();

    // Factory reset: hold BtnA during boot for 5 seconds
    M5.update();
    if (M5.BtnA.isPressed()) {
        unsigned long holdStart = millis();
        while (true) {
            M5.update();
            if (!M5.BtnA.isPressed()) {
                showCentered("INIT...", YELLOW);
                break;
            }
            unsigned long held = (millis() - holdStart) / 1000;
            if (held >= 5) {
                prefs.begin("audio", false);
                prefs.clear();
                prefs.end();
                showCentered("RESET!", RED);
                delay(1500);
                ESP.restart();
            }
            M5.Display.fillScreen(BLACK);
            M5.Display.setTextSize(2);
            M5.Display.setTextColor(YELLOW, BLACK);
            M5.Display.setCursor(15, 15);
            M5.Display.print("FACTORY RESET");
            M5.Display.setCursor(15, 50);
            M5.Display.printf("Hold %lus more...", 5 - held);
            delay(100);
        }
    }

    auto mic_cfg          = M5.Mic.config();
    mic_cfg.sample_rate   = SAMPLE_RATE;
    mic_cfg.dma_buf_count = 8;
    mic_cfg.dma_buf_len   = 256;
    M5.Mic.config(mic_cfg);

    wsClient.onMessage(onWsMessage);
    wsClient.onEvent(onWsEvent);

    // Load saved configuration
    prefs.begin("audio", true);
    cfgSsid      = prefs.getString("ssid", "");
    cfgPassword   = prefs.getString("pass", "");
    cfgServerUrl  = prefs.getString("server", DEFAULT_SERVER);
    deviceToken   = prefs.getString("token", "");
    prefs.end();

    Serial.printf("Config: ssid='%s', server='%s', token=%s\n",
        cfgSsid.c_str(), cfgServerUrl.c_str(),
        deviceToken.length() > 0 ? "present" : "none");
    Serial.printf("Free heap: %u bytes\n", (unsigned)ESP.getFreeHeap());

    if (cfgSsid.length() == 0) {
        appState = STATE_SETUP;
        startCaptivePortal();
    } else {
        appState = STATE_WIFI_CONNECTING;
    }
}

// ── Main Loop ───────────────────────────────────────────
void loop() {
    M5.update();

    switch (appState) {

    case STATE_SETUP: {
        portalDns->processNextRequest();
        portalServer->handleClient();
        break;
    }

    case STATE_WIFI_CONNECTING: {
        if (connectWiFi()) {
            appState = STATE_CHECK_PAIRED;
        } else {
            showTwoLines("NO WiFi", "Retry in 5s", RED);
            Serial.println("WiFi failed. Hold BtnA at boot to reconfigure.");
            delay(5000);
        }
        break;
    }

    case STATE_CHECK_PAIRED: {
        if (deviceToken.length() > 0) {
            Serial.println("Token exists, connecting to cloud...");
            appState = STATE_WS_CONNECTING;
        } else {
            Serial.println("No token, entering pairing mode...");
            pairingCode = generatePairingCode();
            Serial.printf("Pairing code: %s\n", pairingCode.c_str());
            showTwoLines("PAIR CODE:", pairingCode.c_str(), CYAN);
            drawBattery();

            if (registerForPairing()) {
                Serial.println("Registered with cloud, waiting for user to pair...");
            } else {
                Serial.println("Failed to register, will retry...");
            }
            lastPairingPoll = millis();
            appState = STATE_PAIRING;
        }
        break;
    }

    case STATE_PAIRING: {
        if (millis() - lastPairingPoll >= PAIRING_POLL_INTERVAL) {
            lastPairingPoll = millis();
            String token = pollPairingStatus();
            if (token.length() > 0) {
                deviceToken = token;
                prefs.begin("audio", false);
                prefs.putString("token", deviceToken);
                prefs.end();
                Serial.printf("Paired! Token: %s...\n", deviceToken.substring(0, 8).c_str());
                showCentered("PAIRED!", GREEN);
                delay(1500);
                appState = STATE_WS_CONNECTING;
            }
        }
        break;
    }

    case STATE_WS_CONNECTING: {
        if (WiFi.status() != WL_CONNECTED) {
            appState = STATE_WIFI_CONNECTING;
            break;
        }

        if (millis() - lastWsReconnect < WS_RECONNECT_INTERVAL) break;
        lastWsReconnect = millis();

        showCentered("CLOUD..", YELLOW);
        if (connectWebSocket()) {
            appState = STATE_READY;
            showCentered("READY", GREEN);
            drawBattery();
            screenWake();
            Serial.printf("Connected to cloud. Free heap: %u\n", (unsigned)ESP.getFreeHeap());
        } else {
            Serial.println("WS: connection failed, retrying...");
        }
        break;
    }

    case STATE_READY: {
        wsClient.poll();

        if (!wsConnected) {
            appState = STATE_WS_CONNECTING;
            break;
        }

        if (screenOn && millis() - screenOnMs >= SCREEN_TIMEOUT) {
            screenSleep();
        }

        if (M5.BtnA.wasPressed()) {
            if (!screenOn) {
                screenWake();
                showCentered("READY", GREEN);
                drawBattery();
            } else {
                Serial.println("Starting stream...");

                adpcmState.predictor = 0;
                adpcmState.index = 0;
                dcOffset = 0;

                M5.Mic.begin();
                delay(80);

                uint8_t hdr[8];
                uint32_t sr = SAMPLE_RATE;
                uint32_t marker = 1;
                memcpy(hdr, &sr, 4);
                memcpy(hdr + 4, &marker, 4);
                wsClient.sendBinary((const char*)hdr, sizeof(hdr));

                streamStartMs = millis();
                lastWsPing = millis();
                appState = STATE_STREAMING;
                showStreaming(0, nullptr, 0);
                drawBattery();
                screenWake();
                Serial.printf("Stream started. Free heap: %u\n", (unsigned)ESP.getFreeHeap());
            }
        }
        break;
    }

    case STATE_STREAMING: {
        wsClient.poll();

        if (wsConnected && millis() - lastWsPing >= WS_PING_INTERVAL) {
            wsClient.ping();
            lastWsPing = millis();
        }

        if (!wsConnected) {
            M5.Mic.end();
            unsigned long elapsed = (millis() - streamStartMs) / 1000;
            Serial.printf("Stream lost after %lus (RSSI=%d, heap=%u)\n",
                elapsed, WiFi.RSSI(), (unsigned)ESP.getFreeHeap());
            appState = STATE_WS_CONNECTING;
            screenWake();
            showCentered("LOST", RED);
            delay(1000);
            break;
        }

        if (M5.BtnA.wasPressed()) {
            M5.Mic.end();
            delay(100);

            uint8_t eot[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
            wsClient.sendBinary((const char*)eot, sizeof(eot));

            unsigned long elapsed = (millis() - streamStartMs) / 1000;
            Serial.printf("Stream stopped after %lus\n", elapsed);

            appState = STATE_READY;
            screenWake();
            showCentered("READY", GREEN);
            drawBattery();
            break;
        }

        if (M5.Mic.record(pcmChunk, MIC_CHUNK, SAMPLE_RATE)) {
            removeDcInPlace(pcmChunk, MIC_CHUNK);
            adpcm_encode_block(pcmChunk, adpcmBuf, MIC_CHUNK);

            if (wsConnected) {
                wsClient.sendBinary((const char*)adpcmBuf, MIC_CHUNK / 2);
            }
        }

        if (screenOn && millis() - screenOnMs >= SCREEN_TIMEOUT) {
            screenSleep();
        }

        if (screenOn) {
            static int lastDispSec = -1;
            int sec = (millis() - streamStartMs) / 1000;
            if (sec != lastDispSec) {
                showStreaming(sec, pcmChunk, MIC_CHUNK);
                drawBattery();
                lastDispSec = sec;
            }
        }

        break;
    }
    }
}
