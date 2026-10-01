// ============================================================
//  SOUMYA — BT Audio Test (Raw ESP-IDF on Core 2.0.17)
//  TWS scan → connect → melody + AVRCP gesture
// ============================================================

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <math.h>

extern "C" {
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
}

#define OLED_SDA 21
#define OLED_SCL 22
#define OLED_ADDR 0x3C
#define SCR_W 128
#define SCR_H 64

Adafruit_SSD1306 display(SCR_W, SCR_H, &Wire, -1);

// ─── Sine table ───
int16_t sineTable[256];
void initSineTable() {
    for (int i = 0; i < 256; i++)
        sineTable[i] = (int16_t)(sinf(i * 2.0f * PI / 256.0f) * 28000);
}

// ─── Melody ───
const float MELODY[] = {440.0f, 523.25f, 659.25f, 783.99f, 1046.5f};
const int MELODY_LEN = 5;
const unsigned long NOTE_DUR_MS = 350;
float currentFreq = 440.0f;
unsigned long noteStartMs = 0;
int noteIndex = 0;
uint16_t phaseAcc = 0;
bool isPlaying = true;

volatile uint32_t cbCalls = 0;
volatile uint32_t cbFrames = 0;
unsigned long lastReport = 0;
unsigned long connectTimeMs = 0;

// ─── Devices ───
struct BtDevice { uint8_t mac[6]; char name[32]; };
#define MAX_DEVICES 20
BtDevice btDevices[MAX_DEVICES];
int btDeviceCount = 0;

bool a2dConn = false;

// ─── OLED ───
void oledMsg(const char* l1, const char* l2) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    int w1 = strlen(l1) * 6;
    if (w1 > SCR_W) w1 = SCR_W;
    display.setCursor((SCR_W - w1) / 2, 20);
    display.print(l1);
    if (l2 && strlen(l2) > 0) {
        int w2 = strlen(l2) * 6;
        display.setCursor((SCR_W - w2) / 2, 36);
        display.print(l2);
    }
    display.display();
}

// ═══════════════════════════════════════════════════════════
//  A2DP DATA CALLBACK
// ═══════════════════════════════════════════════════════════
int32_t a2d_data_cb(uint8_t *buf, int32_t len) {
    cbCalls++;
    cbFrames += (len / 4);

    if (!isPlaying) {
        memset(buf, 0, len);
        return len;
    }

    unsigned long now = millis();
    if (now - noteStartMs > NOTE_DUR_MS) {
        noteStartMs = now;
        noteIndex = (noteIndex + 1) % MELODY_LEN;
        currentFreq = MELODY[noteIndex];
    }

    uint32_t phaseInc = (uint32_t)(currentFreq / 44100.0f * 65536.0f);

    int16_t *samples = (int16_t*)buf;
    int numI16 = len / 2;
    for (int i = 0; i < numI16; i += 2) {
        phaseAcc += phaseInc;
        uint8_t idx = (phaseAcc >> 8) & 0xFF;
        int16_t s = sineTable[idx];
        samples[i]   = s;
        samples[i+1] = s;
    }
    return len;
}

// ═══════════════════════════════════════════════════════════
//  A2DP CALLBACK
// ═══════════════════════════════════════════════════════════
void a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT: {
            esp_a2d_connection_state_t st = param->conn_stat.state;
            if (st == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                a2dConn = true;
                connectTimeMs = millis();
                Serial.println("\n[A2DP] CONNECTED");
                oledMsg("A2DP Connected", "Wait stream...");
            } else if (st == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                a2dConn = false;
                Serial.println("[A2DP] Disconnected");
                oledMsg("Disconnected", "");
            } else if (st == ESP_A2D_CONNECTION_STATE_CONNECTING) {
                Serial.println("[A2DP] Connecting...");
            }
            break;
        }
        case ESP_A2D_AUDIO_STATE_EVT: {
            esp_a2d_audio_state_t st = param->audio_stat.state;
            if (st == ESP_A2D_AUDIO_STATE_STARTED) {
                Serial.println("[A2DP] AUDIO STREAM STARTED");
                oledMsg("STREAMING", "Playing tone");
            } else if (st == ESP_A2D_AUDIO_STATE_STOPPED) {
                Serial.println("[A2DP] Audio stopped");
            } else if (st == ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND) {
                Serial.println("[A2DP] Audio suspended by remote");
            }
            break;
        }
        case ESP_A2D_AUDIO_CFG_EVT:
            Serial.println("[A2DP] Codec configured");
            break;
        case ESP_A2D_PROF_STATE_EVT:
            Serial.printf("[A2DP] Prof state: %d\n", param->a2d_prof_stat.init_state);
            break;
        default: break;
    }
}

// ═══════════════════════════════════════════════════════════
//  AVRCP CALLBACK
// ═══════════════════════════════════════════════════════════
// ═══════════════════════════════════════════════════════════
//  AVRCP TG CALLBACK — TWS gesture reception
// ═══════════════════════════════════════════════════════════
void avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param) {
    switch (event) {
        case ESP_AVRC_TG_CONNECTION_STATE_EVT:
            Serial.printf("[AVRCP-TG] %s\n",
                param->conn_stat.connected ? "connected" : "disconnected");
            break;
        case ESP_AVRC_TG_REMOTE_FEATURES_EVT:
            Serial.println("[AVRCP-TG] Remote features received");
            break;
        case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT:
            Serial.printf("[TWS TAP] key=0x%02X state=0x%02X\n",
                param->psth_cmd.key_code,
                param->psth_cmd.key_state);
            if (param->psth_cmd.key_state == 0x00) {   // PRESSED
                switch (param->psth_cmd.key_code) {
                    case 0x44: Serial.println("  -> PLAY"); isPlaying = true; break;
                    case 0x46: Serial.println("  -> PAUSE"); isPlaying = false; break;
                    case 0x45: Serial.println("  -> STOP"); isPlaying = false; break;
                    case 0x4B: Serial.println("  -> NEXT"); break;
                    case 0x4C: Serial.println("  -> PREV"); break;
                    case 0x41: Serial.println("  -> VOL+"); break;
                    case 0x42: Serial.println("  -> VOL-"); break;
                    case 0x48: Serial.println("  -> FFWD"); break;
                    case 0x49: Serial.println("  -> REWIND"); break;
                }
            }
            break;
        case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
            Serial.printf("[TWS VOL] absolute vol=%d\n",
                param->set_abs_vol.volume);
            break;
        default:
            Serial.printf("[AVRCP-TG] event=%d\n", event);
            break;
    }
}

void avrc_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param) {
    switch (event) {
        case ESP_AVRC_CT_CONNECTION_STATE_EVT:
            Serial.printf("[AVRCP] %s\n",
                param->conn_stat.connected ? "connected" : "disconnected");
            break;
        case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
            Serial.printf("[AVRCP] features 0x%lx\n",
                (unsigned long)param->rmt_feats.feat_mask);
            break;
        case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
            Serial.printf("[TWS GESTURE] event_id=%d\n", param->change_ntf.event_id);
            if (param->change_ntf.event_id == ESP_AVRC_RN_PLAY_STATUS_CHANGE) {
                uint8_t st = param->change_ntf.event_parameter.playback;
                if (st == 1) { isPlaying = true; Serial.println("[TWS] PLAY"); }
                else if (st == 2) { isPlaying = false; Serial.println("[TWS] PAUSE"); }
            }
            break;
        default: break;
    }
}

// ═══════════════════════════════════════════════════════════
//  GAP CALLBACK
// ═══════════════════════════════════════════════════════════
void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    switch (event) {
        case ESP_BT_GAP_DISC_RES_EVT: {
            uint8_t* bda = param->disc_res.bda;
            char name[32] = "";
            for (int i = 0; i < param->disc_res.num_prop; i++) {
                if (param->disc_res.prop[i].type == ESP_BT_GAP_DEV_PROP_BDNAME) {
                    int len = param->disc_res.prop[i].len;
                    if (len > 31) len = 31;
                    memcpy(name, param->disc_res.prop[i].val, len);
                    name[len] = 0;
                }
            }
            for (int i = 0; i < btDeviceCount; i++)
                if (memcmp(btDevices[i].mac, bda, 6) == 0) return;
            if (btDeviceCount >= MAX_DEVICES) return;

            memcpy(btDevices[btDeviceCount].mac, bda, 6);
            strncpy(btDevices[btDeviceCount].name, name, 31);
            btDevices[btDeviceCount].name[31] = 0;
            btDeviceCount++;
            Serial.printf("  [%2d] %02X:%02X:%02X:%02X:%02X:%02X  %s\n",
                btDeviceCount, bda[0], bda[1], bda[2], bda[3], bda[4], bda[5],
                name[0] ? name : "(unknown)");
            break;
        }
        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
            if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
                Serial.printf("\n[*] Scan done. %d devices.\n", btDeviceCount);
                Serial.println("[*] Type: connect N");
            }
            break;
        case ESP_BT_GAP_AUTH_CMPL_EVT:
            Serial.printf("[GAP] Auth %s\n",
                param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS ? "OK" : "FAIL");
            break;
        case ESP_BT_GAP_CFM_REQ_EVT:
            esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
            break;
        default: break;
    }
}

// ═══════════════════════════════════════════════════════════
//  Commands
// ═══════════════════════════════════════════════════════════
void printHelp() {
    Serial.println("\n── Commands ──");
    Serial.println("  scan          - Scan BT");
    Serial.println("  connect N     - Connect N");
    Serial.println("  play / pause  - Tone control");
    Serial.println("  status        - State");
    Serial.println("  help          - Menu\n");
}

void startScan() {
    btDeviceCount = 0;
    Serial.println("\n[*] Scan BT (10s)...");
    Serial.println("── Devices ──");
    oledMsg("Scanning...", "");
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
}

void connectTo(int idx) {
    if (idx < 1 || idx > btDeviceCount) {
        Serial.printf("[!] Range 1-%d\n", btDeviceCount);
        return;
    }
    BtDevice* d = &btDevices[idx - 1];
    Serial.printf("[*] Connect [%d] %02X:%02X:%02X:%02X:%02X:%02X\n",
        idx, d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5]);
    oledMsg("Connecting...", d->name);
    esp_a2d_source_connect(d->mac);
}

// ═══════════════════════════════════════════════════════════
//  Setup
// ═══════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    delay(800);
    Serial.println("\n=== SOUMYA BT Audio v4 ===\n");

    Wire.begin(OLED_SDA, OLED_SCL);
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        Serial.println("[!] OLED FAIL");
    } else {
        oledMsg("BT Audio v4", "Init...");
    }

    initSineTable();
    Serial.println("[+] Sine table ready");

    Serial.println("[*] btStart()...");
    if (!btStart()) {
        Serial.println("[!] btStart failed — retry");
        delay(500);
        if (!btStart()) {
            Serial.println("[!] Failed twice — abort");
            return;
        }
    }
    delay(200);
    Serial.printf("[+] btStart OK — status: %d\n",
        esp_bt_controller_get_status());

    esp_bluedroid_status_t bd = esp_bluedroid_get_status();
    Serial.printf("[*] Bluedroid: %d\n", bd);
    if (bd == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        Serial.printf("[*] bd_init: %d\n", esp_bluedroid_init());
        Serial.printf("[*] bd_enable: %d\n", esp_bluedroid_enable());
    } else if (bd == ESP_BLUEDROID_STATUS_INITIALIZED) {
        esp_bluedroid_enable();
    }
    Serial.println("[+] Bluedroid ready");

    esp_bt_dev_set_device_name("SOUMYA-Audio");
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    Serial.println("[+] Device: SOUMYA-Audio");

    esp_bt_gap_register_callback(gap_cb);
    esp_a2d_register_callback(a2d_cb);
    esp_a2d_source_register_data_callback(a2d_data_cb);
    Serial.printf("[*] a2d_init: %d\n", esp_a2d_source_init());

    esp_avrc_ct_register_callback(avrc_cb);
    Serial.printf("[*] avrc_ct_init: %d\n", esp_avrc_ct_init());

    esp_avrc_tg_register_callback(avrc_tg_cb);
    Serial.printf("[*] avrc_tg_init: %d\n", esp_avrc_tg_init());

    esp_avrc_rn_evt_cap_mask_t cap = {};
    cap.bits = 0xFFFF;
    esp_avrc_tg_set_rn_evt_cap(&cap);
    Serial.println("[+] AVRCP TG caps set");

    WiFi.mode(WIFI_OFF);
    Serial.println("[+] WiFi off");

    Serial.println("\n=== Ready ===");
    printHelp();
    delay(500);
    startScan();
}

// ═══════════════════════════════════════════════════════════
//  Loop
// ═══════════════════════════════════════════════════════════
void loop() {
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim(); cmd.toLowerCase();
        if (cmd.length() == 0) { delay(20); return; }

        if (cmd == "help" || cmd == "?") printHelp();
        else if (cmd == "scan") startScan();
        else if (cmd.startsWith("connect ")) connectTo(cmd.substring(8).toInt());
        else if (cmd.startsWith("c ")) connectTo(cmd.substring(2).toInt());
        else if (cmd == "play") { isPlaying = true; Serial.println("[*] PLAY"); }
        else if (cmd == "pause") { isPlaying = false; Serial.println("[*] PAUSE"); }
        else if (cmd == "status") {
            Serial.println("\n=== STATUS ===");
            Serial.printf("A2DP: %s\n", a2dConn ? "CONNECTED" : "no");
            Serial.printf("CB calls: %u\n", cbCalls);
            Serial.printf("Frames: %u\n", cbFrames);
            Serial.printf("Heap: %u\n\n", ESP.getFreeHeap());
        }
        else Serial.printf("[!] Unknown: %s\n", cmd.c_str());
    }

    unsigned long now = millis();
    if (now - lastReport >= 3000) {
        lastReport = now;
        if (a2dConn) {
            Serial.printf("[♪] cb=%u frames=%u freq=%.0fHz heap=%u\n",
                cbCalls, cbFrames, currentFreq, ESP.getFreeHeap());
            if (cbCalls == 0) {
                unsigned long since = (now - connectTimeMs) / 1000;
                if (since == 6) Serial.println("[!] No audio callback");
                if (since == 12) Serial.println("[!] TWS refusing stream");
            } else {
                char buf[24];
                snprintf(buf, sizeof(buf), "%.0fHz cb%u", currentFreq, cbCalls);
                oledMsg("PLAYING", buf);
            }
        }
    }
    delay(20);
}
