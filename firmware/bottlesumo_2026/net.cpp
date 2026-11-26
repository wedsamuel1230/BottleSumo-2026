// net.cpp — AP+STA Wi-Fi, Arduino OTA and the bench page.
//
// This task never touches the motors. The one exception is the OTA inhibit
// flag: an upload stops the robot before the first flash write.
#include "net.h"

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "config.h"
#include "control.h"
#include "timing.h"
#include "wifi_defaults.h"

namespace bs {
namespace {

WebServer server(80);
bool mdnsStarted = false;
uint32_t lastWifiTry = 0;
bool uploadAccepted = false;

const char PAGE[] PROGMEM = R"PAGE(<!doctype html><html lang="zh-Hant"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>BottleSumo 2026</title>
<style>
:root{--bg:#0f1115;--card:#171a21;--ink:#e8ebf0;--muted:#8b93a3;--line:#262b36;--ok:#46d18a;--warn:#f0b429;--err:#f0564a}
*{box-sizing:border-box}body{margin:0;font:14px/1.45 -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;background:var(--bg);color:var(--ink)}
header{display:flex;gap:12px;align-items:center;padding:14px 18px;border-bottom:1px solid var(--line);flex-wrap:wrap}
h1{font-size:15px;margin:0;font-weight:600}#state{font-weight:700;color:var(--ok);text-transform:uppercase;letter-spacing:.06em}
.tag{color:var(--muted);font-size:12px}.grow{margin-left:auto}
main{display:grid;grid-template-columns:repeat(auto-fit,minmax(290px,1fr));gap:14px;padding:14px}
section{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px}
h2{font-size:12px;letter-spacing:.08em;text-transform:uppercase;color:var(--muted);margin:0 0 10px}
.bar{height:8px;background:#242936;border-radius:4px;overflow:hidden;margin:4px 0 2px}
.bar i{display:block;height:100%;background:var(--ok);width:0}
.bar.hot i{background:var(--err)}.row{display:flex;justify-content:space-between;gap:8px;font-variant-numeric:tabular-nums}
.ch{margin-bottom:10px}.mono{font-family:ui-monospace,SFMono-Regular,Menlo,monospace}
button{font:inherit;border:1px solid var(--line);background:#1d222c;color:var(--ink);padding:7px 10px;border-radius:7px;cursor:pointer}
button.on{background:#233a2e;border-color:#2f5c45;color:var(--ok)}button.stop{background:#3a2323;border-color:#5c2f2f;color:var(--err)}
button:active{transform:translateY(1px)}.grid2{display:grid;grid-template-columns:1fr 1fr;gap:8px}.grid3{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}
input,select,textarea{font:inherit;background:#0d1016;color:var(--ink);border:1px solid var(--line);border-radius:6px;padding:5px;width:100%}
textarea{min-height:88px;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;resize:vertical}
.hint{color:var(--muted);font-size:12px;margin:6px 0 0}table{width:100%;border-collapse:collapse}td,th{text-align:left;padding:3px 0;font-size:13px}
th{color:var(--muted);font-weight:500}.edge{display:inline-block;padding:2px 7px;border-radius:999px;border:1px solid var(--line);color:var(--muted);margin:0 4px 4px 0;font-size:12px}
.edge.hit{background:#3a2323;border-color:#5c2f2f;color:var(--err)}
</style></head><body>
<header><h1>BottleSumo 2026</h1><span id="state">--</span><span class="tag" id="div"></span>
<span class="tag" id="event"></span><span class="tag grow" id="net"></span><span class="tag" id="tofstate"></span></header>
<main>
<section><h2>Sensor</h2><div id="ir"></div><div class="row"><span>ToF</span><b id="tof">--</b></div>
<div class="row"><span>Edge bits</span><span id="edges"></span></div>
<div class="row"><span>前進</span><b id="fwd">--</b></div>
<p class="hint">IR 數值 0–4095。紅色代表已確認出界。bit3..0 = RR RL FR FL；A0=RL、A1=FL、A2=FR、A3=RR。前進 cm 為 100% 等效估算。</p></section>
<section><h2>控制</h2><div class="grid2">
<button onclick="cmd('/api/start?a=1')">Start A</button><button onclick="cmd('/api/start?a=2')">Start B</button>
<button class="stop" onclick="cmd('/api/stop')">Stop</button><button id="dbg" onclick="toggleDebug()">Debug</button></div>
<p class="hint" id="calprog">校準：--</p>
<div class="grid2"><button onclick="cmd('/api/calibrate?mode=floor')">Cal floor</button>
<button onclick="cmd('/api/calibrate?mode=void')">Cal void</button></div>
<p class="hint">3 秒。校準期間摩打停。</p></section>
<section><h2>Debug 遙控</h2><div class="grid3">
<button data-l="100" data-r="100">前</button><button data-l="0" data-r="0">停</button><button data-l="-100" data-r="-100">後</button>
<button data-l="-100" data-r="100">左轉</button><button data-l="100" data-r="-100">右轉</button></div>
<p class="hint">前後左右全部 100%。按實先會行；放手或 500 ms 無指令自動停。先將車放上支架。</p></section>
<section><h2>Threshold</h2><div id="thr"></div><p class="hint">電壓低於閾值 = bit 1（出界）。先看 Auto 的極性，再決定手動值；閾值要落在地板與落差兩個讀數之間。</p></section>
<section><h2>Route A / B</h2><div class="grid2">
<div><b>A</b><textarea id="ra" rows="4" spellcheck="false"></textarea>
<div class="grid2"><button onclick="saveRoute('A')">Save A</button><button onclick="resetRoute('A')">Default A</button></div></div>
<div><b>B</b><textarea id="rb" rows="4" spellcheck="false"></textarea>
<div class="grid2"><button onclick="saveRoute('B')">Save B</button><button onclick="resetRoute('B')">Default B</button></div></div>
</div><p class="hint">每行 left,right,ms。duty -100..100，時間 10..10000 ms。第一段固定 100/100 全速前進，只改時間；之後的段用 left/right。只可在 Idle / Debug / RoundEnd 儲存。</p></section>
<section><h2>OTA</h2><form id="ota"><input type="file" name="firmware" accept=".bin">
<button type="submit">上載 firmware.bin</button></form><p class="hint" id="otamsg">只可在 Idle 狀態更新。首次要用 USB。</p></section>
<section><h2>Timing</h2><table><tr><th>task</th><th>max</th><th>miss</th></tr>
<tr><td>control</td><td id="cus" class="mono">--</td><td id="cmiss" class="mono">--</td></tr>
<tr><td>tof</td><td id="tus" class="mono">--</td><td id="tmiss" class="mono">--</td></tr>
<tr><td>net</td><td id="nus" class="mono">--</td><td id="nmiss" class="mono">--</td></tr></table></section>
</main>
<script>
const CH=['FL','FR','RL','RR'];let debug=false,drive=null;
async function cmd(u){await fetch(u,{method:'POST'})}
function el(t,c){const e=document.createElement(t);if(c)e.className=c;return e}
document.getElementById('ir').innerHTML=CH.map((n,i)=>`<div class="ch"><div class="row"><span>${n}</span><b id="irv${i}" class="mono">--</b></div><div class="bar" id="irb${i}"><i></i></div></div>`).join('');
document.getElementById('thr').innerHTML=CH.map((n,i)=>`<div class="ch"><div class="row"><span>${n}</span><span id="thv${i}" class="mono">--</span></div>
<div class="grid3"><input id="thi${i}" type="number" min="0" max="4095" placeholder="value">
<select id="thp${i}"><option value="0">低於閾值 (bit=1)</option><option value="1">高於閾值 (bit=1)</option></select>
<button onclick="setThr(${i})">Set</button></div>
<button onclick="cmd('/api/threshold/clear?ch=${i}')">Auto</button></div>`).join('');
function setThr(i){const v=document.getElementById('thi'+i).value;const h=document.getElementById('thp'+i).value;
cmd(`/api/threshold?ch=${i}&value=${v}&high=${h}`)}
document.querySelectorAll('[data-l]').forEach(b=>{
 const send=()=>{if(!debug)return;fetch(`/api/debug/drive?l=${b.dataset.l}&r=${b.dataset.r}`,{method:'POST'})};
 b.addEventListener('pointerdown',e=>{e.preventDefault();b.setPointerCapture(e.pointerId);send();drive=setInterval(send,120)});
 const stop=()=>{if(drive){clearInterval(drive);drive=null}if(debug)cmd('/api/debug/drive?l=0&r=0')};
 b.addEventListener('pointerup',stop);b.addEventListener('pointercancel',stop);b.addEventListener('pointerleave',stop);
});
document.getElementById('ota').addEventListener('submit',async e=>{e.preventDefault();
 const f=e.target.firmware.files[0];if(!f)return;const fd=new FormData();fd.append('firmware',f);
 document.getElementById('otamsg').textContent='上載中…';
 const r=await fetch('/api/update',{method:'POST',body:fd});
 document.getElementById('otamsg').textContent=r.ok?'完成，重啟中':'失敗：'+(await r.text())});
function toggleDebug(){cmd(debug?'/api/debug?on=0':'/api/debug?on=1')}
async function poll(){try{const s=await(await fetch('/api/status')).json();
 document.getElementById('state').textContent=s.state;
 document.getElementById('div').textContent=s.division&&s.division!='-'?('Division '+s.division):'';
 document.getElementById('event').textContent=s.event||'';
 document.getElementById('tof').textContent=s.tofFresh?(s.tof+' mm'):'--';
 document.getElementById('tofstate').textContent=s.tofOk?'ToF ok':'ToF fail';
 document.getElementById('fwd').textContent=(s.fwdMs/1000).toFixed(1)+' s · '+s.fwdCm+' cm';
 document.getElementById('net').textContent=(s.ip||'AP')+(s.rssi?(' · '+s.rssi+' dBm'):'');
 const bits=s.edges;document.getElementById('edges').innerHTML=CH.map((n,i)=>
  `<span class="edge ${bits>>i&1?'hit':''}">${n}</span>`).join('');
 CH.forEach((n,i)=>{const v=s.ir[i];document.getElementById('irv'+i).textContent=v;
  const b=document.getElementById('irb'+i);b.firstChild.style.width=(v/4095*100)+'%';
  b.classList.toggle('hot',!!(bits>>i&1));
  const man=(s.man>>i)&1,hi=(s.manHigh>>i)&1;
  const auto=s.thrAuto[i]?(' | auto '+s.thrAuto[i]):'';
  document.getElementById('thv'+i).textContent=(s.thr[i]?s.thr[i]:'--')+(man?(hi?' high':' low'):' auto')+auto;});
 document.getElementById('calprog').textContent=s.calProg>=0?('校準中 '+s.calProg+'%'):(s.calibrated?'已校準':'未校準（只警告）');
 debug=s.state=='debug';document.getElementById('dbg').classList.toggle('on',debug);
 document.getElementById('cus').textContent=s.ctrlUs+' µs';document.getElementById('cmiss').textContent=s.ctrlMiss;
 document.getElementById('tus').textContent=s.tofUs+' µs';document.getElementById('tmiss').textContent=s.tofMiss;
 document.getElementById('nus').textContent=s.netUs+' µs';document.getElementById('nmiss').textContent=s.netMiss;
}catch(e){document.getElementById('state').textContent='offline'}}
function routeText(list){return (list||[]).map(s=>s.l+','+s.r+','+s.ms).join('\n')}
async function loadRoute(){try{const r=await(await fetch('/api/route')).json();
 document.getElementById('ra').value=routeText(r.A);
 document.getElementById('rb').value=routeText(r.B);}catch(e){}}
async function saveRoute(d){const ta=document.getElementById('r'+d.toLowerCase());
 const rows=ta.value.split('\n').map(s=>s.trim()).filter(Boolean);
 if(!rows.length){alert('至少一段');return}
 const q=new URLSearchParams({div:d,count:rows.length});
 for(let i=0;i<rows.length;i++){const p=rows[i].split(',').map(Number);
  if(p.length!==3||p.some(x=>!Number.isInteger(x))){alert('格式：left,right,ms（整數）');return}
  q.set('l'+i,p[0]);q.set('r'+i,p[1]);q.set('ms'+i,p[2]);}
 const res=await fetch('/api/route?'+q,{method:'POST'});
 if(!res.ok){alert('儲存失敗：'+await res.text());return}
 await new Promise(r=>setTimeout(r,150));
 await loadRoute();}
async function resetRoute(d){if(!confirm('重設 Route '+d+'？'))return;
 const res=await fetch('/api/route/reset?div='+d,{method:'POST'});
 if(!res.ok){alert('重設失敗：'+await res.text());return}
 await new Promise(r=>setTimeout(r,150));
 await loadRoute();}
loadRoute();
setInterval(poll,400);poll();
</script></body></html>)PAGE";

void sendJson(const String &body) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", body);
}

void handleStatus() {
  const Snapshot s = controlSnapshot();
  String j = "{";
  j += "\"version\":\"" + String(VERSION) + "\"";
  j += ",\"state\":\"" + String(stateName(s.state)) + "\"";
  j += ",\"division\":\"" + String(divisionName(s.division)) + "\"";
  j += ",\"stateMs\":" + String(s.stateMs);
  j += ",\"roundMs\":" + String(s.roundMs);
  j += ",\"edges\":" + String(s.edges);
  j += ",\"ir\":[";
  j += String(s.ir[0]) + "," + String(s.ir[1]) + "," + String(s.ir[2]) + "," + String(s.ir[3]);
  j += "],\"thr\":[";
  j += String(s.threshold[0]) + "," + String(s.threshold[1]) + "," +
       String(s.threshold[2]) + "," + String(s.threshold[3]);
  j += "],\"thrAuto\":[";
  j += String(s.autoThreshold[0]) + "," + String(s.autoThreshold[1]) + "," +
       String(s.autoThreshold[2]) + "," + String(s.autoThreshold[3]);
  j += "],\"man\":" + String(s.manualMask) + ",\"manHigh\":" + String(s.manualHigh);
  j += ",\"fwdMs\":" + String(s.forwardMs) + ",\"fwdCm\":" + String(s.forwardCm);
  j += ",\"tof\":" + String(s.tofMm);
  j += ",\"tofFresh\":" + String(s.tofFresh ? 1 : 0);
  j += ",\"tofOk\":" + String(s.tofOk ? 1 : 0);
  j += ",\"calibrated\":" + String(s.calibrated ? 1 : 0);
  j += ",\"calProg\":" + String(int(s.calibrationProgress));
  j += ",\"event\":\"" + String(s.event) + "\"";
  j += ",\"ctrlUs\":" + String(s.controlMaxUs) + ",\"ctrlMiss\":" + String(s.controlMisses);
  j += ",\"tofUs\":" + String(g_tofTiming.maxUs) + ",\"tofMiss\":" + String(g_tofTiming.misses);
  j += ",\"netUs\":" + String(g_netTiming.maxUs) + ",\"netMiss\":" + String(g_netTiming.misses);
  if (WiFi.status() == WL_CONNECTED) {
    j += ",\"ip\":\"" + WiFi.localIP().toString() + "\",\"rssi\":" + String(WiFi.RSSI());
  } else {
    j += ",\"ip\":\"" + WiFi.softAPIP().toString() + "\",\"rssi\":0";
  }
  j += ",\"clients\":" + String(WiFi.softAPgetStationNum());
  j += "}";
  sendJson(j);
}

void post(const Command &command) { controlPost(command); }

void handleStart() {
  Command c;
  c.kind = server.arg("a") == "1" ? CommandKind::StartA : CommandKind::StartB;
  post(c);
  sendJson("{\"ok\":1}");
}

void handleStop() {
  Command c;
  c.kind = CommandKind::Stop;
  post(c);
  sendJson("{\"ok\":1}");
}

void handleDebug() {
  Command c;
  c.kind = server.arg("on") == "1" ? CommandKind::DebugEnter : CommandKind::DebugExit;
  post(c);
  sendJson("{\"ok\":1}");
}

void handleDrive() {
  Command c;
  c.kind = CommandKind::Drive;
  c.left = int8_t(constrain(server.arg("l").toInt(), -100, 100));
  c.right = int8_t(constrain(server.arg("r").toInt(), -100, 100));
  post(c);
  sendJson("{\"ok\":1}");
}

void handleCalibrate() {
  Command c;
  c.kind = server.arg("mode") == "void" ? CommandKind::CalibrateVoid
                                        : CommandKind::CalibrateFloor;
  post(c);
  sendJson("{\"ok\":1}");
}

void handleThreshold() {
  const long ch = server.arg("ch").toInt();
  const long value = server.arg("value").toInt();
  const String high = server.arg("high");
  if (ch < 0 || ch > 3 || value < 1 || value > 4094) {
    server.send(400, "application/json", "{\"ok\":0,\"error\":\"range\"}");
    return;
  }
  // No silent default: the polarity is the difference between "table" and
  // "void". A missing argument used to become "high" and latch false edges.
  if (high != "0" && high != "1") {
    server.send(400, "application/json", "{\"ok\":0,\"error\":\"polarity\"}");
    return;
  }
  Command c;
  c.kind = CommandKind::Manual;
  c.channel = uint8_t(ch);
  c.value = uint16_t(value);
  c.high = high == "1";
  post(c);
  sendJson("{\"ok\":1}");
}

void handleThresholdClear() {
  const long ch = server.arg("ch").toInt();
  if (ch < 0 || ch > 3) {
    server.send(400, "application/json", "{\"ok\":0,\"error\":\"channel\"}");
    return;
  }
  Command c;
  c.kind = CommandKind::ClearManual;
  c.channel = uint8_t(ch);
  post(c);
  sendJson("{\"ok\":1}");
}

bool routeEditable(const Snapshot &s) {
  return s.state == State::Idle || s.state == State::Debug ||
         s.state == State::RoundEnd || s.state == State::Init;
}

void handleRouteGet() {
  const Snapshot s = controlSnapshot();
  String j = "{";
  for (uint8_t d = 0; d < 2; ++d) {
    if (d) j += ",";
    j += d == 0 ? "\"A\":[" : "\"B\":[";
    const RouteConfig &r = s.routes[d];
    for (uint8_t i = 0; i < r.count; ++i) {
      if (i) j += ",";
      j += "{\"l\":" + String(int(r.seg[i].left));
      j += ",\"r\":" + String(int(r.seg[i].right));
      j += ",\"ms\":" + String(r.seg[i].ms) + "}";
    }
    j += "]";
  }
  j += "}";
  sendJson(j);
}

void handleRouteSet() {
  const Snapshot s = controlSnapshot();
  if (!routeEditable(s)) {
    server.send(409, "application/json", "{\"ok\":0,\"error\":\"state\"}");
    return;
  }
  const String div = server.arg("div");
  const long count = server.arg("count").toInt();
  if ((div != "A" && div != "B") || count < 1 || count > ROUTE_MAX_SEGMENTS) {
    server.send(400, "application/json", "{\"ok\":0,\"error\":\"range\"}");
    return;
  }
  Command c{};
  c.kind = CommandKind::RouteSet;
  c.routeDiv = div == "B" ? 1 : 0;
  c.route.count = uint8_t(count);
  for (long i = 0; i < count; ++i) {
    const long l = server.arg(String("l") + String(i)).toInt();
    const long r = server.arg(String("r") + String(i)).toInt();
    const long ms = server.arg(String("ms") + String(i)).toInt();
    if (l < -100 || l > 100 || r < -100 || r > 100 || ms < 10 || ms > 10000) {
      server.send(400, "application/json", "{\"ok\":0,\"error\":\"segment\"}");
      return;
    }
    c.route.seg[i] = {int8_t(l), int8_t(r), uint16_t(ms)};
  }
  post(c);
  sendJson("{\"ok\":1}");
}

void handleRouteReset() {
  const Snapshot s = controlSnapshot();
  if (!routeEditable(s)) {
    server.send(409, "application/json", "{\"ok\":0,\"error\":\"state\"}");
    return;
  }
  const String div = server.arg("div");
  if (div != "A" && div != "B") {
    server.send(400, "application/json", "{\"ok\":0,\"error\":\"division\"}");
    return;
  }
  Command c{};
  c.kind = CommandKind::RouteReset;
  c.routeDiv = div == "B" ? 1 : 0;
  post(c);
  sendJson("{\"ok\":1}");
}

// ---------------------------------------------------------------------------
// OTA
// ---------------------------------------------------------------------------
void uploadHandler() {
  HTTPUpload &u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    uploadAccepted = controlSnapshot().state == State::Idle;
    if (!uploadAccepted) return;
    controlSetInhibited(true);
    if (!Update.begin(u.totalSize, U_FLASH)) {
      uploadAccepted = false;
      Serial.printf("web OTA begin failed: %s\n", Update.errorString());
    }
    return;
  }
  if (u.status == UPLOAD_FILE_WRITE && uploadAccepted) {
    if (Update.write(u.buf, u.currentSize) != u.currentSize) uploadAccepted = false;
    return;
  }
  if (u.status == UPLOAD_FILE_END && uploadAccepted) {
    uploadAccepted = Update.end(true);
  }
}

void uploadDone() {
  if (!uploadAccepted) {
    controlSetInhibited(false);
    server.send(409, "text/plain", "只可在 Idle 狀態更新");
    return;
  }
  server.send(200, "text/plain", "ok");
  delay(200);
  ESP.restart();
}

void beginWifi() {
  const bool sta = WIFI_SSID[0] != '\0';
  WiFi.setAutoReconnect(sta);
  WiFi.mode(sta ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  WiFi.softAPsetHostname("bottlesumo");
  if (sta) {
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("STA joining \"%s\"\n", WIFI_SSID);
  }
  if (!mdnsStarted && MDNS.begin("bottlesumo")) {
    MDNS.addService("http", "tcp", 80);
    mdnsStarted = true;
  }
  Serial.printf("AP %s  http://bottlesumo/\n", WiFi.softAPIP().toString().c_str());
}

void maintainWifi(uint32_t now) {
  if (WIFI_SSID[0] == '\0' || WiFi.status() == WL_CONNECTED) return;
  if (now - lastWifiTry < 10000) return;
  lastWifiTry = now;
  WiFi.reconnect();
}

} // namespace

void netBegin() {
  beginWifi();

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  if (OTA_PASSWORD[0] != '\0') ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([] {
    controlSetInhibited(true);
    Serial.println("OTA: upload starting");
  });
  ArduinoOTA.onEnd([] { Serial.println("OTA: done"); });
  ArduinoOTA.onError([](ota_error_t error) {
    controlSetInhibited(false);
    Serial.printf("OTA: error %u\n", unsigned(error));
  });
  ArduinoOTA.begin();

  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html", PAGE); });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/start", HTTP_POST, handleStart);
  server.on("/api/stop", HTTP_POST, handleStop);
  server.on("/api/debug", HTTP_POST, handleDebug);
  server.on("/api/debug/drive", HTTP_POST, handleDrive);
  server.on("/api/calibrate", HTTP_POST, handleCalibrate);
  server.on("/api/threshold", HTTP_POST, handleThreshold);
  server.on("/api/threshold/clear", HTTP_POST, handleThresholdClear);
  server.on("/api/route", HTTP_GET, handleRouteGet);
  server.on("/api/route", HTTP_POST, handleRouteSet);
  server.on("/api/route/reset", HTTP_POST, handleRouteReset);
  server.on("/api/update", HTTP_POST, uploadDone, uploadHandler);
  server.begin();
  Serial.println("web ready");
}

void netTask(void *arg) {
  (void)arg;
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    const uint32_t start = micros();
    ArduinoOTA.handle();
    server.handleClient();
    maintainWifi(millis());
    timingRecord(g_netTiming, micros() - start, 5000);
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(5));
  }
}

} // namespace bs
