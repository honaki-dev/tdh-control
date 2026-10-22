#define BLYNK_TEMPLATE_ID "TMPL6dyUvImk7"
#define BLYNK_TEMPLATE_NAME "tdh"
#define BLYNK_AUTH_TOKEN "FxhPhssyhJORQbqxoaIohhtBOvTNGh5X"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ESPmDNS.h>
#include <BlynkSimpleEsp32.h>

#define ROOM_COUNTS 4

#define TOUCH_RESET_PIN 27
#define TOUCH_THRESHOLD 30
#define TOUCH_RESET_TIME 3000

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

const uint8_t leds[ROOM_COUNTS] = {19, 21, 22, 23};
const uint8_t btns[ROOM_COUNTS] = {4, 16, 17, 18};
const char *roomNames[ROOM_COUNTS] = {"Phong khach", "Phong bep", "Nha ve sinh", "Phong ngu"};

uint8_t lastState[ROOM_COUNTS] = {HIGH, HIGH, HIGH, HIGH};
unsigned long pressedTime[ROOM_COUNTS] = {0, 0, 0, 0};
bool toggle[ROOM_COUNTS] = {false, false, false, false};
bool isPressing[ROOM_COUNTS] = {false, false, false, false};
bool longPressed[ROOM_COUNTS] = {false, false, false, false};
bool blinkMode = false;

bool blinkLeftState = false;
bool blinkRightState = false;

WiFiManager wm;
WebServer server(8080);
WebSocketsServer webSocket = WebSocketsServer(81);
bool webServerStarted = false;

TaskHandle_t blynkTaskHandle = NULL;
SemaphoreHandle_t blynkMutex = NULL;

void buildStatusJson(char *buf, size_t bufSize);
void broadcastWebSocketStatus();
volatile bool wsNeedBroadcast = false;

void syncToBlynk(int pin, int val)
{
  if (blynkMutex != NULL && Blynk.connected())
  {
    if (xSemaphoreTake(blynkMutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
      Blynk.virtualWrite(pin, val);
      xSemaphoreGive(blynkMutex);
    }
  }
}

void setBlinkMode(bool enable, bool syncBlynk = true)
{
  blinkMode = enable;
  blinkLeftState = false;
  blinkRightState = false;

  if (!blinkMode)
  {
    for (int i = 0; i < ROOM_COUNTS; i++)
    {
      digitalWrite(leds[i], toggle[i] ? HIGH : LOW);
      if (syncBlynk)
      {
        syncToBlynk(i, toggle[i] ? 1 : 0);
      }
    }
  }
  if (syncBlynk)
  {
    syncToBlynk(V4, blinkMode ? 1 : 0);
  }

  wsNeedBroadcast = true;
}

void setLedToggle(int i)
{
  if (i < 0 || i >= ROOM_COUNTS)
    return;
  if (blinkMode)
    return;
  toggle[i] = !toggle[i];
  digitalWrite(leds[i], toggle[i] ? HIGH : LOW);

  syncToBlynk(i, toggle[i] ? 1 : 0);
  wsNeedBroadcast = true;
  Serial.printf("Toggle room %d -> LED (pin %d) = %d\n", i, leds[i], toggle[i] ? 1 : 0);
}

void setLedState(int i, bool state, bool syncBlynk = true)
{
  if (i < 0 || i >= ROOM_COUNTS)
    return;
  if (blinkMode)
    return;
  toggle[i] = state;
  digitalWrite(leds[i], toggle[i] ? HIGH : LOW);

  if (syncBlynk)
  {
    syncToBlynk(i, toggle[i] ? 1 : 0);
  }
  wsNeedBroadcast = true;
  Serial.printf("LED %d (pin %d) = %d\n", i, leds[i], toggle[i] ? 1 : 0);
}

BLYNK_WRITE(V0) { setLedState(0, param.asInt() != 0, false); }
BLYNK_WRITE(V1) { setLedState(1, param.asInt() != 0, false); }
BLYNK_WRITE(V2) { setLedState(2, param.asInt() != 0, false); }
BLYNK_WRITE(V3) { setLedState(3, param.asInt() != 0, false); }
BLYNK_WRITE(V4)
{
  int val = param.asInt();
  Serial.printf("Blynk V4 (Alert Mode): %d\n", val);
  setBlinkMode(val != 0, false);
}

BLYNK_CONNECTED()
{
  Serial.println("Blynk connected! Syncing real LED states...");
  for (int i = 0; i < ROOM_COUNTS; i++)
  {
    Blynk.virtualWrite(i, toggle[i] ? 1 : 0);
  }
  Blynk.virtualWrite(V4, blinkMode ? 1 : 0);
}

void blynkTask(void *pvParameters)
{
  for (;;)
  {
    if (WiFi.status() == WL_CONNECTED)
    {
      if (Blynk.connected())
      {
        if (xSemaphoreTake(blynkMutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
          Blynk.run();
          xSemaphoreGive(blynkMutex);
        }
      }
      else
      {
        static unsigned long lastBlynkAttempt = 0;
        if (millis() - lastBlynkAttempt >= 10000)
        {
          lastBlynkAttempt = millis();
          Serial.println("Reconnecting to Blynk (Core 0)...");
          Blynk.connect(1000);
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(25));
  }
}

void _blinkMode()
{
  unsigned long now = millis() % 1000;

  bool left = (now < 50) || (now >= 100 && now < 150);
  bool right = (now >= 250 && now < 300) || (now >= 350 && now < 400);
  if ((now >= 500 && now < 550) || (now >= 600 && now < 650) || (now >= 700 && now < 750))
  {
    left = right = true;
  }

  if (left != blinkLeftState)
  {
    digitalWrite(leds[0], left ? HIGH : LOW);
    digitalWrite(leds[1], left ? HIGH : LOW);
    blinkLeftState = left;
  }
  if (right != blinkRightState)
  {
    digitalWrite(leds[2], right ? HIGH : LOW);
    digitalWrite(leds[3], right ? HIGH : LOW);
    blinkRightState = right;
  }
}

void buildStatusJson(char *buf, size_t bufSize)
{
  String ipStr;
  if (WiFi.status() == WL_CONNECTED)
  {
    ipStr = WiFi.localIP().toString();
  }
  else
  {
    ipStr = WiFi.softAPIP().toString();
  }
  ipStr += ":8080";

  snprintf(buf, bufSize,
           "{\"leds\":[%d,%d,%d,%d],\"blink\":%d,\"wifi\":%d,\"blynk\":%d,\"ip\":\"%s\"}",
           toggle[0] ? 1 : 0,
           toggle[1] ? 1 : 0,
           toggle[2] ? 1 : 0,
           toggle[3] ? 1 : 0,
           blinkMode ? 1 : 0,
           (WiFi.status() == WL_CONNECTED) ? 1 : 0,
           Blynk.connected() ? 1 : 0,
           ipStr.c_str());
}

void broadcastWebSocketStatus()
{
  char json[192];
  buildStatusJson(json, sizeof(json));
  webSocket.broadcastTXT(json);
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length)
{
  switch (type)
  {
  case WStype_DISCONNECTED:
    Serial.printf("[WS %u] Client disconnected\n", num);
    break;

  case WStype_CONNECTED:
  {
    IPAddress ip = webSocket.remoteIP(num);
    Serial.printf("[WS %u] Client connected from %s\n", num, ip.toString().c_str());
    char json[192];
    buildStatusJson(json, sizeof(json));
    webSocket.sendTXT(num, json);
    break;
  }

  case WStype_TEXT:
  {
    String text;
    text.concat((const char *)payload, length);

    if (text.startsWith("TOGGLE:"))
    {
      int room = text.substring(7).toInt();
      setLedToggle(room);
    }
    else if (text.startsWith("SET:"))
    {
      int firstColon = text.indexOf(':');
      int secondColon = text.indexOf(':', firstColon + 1);
      if (secondColon > 0)
      {
        int room = text.substring(firstColon + 1, secondColon).toInt();
        int val = text.substring(secondColon + 1).toInt();
        setLedState(room, val != 0);
      }
    }
    else if (text == "ALL:1")
    {
      for (int i = 0; i < ROOM_COUNTS; i++)
        setLedState(i, true);
    }
    else if (text == "ALL:0")
    {
      for (int i = 0; i < ROOM_COUNTS; i++)
        setLedState(i, false);
    }
    else if (text == "BLINK:TOGGLE")
    {
      setBlinkMode(!blinkMode);
    }
    else if (text == "GET_STATUS")
    {
      char json[192];
      buildStatusJson(json, sizeof(json));
      webSocket.sendTXT(num, json);
    }
    break;
  }

  default:
    break;
  }
}

const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="vi">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>TDH Control</title>
<link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'><text y='.9em' font-size='90'>🐧</text></svg>">
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,sans-serif;background:#0b1120;color:#f1f5f9;min-height:100vh;display:flex;flex-direction:column;align-items:center}
.header{background:linear-gradient(135deg,#0f172a,#1e293b);width:100%;padding:18px 16px;text-align:center;border-bottom:1px solid #334155;box-shadow:0 4px 20px rgba(0,0,0,0.4)}
.header-top{display:flex;align-items:center;justify-content:center;gap:10px}
.header h1{font-size:1.4rem;font-weight:700;color:#38bdf8;letter-spacing:0.5px}
.status-bar{display:flex;gap:8px;justify-content:center;margin-top:10px;flex-wrap:wrap}
.status-pill{font-size:0.72rem;padding:3px 10px;border-radius:20px;background:#1e293b;border:1px solid #334155;font-weight:600;display:flex;align-items:center;gap:6px;transition:all .3s}
.status-dot{width:7px;height:7px;border-radius:50%;background:#64748b}
.status-pill.on .status-dot{background:#22c55e;box-shadow:0 0 8px #22c55e}
.status-pill.off .status-dot{background:#ef4444}
.status-pill.on{border-color:rgba(34,197,94,0.4);color:#86efac}
.status-pill.off{border-color:rgba(239,68,68,0.4);color:#fca5a5}
.status-pill.ws-on{border-color:#38bdf8;color:#38bdf8;background:rgba(56,189,248,0.1)}
.status-pill.ws-on .status-dot{background:#38bdf8;box-shadow:0 0 8px #38bdf8}

.container{max-width:480px;width:100%;padding:16px;flex:1}

.quick-actions{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:14px}
.action-btn{background:#1e293b;border:1px solid #334155;color:#cbd5e1;padding:10px;border-radius:12px;font-size:0.82rem;font-weight:600;cursor:pointer;display:flex;align-items:center;justify-content:center;gap:6px;transition:all .2s}
.action-btn:hover{background:#334155;color:#fff}
.action-btn:active{transform:scale(0.97)}

.card{background:#131d31;border-radius:16px;padding:14px 16px;margin-bottom:12px;border:1px solid #1e293b;transition:all .25s ease;display:flex;align-items:center;justify-content:space-between}
.card.is-on{background:linear-gradient(135deg,#132238,#162a45);border-color:#0284c7;box-shadow:0 4px 20px rgba(2,132,199,0.2)}
.card.disabled{opacity:0.4;pointer-events:none}
.room-info{display:flex;align-items:center;gap:14px}
.room-icon{width:44px;height:44px;border-radius:12px;display:flex;align-items:center;justify-content:center;font-size:1.4rem;background:#0b1120;border:1px solid #1e293b;transition:all .3s}
.card.is-on .room-icon{background:#0369a1;border-color:#38bdf8;box-shadow:0 0 12px rgba(56,189,248,0.4)}
.room-meta{display:flex;flex-direction:column;gap:2px}
.room-name{font-weight:600;font-size:0.95rem;color:#f8fafc}
.room-desc{font-size:0.75rem;color:#64748b;display:flex;align-items:center;gap:6px}
.room-desc .state-text{font-weight:600}
.card.is-on .room-desc .state-text{color:#38bdf8}

.toggle{position:relative;width:52px;height:28px;cursor:pointer;flex-shrink:0}
.toggle input{position:absolute;opacity:0;width:0;height:0;pointer-events:none;margin:0}
.slider{position:absolute;inset:0;background:#334155;border-radius:14px;transition:.25s ease;box-shadow:inset 0 1px 3px rgba(0,0,0,0.4)}
.slider:before{content:'';position:absolute;height:22px;width:22px;left:3px;bottom:3px;background:#94a3b8;border-radius:50%;transition:.25s cubic-bezier(0.4,0,0.2,1);box-shadow:0 2px 4px rgba(0,0,0,0.3)}
input:checked+.slider{background:#0284c7}
input:checked+.slider:before{transform:translateX(24px);background:#fff}

.blink-card{background:linear-gradient(135deg,#450a0a,#7f1d1d);border:1px solid #991b1b;border-radius:16px;padding:12px;margin-top:16px;box-shadow:0 4px 16px rgba(185,28,28,0.2)}
.blink-card.active{animation:pulse-border 1.2s infinite;background:linear-gradient(135deg,#7f1d1d,#b91c1c)}
@keyframes pulse-border{0%,100%{border-color:#ef4444;box-shadow:0 0 15px rgba(239,68,68,0.5)}50%{border-color:#f87171;box-shadow:0 0 5px rgba(239,68,68,0.2)}}
.blink-btn{width:100%;padding:14px;border:none;border-radius:12px;font-size:0.95rem;font-weight:700;cursor:pointer;transition:all .2s;text-transform:uppercase;letter-spacing:0.8px;display:flex;align-items:center;justify-content:center;gap:8px}
.blink-btn.off{background:#dc2626;color:#fff}
.blink-btn.off:hover{background:#ef4444}
.blink-btn.on{background:#fef2f2;color:#991b1b;animation:pulse-bg 1s infinite}
@keyframes pulse-bg{0%,100%{opacity:1}50%{opacity:0.85}}

.conn-banner{display:none;background:#7f1d1d;color:#fecaca;padding:8px 12px;border-radius:10px;font-size:0.75rem;text-align:center;margin-bottom:12px;font-weight:600;border:1px solid #b91c1c}
.conn-banner.show{display:block}

.footer{text-align:center;padding:16px;color:#475569;font-size:0.75rem}
.footer a{color:#64748b;text-decoration:none}
.footer a:hover{color:#94a3b8}
</style>
</head>
<body>
<div class="header">
  <div class="header-top">
    <h1>🐧 TDH Control</h1>
  </div>
  <div class="status-bar">
    <div class="status-pill ws-on" id="conn-pill"><span id="conn-text">Ok</span></div>
    <div class="status-pill" id="wifi-pill"><span id="wifi-text">WiFi: --</span></div>
    <div class="status-pill" id="blynk-pill"><span id="blynk-text">Blynk: --</span></div>
    <div class="status-pill" id="ip-pill"><span id="ip-text">IP: --</span></div>
  </div>
</div>

<div class="container">
  <div class="conn-banner" id="conn-alert">Mất kết nối với thiết bị, đang thử lại...</div>

  <div class="quick-actions">
    <button class="action-btn" onclick="setAll(true)">💡 Bật tất cả</button>
    <button class="action-btn" onclick="setAll(false)">🌑 Tắt tất cả</button>
  </div>

  <div id="rooms-list"></div>

  <div class="blink-card" id="blink-wrap">
    <button class="blink-btn off" id="blink-button" onclick="togBlink()">
      <span>🔔</span> Bật chế độ cảnh báo
    </button>
  </div>
</div>

<div class="footer">Dev: <a href="https://fb.com/honaki10" target="_blank">Tran Hoang</a></div>

<script>
const rooms=["Phòng khách","Phòng bếp","Nhà vệ sinh","Phòng ngủ"];
const icons=["🛋️","🍳","🚽","🛏️"];
const pins=[19,21,22,23];
let state={leds:[0,0,0,0],blink:0,wifi:0,blynk:0,ip:""};
let isInit=false;
let ws = null;
let wsConnected = false;

function buildUI(){
  const wrap=document.getElementById('rooms-list');
  let h='';
  for(let i=0;i<4;i++){
    h+=`
    <div class="card" id="card-${i}">
      <div class="room-info">
        <div class="room-icon">${icons[i]}</div>
        <div class="room-meta">
          <div class="room-name">${rooms[i]}</div>
          <div class="room-desc">
            <span>GPIO ${pins[i]}</span>
            <span class="state-text" id="state-text-${i}">Off</span>
          </div>
        </div>
      </div>
      <label class="toggle">
        <input type="checkbox" id="chk-${i}" onchange="tog(${i})">
        <span class="slider"></span>
      </label>
    </div>`;
  }
  wrap.innerHTML=h;
  isInit=true;
}

function updateUI(){
  if(!isInit) buildUI();
  if(!state || !Array.isArray(state.leds)) return;

  for(let i=0;i<4;i++){
    const card=document.getElementById('card-'+i);
    const chk=document.getElementById('chk-'+i);
    const txt=document.getElementById('state-text-'+i);
    const isOn=Boolean(state.leds[i]);

    if(chk){
      chk.checked=isOn;
      chk.disabled=Boolean(state.blink);
    }
    if(txt){
      txt.textContent=state.blink?'🔒':(isOn?'On':'Off');
    }
    if(card){
      card.classList.toggle('disabled', Boolean(state.blink));
      card.classList.toggle('is-on', isOn && !state.blink);
    }
  }

  const bw=document.getElementById('blink-wrap');
  const btn=document.getElementById('blink-button');
  if(bw && btn){
    if(state.blink){
      bw.classList.add('active');
      btn.className='blink-btn on';
      btn.innerHTML='<span>🚨</span> CẢNH BÁO ĐANG BẬT';
    } else {
      bw.classList.remove('active');
      btn.className='blink-btn off';
      btn.innerHTML='<span>🔔</span> Bật chế độ cảnh báo';
    }
  }

  const wp=document.getElementById('wifi-pill');
  if(wp){
    wp.className='status-pill '+(state.wifi?'on':'off');
    document.getElementById('wifi-text').textContent='WiFi: '+(state.wifi?'OK':'OFF');
  }

  const bp=document.getElementById('blynk-pill');
  if(bp){
    bp.className='status-pill '+(state.blynk?'on':'off');
    document.getElementById('blynk-text').textContent='Blynk: '+(state.blynk?'OK':'OFF');
  }

  const ipEl=document.getElementById('ip-text');
  if(ipEl) ipEl.textContent='IP: '+(state.ip||'--');
}

function setConnBadge(online){
  const cp=document.getElementById('conn-pill');
  const ct=document.getElementById('conn-text');
  const ca=document.getElementById('conn-alert');
  if(online){
    if(cp) cp.className='status-pill ws-on';
    if(ct) ct.textContent='Ok';
    if(ca) ca.classList.remove('show');
  } else {
    if(cp) cp.className='status-pill off';
    if(ct) ct.textContent='Mất kết nối';
    if(ca) ca.classList.add('show');
  }
}

function initWebSocket(){
  const host = window.location.hostname || '192.168.4.1';
  const wsUrl = 'ws://' + host + ':81';
  console.log('Connecting to WebSocket:', wsUrl);

  try {
    ws = new WebSocket(wsUrl);

    ws.onopen = function(){
      console.log('WebSocket Connected!');
      wsConnected = true;
      setConnBadge(true);
    };

    ws.onmessage = function(event){
      try {
        state = JSON.parse(event.data);
        updateUI();
      } catch(e){
        console.error('WS Parse Error:', e);
      }
    };

    ws.onclose = function(){
      console.log('WebSocket closed, reconnecting in 2s...');
      wsConnected = false;
      setConnBadge(false);
      setTimeout(initWebSocket, 2000);
    };

    ws.onerror = function(err){
      console.error('WebSocket Error:', err);
      ws.close();
    };
  } catch(e) {
    console.error('WS Init Exception:', e);
    setTimeout(initWebSocket, 2000);
  }
}

function tog(i){
  if(state.blink) return;
  if(ws && ws.readyState === WebSocket.OPEN){
    ws.send('TOGGLE:' + i);
  } else {
    fetch('/api/toggle/' + i).catch(()=>{});
  }
}

function setAll(on){
  if(state.blink) return;
  if(ws && ws.readyState === WebSocket.OPEN){
    ws.send(on ? 'ALL:1' : 'ALL:0');
  } else {
    fetch(on ? '/api/all/on' : '/api/all/off').catch(()=>{});
  }
}

function togBlink(){
  if(ws && ws.readyState === WebSocket.OPEN){
    ws.send('BLINK:TOGGLE');
  } else {
    fetch('/api/blink').catch(()=>{});
  }
}

buildUI();
initWebSocket();

document.addEventListener('visibilitychange', () => {
  if (!document.hidden && (!ws || ws.readyState !== WebSocket.OPEN)) {
    fetch('/api/status').then(r=>r.json()).then(d=>{state=d;updateUI();}).catch(()=>{});
  }
});
</script>
</body>
</html>
)rawliteral";

void sendStatusJson()
{
  char json[192];
  buildStatusJson(json, sizeof(json));
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

void setupWebServer()
{
  server.on("/", []()
            { server.send_P(200, "text/html", HTML_PAGE); });

  server.on("/api/status", []()
            { sendStatusJson(); });

  server.on("/api/blink", []()
            {
    setBlinkMode(!blinkMode);
    Serial.printf("Web: blink mode %s\n", blinkMode ? "ON" : "OFF");
    sendStatusJson(); });

  server.on("/api/all/on", []()
            {
    for (int i = 0; i < ROOM_COUNTS; i++)
    {
      setLedState(i, true);
    }
    Serial.println("Web: turn ALL ON");
    sendStatusJson(); });

  server.on("/api/all/off", []()
            {
    for (int i = 0; i < ROOM_COUNTS; i++)
    {
      setLedState(i, false);
    }
    Serial.println("Web: turn ALL OFF");
    sendStatusJson(); });

  for (int i = 0; i < ROOM_COUNTS; i++)
  {
    String path = "/api/toggle/" + String(i);
    server.on(path.c_str(), [i]()
              {
      setLedToggle(i);
      Serial.printf("Web: toggle room %d\n", i);
      sendStatusJson(); });

    String onPath = "/api/set/" + String(i) + "/on";
    server.on(onPath.c_str(), [i]()
              {
      setLedState(i, true);
      sendStatusJson(); });

    String offPath = "/api/set/" + String(i) + "/off";
    server.on(offPath.c_str(), [i]()
              {
      setLedState(i, false);
      sendStatusJson(); });
  }

  server.begin();
  webServerStarted = true;
  Serial.println("Web server started on port 8080");
  Serial.println("Access URL:");
  Serial.printf(" - STA Mode: http://%s:8080 or http://tdh.local:8080\n", WiFi.localIP().toString().c_str());
  Serial.printf(" - AP  Mode: http://%s:8080\n", WiFi.softAPIP().toString().c_str());

  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  Serial.println("WebSocket Server started on port 81 (Real-time mode active!)");
}

String getDeviceName()
{
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  String uniqueID = mac.substring(mac.length() - 6);
  return "TDH-Device-" + uniqueID;
}

void ensureAPIsRunning()
{
  String deviceName = getDeviceName();
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(deviceName.c_str(), "mot-hai-ba", 1, 0, 10);
  Serial.printf("AP Mode active! SSID: %s (Pass: mot-hai-ba) - Max 10 clients\n", deviceName.c_str());
  Serial.printf("AP IP Address: http://%s:8080\n", WiFi.softAPIP().toString().c_str());
}

void setupWiFiAndBlynk()
{
  WiFi.mode(WIFI_AP_STA);
  delay(100);

  wm.setConfigPortalBlocking(false);
  wm.setConnectTimeout(5);

  String deviceName = getDeviceName();
  WiFi.setHostname(deviceName.c_str());

  Serial.println("Starting WiFiManager (non-blocking)...");
  wm.autoConnect(deviceName.c_str(), "mot-hai-ba");

  ensureAPIsRunning();

  Blynk.config(BLYNK_AUTH_TOKEN);

  blynkMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(
      blynkTask,
      "blynkTask",
      4096,
      NULL,
      1,
      &blynkTaskHandle,
      0
  );
}

void handleTouchReset()
{
  if (millis() < 2000)
    return;

  int touchVal = 0;
  for (int s = 0; s < 4; s++)
  {
    touchVal += touchRead(TOUCH_RESET_PIN);
    delayMicroseconds(250);
  }
  touchVal /= 4;

  static unsigned long touchStartTime = 0;
  static bool isTouching = false;

  if (touchVal < TOUCH_THRESHOLD)
  {
    if (!isTouching)
    {
      isTouching = true;
      touchStartTime = millis();
      Serial.printf("Touch detected on GPIO 27 (val: %d). Giu 3s de reset WiFi...\n", touchVal);
    }
    else if (millis() - touchStartTime >= TOUCH_RESET_TIME)
    {
      Serial.println("Resetting WiFi credentials and restarting...");
      for (int k = 0; k < 6; k++)
      {
        digitalWrite(LED_BUILTIN, HIGH);
        delay(80);
        digitalWrite(LED_BUILTIN, LOW);
        delay(80);
      }
      wm.resetSettings();
      delay(500);
      ESP.restart();
    }
  }
  else
  {
    isTouching = false;
  }
}

void setup()
{
  Serial.begin(115200);

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  for (int i = 0; i < ROOM_COUNTS; i++)
  {
    pinMode(leds[i], OUTPUT);
    digitalWrite(leds[i], LOW);
    pinMode(btns[i], INPUT_PULLUP);
    lastState[i] = digitalRead(btns[i]);
  }

  setupWiFiAndBlynk();
  setupWebServer();
}

void loop()
{
  wm.process();

  handleTouchReset();

  bool wifiConnected = (WiFi.status() == WL_CONNECTED);
  static bool lastWifiState = false;
  if (wifiConnected != lastWifiState)
  {
    lastWifiState = wifiConnected;
    digitalWrite(LED_BUILTIN, wifiConnected ? HIGH : LOW);
    Serial.println(wifiConnected ? "WiFi connected!" : "WiFi disconnected!");

    if (wifiConnected)
    {
      MDNS.end();
      if (MDNS.begin("tdh"))
      {
        MDNS.addService("http", "tcp", 8080);
        Serial.println("mDNS: http://tdh.local:8080");
      }
      Serial.printf("Web Server IP (STA): http://%s:8080\n", WiFi.localIP().toString().c_str());
    }
    else
    {
      MDNS.end();
    }
  }

  server.handleClient();
  webSocket.loop();

  if (wsNeedBroadcast)
  {
    wsNeedBroadcast = false;
    broadcastWebSocketStatus();
  }

  if (blinkMode)
  {
    _blinkMode();
  }

  for (int i = 0; i < ROOM_COUNTS; i++)
  {
    int reading = digitalRead(btns[i]);

    if (lastState[i] == HIGH && reading == LOW)
    {
      isPressing[i] = true;
      longPressed[i] = false;
      pressedTime[i] = millis();
    }

    if (isPressing[i] && !longPressed[i])
    {
      if (millis() - pressedTime[i] >= 3000)
      {
        longPressed[i] = true;
        setBlinkMode(!blinkMode);
        Serial.println(blinkMode ? "long: blink mode ON" : "long: blink mode OFF");
      }
    }

    if (lastState[i] == LOW && reading == HIGH)
    {
      if (isPressing[i])
      {
        if (!longPressed[i] && (millis() - pressedTime[i] >= 50))
        {
          setLedToggle(i);
        }
        isPressing[i] = false;
      }
      longPressed[i] = false;
    }

    lastState[i] = reading;
  }
}