// صفحة بصيرة التي تفتح في متصفح الجوال وتنطق الوصف
#pragma once
const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="ar" dir="rtl"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>بصيرة</title>
<style>
:root{--bg:#0d1220;--panel:#151c2e;--line:#26304a;--text:#e9ecf4;--muted:#9aa4bd;--y:#f2c200;--v:#47d1b8;color-scheme:dark}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font-family:Tahoma,"Segoe UI",sans-serif;
padding:20px 16px;display:grid;gap:14px;max-width:520px;margin-inline:auto}
h1{margin:0;font-size:28px;display:flex;align-items:center;gap:10px}
h1 i{width:28px;height:28px;border-radius:50%;background:var(--y);display:inline-block;box-shadow:inset 0 0 0 9px var(--y),inset 0 0 0 14px var(--bg)}
#st{color:var(--muted);font-size:15px}
#tx{background:var(--panel);border:1px solid var(--line);border-radius:14px;padding:20px;font-size:24px;line-height:1.7;min-height:140px}
button{font:inherit;font-size:22px;font-weight:700;border:0;border-radius:14px;padding:22px;width:100%;cursor:pointer}
#go{background:var(--y);color:#111}#snap{background:var(--v);color:#062a23}#again,#cam{background:var(--panel);color:var(--text);border:1px solid var(--line);font-size:18px;padding:16px}
.row{display:grid;grid-template-columns:1fr 1fr;gap:10px}img{width:100%;border-radius:12px;display:none}
small{color:var(--muted);line-height:1.6}
</style></head><body>
<h1><i></i>بصيرة</h1>
<div id="st" role="status">جارٍ الاتصال بالنظارة…</div>
<button id="go">ابدأ — تفعيل الصوت</button>
<div id="tx" aria-live="polite">بصيرة جاهزة</div>
<button id="snap">صوّر الآن</button>
<div class="row"><button id="again">أعد الوصف</button><button id="cam">ماذا ترى الكاميرا؟</button></div>
<img id="pv" alt="صورة الكاميرا الحالية">
<small>اترك هذه الصفحة مفتوحة والشاشة تعمل، واقرن سماعة البلوتوث بالجوال. اضغط زر النظارة لسماع الوصف.</small>
<script>
var started=false,last=-1,wasBusy=false,voice=null,txt='بصيرة جاهزة';
var $=function(i){return document.getElementById(i)};
function pick(){var v=speechSynthesis.getVoices();voice=v.find(function(x){return x.lang==='ar-SA'})||v.find(function(x){return x.lang&&x.lang.indexOf('ar')===0})||null}
if('speechSynthesis' in window){speechSynthesis.onvoiceschanged=pick;pick()}
function say(t){if(!started||!('speechSynthesis' in window))return;speechSynthesis.cancel();
 var u=new SpeechSynthesisUtterance(t);u.lang='ar-SA';if(voice)u.voice=voice;u.rate=1;speechSynthesis.speak(u)}
$('go').onclick=function(){started=true;$('go').textContent='الصوت مفعّل ✓';$('go').style.opacity=.6;say('بصيرة جاهزة. اضغط زر النظارة')};
$('snap').onclick=function(){fetch('/snap')};
$('again').onclick=function(){say(txt)};
$('cam').onclick=function(){var p=$('pv');p.src='/photo?'+Date.now();p.style.display='block'};
function poll(){fetch('/last',{cache:'no-store'}).then(function(r){return r.json()}).then(function(j){
 $('st').textContent=j.busy?'جارٍ وصف المشهد…':'متصلة بالنظارة';
 if(j.busy&&!wasBusy)say('لحظة');wasBusy=j.busy;
 if(last<0){last=j.id}else if(j.id!==last){last=j.id;txt=j.text;$('tx').textContent=txt;say(txt)}
}).catch(function(){$('st').textContent='انقطع الاتصال بالنظارة، تأكد أن نقطة الاتصال تعمل'}).then(function(){setTimeout(poll,700)})}
poll();
</script></body></html>)HTML";
