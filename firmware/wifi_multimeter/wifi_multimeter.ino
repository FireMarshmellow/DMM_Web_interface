#include <HardwareSerial.h>

#include <WiFi.h>

#include <WebServer.h>

#include <vector>

// ===== Wi-Fi config =====

static const char* WIFI_SSID = "";

static const char* WIFI_PASS = "";

static const char* WIFI_HOST = "multimeter-esp32";

// ===== UART config =====

static const int RX_PIN = 17;

static const int TX_PIN = -1;

static const uint32_t SNIFF_BAUD = 9600;   // set 38400 if needed

static const uint32_t GAP_MS     = 50;     // inter-byte idle gap to close a frame (↑ from 15)

// ===== Data-enable control =====

const int DATA_EN_PIN = 6;                 // GPIO that tells the meter to start sending

bool dataEnabled = false;                  // tracked mirror of DATA_EN_PIN

// Gate based on UART activity (leave enabled for 30 s after last byte)

unsigned long lastRxMs = 0;

const unsigned RX_IDLE_GRACE_MS = 30000;   // 30 s grace after last byte

// ===== Web server =====

WebServer server(80);

// ===== State =====

HardwareSerial SniffUart(1);

std::vector<uint8_t> frame;

uint32_t lastByteMs = 0;

String latest_value = "-";

String latest_units = "";

String latest_funcs = "";

String last_frame_hex = "";

String last_bits = "";

String last_bit_indices = "";

// ===== Utilities =====

String hexLine(const uint8_t* d, size_t n){

  static const char HEXCHARS[]="0123456789ABCDEF";

  String s; s.reserve(n*3);

  for(size_t i=0;i<n;i++){

    if(i) s+=' ';

    uint8_t b=d[i];

    s += HEXCHARS[b>>4]; s += HEXCHARS[b & 0x0F];

  }

  return s;

}

static inline uint8_t bitReverseByte(uint8_t x){

  x = (x>>4) | (x<<4);

  x = ((x & 0xCC)>>2) | ((x & 0x33)<<2);

  x = ((x & 0xAA)>>1) | ((x & 0x55)<<1);

  return x;

}

// 7-seg dictionary

char digitFrom7Seg(const String& sig){

  if(sig=="1110111") return '0';

  if(sig=="0010010") return '1';

  if(sig=="1011101") return '2';

  if(sig=="1011011") return '3';

  if(sig=="0111010") return '4';

  if(sig=="1101011") return '5';

  if(sig=="1101111") return '6';

  if(sig=="1010010") return '7';

  if(sig=="1111111") return '8';

  if(sig=="1111011") return '9';

  if(sig=="1111110") return 'A';

  if(sig=="0000111") return 'u';

  if(sig=="0101101") return 't';

  if(sig=="0001111") return 'o';

  if(sig=="0100101") return 'L';

  if(sig=="1101101") return 'E';

  if(sig=="1101100") return 'F';

  if(sig=="0001000") return '-';

  return '?';

}

String decodeValueFromPreparedBits(const String& bits){

  auto safeSlice = [&](int a,int b)->String{

    if(a<0 || b>(int)bits.length() || a>=b) return "";

    return bits.substring(a,b);

  };

  String out;

  if(bits.length()>28 && bits[28]=='1') out += '-';

  auto oneDigit = [&](int start)->char{

    String seg = safeSlice(start, start+8);

    if(seg.length()<8) return '?';

    String sig; sig.reserve(7);

    // signal = seg[3] seg[2] seg[7] seg[6] seg[1] seg[5] seg[4]

    sig += seg[3]; sig += seg[2]; sig += seg[7]; sig += seg[6];

    sig += seg[1]; sig += seg[5]; sig += seg[4];

    return digitFrom7Seg(sig);

  };

  if(bits.length() >= 36) out += oneDigit(28);

  if(bits.length() > 36 && bits[36]=='1') out += '.';

  if(bits.length() >= 44) out += oneDigit(36);

  if(bits.length() > 44 && bits[44]=='1') out += '.';

  if(bits.length() >= 52) out += oneDigit(44);

  if(bits.length() > 52 && bits[52]=='1') out += '.';

  if(bits.length() >= 60) out += oneDigit(52);

  if(out.length()==0) out = "0";

  return out;

}

// Basic units/flags (adjust indices if your model differs)

String decodeUnitsFlags(const String& bits){

  String fn, un;

  auto addIf = [&](int idx, const char* label, bool isFn=false){

    if(idx < (int)bits.length() && idx>=0 && bits[idx]=='1'){

      (isFn ? fn : un) += String(label) + " ";

    }

  };

  addIf(78, "DC");

  addIf(67, "AC");

  addIf(76, "V");

  addIf(79, "A");

  addIf(73, "Ω");

  addIf(72, "Hz");

  addIf(77, "m");

  addIf(80, "Auto");

  addIf(25, "HOLD", true);

  String out = un; out.trim();

  String f = fn;  f.trim();

  if(f.length()){ if(out.length()) out += "  "; out += f; }

  return out;

}

// Convert PLAIN UART frame -> prepared bits (bit-reverse each byte)

String makePreparedBits(const std::vector<uint8_t>& plain){

  String bits; bits.reserve(plain.size()*8);

  for(size_t i=0;i<plain.size();++i){

    uint8_t v = bitReverseByte(plain[i]);

    for(int b=7;b>=0;--b) bits += ((v>>b)&1)?'1':'0';

  }

  return bits;

}

// ===== Web page =====

const char INDEX_HTML[] PROGMEM = R"HTML(

<!doctype html><html lang="en"><head>

<meta charset="utf-8"/><meta name="viewport" content="width=device-width,initial-scale=1"/>

<title>Multimeter</title>

<style>

:root{--bg:#1a1d29;--fg:#fff;--mut:#94a3b8;--card:#2d3748;--grid:#4a5568;--vol:#00d4ff}

*{box-sizing:border-box;margin:0;padding:0}

body{background:var(--bg);color:var(--fg);font-family:JetBrains Mono,Consolas,monospace;margin:20px;font-weight:500}

.wrap{max-width:960px;margin:0 auto}

.header{text-align:center;margin-bottom:20px}

.header h1{font-size:2rem;letter-spacing:2px}

.readout{background:var(--card);border-radius:12px;padding:20px;margin:16px 0;text-align:center}

.value{font-size:clamp(4rem,12vw,8rem);font-weight:900;color:#10b981;text-shadow:0 3px 8px rgba(0,0,0,.5)}

.value small{font-size:.28em;color:var(--mut);margin-left:.3em}

.card{background:var(--card);border-radius:12px;padding:16px;margin:16px 0}

#chart{width:100%;height:320px;background:var(--bg);border:1px solid var(--grid);border-radius:12px}

.legend{display:flex;gap:12px;align-items:center;margin-bottom:8px;color:var(--mut)}

.dot{width:12px;height:12px;border-radius:50%;background:var(--vol)}

.status{color:var(--mut);font-size:.9rem;text-align:center}

</style></head><body><div class="wrap">

<header class="header"><h1>MULTIMETER</h1></header>

<section class="readout"><div class="value"><span id="val">—</span><small id="unit"></small></div></section>

<section class="card"><div class="legend"><span class="dot"></span><span>Live Measurement</span><span id="ymax" style="margin-left:auto"></span></div><canvas id="chart"></canvas></section>

<p class="status" id="status"></p>

</div>

<script>

const val=document.getElementById('val'), unit=document.getElementById('unit');

const chart=document.getElementById('chart'), ymax=document.getElementById('ymax'), status=document.getElementById('status');

let data=[], YMAX=1;

function fit(){ const dpr=window.devicePixelRatio||1; const r=chart.getBoundingClientRect(); chart.width=r.width*(dpr||1); chart.height=320*(dpr||1); }

function draw(){

  const dpr=window.devicePixelRatio||1, ctx=chart.getContext('2d'), W=chart.width, H=chart.height;

  ctx.fillStyle='#1a1d29'; ctx.fillRect(0,0,W,H);

  const L=50*dpr,R=20*dpr,T=15*dpr,B=35*dpr, w=W-L-R, h=H-T-B; ctx.save(); ctx.translate(L,T);

  ctx.strokeStyle='#374151'; ctx.beginPath(); for(let gy=0;gy<=6;gy++){ const y=h*gy/6; ctx.moveTo(0,y); ctx.lineTo(w,y);} ctx.stroke();

  ctx.fillStyle='#e2e8f0'; ctx.font=`bold ${14*dpr}px JetBrains Mono, Consolas, monospace`; ctx.textAlign='right';

  for(let gy=0;gy<=6;gy++){ const y=h*gy/6; const v=(YMAX*(1-gy/6)).toFixed(1); ctx.fillStyle='#00d4ff'; ctx.fillText(v, -8*dpr, y+5*dpr); }

  const now=Date.now()/1000, win=60, t0=now-win, mapX=t=> (t-t0)/win*w, mapY=y=> (1-Math.min(1,Math.max(0,y/YMAX)))*h;

  ctx.strokeStyle='#00d4ff'; ctx.lineWidth=Math.max(3*dpr,3); ctx.lineJoin='round'; ctx.lineCap='round'; ctx.beginPath(); let started=false;

  for(const p of data){ const x=mapX(p.t); if(x<0) continue; const y=mapY(p.y); if(!started){ctx.moveTo(x,y); started=true;} else ctx.lineTo(x,y); }

  ctx.stroke(); ctx.restore(); requestAnimationFrame(draw);

}

addEventListener('resize',fit); fit(); requestAnimationFrame(draw);

async function tick(){

  try{

    const r=await fetch('/api/latest',{cache:'no-store'}); if(!r.ok) throw 0;

    const j=await r.json();

    val.textContent=j.value ?? '—'; unit.textContent=j.unit?(' '+j.unit):'';

    const y=parseFloat(j.value); if(!isNaN(y)){ const now=Date.now()/1000, win=60, cut=now-win; data.push({t:now,y}); while(data.length&&data[0].t<cut) data.shift();

      let m=0; for(const p of data){ if(p.y>m) m=p.y; } const target=Math.max(1, Math.ceil(m*1.1*100)/100); YMAX += (target-YMAX)*0.18; ymax.textContent='Max: '+YMAX.toFixed(2)+' '+(j.unit||''); }

    status.textContent = j.functions || '';

  }catch(e){ /* ignore */ }

}

setInterval(tick, 333); tick();

</script>

</body></html>

)HTML";

// ===== HTTP handlers =====

void handleIndex(){

  server.send(200, "text/html; charset=utf-8", FPSTR(INDEX_HTML));

}

void handleApiLatest(){

  String json = "{\"value\":\"" + latest_value + "\",\"unit\":\"" + latest_units + "\",\"functions\":\"" + latest_funcs + "\"}";

  server.send(200, "application/json; charset=utf-8", json);
}

void handleApiDebug(){

  String json = "{";

  json += "\"frame_hex\":\"" + last_frame_hex + "\",";

  json += "\"bits\":\"" + last_bits + "\",";

  json += "\"bit_indices\":\"" + last_bit_indices + "\"";

  json += "}";

  server.send(200, "application/json; charset=utf-8", json);

}

// ===== Decode a finished frame =====

void decodeAndStore(const std::vector<uint8_t>& plain){

  if(plain.size() < 8) return;

  last_frame_hex = hexLine(plain.data(), plain.size());

  String bits = makePreparedBits(plain);

  last_bits = bits;

  String indices;

  for(int i=0; i<bits.length(); ++i){

    if(bits[i] == '1'){

      if(indices.length()) indices += ' ';

      indices += String(i);

    }

  }

  last_bit_indices = indices;

  latest_value = decodeValueFromPreparedBits(bits);

  latest_units = decodeUnitsFlags(bits);

  latest_funcs = ""; // extend if desired

}

// ===== Frame flush/parse =====

void flushFrame(){

  if(frame.empty()) return;

  // Optional: only accept frames that look real (start with 0x5A 0xA5)

  if(frame.size() >= 2 && frame[0] == 0x5A && frame[1] == 0xA5){

    decodeAndStore(frame);

  }

  frame.clear();

}

// ===== Wi-Fi helpers =====

wl_status_t lastWifiStatus = WL_DISCONNECTED;

unsigned long lastWifiCheckMs = 0;

uint8_t reconnectAttempts = 0;

void beginWifi(){

  WiFi.persistent(false);

  WiFi.mode(WIFI_STA);

  WiFi.setHostname(WIFI_HOST);

  WiFi.setSleep(false);                         // keep radio awake (stability)

  WiFi.setAutoReconnect(true);

  WiFi.setTxPower(WIFI_POWER_19_5dBm);          // max TX power for range

  // Optional static IP (uncomment & set if DHCP is slow on your router)

  // IPAddress ip(192,168,1,123), gw(192,168,1,1), sn(255,255,255,0), dns(1,1,1,1);

  // WiFi.config(ip, gw, sn, dns);

  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("WiFi connecting");

  uint32_t t0=millis();

  while(WiFi.status()!=WL_CONNECTED && millis()-t0<12000){

    delay(250); Serial.print(".");

  }

  Serial.println();

  if(WiFi.status()==WL_CONNECTED){

    Serial.print("WiFi OK  IP: "); Serial.println(WiFi.localIP());

    reconnectAttempts = 0;

  }else{

    Serial.println("WiFi not connected (will keep retrying).");

  }

  lastWifiStatus = WiFi.status();

}

void wifiWatchdog(){

  unsigned long now = millis();

  if(now - lastWifiCheckMs < 2000) return;  // check every 2s

  lastWifiCheckMs = now;

  wl_status_t st = WiFi.status();

  if(st == WL_CONNECTED){

    if(lastWifiStatus != WL_CONNECTED){

      Serial.print("WiFi reconnected. IP: "); Serial.println(WiFi.localIP());

    }

    reconnectAttempts = 0;

  }else{

    static unsigned long lastKick = 0;

    unsigned long backoff = 1000UL * (2 + min<uint8_t>(reconnectAttempts, 7) * 2);

    if(now - lastKick >= backoff){

      Serial.println("WiFi down → reconnect()");

      WiFi.reconnect();

      reconnectAttempts++;

      lastKick = now;

    }

  }

  lastWifiStatus = st;

}

void setup(){

  Serial.begin(115200);

  delay(50);

  // Keep meter enabled by default

  pinMode(DATA_EN_PIN, OUTPUT);

  digitalWrite(DATA_EN_PIN, HIGH);

  dataEnabled = true;

  // Initialize timestamps so gating doesn't immediately disable at boot

  unsigned long now = millis();

  lastRxMs   = now;

  lastByteMs = now;

  beginWifi();

  // Web server

  server.on("/", handleIndex);

  server.on("/api/latest", handleApiLatest);

  server.on("/api/debug", handleApiDebug);

  server.begin();

  Serial.println("HTTP server started");

  // UART from the meter

  SniffUart.begin(SNIFF_BAUD, SERIAL_8N1, RX_PIN, TX_PIN, /*invert=*/false);

  Serial.println("UART sniffer started (GPIO17 @ 9600 8N1)");

}

void loop(){

  // Keep Wi-Fi alive

  wifiWatchdog();

  // Serve HTTP

  server.handleClient();

  // Read UART and frame by gap

  while(SniffUart.available()>0){

    int v = SniffUart.read();

    if(v<0) break;

    frame.push_back((uint8_t)v);

    lastByteMs = millis();

    lastRxMs   = lastByteMs;                 // record meter activity

    if(frame.size()>512){ flushFrame(); }    // safety

  }

  if(!frame.empty() && (millis()-lastByteMs)>=GAP_MS){

    flushFrame();

  }

  // ---- Activity-based gating for DATA_EN (optional) ----

  const bool meterShouldBeOn = (millis() - lastRxMs) <= RX_IDLE_GRACE_MS;

  if (meterShouldBeOn && !dataEnabled) {

    digitalWrite(DATA_EN_PIN, HIGH);

    dataEnabled = true;

  } else if (!meterShouldBeOn && dataEnabled) {

    digitalWrite(DATA_EN_PIN, LOW);

    dataEnabled = false;

  }

  // Yield to TCP/IP stack

  delay(1);

}

