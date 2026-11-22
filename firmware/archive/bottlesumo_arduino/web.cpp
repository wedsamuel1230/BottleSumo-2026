// web.cpp — Wi-Fi AP, configuration web UI, OTA upload and UDP telemetry.
//
// Safety rules enforced here and in the control task:
//   * Tuning and calibration writes are only accepted while the robot is
//     stopped and no round is running.
//   * OTA is only accepted while the robot is stopped, the OTA window is armed,
//     and the control task has confirmed the motors are at zero.
//   * Handlers never block on motor or sensor work; they only post requests.
#include "web.h"
#include "wifi_defaults.h"
#include <WebServer.h>
#include <Wire.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Update.h>
#include <Preferences.h>
#include <esp_ota_ops.h>

namespace BS {

WebServer server(80);
WiFiUDP udp;

// ---------------------------------------------------------------------------
// Wi-Fi configuration, persisted in NVS. Never compiled into an OTA image.
// ---------------------------------------------------------------------------
struct WifiConfig {
  uint32_t magic = WIFI_MAGIC;
  char apPassword[64] = "bottlesumo";   // change from the web UI
  char staSsid[33] = "";
  char staPassword[64] = "";
  uint8_t channel = 6;
};
static WifiConfig wifiConfig;

static bool loadWifiConfig() {
  bool haveRecord = false;
  Preferences prefs;
  if (prefs.begin("bottlesumo", true)) {
    WifiConfig loaded{};
    const size_t bytes = prefs.getBytes("wifi", &loaded, sizeof(loaded));
    prefs.end();
    if (bytes == sizeof(loaded) && loaded.magic == WIFI_MAGIC &&
        strlen(loaded.apPassword) >= 8 && strlen(loaded.apPassword) <= 63) {
      wifiConfig = loaded;
      haveRecord = true;
    }
  }
  if (!haveRecord) {
    // First boot (or an old record): seed from wifi_local.h, the same header
    // movement_test uses, and keep the AP password from that header too.
    wifiConfig.magic = WIFI_MAGIC;
    WiFi.mode(WIFI_AP);
    snprintf(wifiConfig.apPassword, sizeof(wifiConfig.apPassword), "%s", AP_PASSWORD);
    snprintf(wifiConfig.staSsid, sizeof(wifiConfig.staSsid), "%s", WIFI_SSID);
    snprintf(wifiConfig.staPassword, sizeof(wifiConfig.staPassword), "%s", WIFI_PASSWORD);
  } else if (wifiConfig.staSsid[0] == '\0' && WIFI_SSID[0] != '\0') {
    // A saved AP-only config should still pick up credentials from the header.
    snprintf(wifiConfig.staSsid, sizeof(wifiConfig.staSsid), "%s", WIFI_SSID);
    snprintf(wifiConfig.staPassword, sizeof(wifiConfig.staPassword), "%s", WIFI_PASSWORD);
  }
  return true;
}

static bool saveWifiConfig() {
  Preferences prefs;
  if (!prefs.begin("bottlesumo", false)) return false;
  const size_t written = prefs.putBytes("wifi", &wifiConfig, sizeof(wifiConfig));
  prefs.end();
  return written == sizeof(wifiConfig);
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static void sendJson(const String &json) {
  server.send(200, "application/json", json);
}

static void sendReject(int code, const char *message) {
  server.send(code, "application/json", String("{\"error\":\"") + message + "\"}");
}

static bool robotStopped() {
  Telemetry t{};
  if (!xQueuePeek(telemetryQueue, &t, 0)) return false;
  return t.state == 0;   // State::Stopped
}

// Display-state code → human-readable name, matching the desktop dashboard.
static const char *stateNameFromDisplay(uint8_t s) {
  switch (s) {
    case DISPLAY_TARGET_SEEN:    return "target seen";
    case DISPLAY_SEARCH_TURN:    return "search: turn";
    case DISPLAY_SEARCH_OBSERVE: return "search: observe";
    case DISPLAY_SEARCH_ADVANCE: return "search: advance";
    case DISPLAY_BENCH_IDLE:     return "bench: idle";
    case DISPLAY_BENCH_DRIVE:    return "bench: drive";
    case DISPLAY_CAL_FLOOR:      return "calibrate: floor";
    case DISPLAY_CAL_VOID:       return "calibrate: void";
    case DISPLAY_UPDATING:       return "ota updating";
    case DISPLAY_OPENING:        return "opening run";
    case DISPLAY_CARRY:          return "carry";
    case 25:                     return "fault";
    default: break;
  }
  if (s <= 8) return stateName(State(s));
  return "?";
}

static String flagText(uint8_t flags) {
  String out;
  if (flags & Fault) out += "FAULT ";
  if (flags & Calibrated) out += "CAL ";
  if (flags & HardwareConfirmed) out += "HW ";
  if (flags & Updating) out += "OTA ";
  if (flags & Bench) out += "BENCH ";
  if (flags & OtaWindow) out += "OTA-WIN ";
  return out.length() ? out : String("-");
}

// ---------------------------------------------------------------------------
// HTML page
// ---------------------------------------------------------------------------
static const char indexHtml[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="zh-Hant"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>BottleSumo</title>
<style>
:root{--bg:#0f1115;--surface:#15181e;--line:#242830;--fg:#e6e8eb;--dim:#98a1ac;--acc:#4c8dff;--ok:#2f9e6b;--warn:#c98a2b;--bad:#c04040}
*{box-sizing:border-box}html{color-scheme:dark}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.45 -apple-system,system-ui,"Segoe UI","Noto Sans TC",sans-serif;-webkit-font-smoothing:antialiased}
header{position:sticky;top:0;z-index:5;display:flex;align-items:center;gap:10px;flex-wrap:wrap;padding:10px 14px;background:var(--bg);border-bottom:1px solid var(--line)}
.brand{display:flex;align-items:center;gap:8px;margin-right:auto}
.brand b{font-size:15px;letter-spacing:.01em}
.pill{font-size:12px;padding:3px 9px;border-radius:6px;border:1px solid var(--line);color:var(--dim)}
.pill.on{color:#dff3e7;border-color:var(--ok)}
.pill.bad{color:#ffd6d6;border-color:var(--bad)}
.chips{display:flex;gap:6px;flex-wrap:wrap}
.meta{display:flex;gap:10px;font-size:12px;color:var(--dim);width:100%}
.chip{font-size:12px;color:var(--dim);border:1px solid var(--line);border-radius:6px;padding:2px 7px}
.chip.ok{color:#bfe8d2;border-color:#2b6b4c}.chip.bad{color:#ffc9c9;border-color:#7a2b2b}.chip.warn{color:#f0d3a0;border-color:#6e5320}
main{max-width:760px;margin:0 auto;padding:12px 14px 40px;display:flex;flex-direction:column;gap:12px}
section{background:var(--surface);border:1px solid var(--line);border-radius:10px;padding:14px}
h2{font-size:13px;font-weight:600;margin:0 0 10px;color:var(--fg)}
.line{display:flex;gap:12px;flex-wrap:wrap;color:var(--dim);font-size:13px;margin-bottom:10px}
.line b{color:var(--fg);font-weight:600}
.bigbtns{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin:8px 0}
button{font:inherit;font-size:15px;min-height:48px;border-radius:10px;border:1px solid var(--line);background:#1c2027;color:var(--fg);cursor:pointer;padding:10px 14px}
button:active{background:#242a33}
button:disabled{opacity:.45;cursor:not-allowed}
button small{display:block;font-size:11px;color:var(--dim);margin-top:1px}
button.start{background:#1d6b47;border-color:#2a8a5d}
button.stop{background:#7d2626;border-color:#a03434}
button.primary{background:#2b4f86;border-color:#3a68ab}
button.wide{width:100%}
.ready{margin:10px 0 4px;padding:10px 12px;border-radius:8px;border:1px solid var(--line);font-size:14px;color:var(--dim)}
.ready.ok{border-color:#2b6b4c;color:#bfe8d2;background:#13251c}
.ready.bad{border-color:#7a2b2b;color:#ffc9c9;background:#241414}
.hint{color:var(--dim);font-size:12px;margin:8px 0 0}
.row{display:flex;justify-content:space-between;gap:8px;padding:5px 0;border-bottom:1px solid #1d2128;font-size:13px}
.row:last-child{border-bottom:0}.k{color:var(--dim)}.v{font-variant-numeric:tabular-nums;text-align:right}
.bar{height:6px;background:#0b0e12;border-radius:3px;position:relative;overflow:hidden;margin:3px 0 6px}
.bar>i{position:absolute;left:0;top:0;bottom:0;background:var(--acc);display:block}
.bar>e{position:absolute;top:0;bottom:0;width:2px;background:var(--warn);display:block}
.flag{font-size:11px;color:var(--dim);margin-left:6px;border:1px solid var(--line);border-radius:4px;padding:0 4px}
.flag.on{color:#ffd0d0;border-color:#7a2b2b}
.tofList{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-top:8px}
.tof{background:#0b0e12;border:1px solid var(--line);border-radius:8px;padding:8px;text-align:center}
.tof span{display:block;font-size:11px;color:var(--dim)}
.tof b{display:block;font-size:18px;margin-top:2px;font-variant-numeric:tabular-nums}
.tof em{display:block;font-style:normal;font-size:10px;color:#6b7480;margin-top:2px}
table{width:100%;border-collapse:collapse;font-size:12px}
td,th{padding:5px 4px;border-bottom:1px solid #1d2128;text-align:left}
th{color:var(--dim);font-weight:600}
.ok{color:#86d3aa}.warn{color:#e0b26a}.bad{color:#f09595}
.badge{font-size:11px;color:var(--dim);border:1px solid var(--line);border-radius:4px;padding:0 5px;margin-left:6px}
input[type=number],input[type=password],input[type=text],select{background:#0b0e12;border:1px solid var(--line);color:var(--fg);border-radius:6px;padding:5px 6px;font:inherit}
input[type=range]{width:100%;accent-color:var(--acc);margin:4px 0 8px}
label{display:block;color:var(--dim);font-size:12px;margin-top:8px}
.btnrow{display:flex;gap:8px;flex-wrap:wrap;margin-top:8px}
.btnrow button{flex:1 1 auto}
details{border:1px solid var(--line);border-radius:10px;background:var(--surface)}
details>summary{cursor:pointer;list-style:none;padding:14px;font-weight:600;font-size:14px;display:flex;justify-content:space-between}
details>summary::-webkit-details-marker{display:none}
details>summary::after{content:"›";color:var(--dim)}
details[open]>summary{border-bottom:1px solid var(--line)}
details .body{padding:0 14px 14px}
details h3{font-size:13px;font-weight:600;color:var(--fg);margin:16px 0 8px}
#log{white-space:pre-wrap;font:11px/1.5 ui-monospace,Menlo,monospace;color:var(--dim);max-height:160px;overflow:auto}
code{background:#0b0e12;border:1px solid var(--line);border-radius:4px;padding:0 4px;font:12px ui-monospace,Menlo,monospace}
@media(max-width:480px){main{padding:10px}section{padding:12px}.bigbtns button{font-size:17px}}
</style></head><body>
<header>
  <div class="brand"><b>BottleSumo</b><span class="pill" id="state">—</span></div>
  <div class="chips" id="chips"></div>
  <div class="meta"><span id="flags"></span><span id="uptime"></span><span id="link"></span></div>
</header>
<main>
<section>
  <div class="line"><span>地圖 <b id="mapName">—</b></span><span>狀態 <b id="smState">—</b></span><span>計時 <b id="smElapsed">—</b></span><span>更新 <b id="otaWindow">—</b></span></div>
  <div class="ready" id="ready">檢查中…</div>
  <div class="bigbtns" style="margin-top:10px">
    <button class="start" id="startA">A<small>Senior A</small></button>
    <button class="start" id="startB">B<small>Senior B</small></button>
  </div>
  <div class="bigbtns">
    <button id="startC">單板</button>
    <button class="stop" id="stopBtn">停止</button>
  </div>
  <p class="hint">SW2 = A、SW3 = B；比賽中任一鍵即停。</p>
</section>

<section>
  <h2>QRE <span class="badge" id="qreState">LIVE</span></h2>
  <div id="qre"></div>
  <h2 style="margin-top:14px">ToF</h2>
  <div id="tof"></div>
  <div class="row"><span class="k">ToF 狀態</span><span class="v" id="tofState">—</span></div>
  <div class="btnrow"><button id="tofRestart">重新初始化 ToF</button></div>
  <p class="hint">綠線 = 目前閾值；紅色旗 = 觸發邊緣。</p>
</section>

<details id="adv">
  <summary>進階設定</summary>
  <div class="body">
    <h3>QRE 自我校準</h3>
    <div class="row"><span class="k">狀態</span><span class="v" id="calState">—</span></div>
    <div id="calTable"></div>
    <div class="btnrow"><button id="calFloor">1. 採安全面 (3s)</button><button id="calVoid">2. 採落差 (3s)</button><button id="calCancel">取消</button></div>

    <h3>QRE 手動閾值</h3>
    <div id="manualTable"></div>
    <div class="btnrow"><button class="primary" id="saveManual">套用手動閾值</button><button id="clearManual">回復自動</button></div>

    <h3>驅動力調校</h3>
    <label>powerScale <span id="psVal">100</span>%</label>
    <input type="range" id="powerScale" min="20" max="100" value="100">
    <div id="tuning"></div>
    <div class="btnrow"><button class="primary" id="saveTuning">套用並儲存</button><button id="resetTuning">回復預設</button></div>

    <h3>開局腳本 · 帶樽 · 邊緣</h3>
    <div id="strategy" class="hint">—</div>

    <h3>馬達測試（bench）</h3>
    <div class="row"><span class="k">bench 模式</span><span class="v" id="benchState">—</span></div>
    <div class="row"><span class="k">左</span><span class="v" id="benchL">0</span></div>
    <input type="range" id="benchLeft" min="-255" max="255" value="0">
    <div class="row"><span class="k">右</span><span class="v" id="benchR">0</span></div>
    <input type="range" id="benchRight" min="-255" max="255" value="0">
    <div class="btnrow"><button class="stop" id="benchStop">停止</button>
      <button class="arrow" data-l="120" data-r="120">▲</button>
      <button class="arrow" data-l="-120" data-r="-120">▼</button>
      <button class="arrow" data-l="-120" data-r="120">◀</button>
      <button class="arrow" data-l="120" data-r="-120">▶</button>
    </div>
    <div class="row"><span class="k">trim（最近 duty 點）</span><span class="v" id="trimV">L 0 / R 0</span></div>
    <div class="btnrow"><button id="tlM">L−</button><button id="tlP">L+</button><button id="trM">R−</button><button id="trP">R+</button></div>
    <table id="trimTbl" style="width:100%;font-size:12px;margin-top:8px"></table>

    <h3>OTA 更新</h3>
    <div class="row"><span class="k">進度</span><span class="v" id="otaProg">—</span></div>
    <div class="btnrow"><button id="armOta">開啟 60s 更新窗口</button><input type="file" id="otaFile" accept=".bin"><button class="primary" id="doOta" disabled>上傳並重啟</button></div>

    <h3>狀態機</h3>
    <div class="btnrow"><button id="clearFault">清除 fault</button></div>
    <table><thead><tr><th>時間</th><th>轉換</th><th>原因</th><th>邊</th></tr></thead><tbody id="history"></tbody></table>

    <h3>紀錄</h3>
    <div id="log">—</div>
  </div>
</details>
</main>
<script>
const $=id=>document.getElementById(id);
let TAB='sensors';
function showTab(t){TAB=t;
 document.querySelectorAll('.tab').forEach(b=>b.classList.toggle('on',b.dataset.tab===t));
 document.querySelectorAll('.panel').forEach(p=>p.classList.toggle('on',p.id==='p-'+t));
 if(t==='tune'||t==='cal'||t==='motor')refreshConfig();
 if(t==='log')refreshHistory();
}
document.querySelectorAll('.tab').forEach(b=>b.onclick=()=>showTab(b.dataset.tab));
let tuning={};
function log(m){const d=new Date();$('log').textContent='['+d.toLocaleTimeString()+'] '+m+'\n'+$('log').textContent;}
async function api(p,o){try{const r=await fetch(p,o);const j=await r.json().catch(()=>({}));if(!r.ok){log(p+' → '+(j.error||r.status));return null;}return j;}catch(e){log(p+' 失敗');return null;}}
function bar(v,max,th){const pct=Math.max(0,Math.min(100,v/max*100));const t=th!=null?Math.max(0,Math.min(100,th/max*100)):null;
 return '<div class="bar"><i style="width:'+pct+'%"></i>'+(t!=null?'<e style="left:'+t+'%"></e>':'')+'</div>';}
const QN=['左前','右前','左後','右後'];
function drawQre(d){
 let h='';for(let i=0;i<4;i++){const q=d.qre[i],edge=!!(d.edges&(1<<i));
  h+='<div class="row"><span class="k">'+QN[i]+'</span><span class="v">'+q+'<span class="flag'+(edge?' on':'')+'">'+(edge?'EDGE':'edge')+'</span></span></div>'
   +bar(q,4095,d.thresholds[i]);}
 h+='<div class="row"><span class="k">極性</span><span class="v">'+d.polarity.join(' · ')+'</span></div>';
 h+='<div class="row"><span class="k">分離度</span><span class="v">'+d.quality.join(' · ')+'</span></div>';
 $('qre').innerHTML=h;
 let t='<div class="tofList">';
 for(let i=0;i<3;i++){t+='<div class="tof"><span>'+['左','中','右'][i]+'</span><b>'+
  (d.distance[i]?d.distance[i]+'<small> mm</small>':'—')+'</b><em>'+d.rangeAge[i]+' ms</em></div>';}
 $('tof').innerHTML=t+'</div>';
}
function drawCal(d){
 const names=['不良','偏高','可用','良好'];let h='';
 for(let i=0;i<4;i++){const q=d.quality[i];const cls=q>=3?'ok':(q===2?'':'warn');
  h+='<div class="row"><span class="k">'+QN[i]+'</span><span class="v '+cls+'">ratio '+d.ratio[i].toFixed(2)+' · '+names[q]+'</span></div>';}
 $('calTable').innerHTML=h;
 $('calState').textContent=d.calibrated?(d.floorCaptured?'已校準':'已有落差'):'未校準';
}
const TSPEC=[
 ['pushPwm',0,255,1,'推樽 PWM'],
 ['approachNear',0,255,1,'接近（近）PWM'],
 ['approachFar',0,255,1,'接近（遠）PWM'],
 ['searchTurn',0,255,1,'搜尋轉向 PWM'],
 ['searchAdvance',0,255,1,'搜尋前進 PWM'],
 ['escapePwm',0,255,1,'逃離邊緣 PWM'],
 ['turnPwm',0,255,1,'轉向 PWM'],
 ['unstickPwm',0,255,1,'脫困 PWM'],
 ['pushSteerMax',0,100,1,'推樽修正上限'],
 ['openingEnabled',0,1,1,'固定開局 0/1'],
 ['openingPwm',0,255,1,'固定開局 PWM'],
 ['openingCommitMs',2000,10000,500,'固定開局時長 ms'],
 ['openingAbortMm',0,900,10,'開局提早接手距離 mm'],
 ['carryEnabled',0,1,1,'帶樽 0/1'],
 ['carryAfterMs',0,3000,50,'帶樽 接觸多久後開始 ms'],
 ['carryPwm',0,255,1,'帶樽 PWM'],
 ['carryMaxMs',500,8000,100,'帶樽 最長 ms'],
 ['edgeDebounceMs',0,100,5,'邊緣去抖 ms'],
 ['maxEscapesPerRound',0,30,1,'每局最多逃離（0=不限）'],
 ['maxPwm',1,255,1,'PWM 上限']
];
const TDEF={powerScale:100,pushPwm:175,approachNear:115,approachFar:145,searchTurn:80,searchAdvance:100,escapePwm:105,turnPwm:85,unstickPwm:100,pushSteerMax:25,maxPwm:255,openingEnabled:1,openingPwm:140,openingCommitMs:6000,openingAbortMm:300,carryEnabled:1,carryAfterMs:700,carryPwm:120,carryMaxMs:4000,edgeDebounceMs:25,maxEscapesPerRound:12};
function drawTuning(d){
 if($('tuning').querySelector('input:focus'))return;
 let h='';for(const [k,lo,hi,st,lab] of TSPEC){
  if(!(k in d))continue;
  h+='<label>'+lab+' (<span id="v_'+k+'">'+d[k]+'</span>)</label>'+
     '<input type="range" min="'+lo+'" max="'+hi+'" step="'+st+'" value="'+d[k]+'" data-k="'+k+'">';}
 $('tuning').innerHTML=h;
 $('tuning').querySelectorAll('input').forEach(i=>{
   i.oninput=()=>{tuning[i.dataset.k]=+i.value;$('v_'+i.dataset.k).textContent=i.value;};
 });
}
function drawStrategy(d){
 const on=x=>x?'<span class="ok">開</span>':'<span class="k">關</span>';
 $('strategy').innerHTML=
  '固定開局：'+on(d.openingEnabled)+' PWM '+d.openingPwm+'，上限 '+d.openingCommitMs+'ms，中心 '+d.openingAbortMm+'mm 內提早接手<br>'+
  'Senior A：直行 1300 → 右轉 420 → 直行 1700 → 右弧 380 → 直行 1200<br>'+
  'Senior B：直行 1300 → 右轉 420 → 直行 1600 → 左弧 380 → 直行 1200<br>'+
  '單板：直行 1400 → 右轉 400 → 直行 2000 → 左轉 400 → 直行 1100<br>'+
  '帶樽：'+on(d.carryEnabled)+' 接觸 '+d.carryAfterMs+'ms 後直推 '+d.carryMaxMs+'ms；正面持續接觸，只作小幅修正（規則：旋轉中掉落屬非故意）<br>'+
  '邊緣：去抖 '+d.edgeDebounceMs+'ms，每局上限 '+d.maxEscapesPerRound+' 次（固定開局期間同樣生效）';
}
function drawManual(d){
 if($('manualTable').querySelector('input:focus,select:focus'))return;
 let h='';
 for(let i=0;i<4;i++){
  h+='<div class="row"><span class="k">'+QN[i]+' <span class="flag'+(d.manual[i]?' on':'')+'">手</span></span><span class="v">'+
     '<input type="checkbox" data-m="'+i+'" '+(d.manual[i]?'checked':'')+'> '+
     '<input type="number" min="1" max="4094" value="'+(d.manualThreshold[i]||d.thresholds[i])+'" data-t="'+i+'"> '+
     '<select data-h="'+i+'"><option value="1"'+(d.manualHigh[i]?' selected':'')+'>邊界=高</option>'+
     '<option value="0"'+(d.manualHigh[i]?'':' selected')+'>邊界=低</option></select></span></div>';
 }
 h+='<div class="hint">自動閾值：'+d.thresholds.join(' / ')+'</div>';
 $('manualTable').innerHTML=h;
}
function drawHistory(h){
 let rows='';for(const t of h){rows+='<tr><td>'+t.at+'</td><td>'+t.from+' → '+t.to+'</td><td>'+t.reason+'</td><td>0x'+t.edges.toString(16)+'</td></tr>';}
 $('history').innerHTML=rows;
}
async function refreshConfig(){
 const c=await api('/api/config');if(!c)return;
 drawCal(c);drawStrategy(c.tuning);
 if(!$('tuning').querySelector('input:focus')){
  tuning=c.tuning;$('powerScale').value=tuning.powerScale;$('psVal').textContent=tuning.powerScale;}
 drawTuning(c.tuning);
}
async function refreshHistory(){const hh=await api('/api/history');if(hh)drawHistory(hh);}
async function refresh(){
 const s=await api('/api/status');if(!s)return;
 $('state').textContent=s.stateName;
 $('state').style.background=s.active?'#1b4f8a':(s.faulted?'#7f1d1d':'#1d2a3a');
 $('flags').textContent=s.flagText;$('uptime').textContent=(s.uptime_ms/1000).toFixed(1)+'s';
 $('link').innerHTML=s.wifiClients?('<span class="k">client '+s.wifiClients+'</span>'):'';
 const ts=$('tofState');
 ts.textContent=s.tofFresh?'OK':('失敗：'+(s.tofError||'—')+(s.tofHead<3?(' · head '+s.tofHead):''));
 $('mapName').textContent=s.mapName||'—';
 $('smState').textContent=s.stateName;
 $('smElapsed').textContent=(s.state_elapsed_ms/1000).toFixed(1)+' s';
 $('otaWindow').textContent=s.otaWindow?'已開啟':'未開啟';
 $('benchState').textContent=s.bench?'已啟用（停止時可驅動）':'未啟用（開機按住 SW2）';
 const ready=s.canStart&&s.tofFresh;
 const why=[];
 if(s.faulted)why.push('FAULT');
 if(!s.calibrated)why.push('未校準');
 if(!s.tofFresh)why.push('ToF 無新資料');
 if(s.updating)why.push('更新中');
 if(s.bench)why.push('bench 模式');
 const rEl=$('ready');
 rEl.className='ready '+(ready?'ok':'bad');
 rEl.textContent=ready?'可以開始回合（實體 SW2／SW3 或上方按鈕）':'未能開始：'+(why.join(' · ')||'條件未滿足');
 const chip=[];
 chip.push('<span class="chip '+(s.calibrated?'ok':'warn')+'"><i></i>校準 '+(s.calibrated?'OK':'未做')+'</span>');
 chip.push('<span class="chip '+(s.tofFresh?'ok':'bad')+'"><i></i>ToF '+(s.tofFresh?'OK':'無資料')+'</span>');
 if(s.faulted)chip.push('<span class="chip bad"><i></i>FAULT</span>');
 if(s.bench)chip.push('<span class="chip warn"><i></i>BENCH</span>');
 if(s.otaWindow)chip.push('<span class="chip warn"><i></i>OTA 窗口</span>');
 if(s.updating)chip.push('<span class="chip warn"><i></i>寫入中</span>');
 chip.push('<span class="chip"><i></i>'+(s.wifiClients||0)+' client</span>');
 $('chips').innerHTML=chip.join('');
 $('startA').disabled=!ready;$('startB').disabled=!ready;$('startC').disabled=!ready;
 $('stopBtn').disabled=!s.active;
 const advOpen=$('adv').open;
 const r=await api('/api/raw');
 if(r){drawQre(r);if(advOpen)drawManual(r);}
 if(advOpen){await refreshConfig();await refreshHistory();}
}
$('adv').addEventListener('toggle',()=>{if($('adv').open){refreshConfig();refreshHistory();}});
$('tofRestart').onclick=async()=>{const j=await api('/api/tof-restart',{method:'POST'});log('ToF 重啟 '+(j?'已送出':'失敗'));setTimeout(refresh,800);};
$('powerScale').oninput=e=>{$('psVal').textContent=e.target.value;tuning.powerScale=+e.target.value;};
$('saveTuning').onclick=async()=>{tuning.powerScale=+$('powerScale').value;const j=await api('/api/tuning',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(tuning)});log('tuning '+(j?'已儲存':'被拒絕'));};
$('resetTuning').onclick=()=>{tuning={...TDEF};$('powerScale').value=100;$('psVal').textContent=100;drawTuning(tuning);drawStrategy(tuning);};
$('calFloor').onclick=()=>api('/api/calibrate/floor',{method:'POST'}).then(()=>log('floor 取樣開始'));
$('calVoid').onclick=()=>api('/api/calibrate/void',{method:'POST'}).then(()=>log('void 取樣開始'));
$('calCancel').onclick=()=>api('/api/calibrate/cancel',{method:'POST'});
$('saveManual').onclick=async()=>{
 const body={mask:0,high:0};
 for(let i=0;i<4;i++){
  const on=$('manualTable').querySelector('[data-m="'+i+'"]').checked;
  const tv=+$('manualTable').querySelector('[data-t="'+i+'"]').value;
  const hi=+$('manualTable').querySelector('[data-h="'+i+'"]').value;
  body['t'+i]=tv;
  if(on){body.mask|=1<<i;if(hi)body.high|=1<<i;}
 }
 const j=await api('/api/manual',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
 log('手動閾值 '+(j?'已套用':'被拒絕'));
};
$('clearManual').onclick=async()=>{const j=await api('/api/manual',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({mask:0,high:0,t0:0,t1:0,t2:0,t3:0})});log('手動閾值 '+(j?'已清除':'被拒絕'));};
function startMap(map){api('/api/start',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({map})})
 .then(j=>log('start map '+map+' '+(j?'已送出':'被拒絕')));}
$('startA').onclick=()=>startMap(0);
$('startB').onclick=()=>startMap(1);
$('startC').onclick=()=>startMap(2);
$('stopBtn').onclick=()=>api('/api/stop',{method:'POST'});
$('clearFault').onclick=()=>api('/api/fault-clear',{method:'POST'});
$('armOta').onclick=()=>api('/api/ota-arm',{method:'POST'}).then(j=>{if(j)$('doOta').disabled=!j.armed;});
$('otaFile').onchange=e=>{$('doOta').disabled=!e.target.files.length;};
$('doOta').onclick=async()=>{const f=$('otaFile').files[0];if(!f)return;log('OTA 開始 '+f.size+' bytes');
 // A raw body never reaches the WebServer upload callback; send multipart.
 const fd=new FormData();fd.append('file',f,f.name);
 const r=await fetch('/api/ota',{method:'POST',body:fd});log('OTA '+(r.ok?'完成，重啟中':'失敗 '+r.status));};
$('benchLeft').oninput=e=>$('benchL').textContent=e.target.value;
$('benchRight').oninput=e=>$('benchR').textContent=e.target.value;
let benchTimer=null;
function benchSend(){api('/api/bench',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({left:+$('benchLeft').value,right:+$('benchRight').value})});}
$('benchLeft').onchange=$('benchRight').onchange=()=>{clearInterval(benchTimer);benchSend();benchTimer=setInterval(benchSend,200);};
$('benchStop').onclick=()=>{clearInterval(benchTimer);$('benchLeft').value=0;$('benchRight').value=0;$('benchL').textContent=0;$('benchR').textContent=0;benchSend();};
document.querySelectorAll('button.arrow').forEach(b=>{
 const l=+b.dataset.l,r=+b.dataset.r;
 b.addEventListener('pointerdown',e=>{e.preventDefault();
  $('benchLeft').value=l;$('benchRight').value=r;$('benchL').textContent=l;$('benchR').textContent=r;
  clearInterval(benchTimer);benchSend();benchTimer=setInterval(benchSend,200);});
 ['pointerup','pointerleave','pointercancel'].forEach(ev=>b.addEventListener(ev,()=>$('benchStop').click()));
});
window.addEventListener('blur',()=>$('benchStop').click());
document.addEventListener('visibilitychange',()=>{if(document.hidden)$('benchStop').click();});
function trimSend(wheel,delta){
 const duty=Math.max(Math.abs(+$('benchLeft').value),Math.abs(+$('benchRight').value),64);
 api('/api/trim',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({wheel:wheel,delta:delta,duty:duty})}).then(refreshTrim);
}
$('tlM').onclick=()=>trimSend(0,-1);$('tlP').onclick=()=>trimSend(0,1);
$('trM').onclick=()=>trimSend(1,-1);$('trP').onclick=()=>trimSend(1,1);
function refreshTrim(){api('/api/trim').then(j=>{if(!j||!j.tl)return;
 $('trimV').textContent='L '+j.tl[1]+' / R '+j.tr[1];
 let h='<tr><th></th><th>L</th><th>R</th></tr>';
 j.duty.forEach((d,i)=>{h+='<tr><td>'+d+'</td><td>'+j.tl[i]+'</td><td>'+j.tr[i]+'</td></tr>';});
 $('trimTbl').innerHTML=h;});}
refreshTrim();
refresh();setInterval(refresh,250);
</script></body></html>)HTML";

// ---------------------------------------------------------------------------
// API handlers
// ---------------------------------------------------------------------------
static void handleStatus() {
  Telemetry t{};
  const bool have = xQueuePeek(telemetryQueue, &t, 0);
  String j = "{";
  if (have) {
    j += "\"boot\":" + String(t.boot) +
         ",\"sequence\":" + String(t.sequence) +
         ",\"uptime_ms\":" + String(t.uptime_ms) +
         ",\"state_elapsed_ms\":" + String(t.state_elapsed_ms) +
         ",\"display_state\":" + String(t.state) +
         ",\"edges\":" + String(t.edges) +
         ",\"flags\":" + String(t.flags) +
         ",\"qre\":[" + String(t.qre[0]) + "," + String(t.qre[1]) + "," + String(t.qre[2]) + "," + String(t.qre[3]) + "]" +
         ",\"distance\":[" + String(t.distance[0]) + "," + String(t.distance[1]) + "," + String(t.distance[2]) + "]" +
         ",\"range_age_ms\":[" + String(t.range_age_ms[0]) + "," + String(t.range_age_ms[1]) + "," + String(t.range_age_ms[2]) + "]" +
         ",\"requested\":[" + String(t.requested[0]) + "," + String(t.requested[1]) + "]" +
         ",\"applied\":[" + String(t.applied[0]) + "," + String(t.applied[1]) + "]" +
         ",\"control_max_us\":" + String(t.control_max_us) +
         ",\"deadline_misses\":" + String(t.deadline_misses);
  } else {
    j += "\"boot\":0,\"sequence\":0,\"uptime_ms\":0,\"state_elapsed_ms\":0,\"display_state\":0,"
         "\"edges\":0,\"flags\":0,\"qre\":[0,0,0,0],\"distance\":[0,0,0],"
         "\"range_age_ms\":[0,0,0],\"requested\":[0,0],\"applied\":[0,0],"
         "\"control_max_us\":0,\"deadline_misses\":0";
  }
  lockConfig();
  const bool calibrated = live.calibrated;
  const bool bench = live.benchMode;
  const bool updating = live.updating;
  const bool armed = live.otaWindowArmed && live.otaWindowUntil > millis();
  const bool faulted = live.faulted;
  const bool bootOk = live.bootVerified;
  const uint32_t bootId = live.bootId;
  const uint8_t progress = live.otaProgress;
  const uint8_t clients = live.wifiClients;
  const uint8_t escapes = live.escapeCount;
  const uint8_t mapType = live.mapType;
  uint16_t stack[4] = {}, hz[4] = {};
  for (uint8_t i = 0; i < 4; ++i) { stack[i] = live.stackFree[i]; hz[i] = live.taskHz[i]; }
  unlockConfig();
  // A start also needs a recent sample from every fitted head; the control
  // task refuses on the same condition. 600 ms keeps the 250 ms UI refresh
  // from flickering the button.
  bool tofFresh = have;
  for (uint8_t k = 0; k < TOF_HEAD_COUNT && tofFresh; ++k) {
    if (t.range_age_ms[TOF_HEADS[k]] >= 600) tofFresh = false;
  }
  j += ",\"calibrated\":" + String(calibrated ? 1 : 0);
  j += ",\"bench\":" + String(bench ? 1 : 0);
  j += ",\"updating\":" + String(updating ? 1 : 0);
  j += ",\"otaWindow\":" + String(armed ? 1 : 0);
  j += ",\"faulted\":" + String(faulted ? 1 : 0);
  j += ",\"bootVerified\":" + String(bootOk ? 1 : 0);
  j += ",\"otaProgress\":" + String(progress);
  j += ",\"wifiClients\":" + String(clients);
  j += ",\"escapes\":" + String(escapes);
  j += ",\"map\":" + String(mapType);
  j += ",\"mapName\":\"" + String(mapName(MapType(mapType))) + "\"";
  j += ",\"stack\":[" + String(stack[0]) + "," + String(stack[1]) + "," +
       String(stack[2]) + "," + String(stack[3]) + "]";
  j += ",\"hz\":[" + String(hz[0]) + "," + String(hz[1]) + "," +
       String(hz[2]) + "," + String(hz[3]) + "]";
  j += ",\"bootId\":" + String(bootId);
  j += ",\"stateName\":\"" + String(stateNameFromDisplay(t.state)) + "\"";
  j += ",\"active\":" + String(t.state != 0 ? 1 : 0);
  j += ",\"canStart\":" + String((calibrated && !updating && !faulted && !bench) ? 1 : 0);
  j += ",\"tofFresh\":" + String(tofFresh ? 1 : 0);
  j += ",\"flagText\":\"" + String(flagText(t.flags)) + "\"";
  j += ",\"tofError\":\"" + String(live.tofError) + "\"";
  j += ",\"tofHead\":" + String(int(live.tofFailHead));
  j += ",\"ip\":\"" +
       (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString()
                                      : WiFi.softAPIP().toString()) +
       "\"";
  j += ",\"staRssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
  // AP and STA share one radio: once the station associates, the AP follows
  // the router's channel, so the startup channel=6 is only a pre-join default.
  j += ",\"channel\":" + String(WiFi.channel());
  j += "}";
  sendJson(j);
}

static void handleRaw() {
  Telemetry t{};
  xQueuePeek(telemetryQueue, &t, 0);
  lockConfig();
  String j = "{";
  j += "\"qre\":[" + String(t.qre[0]) + "," + String(t.qre[1]) + "," +
       String(t.qre[2]) + "," + String(t.qre[3]) + "]";
  j += ",\"thresholds\":[" + String(live.calibration.threshold(0)) + "," +
       String(live.calibration.threshold(1)) + "," +
       String(live.calibration.threshold(2)) + "," +
       String(live.calibration.threshold(3)) + "]";
  j += ",\"polarity\":[" + String(live.calibration.edgeIsHigh(0) ? 1 : 0) + "," +
       String(live.calibration.edgeIsHigh(1) ? 1 : 0) + "," +
       String(live.calibration.edgeIsHigh(2) ? 1 : 0) + "," +
       String(live.calibration.edgeIsHigh(3) ? 1 : 0) + "]";
  j += ",\"quality\":[" + String(live.calibration.quality(0)) + "," +
       String(live.calibration.quality(1)) + "," +
       String(live.calibration.quality(2)) + "," +
       String(live.calibration.quality(3)) + "]";
  j += ",\"ratio\":[" + String(live.calibration.separationRatio(0), 2) + "," +
       String(live.calibration.separationRatio(1), 2) + "," +
       String(live.calibration.separationRatio(2), 2) + "," +
       String(live.calibration.separationRatio(3), 2) + "]";
  j += ",\"manual\":[" + String(live.calibration.manual(0) ? 1 : 0) + "," +
       String(live.calibration.manual(1) ? 1 : 0) + "," +
       String(live.calibration.manual(2) ? 1 : 0) + "," +
       String(live.calibration.manual(3) ? 1 : 0) + "]";
  j += ",\"manualHigh\":[" + String(live.calibration.manualHigh & 1 ? 1 : 0) + "," +
       String(live.calibration.manualHigh & 2 ? 1 : 0) + "," +
       String(live.calibration.manualHigh & 4 ? 1 : 0) + "," +
       String(live.calibration.manualHigh & 8 ? 1 : 0) + "]";
  j += ",\"manualThreshold\":[" + String(live.calibration.manualThreshold[0]) + "," +
       String(live.calibration.manualThreshold[1]) + "," +
       String(live.calibration.manualThreshold[2]) + "," +
       String(live.calibration.manualThreshold[3]) + "]";
  j += ",\"calibrated\":" + String(live.calibrated ? 1 : 0);
  j += ",\"floorCaptured\":" + String(live.floorCaptured ? 1 : 0);
  j += ",\"calMode\":\"" + String(live.calibrationMode ? live.calibrationMode : '0') + "\"";
  j += ",\"distance\":[" + String(t.distance[0]) + "," + String(t.distance[1]) + "," +
       String(t.distance[2]) + "]";
  j += ",\"rangeAge\":[" + String(t.range_age_ms[0]) + "," + String(t.range_age_ms[1]) + "," +
       String(t.range_age_ms[2]) + "]";
  j += ",\"edges\":" + String(t.edges);
  unlockConfig();
  j += "}";
  sendJson(j);
}

static void handleConfig() {
  lockConfig();
  const Tuning &t = live.tuning;
  const QreCalibration &c = live.calibration;
  String j = "{";
  j += "\"tuning\":{";
  j += "\"powerScale\":" + String(t.powerScale) +
       ",\"pushPwm\":" + String(t.pushPwm) +
       ",\"approachNear\":" + String(t.approachNear) +
       ",\"approachFar\":" + String(t.approachFar) +
       ",\"searchTurn\":" + String(t.searchTurn) +
       ",\"searchAdvance\":" + String(t.searchAdvance) +
       ",\"escapePwm\":" + String(t.escapePwm) +
       ",\"turnPwm\":" + String(t.turnPwm) +
       ",\"unstickPwm\":" + String(t.unstickPwm) +
       ",\"pushSteerMax\":" + String(t.pushSteerMax) +
       ",\"maxPwm\":" + String(t.maxPwm) +
       ",\"openingEnabled\":" + String(t.openingEnabled ? 1 : 0) +
       ",\"openingPwm\":" + String(t.openingPwm) +
       ",\"openingCommitMs\":" + String(t.openingCommitMs) +
       ",\"openingAbortMm\":" + String(t.openingAbortMm) +
       ",\"carryEnabled\":" + String(t.carryEnabled ? 1 : 0) +
       ",\"carryAfterMs\":" + String(t.carryAfterMs) +
       ",\"carryPwm\":" + String(t.carryPwm) +
       ",\"carryMaxMs\":" + String(t.carryMaxMs) +
       ",\"edgeDebounceMs\":" + String(t.edgeDebounceMs) +
       ",\"maxEscapesPerRound\":" + String(t.maxEscapesPerRound);
  j += "}";
  j += ",\"quality\":[" + String(c.quality(0)) + "," + String(c.quality(1)) + "," +
       String(c.quality(2)) + "," + String(c.quality(3)) + "]";
  j += ",\"ratio\":[" + String(c.separationRatio(0), 2) + "," + String(c.separationRatio(1), 2) + "," +
       String(c.separationRatio(2), 2) + "," + String(c.separationRatio(3), 2) + "]";
  j += ",\"manual\":[" + String(live.calibration.manual(0) ? 1 : 0) + "," +
       String(live.calibration.manual(1) ? 1 : 0) + "," +
       String(live.calibration.manual(2) ? 1 : 0) + "," +
       String(live.calibration.manual(3) ? 1 : 0) + "]";
  j += ",\"manualHigh\":[" + String(live.calibration.manualHigh & 1 ? 1 : 0) + "," +
       String(live.calibration.manualHigh & 2 ? 1 : 0) + "," +
       String(live.calibration.manualHigh & 4 ? 1 : 0) + "," +
       String(live.calibration.manualHigh & 8 ? 1 : 0) + "]";
  j += ",\"manualThreshold\":[" + String(live.calibration.manualThreshold[0]) + "," +
       String(live.calibration.manualThreshold[1]) + "," +
       String(live.calibration.manualThreshold[2]) + "," +
       String(live.calibration.manualThreshold[3]) + "]";
  j += ",\"calibrated\":" + String(live.calibrated ? 1 : 0);
  j += ",\"floorCaptured\":" + String(live.floorCaptured ? 1 : 0);
  j += ",\"wifi\":{";
  j += "\"ssid\":\"BottleSumo\"";
  j += ",\"channel\":" + String(wifiConfig.channel);
  j += ",\"apPasswordSet\":" + String(strlen(wifiConfig.apPassword) ? 1 : 0);
  j += ",\"staSsid\":\"" + String(wifiConfig.staSsid) + "\"";
  j += "}";
  unlockConfig();
  j += "}";
  sendJson(j);
}

static void handleHistory() {
  lockConfig();
  String j = "[";
  for (uint8_t i = 0; i < live.history.count; ++i) {
    const Transition *t = live.history.newest(i);
    if (!t) break;
    if (i) j += ",";
    j += "{\"at\":" + String(t->atMs) +
         ",\"from\":\"" + String(stateName(t->from)) + "\"" +
         ",\"to\":\"" + String(stateName(t->to)) + "\"" +
         ",\"reason\":\"" + String(reasonName(t->reason)) + "\"" +
         ",\"edges\":" + String(t->edges) +
         ",\"qre\":[" + t->qre[0] + "," + t->qre[1] + "," + t->qre[2] + "," + t->qre[3] + "]}";
  }
  unlockConfig();
  j += "]";
  sendJson(j);
}

static bool tuningFromJson(const String &body, Tuning &out) {
  // Minimal, dependency-free JSON extraction for flat numeric fields.
  static const char *keys[] = {"powerScale", "pushPwm", "approachNear", "approachFar",
                               "searchTurn", "searchAdvance", "escapePwm", "turnPwm",
                               "unstickPwm", "pushSteerMax", "maxPwm",
                               "openingEnabled", "openingPwm", "openingCommitMs",
                               "openingAbortMm", "carryEnabled", "carryAfterMs",
                               "carryPwm", "carryMaxMs",
                               "edgeDebounceMs", "maxEscapesPerRound"};
  Tuning t = out;
  for (const char *key : keys) {
    const String needle = String("\"") + key + "\"";
    const int at = body.indexOf(needle);
    if (at < 0) continue;
    const int colon = body.indexOf(':', at + needle.length());
    if (colon < 0) return false;
    int end = colon + 1;
    while (end < body.length() && (isDigit(body[end]) || body[end] == '-')) end++;
    if (end == colon + 1) return false;
    const long value = body.substring(colon + 1, end).toInt();
    if (String(key) == "powerScale") t.powerScale = uint16_t(constrain(value, 1, 100));
    else if (String(key) == "pushPwm") t.pushPwm = uint16_t(constrain(value, 0, 255));
    else if (String(key) == "approachNear") t.approachNear = uint16_t(constrain(value, 0, 255));
    else if (String(key) == "approachFar") t.approachFar = uint16_t(constrain(value, 0, 255));
    else if (String(key) == "searchTurn") t.searchTurn = uint16_t(constrain(value, 0, 255));
    else if (String(key) == "searchAdvance") t.searchAdvance = uint16_t(constrain(value, 0, 255));
    else if (String(key) == "escapePwm") t.escapePwm = uint16_t(constrain(value, 0, 255));
    else if (String(key) == "turnPwm") t.turnPwm = uint16_t(constrain(value, 0, 255));
    else if (String(key) == "unstickPwm") t.unstickPwm = uint16_t(constrain(value, 0, 255));
    else if (String(key) == "pushSteerMax") t.pushSteerMax = int16_t(constrain(value, 0, 100));
    else if (String(key) == "maxPwm") t.maxPwm = uint16_t(constrain(value, 1, 255));
    else if (String(key) == "openingEnabled") t.openingEnabled = value != 0;
    else if (String(key) == "openingPwm") t.openingPwm = uint16_t(constrain(value, 0, 255));
    else if (String(key) == "openingCommitMs") t.openingCommitMs = uint16_t(constrain(value, 2000, 10000));
    else if (String(key) == "openingAbortMm") t.openingAbortMm = uint16_t(constrain(value, 0, 900));
    else if (String(key) == "carryEnabled") t.carryEnabled = value != 0;
    else if (String(key) == "carryAfterMs") t.carryAfterMs = uint16_t(constrain(value, 0, 3000));
    else if (String(key) == "carryPwm") t.carryPwm = uint16_t(constrain(value, 0, 255));
    else if (String(key) == "carryMaxMs") t.carryMaxMs = uint16_t(constrain(value, 200, 8000));
    else if (String(key) == "edgeDebounceMs") t.edgeDebounceMs = uint8_t(constrain(value, 0, 100));
    else if (String(key) == "maxEscapesPerRound") t.maxEscapesPerRound = uint8_t(constrain(value, 0, 30));
  }
  out = t;
  return true;
}

static void handleTuning() {
  if (!robotStopped()) { sendReject(409, "只可在停止狀態修改"); return; }
  const String body = server.arg("plain");
  Tuning updated{};
  lockConfig();
  updated = live.tuning;
  unlockConfig();
  if (!tuningFromJson(body, updated)) { sendReject(400, "JSON 解析失敗"); return; }
  lockConfig();
  live.tuning = updated;
  unlockConfig();
  if (!saveTuning(updated)) { sendReject(500, "NVS 寫入失敗"); return; }
  sendRequest(Command::ApplyTuning);
  sendJson("{\"ok\":1}");
}

static void handleCalibrate(const char mode) {
  if (!robotStopped()) { sendReject(409, "只可在停止狀態校正"); return; }
  if (mode == 'f') sendRequest(Command::CalibrateFloor);
  else if (mode == 'e') sendRequest(Command::CalibrateVoid);
  else sendRequest(Command::CalibrateCancel);
  sendJson("{\"ok\":1}");
}

static void handleWifi() {
  const String body = server.arg("plain");
  WifiConfig updated = wifiConfig;
  const int ap = body.indexOf("\"apPassword\"");
  if (ap >= 0) {
    const int colon = body.indexOf(':', ap);
    const int open = body.indexOf('"', colon);
    const int close = body.indexOf('"', open + 1);
    if (open < 0 || close < 0) { sendReject(400, "JSON 解析失敗"); return; }
    const String value = body.substring(open + 1, close);
    if (value.length() < 8 || value.length() > 63) { sendReject(400, "AP 密碼需 8-63 字元"); return; }
    memset(updated.apPassword, 0, sizeof(updated.apPassword));
    strncpy(updated.apPassword, value.c_str(), sizeof(updated.apPassword) - 1);
  }
  const int ch = body.indexOf("\"channel\"");
  if (ch >= 0) {
    const int colon = body.indexOf(':', ch);
    updated.channel = uint8_t(constrain(body.substring(colon + 1).toInt(), 1, 11));
  }
  wifiConfig = updated;
  if (!saveWifiConfig()) { sendReject(500, "NVS 寫入失敗"); return; }
  sendJson("{\"ok\":1,\"restartRequired\":1}");
}

// ---------------------------------------------------------------------------
// OTA
// ---------------------------------------------------------------------------
static bool otaActive = false;
static size_t otaReceived = 0;

static void handleManual() {
  if (!robotStopped()) { sendReject(409, "只可在停止狀態修改"); return; }
  const String body = server.arg("plain");
  auto grab = [&](const char *key, long &out) {
    const String needle = String("\"") + key + "\"";
    const int at = body.indexOf(needle);
    if (at < 0) return false;
    const int colon = body.indexOf(':', at + needle.length());
    if (colon < 0) return false;
    int end = colon + 1;
    if (end < body.length() && body[end] == '-') end++;
    const int digits = end;
    while (end < body.length() && isDigit(body[end])) end++;
    if (end == digits) return false;
    out = body.substring(colon + 1, end).toInt();
    return true;
  };
  long mask = -1, high = -1, raw[4] = {-1, -1, -1, -1};
  if (!grab("mask", mask) || !grab("high", high)) { sendReject(400, "JSON 解析失敗"); return; }
  grab("t0", raw[0]); grab("t1", raw[1]); grab("t2", raw[2]); grab("t3", raw[3]);

  lockConfig();
  QreCalibration cal = live.calibration;
  cal.manualMask = uint8_t(mask & 0x0F);
  cal.manualHigh = uint8_t(high & 0x0F);
  for (uint8_t i = 0; i < 4; ++i) {
    if (raw[i] > 0) cal.manualThreshold[i] = uint16_t(constrain(raw[i], 1, 4094));
    else if (!cal.manual(i)) cal.manualThreshold[i] = cal.threshold(i);
  }
  if (!cal.valid()) { unlockConfig(); sendReject(400, "閾值無效：先做一次自動校準，或四個通道全部手動"); return; }
  live.calibration = cal;
  live.calibrated = true;
  if (cal.manualMask) live.floorCaptured = true;
  unlockConfig();
  if (!saveCalibration(cal)) { sendReject(500, "NVS 寫入失敗"); return; }
  sendJson("{\"ok\":1}");
}

static void handleOtaUpload() {
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    if (!robotStopped()) { server.client().stop(); otaActive = false; return; }
    lockConfig();
    live.updating = true;
    live.otaProgress = 0;
    unlockConfig();
    otaReceived = 0;
    otaActive = Update.begin(upload.totalSize, U_FLASH);
    if (!otaActive) { Serial.println("OTA begin failed (slot too small?)"); return; }
  } else if (upload.status == UPLOAD_FILE_WRITE && otaActive) {
    const size_t written = Update.write(upload.buf, upload.currentSize);
    if (written != upload.currentSize) { Update.abort(); otaActive = false; return; }
    otaReceived += upload.currentSize;
    lockConfig();
    live.otaProgress = uint8_t(upload.totalSize > 0 ? (100ul * otaReceived / upload.totalSize) : 0);
    unlockConfig();
  } else if (upload.status == UPLOAD_FILE_END) {
    const bool ok = Update.end(true);
    lockConfig();
    live.otaProgress = ok ? 100 : 0;
    unlockConfig();
    otaActive = false;
  }
}

static void handleOtaDone() {
  lockConfig();
  const bool armed = live.otaWindowArmed && live.otaWindowUntil > millis();
  unlockConfig();
  if (!armed) { sendReject(409, "先開啟更新窗口"); return; }
  if (!robotStopped()) { sendReject(409, "只可在停止狀態更新"); return; }
  lockConfig();
  live.updating = false;
  live.otaWindowArmed = false;
  unlockConfig();
  sendJson(Update.hasError() ? "{\"ok\":0,\"error\":\"write failed\"}" : "{\"ok\":1,\"rebooting\":1}");
  delay(200);
  ESP.restart();
}

static void handleOtaArm() {
  lockConfig();
  live.otaWindowArmed = true;
  live.otaWindowUntil = millis() + OTA_WINDOW_MS;
  unlockConfig();
  sendJson("{\"armed\":1}");
}

// ---------------------------------------------------------------------------
// Task
// ---------------------------------------------------------------------------
static IPAddress subscriberIp{};
static uint16_t subscriberPort = 0;
static uint32_t subscriberAt = 0;
static uint32_t lastTelemetry = 0;

void webBegin() {
  loadWifiConfig();
  const bool sta = wifiConfig.staSsid[0] != '\0';
  WiFi.setAutoReconnect(sta);
  WiFi.mode(sta ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(AP_SSID, wifiConfig.apPassword, wifiConfig.channel, false, 4);
  WiFi.softAPsetHostname("bottlesumo");
  if (sta) {
    WiFi.setSleep(false);
    WiFi.begin(wifiConfig.staSsid, wifiConfig.staPassword);
    Serial.printf("STA: joining \"%s\"\n", wifiConfig.staSsid);
    MDNS.begin("bottlesumo");
    MDNS.addService("http", "tcp", 80);
  }
  // Arduino OTA (espota) is the update path: it needs no armed window and no
  // HTTP handler, and works on the AP as well as on the station link.
  ArduinoOTA.setHostname("bottlesumo");
  if (OTA_PASSWORD[0] != '\0') ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    lockConfig();
    live.updating = true;
    unlockConfig();
    Serial.println("OTA: upload starting, outputs stopping");
  });
  ArduinoOTA.onEnd([]() {
    lockConfig();
    live.updating = false;
    unlockConfig();
    Serial.println("OTA: finished, rebooting");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    lockConfig();
    live.updating = false;
    unlockConfig();
    Serial.printf("OTA: error %u\n", static_cast<unsigned>(error));
  });
  ArduinoOTA.begin();
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html", indexHtml, sizeof(indexHtml)); });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/raw", HTTP_GET, handleRaw);
  server.on("/api/config", HTTP_GET, handleConfig);
  server.on("/api/history", HTTP_GET, handleHistory);
  server.on("/api/tuning", HTTP_POST, handleTuning);
  server.on("/api/manual", HTTP_POST, handleManual);
  server.on("/api/wifi", HTTP_POST, handleWifi);
  server.on("/api/calibrate/floor", HTTP_POST, [] { handleCalibrate('f'); });
  server.on("/api/calibrate/void", HTTP_POST, [] { handleCalibrate('e'); });
  server.on("/api/calibrate/cancel", HTTP_POST, [] { handleCalibrate(0); });
  server.on("/api/start", HTTP_POST, [] {
    const String body = server.arg("plain");
    const int at = body.indexOf("\"map\"");
    const int colon = at >= 0 ? body.indexOf(':', at) : -1;
    const int map = colon >= 0 ? constrain(body.substring(colon + 1).toInt(), 0, 2) : 0;
    sendRequest(Command::Start, int16_t(map), 0, Reason::WebStart);
    sendJson(String("{\"ok\":1,\"map\":") + map + "}");
  });
  server.on("/api/stop", HTTP_POST, [] { sendRequest(Command::Stop); sendJson("{\"ok\":1}"); });
  server.on("/api/fault-clear", HTTP_POST, [] { sendRequest(Command::ClearFault); sendJson("{\"ok\":1}"); });
  server.on("/api/bench", HTTP_POST, [] {
    const String body = server.arg("plain");
    const int lp = body.indexOf("\"left\"");
    const int rp = body.indexOf("\"right\"");
    if (lp < 0 || rp < 0) { sendReject(400, "JSON 解析失敗"); return; }
    const int lc = body.indexOf(':', lp), rc = body.indexOf(':', rp);
    sendRequest(Command::BenchDrive,
                int16_t(constrain(body.substring(lc + 1).toInt(), -255, 255)),
                int16_t(constrain(body.substring(rc + 1).toInt(), -255, 255)));
    sendJson("{\"ok\":1}");
  });
  server.on("/api/trim", HTTP_GET, [] {
    String j = "{\"ok\":1,\"duty\":[";
    for (uint8_t i = 0; i < TrimTable::N; ++i) {
      if (i) j += ",";
      j += String(trimTable.duty[i]);
    }
    j += "],\"tl\":[";
    for (uint8_t i = 0; i < TrimTable::N; ++i) {
      if (i) j += ",";
      j += String(trimTable.trim[0][i]);
    }
    j += "],\"tr\":[";
    for (uint8_t i = 0; i < TrimTable::N; ++i) {
      if (i) j += ",";
      j += String(trimTable.trim[1][i]);
    }
    j += "]}";
    sendJson(j);
  });
  server.on("/api/trim", HTTP_POST, [] {
    if (!robotStopped()) { sendReject(409, "行進中不可改 trim"); return; }
    const String body = server.arg("plain");
    auto intAt = [&](const char *key, int fallback) {
      const int at = body.indexOf(key);
      if (at < 0) return fallback;
      const int colon = body.indexOf(':', at + strlen(key));
      if (colon < 0) return fallback;
      return int(body.substring(colon + 1).toInt());
    };
    const int wheel = intAt("\"wheel\"", -1);
    const int delta = intAt("\"delta\"", 0);
    const int duty = intAt("\"duty\"", 120);
    if (wheel < 0 || wheel > 1 || delta == 0) { sendReject(400, "wheel/delta 不正確"); return; }
    const uint8_t point = trimTable.pointFor(duty);
    int next = int(trimTable.trim[wheel][point]) + (delta > 0 ? 1 : -1);
    if (next > 40) next = 40;
    if (next < -40) next = -40;
    trimTable.trim[wheel][point] = int8_t(next);
    saveTrimTable();
    String j = "{\"ok\":1,\"wheel\":" + String(wheel) + ",\"point\":" + String(point) +
               ",\"value\":" + String(trimTable.trim[wheel][point]) + "}";
    sendJson(j);
  });
  server.on("/api/tof-restart", HTTP_POST, [] {
    tofRestartRequest = true;
    sendJson("{\"ok\":1}");
  });
  server.on("/api/i2c", HTTP_GET, [] {
    i2cProbe.ready = false;
    i2cProbe.request = true;
    const uint32_t start = millis();
    while (!i2cProbe.ready && millis() - start < 800) delay(5);
    if (!i2cProbe.ready) { sendReject(503, "bus busy"); return; }
    String j = "{\"ok\":1,\"ack\":[";
    bool first = true;
    for (uint8_t a = 8; a < 120; ++a) {
      if (i2cProbe.code[a - 8] != 0) continue;
      if (!first) j += ",";
      j += String(a);
      first = false;
    }
    j += "],\"codes\":{";
    // Histogram of endTransmission codes: 0 ack, 2 addr nack, 3 data nack, 4 other.
    for (uint8_t c = 0; c < 5; ++c) {
      uint16_t n = 0;
      for (uint8_t a = 8; a < 120; ++a) if (i2cProbe.code[a - 8] == c) ++n;
      if (c) j += ",";
      j += "\"" + String(c) + "\":" + String(n);
    }
    j += "},\"model29\":" + String(int(i2cProbe.model29)) + "}";
    sendJson(j);
  });
  server.on("/api/reset", HTTP_POST, [] { sendJson("{\"ok\":1}"); delay(200); ESP.restart(); });
  server.on("/api/ota-arm", HTTP_POST, handleOtaArm);
  server.on("/api/ota", HTTP_POST, handleOtaDone, handleOtaUpload);
  server.begin();

  udp.begin(4242);
  Serial.println("Web UI on http://bottlesumo (UDP telemetry on 4242)");
}

void webTask(void *arg) {
  (void)arg;
  TickType_t wake = xTaskGetTickCount();
  TaskHealth health;
  while (true) {
    ArduinoOTA.handle();
    server.handleClient();
    health.sample(2, millis());
    lockConfig();
    live.webTicks++;
    unlockConfig();

    // Accept a SUB\n handshake from the desktop debugger.
    int length = udp.parsePacket();
    if (length == 4) {
      char data[8] = {};
      udp.read(data, sizeof(data) - 1);
      if (!memcmp(data, "SUB\n", 4)) {
        subscriberIp = udp.remoteIP();
        subscriberPort = udp.remotePort();
        subscriberAt = millis();
      }
    }

    if (millis() - lastTelemetry >= 20) {
      lastTelemetry = millis();
      Telemetry t{};
      if (xQueuePeek(telemetryQueue, &t, 0)) {
        t.sequence++;
        if (subscriberPort && millis() - subscriberAt < 3000) {
          udp.beginPacket(subscriberIp, subscriberPort);
          udp.write(reinterpret_cast<uint8_t *>(&t), sizeof(t));
          udp.endPacket();
        }
      }
      lockConfig();
      live.wifiClients = WiFi.softAPgetStationNum();
      unlockConfig();
    }

    // Expire the OTA window.
    lockConfig();
    if (live.otaWindowArmed && millis() >= live.otaWindowUntil) live.otaWindowArmed = false;
    unlockConfig();

    vTaskDelayUntil(&wake, pdMS_TO_TICKS(5));
  }
}

} // namespace BS
