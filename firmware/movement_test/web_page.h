// Control page served from the robot's own access point, and from the house
// network over STA.
//
// Kept in a header rather than the .ino: PlatformIO's .ino-to-C++ converter
// hoists anything that looks like a function definition, and the JavaScript
// `function send(q){...}` lines in this page were hoisted as C++ prototypes,
// which fails the build with "'function' does not name a type".
#pragma once

static const char PAGE[] PROGMEM = R"rawliteral(<!doctype html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0b0f14">
<title>BottleSumo bench</title>
<style>
:root{color-scheme:dark;--bg:#0b0f14;--card:#111925;--line:#22314a;--fg:#e9eff8;--dim:#8ea3bd;
--accent:#5ee1c1;--warn:#ffb454;--err:#ff6b6b;--r:18px}
*{box-sizing:border-box}
body{margin:0;min-height:100vh;color:var(--fg);font:15px/1.45 ui-sans-serif,system-ui,-apple-system,"Segoe UI",sans-serif;
background:radial-gradient(900px 420px at 50% -8%,#16233c 0%,var(--bg) 62%);padding:16px 14px 30px}
.wrap{max-width:820px;margin:0 auto;display:flex;flex-direction:column;gap:12px}
header{display:flex;align-items:center;justify-content:space-between;gap:10px;padding:2px 2px 0}
h1{font-size:16px;margin:0;letter-spacing:.3px;font-weight:650}
.sub{color:var(--dim);font-size:12px}
.pills{display:flex;gap:6px;flex-wrap:wrap;justify-content:flex-end}
.pill{font:11px/1 ui-monospace,SFMono-Regular,Menlo,monospace;padding:6px 9px;border-radius:999px;
border:1px solid var(--line);color:var(--dim);background:#0d1520}
.pill.ok{color:#04302a;background:var(--accent);border-color:transparent}
.pill.warn{color:#3a2600;background:var(--warn);border-color:transparent}
.pill.bad{color:#3a0d0d;background:var(--err);border-color:transparent}
.cols{display:grid;grid-template-columns:1fr 1fr;gap:12px;align-items:start}
@media(max-width:620px){.cols{grid-template-columns:1fr}}
.card{background:linear-gradient(180deg,#131c2a,#0f1621);border:1px solid var(--line);border-radius:var(--r);
padding:14px}
.card h2{margin:0 0 10px;font-size:11px;letter-spacing:.14em;text-transform:uppercase;color:var(--dim);font-weight:600}
.pad{display:grid;grid-template-columns:repeat(3,76px);grid-template-rows:repeat(3,76px);gap:8px;justify-content:center;margin:2px auto 10px}
.pad button{font-size:22px;border-radius:20px;border:1px solid var(--line);background:#16202e;color:var(--fg);
touch-action:none;user-select:none;-webkit-user-select:none;transition:transform .06s,background .12s}
.pad button:active,.pad button.on{background:var(--accent);color:#04231d;transform:scale(.96)}
#stop{grid-area:2/2;background:#3a1016;border-color:#5c1a23;font-size:12px;letter-spacing:.08em}
#stop:active{background:var(--err);color:#2b0505}
.pad .up{grid-area:1/2}.pad .left{grid-area:2/1}.pad .right{grid-area:2/3}.pad .down{grid-area:3/2}
.duty{display:flex;align-items:center;gap:10px;font-size:12px;color:var(--dim)}
input[type=range]{-webkit-appearance:none;appearance:none;flex:1;height:26px;background:transparent}
input[type=range]::-webkit-slider-runnable-track{height:6px;border-radius:99px;background:#1d2a3d}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:22px;height:22px;margin-top:-8px;border-radius:50%;
background:var(--accent);border:0;box-shadow:0 0 0 4px rgba(94,225,193,.16)}
output{font:600 13px/1 ui-monospace,Menlo,monospace;color:var(--fg);min-width:34px;text-align:right}
.wheel{position:relative}
.wheel .top{display:flex;justify-content:space-between;align-items:center;margin-bottom:2px}
.tag{font:11px/1 ui-monospace,Menlo,monospace;letter-spacing:.12em;color:var(--dim)}
.dot{width:9px;height:9px;border-radius:50%;background:#31425c;display:inline-block;margin-left:6px}
.dot.ok{background:var(--accent);box-shadow:0 0 10px rgba(94,225,193,.65)}
.dot.noise{background:var(--warn)}
.rpm{font:700 42px/1 ui-sans-serif,system-ui,-apple-system,sans-serif;font-variant-numeric:tabular-nums;letter-spacing:-1px;margin:6px 0 2px}
.rpm small{font:500 12px/1 ui-sans-serif;color:var(--dim);letter-spacing:0;margin-left:6px}
.meter{height:8px;border-radius:99px;background:#1a2637;overflow:hidden;margin:8px 0 12px}
.meter i{display:block;height:100%;width:0;border-radius:99px;background:linear-gradient(90deg,#3bb9a0,var(--accent));transition:width .18s}
.kv{display:grid;grid-template-columns:1fr 1fr;gap:6px 12px;margin:0;font-size:12px}
.kv div{display:flex;justify-content:space-between;gap:8px;border-bottom:1px dashed #1c2a3d;padding-bottom:4px}
.kv dt{color:var(--dim)}.kv dd{margin:0;font:600 12px/1.2 ui-monospace,Menlo,monospace}
.tbl{width:100%;border-collapse:collapse;margin-top:10px;font:12px/1.5 ui-monospace,Menlo,monospace}
.tbl th{font:600 10px/1 ui-sans-serif;letter-spacing:.1em;text-transform:uppercase;color:var(--dim);text-align:right;padding:2px 4px}
.tbl th:first-child{text-align:left}
.tbl td{padding:3px 4px;text-align:right;border-top:1px dashed #1c2a3d}
.tbl td:first-child{text-align:left;color:var(--dim)}
.loop .row{display:flex;align-items:center;justify-content:space-between;gap:8px;margin-bottom:8px}
.hint{margin:0 0 10px;color:var(--dim);font-size:12.5px}
.bench{display:grid;grid-template-columns:repeat(6,1fr);gap:7px}
@media(max-width:620px){.bench{grid-template-columns:repeat(4,1fr)}}
.bench button{font-size:11px;line-height:1.25;padding:9px 2px;border-radius:12px;border:1px solid var(--line);
background:#16202e;color:var(--fg);touch-action:manipulation;user-select:none}
.bench button:active{background:#22405a}
.bench button b{display:block;font:600 12px/1.4 ui-monospace,Menlo,monospace;color:var(--accent)}
footer{font:11px/1.7 ui-monospace,Menlo,monospace;color:var(--dim);text-align:center;overflow-wrap:anywhere}
</style></head><body>
<div class="wrap">
<header>
  <div><h1>BottleSumo bench</h1><div class="sub" id="sta">connecting…</div></div>
  <div class="pills"><span class="pill" id="ota">ota idle</span><span class="pill" id="cl">0 clients</span></div>
</header>

<div class="cols">
  <section class="card wheel">
    <div class="top"><span class="tag">LEFT</span><span class="dot" id="dotL"></span></div>
    <div class="rpm"><span id="rL">0</span><small>rpm</small></div>
    <div class="meter"><i id="barL"></i></div>
    <dl class="kv">
      <div><dt>Hz</dt><dd id="hzL">0</dd></div>
      <div><dt>period</dt><dd id="perL">–</dd></div>
      <div><dt>bal trim</dt><dd id="tL">0</dd></div>
      <div><dt>table</dt><dd id="tbL">0</dd></div>
    </dl>
  </section>

  <section class="card wheel">
    <div class="top"><span class="tag">RIGHT</span><span class="dot" id="dotR"></span></div>
    <div class="rpm"><span id="rR">0</span><small>rpm</small></div>
    <div class="meter"><i id="barR"></i></div>
    <dl class="kv">
      <div><dt>Hz</dt><dd id="hzR">0</dd></div>
      <div><dt>period</dt><dd id="perR">–</dd></div>
      <div><dt>bal trim</dt><dd id="tR">0</dd></div>
      <div><dt>table</dt><dd id="tbR">0</dd></div>
    </dl>
  </section>

</div>
<div class="cols">
  <section class="card">
    <h2>Drive · hold to run</h2>
    <div class="pad">
      <button class="up" data-d="f" aria-label="forward">&#9650;</button>
      <button class="left" data-d="l" aria-label="spin left">&#9664;</button>
      <button id="stop" aria-label="stop">STOP</button>
      <button class="right" data-d="r" aria-label="spin right">&#9654;</button>
      <button class="down" data-d="b" aria-label="backward">&#9660;</button>
    </div>
    <div class="duty"><span>duty</span>
      <input id="duty" type="range" min="32" max="255" step="1" value="120" aria-label="duty">
      <output id="dv">120</output></div>
  </section>

  <section class="card loop">
    <div class="row"><h2 style="margin:0">Balance loop</h2><span class="pill" id="bal">off</span></div>
    <p class="hint" id="balhint">…</p>
    <dl class="kv">
      <div><dt>deadband</dt><dd id="db">2 rpm</dd></div>
      <div><dt>limit</dt><dd id="lim">±40</dd></div>
      <div><dt>pulses</dt><dd id="ppr">9 / rev</dd></div>
      <div><dt>PWM</dt><dd id="pwm">20 kHz</dd></div>
      <div><dt>mode</dt><dd>integral</dd></div>
      <div><dt>step</dt><dd>1 / 100 ms</dd></div>
    </dl>
    <table class="tbl"><thead><tr><th>duty</th><th>L</th><th>R</th></tr></thead>
      <tbody id="tbl"></tbody></table>
    <p class="hint" style="margin:8px 0 0">Fixed trim table · keys edit the point nearest the duty slider</p>
  </section>
</div>

<section class="card">
  <h2>Bench keys</h2>
  <div class="bench">
    <button data-k="w">sweep<b>w</b></button>
    <button data-k="k">balance<b>k</b></button>
    <button data-k="p">polarity<b>p</b></button>
    <button data-k="c">pairs<b>c</b></button>
    <button data-k="1">L dir<b>1</b></button>
    <button data-k="2">R dir<b>2</b></button>
    <button data-k="d">both dir<b>d</b></button>
    <button data-k="a">demo<b>a</b></button>
    <button data-k="&lt;">L trim−<b>&lt;</b></button>
    <button data-k="&gt;">L trim+<b>&gt;</b></button>
    <button data-k=",">R trim−<b>,</b></button>
    <button data-k=".">R trim+<b>.</b></button>
  </div>
</section>

<footer id="foot">–</footer>
</div>
<script>
var st=document.getElementById('sta'),dv=document.getElementById('dv');
var timer=null;
function send(q){return fetch(q,{cache:'no-store'}).then(function(r){return r.text();})
 .catch(function(){return 'offline';});}
function start(d){send('/cmd?d='+d);clearInterval(timer);
 timer=setInterval(function(){send('/cmd?d='+d);},100);}
function stopNow(){if(timer){clearInterval(timer);timer=null;}send('/cmd?d=s');}
document.querySelectorAll('[data-d]').forEach(function(b){
 b.addEventListener('pointerdown',function(e){e.preventDefault();b.classList.add('on');start(b.dataset.d);});
 ['pointerup','pointerleave','pointercancel'].forEach(function(ev){
  b.addEventListener(ev,function(){b.classList.remove('on');stopNow();});});
});
document.getElementById('stop').addEventListener('pointerdown',function(e){e.preventDefault();stopNow();});
window.addEventListener('blur',stopNow);
document.addEventListener('visibilitychange',function(){if(document.hidden)stopNow();});
document.querySelectorAll('[data-k]').forEach(function(b){
 b.addEventListener('click',function(){stopNow();
  send('/key?k='+encodeURIComponent(b.dataset.k)).then(draw);});
});
var duty=document.getElementById('duty');
duty.addEventListener('input',function(){dv.textContent=duty.value;});
duty.addEventListener('change',function(){send('/duty?v='+duty.value);});
function ms(us){return us?(us/1000).toFixed(1)+' ms':'–';}
function signed(v){return (v>0?'+':'')+v;}
function draw(){
 fetch('/status',{cache:'no-store'}).then(function(r){return r.json();}).then(function(j){
  dv.textContent=j.duty;
  var scale=Math.max(120,j.rpmL,j.rpmR);
  document.getElementById('rL').textContent=j.rpmL;
  document.getElementById('rR').textContent=j.rpmR;
  document.getElementById('hzL').textContent=j.hzL;
  document.getElementById('hzR').textContent=j.hzR;
  document.getElementById('perL').textContent=ms(j.perL);
  document.getElementById('perR').textContent=ms(j.perR);
  document.getElementById('tL').textContent=signed(j.trimL);
  document.getElementById('tR').textContent=signed(j.trimR);
  var d=j.duty, idx=0, best=999;
  [64,128,192,255].forEach(function(p,k){var q=Math.abs(p-d);if(q<best){best=q;idx=k;}});
  document.getElementById('tbL').textContent=signed(j.tl[idx]);
  document.getElementById('tbR').textContent=signed(j.tr[idx]);
  var rows='';
  [64,128,192,255].forEach(function(p,k){
    rows+='<tr><td>'+p+'</td><td>'+signed(j.tl[k])+'</td><td>'+signed(j.tr[k])+'</td></tr>';});
  document.getElementById('tbl').innerHTML=rows;
  document.getElementById('barL').style.width=(100*j.rpmL/scale)+'%';
  document.getElementById('barR').style.width=(100*j.rpmR/scale)+'%';
  var dL=document.getElementById('dotL'),dR=document.getElementById('dotR');
  dL.className='dot'+(j.hzL?' ok':(j.perL?' noise':''));
  dR.className='dot'+(j.hzR?' ok':(j.perR?' noise':''));
  var bal=document.getElementById('bal');
  bal.textContent=j.bal?'on':'off';
  bal.className='pill '+(j.bal?'ok':'');
  document.getElementById('balhint').textContent=
   (j.rpmL&&j.rpmR)?'Tracking: gap deadband '+j.db+' rpm, correction ±1 per 100 ms.'
   :'No valid pulses on '+(j.rpmL?'right':'left')+': balance idle, trims frozen at 0.';
  document.getElementById('db').textContent=j.db+' rpm';
  document.getElementById('lim').textContent='±'+j.lim;
  document.getElementById('ppr').textContent=j.ppr+' / rev';
  document.getElementById('pwm').textContent=(j.pwmHz/1000)+' kHz';
  var ota=document.getElementById('ota');
  ota.textContent=j.ota?'ota busy':'ota idle';
  ota.className='pill '+(j.ota?'bad':'');
  document.getElementById('cl').textContent=j.clients+' clients';
  document.getElementById('foot').textContent=
   'drive '+j.l+' / '+j.r+' · '+(j.pair?'crossed pair':'netlist pair')+' · '+
   (j.low?'active-low':'active-high')+' · DIR '+(j.dirL?'H':'L')+'/'+(j.dirR?'H':'L')+
   ' · sweep '+(j.sweep?j.sweep+'/4':'idle');
  st.textContent=(j.sta&&j.sta!=='off')?'linked · '+j.sta:'no station link';
 }).catch(function(){st.textContent='no link';});
}
setInterval(draw,500);
draw();
</script></body></html>)rawliteral";
