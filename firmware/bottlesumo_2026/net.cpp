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
input,select{font:inherit;background:#0d1016;color:var(--ink);border:1px solid var(--line);border-radius:6px;padding:5px;width:100%}
.hint{color:var(--muted);font-size:12px;margin:6px 0 0}table{width:100%;border-collapse:collapse}td,th{text-align:left;padding:3px 0;font-size:13px}
th{color:var(--muted);font-weight:500}.edge{display:inline-block;padding:2px 7px;border-radius:999px;border:1px solid var(--line);color:var(--muted);margin:0 4px 4px 0;font-size:12px}
.edge.hit{background:#3a2323;border-color:#5c2f2f;color:var(--err)}
</style></head><body>
<header><h1>BottleSumo 2026</h1><span id="state">--</span><span class="tag" id="div"></span>
<span class="tag" id="event"></span><span class="tag grow" id="net"></span><span class="tag" id="tofstate"></span></header>
<main>
<section><h2>Sensor</h2><div id="ir"></div><div class="row"><span>ToF</span><b id="tof">--</b></div>
<div class="row"><span>Edge bits</span><span id="edges"></span></div>
<p class="hint">IR 數值 0–4095。紅色代表已確認出界。</p></section>
<section><h2>控制</h2><div class="grid2">
<button onclick="cmd('/api/start?a=1')">Start A</button><button onclick="cmd('/api/start?a=2')">Start B</button>
<button class="stop" onclick="cmd('/api/stop')">Stop</button><button id="dbg" onclick="toggleDebug()">Debug</button></div>
<p class="hint" id="calprog">校準：--</p>
<div class="grid2"><button onclick="cmd('/api/calibrate?mode=floor')">Cal floor</button>
<button onclick="cmd('/api/calibrate?mode=void')">Cal void</button></div>
<p class="hint">3 秒。校準期間摩打停。</p></section>
<section><h2>Debug 遙控</h2><div class="grid3">
<button data-l="60" data-r="60">前</button><button data-l="0" data-r="0">停</button><button data-l="-60" data-r="-60">後</button>
<button data-l="-60" data-r="60">左轉</button><button></button><button data-l="60" data-r="-60">右轉</button></div>
<p class="hint">按實先會行；放手或 500 ms 無指令自動停。先將車放上支架。</p></section>
<section><h2>Threshold</h2><div id="thr"></div><p class="hint">未校準或想手動覆寫時使用；極性要同實物一致。</p></section>
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
<select id="thp${i}"><option value="1">high</option><option value="0">low</option></select>
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
 document.getElementById('edges').textContent='0b'+s.edges.toString(2).padStart(4,'0');
 document.getElementById('net').textContent=(s.ip||'AP')+(s.rssi?(' · '+s.rssi+' dBm'):'');
 const bits=s.edges;document.getElementById('edges').innerHTML=CH.map((n,i)=>
  `<span class="edge ${bits>>i&1?'hit':''}">${n}</span>`).join('');
 CH.forEach((n,i)=>{const v=s.ir[i];document.getElementById('irv'+i).textContent=v;
  const b=document.getElementById('irb'+i);b.firstChild.style.width=(v/4095*100)+'%';
  b.classList.toggle('hot',!!(bits>>i&1));
  document.getElementById('thv'+i).textContent=s.thr[i]?s.thr[i]:'--';});
 document.getElementById('calprog').textContent=s.calProg>=0?('校準中 '+s.calProg+'%'):(s.calibrated?'已校準':'未校準（只警告）');
 debug=s.state=='debug';document.getElementById('dbg').classList.toggle('on',debug);
 document.getElementById('cus').textContent=s.ctrlUs+' µs';document.getElementById('cmiss').textContent=s.ctrlMiss;
 document.getElementById('tus').textContent=s.tofUs+' µs';document.getElementById('tmiss').textContent=s.tofMiss;
 document.getElementById('nus').textContent=s.netUs+' µs';document.getElementById('nmiss').textContent=s.netMiss;
}catch(e){document.getElementById('state').textContent='offline'}}
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
  j += "],\"tof\":" + String(s.tofMm);
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
  if (ch < 0 || ch > 3 || value < 1 || value > 4094) {
    server.send(400, "application/json", "{\"ok\":0,\"error\":\"range\"}");
    return;
  }
  Command c;
  c.kind = CommandKind::Manual;
  c.channel = uint8_t(ch);
  c.value = uint16_t(value);
  c.high = server.arg("high") != "0";
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
