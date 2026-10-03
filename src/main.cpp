// ============================================================
//  SOUMYA — MP3 Player + BT TWS (Complete Fixed Version)
//  Fixes: BT controller init, Bluedroid init, A2DP source
//  Serial: play / next / prev / stop / list / vol / status
// ============================================================

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <BluetoothA2DPSource.h>
#include <AudioFileSourceSD.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutput.h>
#include "esp32-hal-bt.h"
#include "esp_bt.h"
#include "esp_bt_main.h"

// ─── SD Pins ───
#define PIN_SD_CS    5
#define PIN_SD_SCK   18
#define PIN_SD_MISO  19
#define PIN_SD_MOSI  23

// ─── Config ───
#define MUSIC_DIR  "/music"
#define BT_NAME    "SOUMYA-Music"
#define RING_SIZE  32768

// ─── BT Audio ───
BluetoothA2DPSource a2dp;
static bool a2dpStarted = false;

// ═══════════════════════════════════════════════════════════
//  Ring Buffer
// ═══════════════════════════════════════════════════════════
static int16_t ringBuf[RING_SIZE];
static volatile int rbHead = 0, rbTail = 0;
static portMUX_TYPE rbMux = portMUX_INITIALIZER_UNLOCKED;

static inline int rb_avail() {
    return (rbHead - rbTail + RING_SIZE) % RING_SIZE;
}

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

// ═══════════════════════════════════════════════════════════
//  Custom AudioOutput → ring buffer
// ═══════════════════════════════════════════════════════════
class AudioOutputRingBuffer : public AudioOutput {
public:
    bool ConsumeSample(int16_t sample[2]) override {
        rb_push(sample[0], sample[1]);
        return true;
    }
    bool begin() override { return true; }
    bool stop()  override { return true; }
};

// ═══════════════════════════════════════════════════════════
//  A2DP data callback
// ═══════════════════════════════════════════════════════════
static int32_t a2dp_data_cb(Frame* frames, int32_t n) {
    for (int i = 0; i < n; i++) {
        int16_t L = 0, R = 0;
        rb_pop(L, R);
        frames[i].channel1 = L;
        frames[i].channel2 = R;
    }
    return n;
}

// ═══════════════════════════════════════════════════════════
//  Audio objects
// ═══════════════════════════════════════════════════════════
static AudioFileSource*       audioSrc = nullptr;
static AudioGeneratorMP3*     audioMP3 = nullptr;
static AudioOutputRingBuffer* audioRB  = nullptr;

static bool isPlaying = false;
static int  volume = 100;
static int  currentSong = -1;

// ═══════════════════════════════════════════════════════════
//  Song list
// ═══════════════════════════════════════════════════════════
#define MAX_SONGS 64
static String songFiles[MAX_SONGS];
static int songCount = 0;

String songName(int idx) {
    if (idx < 0 || idx >= songCount) return "(none)";
    String p = songFiles[idx];
    int slash = p.lastIndexOf('/');
    return slash >= 0 ? p.substring(slash + 1) : p;
}

void loadSongList() {
    songCount = 0;
    File dir = SD.open(MUSIC_DIR);
    if (!dir || !dir.isDirectory()) {
        Serial.println("[!] /music folder missing");
        return;
    }
    File f = dir.openNextFile();
    while (f && songCount < MAX_SONGS) {
        String name = String(f.name());
        if (!f.isDirectory()) {
            String lower = name;
            lower.toLowerCase();
            if (lower.endsWith(".mp3")) {
                songFiles[songCount] = String(MUSIC_DIR) + "/" + name;
                songCount++;
            }
        }
        f = dir.openNextFile();
    }
    dir.close();
    Serial.printf("[+] Found %d songs\n", songCount);
}

// ═══════════════════════════════════════════════════════════
//  BT A2DP Init
// ═══════════════════════════════════════════════════════════
void ensureBT() {
    if (a2dpStarted) return;

    Serial.println("[*] Releasing BLE memory...");
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);

    Serial.println("[*] btStart()...");
    if (!btStart()) {
        Serial.println("[!] btStart FAILED");
        delay(500);
        if (!btStart()) {
            Serial.println("[!] btStart FAILED twice — abort");
            return;
        }
    }
    delay(200);
    Serial.printf("[+] BT status: %d\n", esp_bt_controller_get_status());

    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        esp_bluedroid_init();
        Serial.println("[+] Bluedroid initialized");
    }
    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
        esp_bluedroid_enable();
        Serial.println("[+] Bluedroid enabled");
    }

    Serial.println("[*] Starting A2DP source...");
    a2dp.set_auto_reconnect(false);
    a2dp.set_volume(127);
    a2dp.start(BT_NAME, a2dp_data_cb);
    a2dpStarted = true;
    delay(500);
    Serial.println("[+] A2DP source started — device broadcasting as SOUMYA-Music");
}

// ═══════════════════════════════════════════════════════════
//  Playback control
// ═══════════════════════════════════════════════════════════
void stopSong() {
    if (audioMP3 && audioMP3->isRunning()) audioMP3->stop();
    if (audioSrc) audioSrc->close();
    delete audioMP3; audioMP3 = nullptr;
    delete audioSrc; audioSrc = nullptr;
    isPlaying = false;
    rbHead = rbTail = 0;
}

void playSong(int idx) {
    if (idx < 0 || idx >= songCount) {
        Serial.printf("[!] Invalid index: %d\n", idx);
        return;
    }
    stopSong();
    delay(100);
    currentSong = idx;
    String path = songFiles[idx];

    Serial.printf("\n[PLAY] %s\n", songName(idx).c_str());
    Serial.printf("       Path: %s\n", path.c_str());

    if (!a2dpStarted) ensureBT();

    audioSrc = new AudioFileSourceSD(path.c_str());
    if (!audioSrc) { Serial.println("[!] Cannot open file"); return; }

    audioMP3 = new AudioGeneratorMP3();
    if (!audioMP3) {
        Serial.println("[!] MP3 decoder create failed");
        delete audioSrc; audioSrc = nullptr;
        return;
    }

    if (audioMP3->begin(audioSrc, audioRB)) {
        isPlaying = true;
        Serial.println("[+] MP3 playback started");
    } else {
        Serial.println("[!] MP3 begin failed");
        stopSong();
    }
}

void nextSong() {
    if (songCount == 0) return;
    playSong((currentSong + 1) % songCount);
}

void prevSong() {
    if (songCount == 0) return;
    playSong((currentSong - 1 + songCount) % songCount);
}

// ═══════════════════════════════════════════════════════════
//  Commands
// ═══════════════════════════════════════════════════════════
void printHelp() {
    Serial.println();
    Serial.println("═══════════════════════════════════");
    Serial.println("     SOUMYA MP3 → BT TWS");
    Serial.println("═══════════════════════════════════");
    Serial.println(" list            Show songs");
    Serial.println(" play <n>        Play song #n (1-based)");
    Serial.println(" play            Play first song");
    Serial.println(" next / prev     Navigate");
    Serial.println(" stop            Stop playback");
    Serial.println(" vol <0-100>     Volume");
    Serial.println(" status          Show state");
    Serial.println(" bt              BT status");
    Serial.println(" help            This menu");
    Serial.println("═══════════════════════════════════");
    Serial.println();
}

void showStatus() {
    Serial.println();
    Serial.println("═══ STATUS ═══");
    Serial.printf("Songs:    %d\n", songCount);
    Serial.printf("Playing:  %s\n", isPlaying ? "YES" : "no");
    Serial.printf("Current:  %s\n", currentSong >= 0 ? songName(currentSong).c_str() : "(none)");
    Serial.printf("Volume:   %d%%\n", volume);
    Serial.printf("Ring:     %d / %d\n", rb_avail(), RING_SIZE);
    Serial.printf("BT:       %s\n", a2dp.is_connected() ? "CONNECTED" : "waiting");
    Serial.printf("Heap:     %u\n", ESP.getFreeHeap());
    Serial.println("═════════════");
    Serial.println();
}

void handleCmd(String cmd) {
    cmd.trim();
    if (cmd.length() == 0) return;
    String lc = cmd;
    lc.toLowerCase();

    if (lc == "help" || lc == "?") { printHelp(); return; }
    if (lc == "status" || lc == "st") { showStatus(); return; }
    if (lc == "bt") {
        Serial.printf("BT: %s\n", a2dp.is_connected() ? "CONNECTED" : "waiting");
        Serial.printf("BT controller status: %d\n", esp_bt_controller_get_status());
        Serial.printf("Bluedroid status: %d\n", esp_bluedroid_get_status());
        return;
    }
    if (lc == "list") {
        Serial.printf("\nSongs (%d):\n", songCount);
        for (int i = 0; i < songCount; i++) {
            Serial.printf("  %2d. %s\n", i + 1, songName(i).c_str());
        }
        if (songCount == 0) Serial.println("  (none)");
        Serial.println();
        return;
    }
    if (lc == "stop") { stopSong(); Serial.println("[*] Stopped"); return; }
    if (lc == "next") { nextSong(); return; }
    if (lc == "prev") { prevSong(); return; }

    if (lc.startsWith("play ")) {
        int n = cmd.substring(5).toInt();
        playSong(n - 1);
        return;
    }
    if (lc == "play") {
        if (songCount > 0) playSong(0);
        return;
    }

    if (lc.startsWith("vol ")) {
        int v = constrain(cmd.substring(4).toInt(), 0, 100);
        volume = v;
        a2dp.set_volume(v * 2);
        Serial.printf("[+] Volume %d%%\n", v);
        return;
    }

    Serial.printf("[!] Unknown: %s\n", cmd.c_str());
}

// ═══════════════════════════════════════════════════════════
//  Setup
// ═══════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println("╔══════════════════════════════════════╗");
    Serial.println("║   SOUMYA MP3 → BT TWS v1.1          ║");
    Serial.println("╚══════════════════════════════════════╝");
    Serial.println();

    // SD init
    SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    if (!SD.begin(PIN_SD_CS)) {
        Serial.println("[!] SD mount FAIL");
    } else {
        Serial.println("[+] SD mounted");
        loadSongList();
    }

    // Ring buffer
    audioRB = new AudioOutputRingBuffer();

    // WiFi OFF
    WiFi.mode(WIFI_OFF);

    // BT early init — broadcast SOUMYA-Music
    ensureBT();

    Serial.println();
    Serial.println("Ready. Type 'help'.");
    Serial.println();
}

// ═══════════════════════════════════════════════════════════
//  Loop
// ═══════════════════════════════════════════════════════════
void loop() {
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        handleCmd(cmd);
    }

    if (isPlaying && audioMP3) {
        if (audioMP3->isRunning()) {
            if (!audioMP3->loop()) {
                Serial.println("[*] Song ended");
                nextSong();
            }
        } else {
            isPlaying = false;
        }
    }

    static unsigned long lastMs = 0;
    if (millis() - lastMs > 5000) {
        lastMs = millis();
        if (isPlaying || a2dpStarted) {
            Serial.printf("[♪] ring=%d BT=%s\n",
                rb_avail(),
                a2dp.is_connected() ? "YES" : "no");
        }
    }

    delay(1);
}
