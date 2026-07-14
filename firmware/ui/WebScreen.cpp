#include "UITask.h"
#include "Theme.h"
#include <WiFi.h>
#include <WebServer.h>

/*
 * Remote screen: a tiny web server that mirrors the T-Deck's framebuffer to a
 * browser over WiFi and feeds clicks/keystrokes back as input. All our own
 * code (no GPL sources) so MeshDeck stays MIT.
 *
 *   GET /                -> the viewer page (big canvas + on-screen controls)
 *   GET /screen.rgb565   -> raw RGB565 framebuffer (width*height*2 bytes)
 *   GET /tap?x=&y=       -> inject a screen tap
 *   GET /key?c=          -> inject a key (ASCII code)
 *   GET /nav?e=          -> inject a nav event (1=up 2=down 3=left 4=right 5=select 6=back)
 */

static const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MeshDeck remote</title>
<style>
body{background:#0d0f12;color:#cdd6de;font-family:system-ui,sans-serif;text-align:center;margin:0;padding:12px}
h3{font-weight:600;margin:8px}
canvas{border:2px solid #2b3138;border-radius:8px;image-rendering:pixelated;
 width:min(640px,96vw);aspect-ratio:4/3;height:auto;touch-action:none;cursor:pointer;background:#000}
.pad{margin:14px auto;display:grid;grid-template-columns:repeat(3,72px);gap:8px;justify-content:center}
button{font-size:20px;padding:14px 0;border:0;border-radius:8px;background:#222a31;color:#e6edf3;cursor:pointer}
button:active{background:#3a4650}
#t{margin-top:6px;padding:12px;width:min(640px,96vw);box-sizing:border-box;border-radius:8px;
 border:1px solid #333;background:#11151a;color:#e6edf3;font-size:16px}
.hint{color:#8a949c;font-size:13px;margin-top:8px}
</style></head><body>
<h3>MeshDeck &mdash; remote screen</h3>
<canvas id="s" width="320" height="240"></canvas>
<div class="pad">
 <span></span><button data-nav="1">&#9650;</button><span></span>
 <button data-nav="3">&#9664;</button><button data-nav="5">OK</button><button data-nav="4">&#9654;</button>
 <span></span><button data-nav="2">&#9660;</button><span></span>
 <button data-nav="6">Back</button><button data-key="8">&#9003;</button><button data-key="13">Enter</button>
</div>
<input id="t" placeholder="tap here to type on the device">
<div class="hint">click the screen to tap &middot; use the buttons or your keyboard</div>
<script>
const c=document.getElementById('s'),g=c.getContext('2d'),W=320,H=240;
const img=g.createImageData(W,H),d=img.data;
async function frame(){
 try{
  const r=await fetch('/screen.rgb565?'+Date.now());
  const b=new Uint8Array(await r.arrayBuffer());
  for(let i=0,j=0;i<W*H;i++,j+=2){const v=b[j]|(b[j+1]<<8);
   d[i*4]=((v>>11)&31)*255/31|0;d[i*4+1]=((v>>5)&63)*255/63|0;d[i*4+2]=(v&31)*255/31|0;d[i*4+3]=255;}
  g.putImageData(img,0,0);
 }catch(e){}
 setTimeout(frame,400);
}
frame();
c.addEventListener('click',e=>{const r=c.getBoundingClientRect();
 fetch('/tap?x='+Math.round((e.clientX-r.left)*W/r.width)+'&y='+Math.round((e.clientY-r.top)*H/r.height));});
document.querySelectorAll('button').forEach(bn=>bn.addEventListener('click',()=>{
 if(bn.dataset.nav)fetch('/nav?e='+bn.dataset.nav);
 else if(bn.dataset.key)fetch('/key?c='+bn.dataset.key);}));
const ti=document.getElementById('t');
ti.addEventListener('input',()=>{for(const ch of ti.value)fetch('/key?c='+ch.charCodeAt(0));ti.value='';});
ti.addEventListener('keydown',e=>{if(e.key==='Enter'){fetch('/key?c=13');e.preventDefault();}
 else if(e.key==='Backspace'&&!ti.value){fetch('/key?c=8');}});
document.addEventListener('keydown',e=>{if(e.target===ti)return;
 const nav={ArrowUp:1,ArrowDown:2,ArrowLeft:3,ArrowRight:4};
 if(nav[e.key]!==undefined){fetch('/nav?e='+nav[e.key]);e.preventDefault();return;}
 const sp={Enter:13,Escape:27,Backspace:8,Tab:9};
 let code=sp[e.key]!==undefined?sp[e.key]:(e.key.length===1?e.key.charCodeAt(0):0);
 if(code){fetch('/key?c='+code);e.preventDefault();}});
</script></body></html>)HTML";

void UITask::startRemoteScreen() {
  if (_remote_on) return;
  if (wifiState() != 2) { toast("Connect WiFi first", C_YELLOW); return; }

  _web = new WebServer(80);

  _web->on("/", [this]() {
    _web->send_P(200, "text/html", PAGE);
  });

  _web->on("/screen.rgb565", [this]() {
    GFXcanvas16& c = cv();
    size_t n = (size_t)c.width() * (size_t)c.height() * 2;
    _web->setContentLength(n);
    _web->send(200, "application/octet-stream", "");
    // Stream the framebuffer in small chunks, yielding between each so the
    // LoRa mesh loop and the RTOS watchdog keep running (a single 150 KB
    // blocking write starves the radio and resets the device).
    WiFiClient client = _web->client();
    const uint8_t* buf = (const uint8_t*)c.getBuffer();
    size_t sent = 0;
    const size_t CHUNK = 1460;                 // ~one TCP segment
    while (sent < n && client.connected()) {
      size_t want = (n - sent) < CHUNK ? (n - sent) : CHUNK;
      size_t wrote = client.write(buf + sent, want);
      if (wrote == 0) { delay(1); }             // TCP buffer full: let it drain
      else sent += wrote;
      yield();                                  // feed the watchdog, run other tasks
    }
  });

  _web->on("/tap", [this]() {
    _inj_x = (int16_t)_web->arg("x").toInt();
    _inj_y = (int16_t)_web->arg("y").toInt();
    _inj_tap = true;
    _web->send(200, "text/plain", "ok");
  });

  _web->on("/key", [this]() {
    _inj_key = (uint8_t)_web->arg("c").toInt();
    _web->send(200, "text/plain", "ok");
  });

  _web->on("/nav", [this]() {
    int e = _web->arg("e").toInt();
    if (e >= 1 && e <= 6) _inj_nav = e;
    _web->send(200, "text/plain", "ok");
  });

  _web->begin();
  _remote_on = true;
  snprintf(_remote_url, sizeof(_remote_url), "http://%s/", WiFi.localIP().toString().c_str());
  toast("Remote screen ON", C_GREEN);
}

void UITask::stopRemoteScreen() {
  if (_web) { _web->stop(); delete _web; _web = nullptr; }
  _remote_url[0] = 0;
  if (_remote_on) toast("Remote screen off", C_YELLOW);
  _remote_on = false;
}

const char* UITask::remoteScreenURL() { return _remote_url; }
