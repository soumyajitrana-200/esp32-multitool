// ============================================================
//  SOUMYA BT Audio v5 — COMPLETE (works + TG gesture)
// ============================================================

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <math.h>
#include "esp32-hal-bt.h"

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

int16_t sineTable[256];
void initSineTable() {
    for (int i = 0; i < 256; i++)
        sineTable[i] = (int16_t)(sinf(i * 2.0f * PI / 256.0f) * 28000);
}

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

struct BtDevice { uint8_t mac[6]; char name[32]; };
#define MAX_DEVICES 20
BtDevice btDevices[MAX_DEVICES];
int btDeviceCount = 0;
bool a2dConn = false;
volatile bool requestStartStream = false;
volatile bool streamStarted = false;

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

// ═══ Audio callback ═══
int32_t a2d_data_cb(uint8_t *buf, int32_t len) {
    cbCalls++;
    cbFrames += (len / 4);
    if (!isPlaying) { memset(buf, 0, len); return len; }

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
        samples[i] = s;
        samples[i+1] = s;
    }
    return len;
}

// ═══ A2DP callback ═══
void a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT: {
            esp_a2d_connection_state_t st = param->conn_stat.state;
            if (st == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                a2dConn = true;
                connectTimeMs = millis();
                Serial.println("\n[A2DP] CONNECTED");
                oledMsg("A2DP Connected", "Start stream...");
                requestStartStream = true;    // ← KEY: trigger start
            } else if (st == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                a2dConn = false;
                streamStarted = false;
                requestStartStream = false;
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
                streamStarted = true;
                Serial.println("[A2DP] ★ AUDIO STREAM STARTED ★");
                oledMsg("STREAMING", "Playing tone");
            } else if (st == ESP_A2D_AUDIO_STATE_STOPPED) {
                streamStarted = false;
                Serial.println("[A2DP] Audio stopped");
            } else if (st == ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND) {
                streamStarted = false;
                Serial.println("[A2DP] Suspended by remote");
            }
            break;
        }
        case ESP_A2D_AUDIO_CFG_EVT:
            Serial.println("[A2DP] Codec configured");
            requestStartStream = true;       // ← also trigger here
            break;
        case ESP_A2D_MEDIA_CTRL_ACK_EVT:
            Serial.printf("[A2DP] Media ack: cmd=%d\n",
                          param->media_ctrl_stat.cmd);
            break;
        default: break;
    }
}

// ═══ AVRCP CT callback ═══
void avrc_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param) {
    switch (event) {
        case ESP_AVRC_CT_CONNECTION_STATE_EVT:
            Serial.printf("[AVRCP-CT] %s\n",
                param->conn_stat.connected ? "connected" : "disconnected");
            break;
        case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
            Serial.printf("[TWS] event=%d\n", param->change_ntf.event_id);
            break;
        default: break;
    }
}

// ═══ AVRCP TG callback (gesture) ═══
void avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param) {
    switch (event) {
        case ESP_AVRC_TG_CONNECTION_STATE_EVT:
            Serial.printf("[AVRCP-TG] %s\n",
                param->conn_stat.connected ? "connected" : "disconnected");
            break;
        case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT:
            Serial.printf("[TWS TAP] key=0x%02X state=0x%02X\n",
                param->psth_cmd.key_code,
                param->psth_cmd.key_state);
            break;
        case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
            Serial.printf("[TWS VOL] %d\n", param->set_abs_vol.volume);
            break;
        default: break;
    }
}

// ═══ GAP callback ═══
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

void printHelp() {
    Serial.println("\n── Commands ──");
    Serial.println("  scan          - Scan BT");
    Serial.println("  connect N     - Connect N");
    Serial.println("  start         - Force stream start");
    Serial.println("  play / pause  - Tone");
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

// ═══ Setup ═══
void setup() {
    Serial.begin(115200);
    delay(800);
    Serial.println("\n=== SOUMYA BT Audio v5 ===\n");

    Wire.begin(OLED_SDA, OLED_SCL);
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        Serial.println("[!] OLED FAIL");
    } else {
        oledMsg("BT Audio v5", "Init...");
    }

    initSineTable();
    Serial.println("[+] Sine table ready");

    Serial.println("[*] btStart()...");
    if (!btStart()) {
        Serial.println("[!] btStart failed — retry");
        delay(500);
        if (!btStart()) { Serial.println("[!] Abort"); return; }
    }
    delay(200);
    Serial.printf("[+] btStart OK — status: %d\n", esp_bt_controller_get_status());

    esp_bluedroid_status_t bd = esp_bluedroid_get_status();
    if (bd == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        Serial.printf("[*] bd_init: %d\n", esp_bluedroid_init());
        Serial.printf("[*] bd_enable: %d\n", esp_bluedroid_enable());
    } else if (bd == ESP_BLUEDROID_STATUS_INITIALIZED) {
        esp_bluedroid_enable();
    }
    Serial.println("[+] Bluedroid ready");

    esp_bt_dev_set_device_name("SOUMYA-Audio");
        // ═══ Set CoD as SMARTPHONE so TWS sends gestures ═══
    esp_bt_cod_t cod = {};
    cod.service = 0x4300;   // AUDIO | RENDERING | TELEPHONY
    cod.major = 0x02;       // PHONE (major device class)
    cod.minor = 0x04;       // SMART_PHONE
    esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_MAJOR_MINOR);
    Serial.println("[+] CoD set to SMARTPHONE (TWS gestures enabled)");
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

// ═══ Loop ═══
void loop() {
    // ═══ Handle stream start request ═══
    if (requestStartStream && a2dConn) {
        requestStartStream = false;
        delay(400);
        esp_err_t err = esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
        Serial.printf("[A2DP] Media START: %d (%s)\n",
                      err, esp_err_to_name(err));
        delay(100);
        esp_avrc_ct_send_passthrough_cmd(0,
            ESP_AVRC_PT_CMD_PLAY, ESP_AVRC_PT_CMD_STATE_PRESSED);
        delay(30);
        esp_avrc_ct_send_passthrough_cmd(0,
            ESP_AVRC_PT_CMD_PLAY, ESP_AVRC_PT_CMD_STATE_RELEASED);
        Serial.println("[AVRCP] PLAY sent");
    }

    // ═══ Retry if stream didn't start ═══
    static unsigned long lastRetry = 0;
    if (a2dConn && !streamStarted && millis() - connectTimeMs > 5000) {
        if (millis() - lastRetry > 5000) {
            lastRetry = millis();
            Serial.println("[A2DP] Retry stream...");
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
            esp_avrc_ct_send_passthrough_cmd(0,
                ESP_AVRC_PT_CMD_PLAY, ESP_AVRC_PT_CMD_STATE_PRESSED);
            delay(30);
            esp_avrc_ct_send_passthrough_cmd(0,
                ESP_AVRC_PT_CMD_PLAY, ESP_AVRC_PT_CMD_STATE_RELEASED);
        }
    }

    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim(); cmd.toLowerCase();
        if (cmd.length() == 0) { delay(20); return; }

        if (cmd == "help" || cmd == "?") printHelp();
        else if (cmd == "scan") startScan();
        else if (cmd.startsWith("connect ")) connectTo(cmd.substring(8).toInt());
        else if (cmd.startsWith("c ")) connectTo(cmd.substring(2).toInt());
        else if (cmd == "start") {
            Serial.println("[*] Force stream start...");
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
            esp_avrc_ct_send_passthrough_cmd(0,
                ESP_AVRC_PT_CMD_PLAY, ESP_AVRC_PT_CMD_STATE_PRESSED);
            delay(30);
            esp_avrc_ct_send_passthrough_cmd(0,
                ESP_AVRC_PT_CMD_PLAY, ESP_AVRC_PT_CMD_STATE_RELEASED);
        }
        else if (cmd == "play") { isPlaying = true; Serial.println("[*] PLAY"); }
        else if (cmd == "pause") { isPlaying = false; Serial.println("[*] PAUSE"); }
        else if (cmd == "status") {
            Serial.println("\n=== STATUS ===");
            Serial.printf("A2DP: %s\n", a2dConn ? "YES" : "no");
            Serial.printf("Stream: %s\n", streamStarted ? "STARTED" : "no");
            Serial.printf("CB: %u  Frames: %u\n", cbCalls, cbFrames);
            Serial.printf("Heap: %u\n\n", ESP.getFreeHeap());
        }
        else Serial.printf("[!] Unknown: %s\n", cmd.c_str());
    }

    unsigned long now = millis();
    if (now - lastReport >= 3000) {
        lastReport = now;
        if (a2dConn) {
            Serial.printf("[♪] cb=%u frames=%u freq=%.0fHz stream=%s\n",
                cbCalls, cbFrames, currentFreq,
                streamStarted ? "YES" : "no");
            if (cbCalls == 0) {
                unsigned long since = (now - connectTimeMs) / 1000;
                if (since == 8) Serial.println("[!] Auto retry...");
                if (since == 15) Serial.println("[!] TWS refuses stream");
            } else {
                char buf[24];
                snprintf(buf, sizeof(buf), "%.0fHz cb%u", currentFreq, cbCalls);
                oledMsg("PLAYING", buf);
            }
        }
    }
    delay(20);
}
