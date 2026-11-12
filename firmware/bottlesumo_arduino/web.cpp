// web.cpp — Wi-Fi AP, configuration web UI, OTA upload and UDP telemetry.
//
// Safety rules enforced here and in the control task:
//   * Tuning and calibration writes are only accepted while the robot is
//     stopped and no round is running.
//   * OTA is only accepted while the robot is stopped, the OTA window is armed,
//     and the control task has confirmed the motors are at zero.
//   * Handlers never block on motor or sensor work; they only post requests.
#include "web.h"
#include <WebServer.h>
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
  Preferences prefs;
  if (!prefs.begin("bottlesumo", true)) return false;
  WifiConfig loaded{};
  const size_t bytes = prefs.getBytes("wifi", &loaded, sizeof(loaded));
  prefs.end();
  if (bytes != sizeof(loaded) || loaded.magic != WIFI_MAGIC) return false;
  if (strlen(loaded.apPassword) < 8 || strlen(loaded.apPassword) > 63) return false;
  wifiConfig = loaded;
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
:root{--bg:#12161c;--card:#1c232d;--line:#2c3644;--fg:#e6edf3;--dim:#8b9bb0;--ok:#3fb950;--warn:#d29922;--bad:#f85149;--acc:#58a6ff}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
header{padding:12px 16px;border-bottom:1px solid var(--line);display:flex;gap:12px;align-items:center;flex-wrap:wrap}
h1{font-size:17px;margin:0}state{font-weight:600;padding:3px 10px;border-radius:12px;background:#233042}
main{display:grid;grid-template-columns:repeat(auto-fit,minmax(310px,1fr));gap:12px;padding:12px}
section{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:12px}
h2{font-size:14px;margin:0 0 8px;color:var(--acc);text-transform:uppercase;letter-spacing:.04em}
.row{display:flex;justify-content:space-between;gap:8px;padding:2px 0;border-bottom:1px dotted #263140}
.row:last-child{border-bottom:0}.k{color:var(--dim)}.v{font-variant-numeric:tabular-nums;text-align:right}
button{background:#233042;color:var(--fg);border:1px solid var(--line);border-radius:6px;padding:6px 10px;cursor:pointer;margin:2px 2px 2px 0}
button:hover{border-color:var(--acc)}button:disabled{opacity:.45;cursor:not-allowed}
button.primary{background:#1f6feb;border-color:#1f6feb}button.danger{background:#5c1f24;border-color:#8c2f33}
input[type=number],input[type=password],input[type=text]{background:#0d1117;border:1px solid var(--line);color:var(--fg);border-radius:5px;padding:4px 6px;width:76px}
select{background:#0d1117;border:1px solid var(--line);color:var(--fg);border-radius:5px;padding:4px 6px}
input[type=range]{width:100%}label{color:var(--dim);font-size:12px;display:block;margin-top:6px}
.bar{height:16px;background:#0d1117;border-radius:4px;position:relative;overflow:hidden;margin:2px 0}
.bar>i{position:absolute;left:0;top:0;bottom:0;background:var(--acc);display:block}
.bar>e{position:absolute;top:-2px;bottom:-2px;width:2px;background:var(--warn);display:block}
.flag{display:inline-block;min-width:20px;text-align:center;border-radius:3px;background:#0d1117;color:var(--dim);padding:1px 3px;margin-right:3px}
.flag.on{background:var(--bad);color:#fff}
table{width:100%;border-collapse:collapse;font-size:12px}td,th{padding:2px 4px;border-bottom:1px solid #263140;text-align:left}
th{color:var(--dim);font-weight:500}
.ok{color:var(--ok)}.warn{color:var(--warn)}.bad{color:var(--bad)}
.hint{color:var(--dim);font-size:12px;margin-top:6px}
#log{white-space:pre-wrap;font:11px/1.4 ui-monospace,Menlo,monospace;color:var(--dim);max-height:120px;overflow:auto}
</style></head><body>
<header><h1>BottleSumo</h1><state id="state">—</state>
  <span class="k" id="flags"></span><span class="k" id="uptime"></span><span id="link"></span></header>
<main>
<section><h2>感測器 (live)</h2>
  <div id="qre"></div>
  <div id="tof" style="margin-top:8px"></div>
  <div class="hint">綠線 = 目前閾值；紅色旗 = 觸發邊緣。RAW 每 250ms 更新。</div>
</section>

<section><h2>QRE 自我校準</h2>
  <div class="row"><span class="k">狀態</span><span class="v" id="calState">—</span></div>
  <div id="calTable"></div>
  <div style="margin-top:8px">
    <button id="calFloor">1. 採安全面 (3s)</button>
    <button id="calVoid">2. 採落差 (3s)</button>
    <button id="calCancel">取消</button>
  </div>
  <div class="hint">先放車在枱面按「採安全面」；再用支架撐起車體、保持感測器正常高度、四窗都看到真實落差，按「採落差」。每通道間隔需 ≥300 counts。</div>
</section>

<section><h2>驅動力 (電壓) 調校</h2>
  <label>總輸出比例 powerScale <span id="psVal">100</span>%</label>
  <input type="range" id="powerScale" min="20" max="100" value="100">
  <div id="tuning"></div>
  <div style="margin-top:8px"><button class="primary" id="saveTuning">套用並儲存</button>
  <button id="resetTuning">回復預設</button></div>
  <div class="hint">只有在停止狀態才能改。改動即時生效並寫入 NVS，重開機仍保留。</div>
</section>

<section><h2>QRE 手動閾值（覆寫自動）</h2>
  <div id="manualTable"></div>
  <div style="margin-top:8px"><button class="primary" id="saveManual">套用手動閾值</button>
  <button id="clearManual">全部回復自動</button></div>
  <div class="hint">勾選＝該通道停用自動中點，改用你打的數值。「邊界」指越界時 ADC 變大還是變小。這是去年「threshold 錯又無得調」的解法：任何時候都可以手動釘死，並即時見到 RAW 值。</div>
</section>

<section><h2>開局腳本 / 帶樽 / 邊緣</h2>
  <div id="strategy" class="hint">—</div>
</section>

<section><h2>狀態機</h2>
  <div class="row"><span class="k">目前狀態</span><span class="v" id="smState">—</span></div>
  <div class="row"><span class="k">已計時</span><span class="v" id="smElapsed">—</span></div>
  <div style="margin-top:8px"><button id="startBtn">啟動</button><button class="danger" id="stopBtn">停止</button>
  <button id="clearFault">清除 fault</button></div>
  <table style="margin-top:8px"><thead><tr><th>時間</th><th>轉換</th><th>原因</th><th>邊</th></tr></thead>
  <tbody id="history"></tbody></table>
  <div class="hint">最近 64 次狀態轉換；最長可回溯整個比賽。</div>
</section>

<section><h2>OTA 更新</h2>
  <div class="row"><span class="k">窗口</span><span class="v" id="otaWindow">—</span></div>
  <div class="row"><span class="k">進度</span><span class="v" id="otaProg">—</span></div>
  <div style="margin-top:8px"><button id="armOta">開啟 60s 更新窗口</button>
  <input type="file" id="otaFile" accept=".bin"><button class="primary" id="doOta" disabled>上傳並重啟</button></div>
  <div class="hint">必須停止。上傳後新映像會先自我測試（感測器／PSRAM／網路），失敗會自動回復到上一個可開機的 slot。</div>
</section>

<section><h2>馬達測試 (bench)</h2>
  <div class="row"><span class="k">bench 模式</span><span class="v" id="benchState">—</span></div>
  <div class="row"><span class="k">左</span><span class="v" id="benchL">0</span></div>
  <input type="range" id="benchLeft" min="-255" max="255" value="0">
  <div class="row"><span class="k">右</span><span class="v" id="benchR">0</span></div>
  <input type="range" id="benchRight" min="-255" max="255" value="0">
  <div style="margin-top:8px"><button class="danger" id="benchStop">停止</button></div>
  <div class="hint">只在開機時按住 BUTTON2 的 bench 模式出現；自主比賽模式下無法驅動。</div>
</section>

<section><h2>紀錄</h2><div id="log">—</div>
<section>
</main>
<script>
const $=id=>document.getElementById(id);
let tuning={};
function log(m){const d=new Date();$('log').textContent='['+d.toLocaleTimeString()+'] '+m+'\n'+$('log').textContent;}
async function api(p,o){try{const r=await fetch(p,o);const j=await r.json().catch(()=>({}));if(!r.ok){log(p+' → '+(j.error||r.status));return null;}return j;}catch(e){log(p+' 失敗');return null;}}
function bar(v,max,th){const pct=Math.max(0,Math.min(100,v/max*100));const t=th!=null?Math.max(0,Math.min(100,th/max*100)):null;
 return '<div class="bar"><i style="width:'+pct+'%"></i>'+(t!=null?'<e style="left:'+t+'%"></e>':'')+'</div>';}
const QN=['左前','右前','左後','右後'];
function drawQre(d){
 let h='';for(let i=0;i<4;i++){const q=d.qre[i];
  h+='<div class="row"><span class="k">'+QN[i]+'</span><span class="v">'+q+' <span class="flag'+(d.edges&(1<<i)?' on':'')+'">邊</span></span></div>'
   +bar(q,4095,d.thresholds[i]);}
 h+='<div class="row"><span class="k">極性</span><span class="v">'+d.polarity.join(',')+'</span></div>';
 h+='<div class="row"><span class="k">分離度</span><span class="v">'+d.quality.join(',')+'</span></div>';
 $('qre').innerHTML=h;
 let t='';for(let i=0;i<3;i++){t+='<div class="row"><span class="k">'+['左','中','右'][i]+'</span><span class="v">'+(d.distance[i]?d.distance[i]+' mm':'—')+' <span class="k">'+d.rangeAge[i]+'ms</span></span></div>';}
 $('tof').innerHTML=t;
}
function drawCal(d){
 const names=['良好','可用','偏高','不良'];let h='';
 for(let i=0;i<4;i++){
  if(d.manual&&d.manual[i]){h+='<div class="row"><span class="k">'+QN[i]+'</span><span class="v">手動 '+d.manualThreshold[i]+' · '+(d.manualHigh[i]?'高':'低')+'</span></div>';continue;}
  const q=d.quality[i];const cls=q>=3?'ok':(q===2?'':'warn');
  h+='<div class="row"><span class="k">'+QN[i]+'</span><span class="v '+cls+'">ratio '+d.ratio[i].toFixed(2)+' · '+names[q+1]+'</span></div>';}
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
 ['openingEnabled',0,1,1,'開局腳本 0/1'],
 ['openingPwm',0,255,1,'開局 PWM'],
 ['openingForwardMs',0,4000,10,'開局 直行 ms'],
 ['openingTurnMs',0,1500,10,'開局 轉向 ms'],
 ['openingTurnDir',-1,1,2,'開局 方向 -1左/+1右'],
 ['openingForward2Ms',0,4000,10,'開局 第二段直行 ms'],
 ['openingAbortMm',0,900,10,'開局 提早接手距離 mm'],
 ['carryEnabled',0,1,1,'帶樽 0/1'],
 ['carryAfterMs',0,3000,50,'帶樽 接觸多久後開始 ms'],
 ['carryPwm',0,255,1,'帶樽 PWM'],
 ['carryBias',0,80,1,'帶樽 轉向差'],
 ['carryFarMm',100,900,10,'帶樽 放手指距 mm'],
 ['carrySweepMs',100,3000,50,'帶樽 左右切換 ms'],
 ['carryMaxMs',500,8000,100,'帶樽 最長 ms'],
 ['edgeDebounceMs',0,100,5,'邊緣去抖 ms'],
 ['escapeMaxMs',100,2000,50,'逃離最長時間 ms'],
 ['maxEscapesPerRound',0,30,1,'每局最多逃離（0=不限）'],
 ['maxPwm',1,255,1,'PWM 上限']
];
const TDEF={powerScale:100,pushPwm:175,approachNear:115,approachFar:145,searchTurn:80,searchAdvance:100,escapePwm:105,turnPwm:85,unstickPwm:100,pushSteerMax:25,maxPwm:255,openingEnabled:1,openingPwm:140,openingForwardMs:1400,openingTurnMs:380,openingTurnDir:1,openingForward2Ms:1200,openingAbortMm:300,carryEnabled:1,carryAfterMs:700,carryPwm:120,carryBias:28,carrySweepMs:900,carryMaxMs:4000,carryFarMm:260,edgeDebounceMs:25,maxEscapesPerRound:12,escapeMaxMs:700};
function drawTuning(d){
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
  '開局：'+on(d.openingEnabled)+' 直行 '+d.openingForwardMs+'ms → 轉 '+(d.openingTurnDir>0?'右':'左')+' '+d.openingTurnMs+'ms → 直行 '+d.openingForward2Ms+'ms；'+d.openingAbortMm+'mm 內提早接手<br>'+
  '帶樽：'+on(d.carryEnabled)+' 接觸 '+d.carryAfterMs+'ms 後起弧，'+d.carrySweepMs+'ms 換邊，最長 '+d.carryMaxMs+'ms<br>'+
  '邊緣：去抖 '+d.edgeDebounceMs+'ms，每局上限 '+d.maxEscapesPerRound+' 次';
}
function drawManual(d){
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
async function refresh(){
 const s=await api('/api/status');if(!s)return;
 $('state').textContent=s.stateName;
 $('state').style.background=s.active?'#1f6feb':(s.faulted?'#8c2f33':'#233042');
 $('flags').textContent=s.flagText;$('uptime').textContent=(s.uptime_ms/1000).toFixed(1)+'s';
 $('link').innerHTML=s.wifiClients?('<span class="k">client '+s.wifiClients+'</span>'):'';
 $('otaWindow').textContent=s.otaWindow?'已開啟':'未開啟';
 $('benchState').textContent=s.bench?'已啟用（停止時可驅動）':'未啟用（開機按住 BUTTON2）';
 $('startBtn').disabled=!s.canStart;$('stopBtn').disabled=!s.active;
 const r=await api('/api/raw');if(r)drawQre(r);
 const c=await api('/api/config');if(c){drawCal(Object.assign({},c,r||{}));drawTuning(c.tuning);drawStrategy(c.tuning);tuning=c.tuning;$('powerScale').value=tuning.powerScale;$('psVal').textContent=tuning.powerScale;}
 if(r)drawManual(r);
 const hh=await api('/api/history');if(hh)drawHistory(hh);
}
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
$('startBtn').onclick=()=>api('/api/start',{method:'POST'});
$('stopBtn').onclick=()=>api('/api/stop',{method:'POST'});
$('clearFault').onclick=()=>api('/api/fault-clear',{method:'POST'});
$('armOta').onclick=()=>api('/api/ota-arm',{method:'POST'}).then(j=>{if(j)$('doOta').disabled=!j.armed;});
$('otaFile').onchange=e=>{$('doOta').disabled=!e.target.files.length;};
$('doOta').onclick=async()=>{const f=$('otaFile').files[0];if(!f)return;log('OTA 開始 '+f.size+' bytes');
 const r=await fetch('/api/ota',{method:'POST',body:f});log('OTA '+(r.ok?'完成，重啟中':'失敗 '+r.status));};
$('benchLeft').oninput=e=>$('benchL').textContent=e.target.value;
$('benchRight').oninput=e=>$('benchR').textContent=e.target.value;
let benchTimer=null;
function benchSend(){api('/api/bench',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({left:+$('benchLeft').value,right:+$('benchRight').value})});}
$('benchLeft').onchange=$('benchRight').onchange=()=>{clearInterval(benchTimer);benchSend();benchTimer=setInterval(benchSend,200);};
$('benchStop').onclick=()=>{clearInterval(benchTimer);$('benchLeft').value=0;$('benchRight').value=0;$('benchL').textContent=0;$('benchR').textContent=0;benchSend();};
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
  uint16_t stack[4] = {}, hz[4] = {};
  for (uint8_t i = 0; i < 4; ++i) { stack[i] = live.stackFree[i]; hz[i] = live.taskHz[i]; }
  unlockConfig();
  j += ",\"calibrated\":" + String(calibrated ? 1 : 0);
  j += ",\"bench\":" + String(bench ? 1 : 0);
  j += ",\"updating\":" + String(updating ? 1 : 0);
  j += ",\"otaWindow\":" + String(armed ? 1 : 0);
  j += ",\"faulted\":" + String(faulted ? 1 : 0);
  j += ",\"bootVerified\":" + String(bootOk ? 1 : 0);
  j += ",\"otaProgress\":" + String(progress);
  j += ",\"wifiClients\":" + String(clients);
  j += ",\"escapes\":" + String(escapes);
  j += ",\"stack\":[" + String(stack[0]) + "," + String(stack[1]) + "," +
       String(stack[2]) + "," + String(stack[3]) + "]";
  j += ",\"hz\":[" + String(hz[0]) + "," + String(hz[1]) + "," +
       String(hz[2]) + "," + String(hz[3]) + "]";
  j += ",\"bootId\":" + String(bootId);
  j += ",\"stateName\":\"" + String(stateNameFromDisplay(t.state)) + "\"";
  j += ",\"active\":" + String(t.state != 0 ? 1 : 0);
  // The machine must also be in Stopped, or the button looks enabled while
  // /api/start silently does nothing (a machine Fault leaves the round dead).
  j += ",\"canStart\":" + String((calibrated && !updating && !faulted && !bench &&
                                 t.state == 0) ? 1 : 0);
  j += ",\"flagText\":\"" + String(flagText(t.flags)) + "\"";
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
       ",\"openingForwardMs\":" + String(t.openingForwardMs) +
       ",\"openingTurnMs\":" + String(t.openingTurnMs) +
       ",\"openingTurnDir\":" + String(t.openingTurnDir) +
       ",\"openingForward2Ms\":" + String(t.openingForward2Ms) +
       ",\"openingAbortMm\":" + String(t.openingAbortMm) +
       ",\"carryEnabled\":" + String(t.carryEnabled ? 1 : 0) +
       ",\"carryAfterMs\":" + String(t.carryAfterMs) +
       ",\"carryPwm\":" + String(t.carryPwm) +
       ",\"carryBias\":" + String(t.carryBias) +
       ",\"carryFarMm\":" + String(t.carryFarMm) +
       ",\"escapeMaxMs\":" + String(t.escapeMaxMs) +
       ",\"carrySweepMs\":" + String(t.carrySweepMs) +
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
                               "openingEnabled", "openingPwm", "openingForwardMs",
                               "openingTurnMs", "openingTurnDir", "openingForward2Ms",
                               "openingAbortMm", "carryEnabled", "carryAfterMs",
                               "carryPwm", "carryBias", "carrySweepMs", "carryMaxMs",
                               "edgeDebounceMs", "maxEscapesPerRound",
                               "carryFarMm", "escapeMaxMs"};
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
    else if (String(key) == "openingForwardMs") t.openingForwardMs = uint16_t(constrain(value, 0, 4000));
    else if (String(key) == "openingTurnMs") t.openingTurnMs = uint16_t(constrain(value, 0, 1500));
    else if (String(key) == "openingTurnDir") t.openingTurnDir = value < 0 ? -1 : 1;
    else if (String(key) == "openingForward2Ms") t.openingForward2Ms = uint16_t(constrain(value, 0, 4000));
    else if (String(key) == "openingAbortMm") t.openingAbortMm = uint16_t(constrain(value, 0, 900));
    else if (String(key) == "carryEnabled") t.carryEnabled = value != 0;
    else if (String(key) == "carryAfterMs") t.carryAfterMs = uint16_t(constrain(value, 0, 3000));
    else if (String(key) == "carryPwm") t.carryPwm = uint16_t(constrain(value, 0, 255));
    else if (String(key) == "carryBias") t.carryBias = int16_t(constrain(value, -120, 120));
    else if (String(key) == "carrySweepMs") t.carrySweepMs = uint16_t(constrain(value, 50, 3000));
    else if (String(key) == "carryMaxMs") t.carryMaxMs = uint16_t(constrain(value, 200, 8000));
    else if (String(key) == "edgeDebounceMs") t.edgeDebounceMs = uint8_t(constrain(value, 0, 100));
    else if (String(key) == "maxEscapesPerRound") t.maxEscapesPerRound = uint8_t(constrain(value, 0, 30));
    else if (String(key) == "carryFarMm") t.carryFarMm = uint16_t(constrain(value, 100, 900));
    else if (String(key) == "escapeMaxMs") t.escapeMaxMs = uint16_t(constrain(value, 100, 2000));
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
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_AP);
  WiFi.softAP("BottleSumo", wifiConfig.apPassword, wifiConfig.channel, false, 4);
  WiFi.softAPsetHostname("bottlesumo");
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
  server.on("/api/start", HTTP_POST, [] { sendRequest(Command::Start); sendJson("{\"ok\":1}"); });
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
