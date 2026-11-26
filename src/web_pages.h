/*
 * web_pages.h  -  ALL HTML / CSS / JavaScript of the web interface.
 *
 * Page = HTML_HEAD_A + <title> + HTML_HEAD_B + HTML_NAV + PAGE_xxx + HTML_FOOT   (assembled in web_server.cpp)
 *
 *   PAGE_DASHBOARD  http://mydevice.local/          live status, latest JSON, frame log
 *   PAGE_SETTINGS   http://mydevice.local/settings  network interface (WiFi / LAN cable), WiFi networks, server, connected device
 *   PAGE_OTA        http://mydevice.local/ota       firmware update: upload (with model/serial guard) + server-managed update
 *
 * To change the look (colors, menu) edit HTML_HEAD_B / HTML_NAV.
 * The pages talk to the firmware through the /api/... URLs listed in web_server.cpp.
 */
#pragma once
#include "gw_compat.h"

// ======================================================================
// Common parts
// ======================================================================
static const char HTML_HEAD_A[] PROGMEM = R"HTML(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>)HTML";

static const char HTML_HEAD_B[] PROGMEM = R"HTML(</title>
<style>
 body{font-family:system-ui,Arial;margin:0;padding:0 16px 16px;background:#f4f5f7;color:#222}
 .nav{display:flex;gap:4px;margin:0 -16px 14px;padding:10px 16px;background:#0b3d6e;flex-wrap:wrap}
 .nav a{color:#cfe3f7;text-decoration:none;padding:7px 14px;border-radius:6px;font-size:15px}
 .nav a.on{background:#fff;color:#0b3d6e;font-weight:600}
 h2{margin:0 0 8px;font-size:18px} h3{margin:14px 0 6px;font-size:14px;color:#444}
 .card{background:#fff;border-radius:10px;padding:14px;margin-bottom:14px;box-shadow:0 1px 3px #0002}
 button{padding:9px 18px;border:0;border-radius:6px;background:#1a73e8;color:#fff;font-size:15px;cursor:pointer;margin-right:6px}
 button.green{background:#2e7d32} button.red{background:#c62828} button.small{padding:5px 12px;font-size:13px;margin:0}
 button.net{display:block;width:100%;text-align:left;background:#eef3fa;color:#123;margin:0 0 4px;padding:8px 10px;font-size:14px}
 button:disabled{background:#999} progress{width:100%;height:18px;margin-top:10px}
 input[type=text],input[type=password],input[type=number],select{width:100%;box-sizing:border-box;padding:9px;font-size:14px;border:1px solid #bbb;border-radius:6px;margin-bottom:8px;background:#fff}
 label{font-size:12px;color:#555;display:block;margin-bottom:2px}
 .grid{display:grid;grid-template-columns:110px 1fr;gap:0 10px} .grid2{display:grid;grid-template-columns:1fr 1fr;gap:0 10px}
 .row{display:flex;justify-content:space-between;align-items:center;padding:6px 0;border-bottom:1px solid #eee}
 .hint{font-size:12px;color:#777;margin:2px 0 10px} .msg{margin-top:8px;font-size:14px}
 pre{background:#111;color:#8f8;padding:10px;border-radius:6px;max-height:380px;overflow:auto;font-size:12px;white-space:pre-wrap;word-break:break-all}
 #info{font-size:13px;color:#444;line-height:1.7}
</style></head><body>
)HTML";

static const char HTML_NAV[] PROGMEM = R"HTML(<div class="nav"><a href="/">Dashboard</a><a href="/monitor">Patient monitor</a><a href="/devices">Infusion + PCA</a><a href="/settings">Settings</a><a href="/ota">Firmware update</a></div>
)HTML";

static const char HTML_FOOT[] PROGMEM = R"HTML(
<script>
(function(){var a=document.querySelectorAll('.nav a');for(var i=0;i<a.length;i++){if(a[i].getAttribute('href')===location.pathname)a[i].className='on';}})();
</script></body></html>
)HTML";

// ======================================================================
// Dashboard  (/)
// ======================================================================
static const char PAGE_DASHBOARD[] PROGMEM = R"HTML(
<div class="card"><h2>Agilia UART Router</h2><div id="info">loading...</div></div>
<div class="card"><h2>Latest decoded JSON</h2><pre id="latest">waiting for pump data...</pre></div>
<div class="card"><h2>Live frame log</h2><pre id="log">waiting...</pre></div>
<script>
function $(i){return document.getElementById(i);}
function atBottom(p){return p.scrollTop+p.clientHeight>=p.scrollHeight-20;}
function refresh(){
  fetch('/api/log').then(function(r){return r.text();}).then(function(t){
    var p=$('log');var b=atBottom(p);p.textContent=t||'(no data yet)';if(b)p.scrollTop=p.scrollHeight;}).catch(function(){});
  fetch('/api/info').then(function(r){return r.text();}).then(function(t){$('info').innerHTML=t;}).catch(function(){});
  fetch('/api/latest').then(function(r){return r.text();}).then(function(t){
    try{ $('latest').textContent=JSON.stringify(JSON.parse(t),null,1); }catch(e){ $('latest').textContent=t; }
  }).catch(function(){});
}
setInterval(refresh,2000); refresh();
</script>
)HTML";

// ======================================================================
// Patient monitor  (/monitor) : HL7 v2 data received over the LAN (MLLP / TCP)
// ======================================================================
static const char PAGE_MONITOR[] PROGMEM = R"HTML(
<style>
 .tiles{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:10px}
 .tile{background:#0f1c2b;color:#9fe;border-radius:10px;padding:10px 12px}
 .tile .n{font-size:12px;color:#8ab;min-height:30px} .tile .v{font-size:34px;font-weight:700;line-height:1.1}
 .tile .u{font-size:12px;color:#8ab} .tile.H,.tile.HH,.tile.L,.tile.LL,.tile.A{background:#5a1010;color:#fff} .tile.HH,.tile.LL{animation:bl 1s infinite}
 @keyframes bl{50%{opacity:.55}}
 .dot{display:inline-block;width:10px;height:10px;border-radius:50%;background:#bbb;margin-right:6px} .dot.on{background:#2e7d32}
</style>
<div class="card"><h2>HL7 patient monitor</h2><div id="st">loading...</div></div>
<div class="card"><h2>Vital signs <span id="bed" style="font-weight:400;color:#666"></span></h2><div class="tiles" id="tiles"><span class="hint">waiting for HL7 data...</span></div></div>
<div class="card"><h2>Latest decoded JSON</h2><pre id="js">waiting...</pre></div>
<div class="card"><h2>Message log</h2><pre id="lg">waiting...</pre></div>
<script>
function $(i){return document.getElementById(i);}
function esc(s){return String(s).replace(/[&<>"]/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c];});}
function poll(){
  fetch('/api/hl7').then(function(r){return r.json();}).then(function(d){
    $('st').innerHTML='<span class="dot'+(d.connected?' on':'')+'"></span>Monitor <b>'+(d.connected?'connected from '+esc(d.peer):'not connected')+
      '</b> &nbsp; send HL7 (MLLP) to <b>'+esc(d.ip)+':'+d.port+'</b><br>messages OK <b>'+d.ok+'</b> &nbsp; rejected '+d.err+' &nbsp; connections '+d.clients+
      ' &nbsp; last message '+(d.age_s<0?'never':d.age_s+' s ago');
    var L=d.latest; if(!L){return;}
    $('bed').textContent=(L.bed?'Bed '+L.bed+' ':'')+(L.sender?'('+L.sender+')':'');
    var h='';
    (L.observations||[]).forEach(function(o){
      var nm=(o.name||o.code)+(o.sub_id?' ['+o.sub_id+']':'');
      h+='<div class="tile '+esc(o.flag||'')+'"><div class="n">'+esc(nm)+'</div><div class="v">'+esc(o.value)+'</div><div class="u">'+esc(o.unit||'')+(o.flag&&o.flag!='N'?' &nbsp;['+esc(o.flag)+']':'')+'</div></div>';
    });
    $('tiles').innerHTML=h;
    $('js').textContent=JSON.stringify(L,null,1);
  }).catch(function(){});
  fetch('/api/hl7/log').then(function(r){return r.text();}).then(function(t){
    var p=$('lg');var b=p.scrollTop+p.clientHeight>=p.scrollHeight-20;p.textContent=t||'(no messages yet)';if(b)p.scrollTop=p.scrollHeight;}).catch(function(){});
}
setInterval(poll,1000); poll();
</script>
)HTML";

// ======================================================================
// Infusion pump + PCA pump  (/devices) : data from the UART line or from the PC simulator (TCP)
// ======================================================================
static const char PAGE_DEVICES[] PROGMEM = R"HTML(
<style>
 .tiles{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:10px}
 .tile{background:#0f1c2b;color:#9fe;border-radius:10px;padding:10px 12px}
 .tile .n{font-size:12px;color:#8ab;min-height:30px} .tile .v{font-size:28px;font-weight:700;line-height:1.1;word-break:break-word}
 .tile .u{font-size:12px;color:#8ab} .tile.bad{background:#5a1010;color:#fff;animation:bl 1s infinite}
 .tile.run{background:#0d3b1e;color:#bff} .tile.pause{background:#4a3a08;color:#ffe}
 @keyframes bl{50%{opacity:.55}}
 .dot{display:inline-block;width:10px;height:10px;border-radius:50%;background:#bbb;margin-right:6px} .dot.on{background:#2e7d32}
</style>
<div class="card"><h2>Infusion pump <span id="s_inf" style="font-weight:400"></span></h2><div id="st_inf">loading...</div>
 <div class="tiles" id="t_inf"><span class="hint">waiting for data...</span></div>
 <h3>Latest JSON</h3><pre id="js_inf">waiting...</pre><h3>Log</h3><pre id="lg_inf">waiting...</pre></div>
<div class="card"><h2>PCA pump <span id="s_pca" style="font-weight:400"></span></h2><div id="st_pca">loading...</div>
 <div class="tiles" id="t_pca"><span class="hint">waiting for data...</span></div>
 <h3>Latest JSON</h3><pre id="js_pca">waiting...</pre><h3>Log</h3><pre id="lg_pca">waiting...</pre></div>
<script>
function $(i){return document.getElementById(i);}
function esc(s){return String(s).replace(/[&<>"]/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c];});}
function tile(n,v,u,cls){return '<div class="tile '+(cls||'')+'"><div class="n">'+esc(n)+'</div><div class="v">'+esc(v)+'</div><div class="u">'+esc(u||'')+'</div></div>';}
function render(k,L){
  var st=L.status||{},dv=L.device||{},h='';
  var sc=st.alarm_present?'bad':(st.state==='RUNNING'?'run':(st.state==='PAUSED'?'pause':''));
  h+=tile('State',st.state,'serial '+(dv.serial||''),sc);
  h+=tile('Alarm',st.alarm,'',st.alarm_present?'bad':'');
  if(k==='inf'){var x=L.infusion||{};
    h+=tile('Flow rate',x.flow_rate_ml_h,'ml/h')+tile('Infused',x.infused_volume_ml,'of '+x.vtbi_ml+' ml (VTBI)')+tile('Pressure',x.pressure_mmhg,'mmHg')+tile('Drug',x.drug_name,'');
  }else{var p=L.pca||{};
    h+=tile('Drug',p.drug_name,p.concentration_mg_ml+' mg/ml')+tile('Bolus dose',p.bolus_dose_mg,'mg, lockout '+p.lockout_min+' min')+tile('Basal rate',p.basal_rate_mg_h,'mg/h')+
      tile('Total given',p.total_given_mg,'mg (limit '+p.hourly_limit_mg+' mg/h)')+tile('Demands',p.demands_total,'delivered '+p.boluses_delivered);
  }
  $('t_'+k).innerHTML=h;
}
function poll(k){
  fetch('/api/dev?d='+k).then(function(r){return r.json();}).then(function(d){
    $('s_'+k).innerHTML='';
    if(!d.enabled){$('st_'+k).textContent='switched off in config.h';return;}
    var live=d.age_s>=0&&d.age_s<10;
    $('st_'+k).innerHTML='<span class="dot'+((d.connected||live)?' on':'')+'"></span>'+
      (d.uart?'UART line <b>RX GPIO'+d.rx+'</b> (receive only) &nbsp; ':'UART line off &nbsp; ')+
      (d.tcp?'TCP simulator <b>'+esc(d.ip)+':'+d.port+'</b> '+(d.connected?'connected from '+esc(d.peer):'not connected'):'TCP off')+
      '<br>lines OK <b>'+d.ok+'</b> &nbsp; rejected '+d.err+(d.last_error?' (last: '+esc(d.last_error)+')':'')+' &nbsp; last data '+(d.age_s<0?'never':d.age_s+' s ago'+(d.transport?' via '+d.transport:''));
    if(d.latest){render(k,d.latest);$('js_'+k).textContent=JSON.stringify(d.latest,null,1);}
  }).catch(function(){});
  fetch('/api/devlog?d='+k).then(function(r){return r.text();}).then(function(t){
    var p=$('lg_'+k);var b=p.scrollTop+p.clientHeight>=p.scrollHeight-20;p.textContent=t||'(no data yet)';if(b)p.scrollTop=p.scrollHeight;}).catch(function(){});
}
function all(){poll('inf');poll('pca');}
setInterval(all,1000);all();
</script>
)HTML";

// ======================================================================
// Settings  (/settings)  : Network interface + WiFi + Server + Connected device
// ======================================================================
static const char PAGE_SETTINGS[] PROGMEM = R"HTML(
<div class="card"><h2>Network interface</h2>
 <div id="nstat" style="font-size:14px;line-height:1.6;white-space:pre-line">loading...</div>
 <h3>Use</h3>
 <select id="nmode" onchange="netUi()">
  <option value="0">WiFi only</option>
  <option value="1">LAN cable (Ethernet) only - the WiFi radio stays off</option>
  <option value="2">LAN cable first, WiFi as backup</option>
 </select>
 <div id="lanbox">
  <h3>LAN address</h3>
  <select id="ipmode" onchange="netUi()">
   <option value="0">DHCP (address given by the network)</option>
   <option value="1">Static (fixed address)</option>
   <option value="2">DHCP first, static address if no DHCP server answers</option>
  </select>
  <div id="statbox">
   <div class="grid2">
    <div><label>IP address</label><input type="text" id="nip" placeholder="192.168.50.2" autocapitalize="off"></div>
    <div><label>Subnet mask</label><input type="text" id="nmask" placeholder="255.255.255.0" autocapitalize="off"></div>
   </div>
   <div class="grid2">
    <div><label>Gateway (optional)</label><input type="text" id="ngw" placeholder="192.168.50.1" autocapitalize="off"></div>
    <div><label>DNS server (optional)</label><input type="text" id="ndns" placeholder="192.168.50.1" autocapitalize="off"></div>
   </div>
  </div>
  <div id="dhcpbox">
   <label>Wait for DHCP before using the static address (seconds, 5-120)</label>
   <input type="number" id="dhw" min="5" max="120">
   <div class="hint">Office switches can need 30 s or more to open a port after the cable is plugged in. If this page shows the <b>static</b> address instead of an address
    from your office network, raise this value, or choose <b>DHCP</b> (not &quot;DHCP first, static...&quot;) when your network has DHCP.</div>
  </div>
  <h3>LAN MAC address</h3>
  <select id="nmacm" onchange="netUi()">
   <option value="0">Automatic (unique, derived from this chip)</option>
   <option value="1">Fixed address (below)</option>
  </select>
  <div id="macbox">
   <label>MAC address</label>
   <input type="text" id="nmac" placeholder="DE:AD:BE:EF:FE:ED" autocapitalize="characters">
   <div class="hint">Some office networks only let KNOWN devices connect (MAC filtering): then no DHCP answer arrives and the gateway ends on the static address.
    The working reference firmware used the fixed address <b id="refmac"></b>. Use a fixed MAC only if your network needs it, and only on ONE gateway per network: two devices with the same MAC cannot work together.</div>
   <button onclick="$('nmac').value=$('refmac').textContent">Use the reference firmware's MAC</button>
  </div>
  <h3>LAN chip pins (WIZnet W5500 on SPI)</h3>
  <label>Wiring preset</label>
  <select id="npre" onchange="preset()">
   <option value="minew">Minew standalone gateway (pins from the reference firmware, no INT) - default</option>
   <option value="devkit">ESP32-S3 module (WROOM-1U) + W5500, FSPI pins (INT on GPIO4)</option>
   <option value="waveshare">Waveshare ESP32-S3-ETH</option>
   <option value="lilygo">LilyGO T-ETH-Lite ESP32-S3</option>
   <option value="custom">Custom wiring</option>
  </select>
  <div class="grid2">
   <div><label>SCLK (W5500 SCLK)</label><input type="number" id="psclk" min="0" max="48" oninput="$('npre').value='custom'"></div>
   <div><label>MISO (W5500 MISO)</label><input type="number" id="pmiso" min="0" max="48" oninput="$('npre').value='custom'"></div>
  </div>
  <div class="grid2">
   <div><label>MOSI (W5500 MOSI)</label><input type="number" id="pmosi" min="0" max="48" oninput="$('npre').value='custom'"></div>
   <div><label>CS (W5500 SCS)</label><input type="number" id="pcs" min="0" max="48" oninput="$('npre').value='custom'"></div>
  </div>
  <div class="grid2">
   <div><label>INT (W5500 INT, empty = not wired)</label><input type="number" id="pint" min="0" max="48" oninput="$('npre').value='custom'"></div>
   <div><label>RST (W5500 RSTn, empty = not wired)</label><input type="number" id="prst" min="0" max="48" oninput="$('npre').value='custom'"></div>
  </div>
  <label>SPI clock (MHz, 1-40; use 10 with long jumper wires)</label>
  <input type="number" id="pmhz" min="1" max="40">
  <div class="hint">GPIO numbers of the ESP32-S3-WROOM-1(U) module. Usable: 0-21 and 35-48 (35-37 not on N16R8 / N8R8 modules). Not usable: 43/44 (UART0 = the
   debug console), the pump UART pins (GPIO44 by default). Avoid the boot pins 0, 3, 45, 46 and the USB pins 19/20.
   Leave <b>INT</b> empty when the INT pin is not wired (the Minew gateway): the firmware then polls the chip. Wrong pins give &quot;LAN: NO ANSWER&quot; in the serial monitor.</div>
  <div id="probe" style="font-size:14px;white-space:pre-line;margin-top:6px"></div>
  <button onclick="scanLan()">Find the LAN chip</button>
  <div id="scanout"></div>
  <div class="hint">Tries the pin sets of the known wirings and reads the chip's version register. Only possible while the LAN chip is not running. It briefly drives those pins:
   fine on a bench, but check the schematic first if other hardware is wired to them.</div>
  <div class="hint"><b>Cable straight to a Windows PC (no router):</b> choose &quot;DHCP first, static address...&quot; or &quot;Static&quot;, keep 192.168.50.2 / 255.255.255.0 here,
   and give the PC's Ethernet adapter the fixed address 192.168.50.1 / 255.255.255.0. Then set the Server card below to host 192.168.50.1.</div>
 </div>
 <div class="hint">Saving reboots the device (about 15 s). If this page becomes unreachable after a wrong address: switch the gateway's power off and on 5 times quickly (each time within 8 s), or hold the BOOT button for 5 s, to restore the default network settings.</div>
 <button onclick="saveNet()" class="green">Save &amp; reboot</button>
 <div id="nmsg" class="msg"></div>
</div>

<div class="card"><h2>WiFi</h2>
 <div id="wstat" style="font-size:14px">loading...</div>

 <h3>Saved networks (tried first)</h3>
 <div id="wsaved"></div>
 <div class="hint">Built-in fallback networks: <span id="wdef"></span></div>

 <h3>Add / change a network</h3>
 <button onclick="scanWifi()" style="margin-bottom:8px">Scan networks</button>
 <div id="nets"></div>
 <label>SSID (network name)</label>
 <input type="text" id="ssid" autocomplete="off" autocapitalize="off">
 <label>Password (8-63 characters, empty = open network)</label>
 <input type="password" id="pass" autocomplete="off">
 <label style="display:inline"><input type="checkbox" onclick="$('pass').type=this.checked?'text':'password'"> show password</label>
 <div class="hint">Saving reboots the device (about 15 s). If the password is wrong it falls back to the setup hotspot.</div>
 <button onclick="addWifi()" class="green">Save &amp; reconnect</button>
 <div id="wmsg" class="msg"></div>
</div>

<div class="card"><h2>Server (where the JSON is sent)</h2>
 <div class="grid2">
  <div><label>Protocol</label>
   <select id="scheme" onchange="updPrev()"><option value="https">https</option><option value="http">http</option></select></div>
  <div><label>Port (empty = default 443 / 80)</label>
   <input type="number" id="port" min="1" max="65535" oninput="updPrev()"></div>
 </div>
 <label>Server IP address or domain name</label>
 <input type="text" id="host" placeholder="192.168.1.50  or  api.example.com" autocapitalize="off" oninput="updPrev()">
 <label>Path</label>
 <input type="text" id="path" placeholder="/api/v1/data" autocapitalize="off" oninput="updPrev()">
 <label>Optional header name (e.g. Authorization or x-api-key)</label>
 <input type="text" id="hname" autocapitalize="off">
 <label>Optional header value (e.g. Bearer abc123)</label>
 <input type="password" id="hval" autocomplete="off">
 <div class="hint">Full URL used: <b id="prev"></b><br>Leave the IP / domain empty to disable sending. Saved in flash, no reboot needed.</div>
 <button onclick="saveServer()">Save</button><button onclick="sendNow()" class="green">Send latest now</button>
 <div id="smsg" class="msg"></div>
</div>

<div class="card"><h2>Connection test (gateway to server)</h2>
 <div class="hint" style="margin-top:0">Finds out where the way from this gateway to your computer breaks: no IP address, wrong network, firewall, closed port or server error.
  Empty fields = the Server card above (save it first).</div>
 <div class="grid2">
  <div><label>Test host / IP</label><input type="text" id="tth" autocapitalize="off"></div>
  <div><label>Port</label><input type="number" id="ttp" min="1" max="65535"></div>
 </div>
 <label style="display:inline"><input type="checkbox" id="ttpost"> also send one small test message to the Server path</label>
 <div style="margin-top:8px"><button id="tbtn" onclick="runTest()" class="green">Run test</button></div>
 <div id="tout" style="margin-top:8px"></div>
</div>

<div class="card"><h2>Pump serial input (UART)</h2>
 <div id="ustat" style="font-size:14px;line-height:1.6;white-space:pre-line">loading...</div>
 <div class="grid2">
  <div><label>RX pin (receives the pump's TX)</label><input type="number" id="urx" min="0" max="48"></div>
  <div><label>TX pin (empty = none, recommended)</label><input type="number" id="utx" min="0" max="48"></div>
 </div>
 <label>Baud rate (the Agilia pump: 115200)</label>
 <select id="ubaud"><option>9600</option><option>19200</option><option>38400</option><option>57600</option><option>115200</option><option>230400</option><option>460800</option><option>921600</option></select>
 <label style="display:inline"><input type="checkbox" id="uauto"> if no pump data arrives after start-up, find the pump line once by itself</label><br>
 <label style="display:inline"><input type="checkbox" id="ucons"> keep the serial console on TXD0 (GPIO43) while the pump uses RXD0 (GPIO44) - debugging only</label>
 <div style="margin-top:8px"><button onclick="scanPump()">Find the pump line now</button><button onclick="savePump()" class="green">Save &amp; reboot</button></div>
 <div id="uscan" style="margin-top:8px"></div>
 <div id="umsg" class="msg"></div>
 <div class="hint"><b>Your UART line (ESP32-S3-WROOM-1):</b> pad 36 = RXD0 = U0RXD = <b>GPIO44</b> receives; pad 37 = TXD0 = U0TXD = GPIO43 transmits.
  <b>Wiring:</b> pump TX to RXD0 (GPIO44), pump GND to gateway GND. Leave the <b>pump RX line unconnected</b> and TXD0 unused: this firmware only listens.
  UART0 is also the console: while the pump uses it the console is switched off (read the log on the Dashboard page, LAN and test results are on this page).
  For debugging you can keep the console on TXD0 and connect your USB-UART adapter's <b>RX</b> to TXD0 (adapter TX stays unconnected, GND common) - only if the pump's RX wire is NOT on TXD0.
  <b>Find the pump line</b> listens on GPIO44, 43, 17 and 18 for valid pump frames and shows where they arrive.</div>
</div>

<div class="card"><h2>Connected device</h2>
 <div id="dstat" style="font-size:14px;line-height:1.6;white-space:pre-line">loading...</div>
 <label>How the gateway learns what it is connected to</label>
 <select id="dman" onchange="devUi()">
  <option value="0">Detect automatically (Agilia syringe and infusion pumps)</option>
  <option value="1">I type it in (devices the gateway cannot decode, for example a patient monitor)</option>
 </select>
 <div id="dbox">
  <label>Device class</label>
  <select id="dcls">
   <option value="syringe_pump">Syringe pump</option>
   <option value="infusion_pump">Infusion pump</option>
   <option value="patient_monitor">Patient monitor</option>
   <option value="other">Other</option>
  </select>
  <label>Model</label>
  <input type="text" id="dmod" maxlength="40" placeholder="e.g. Philips IntelliVue MX450" autocapitalize="off">
  <label>Serial number</label>
  <input type="text" id="dser" maxlength="30" placeholder="serial number of the device" autocapitalize="off">
 </div>
 <div class="hint">Firmware updates by model or serial number (Firmware update page) use this identity. Saved in flash, no reboot needed.</div>
 <button onclick="saveDev()">Save</button>
 <div id="dmsg" class="msg"></div>
</div>

<script>
function $(i){return document.getElementById(i);}
var HDR={'Content-Type':'application/x-www-form-urlencoded'};
function say(id,ok,t){var m=$(id);m.style.color=ok?'#2e7d32':'#c62828';m.textContent=t;}
function post(url,body,id){
  return fetch(url,{method:'POST',headers:HDR,body:body}).then(function(r){
    return r.text().then(function(t){say(id,r.ok,t);return r.ok;});
  }).catch(function(){say(id,false,'Request failed');return false;});
}

/* ---------------- WiFi ---------------- */
function loadWifi(){
  fetch('/api/wifi').then(function(r){return r.json();}).then(function(w){
    var s=$('wstat');
    if(w.connected) s.textContent='Connected to "'+w.ssid+'"  -  IP '+w.ip;
    else if(w.ap) s.textContent='Not connected. Setup hotspot "'+w.ap_ssid+'" is ON - open http://'+w.ap_ip;
    else s.textContent='Not connected';
    var l=$('wsaved'); l.innerHTML='';
    w.saved.forEach(function(name,i){
      var d=document.createElement('div'); d.className='row';
      var sp=document.createElement('span'); sp.textContent=name;
      var b=document.createElement('button'); b.textContent='Remove'; b.className='red small';
      b.onclick=function(){removeWifi(i,name);};
      d.appendChild(sp); d.appendChild(b); l.appendChild(d);
    });
    if(!w.saved.length) l.textContent='(none saved yet)';
    $('wdef').textContent=w.defaults.length?w.defaults.join(', '):'(disabled)';
  }).catch(function(){});
}
var scanTries=0;
function scanWifi(){ scanTries=0; $('nets').innerHTML=''; say('wmsg',true,'Scanning...'); pollScan(); }
function pollScan(){
  fetch('/api/wifi/scan').then(function(r){return r.json();}).then(function(d){
    if(d.status==='off'){ say('wmsg',false,'WiFi is switched off (LAN-only mode). Change it in Network interface above.'); return; }
    if(d.status==='scanning'){ if(++scanTries<12) setTimeout(pollScan,1500); else say('wmsg',false,'Scan timed out'); return; }
    var box=$('nets'); box.innerHTML='';
    d.networks.sort(function(a,b){return b.rssi-a.rssi;}).forEach(function(n){
      var b=document.createElement('button'); b.className='net';
      b.textContent=n.ssid+'   ('+n.rssi+' dBm'+(n.secure?'':', open')+')';
      b.onclick=function(){$('ssid').value=n.ssid;$('pass').focus();};
      box.appendChild(b);
    });
    say('wmsg',true,d.networks.length+' network(s) found - tap one to select it');
  }).catch(function(){say('wmsg',false,'Scan failed');});
}
function addWifi(){
  var s=$('ssid').value,p=$('pass').value;
  if(!s){say('wmsg',false,'Enter the SSID');return;}
  post('/api/wifi/add','ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p),'wmsg');
}
function removeWifi(i,name){
  if(!confirm('Remove "'+name+'" ? The device will reboot.')) return;
  post('/api/wifi/remove','idx='+i,'wmsg');
}

/* ---------------- Server ---------------- */
function updPrev(){
  var sc=$('scheme').value,h=$('host').value.trim(),p=$('port').value.trim(),pa=$('path').value.trim()||'/';
  var def=(sc==='https')?443:80;
  $('port').placeholder=String(def);
  if(pa.charAt(0)!=='/') pa='/'+pa;
  $('prev').textContent=h?(sc+'://'+h+((p&&parseInt(p,10)!==def)?(':'+p):'')+pa):'(sending disabled - host is empty)';
}
function loadServer(){
  fetch('/api/server').then(function(r){return r.json();}).then(function(c){
    $('scheme').value=c.scheme; $('host').value=c.host; $('port').value=c.port?c.port:'';
    $('path').value=c.path; $('hname').value=c.hname; $('hval').value=c.hval; updPrev();
    $('tth').placeholder=c.host||'IP address of your computer'; $('ttp').placeholder=c.port?c.port:(c.scheme==='https'?443:80);
  }).catch(function(){});
}
function saveServer(){
  var b='scheme='+encodeURIComponent($('scheme').value)+'&host='+encodeURIComponent($('host').value)+
        '&port='+encodeURIComponent($('port').value)+'&path='+encodeURIComponent($('path').value)+
        '&hname='+encodeURIComponent($('hname').value)+'&hval='+encodeURIComponent($('hval').value);
  post('/api/server',b,'smsg');
}
function sendNow(){ post('/api/sendnow','','smsg'); }

/* ---------------- Network interface ---------------- */
var PRESETS={minew:[41,40,39,42,-1,37],devkit:[12,13,11,10,4,5],waveshare:[13,12,11,14,10,9],lilygo:[10,11,12,9,13,14]};   /* sclk,miso,mosi,cs,int,rst  (-1 = not wired) */
function presetOf(p){
  var a=[p.sclk,p.miso,p.mosi,p.cs,p.int,p.rst];
  for(var k in PRESETS){ if(PRESETS[k].join()===a.join()) return k; }
  return 'custom';
}
function preset(){
  var v=PRESETS[$('npre').value]; if(!v) return;
  $('psclk').value=v[0]; $('pmiso').value=v[1]; $('pmosi').value=v[2]; $('pcs').value=v[3]; $('pint').value=(v[4]<0)?'':v[4]; $('prst').value=(v[5]<0)?'':v[5];
}
function netUi(){
  $('lanbox').style.display=($('nmode').value==='0')?'none':'';
  $('statbox').style.display=($('ipmode').value==='0')?'none':'';
  $('dhcpbox').style.display=($('ipmode').value==='2')?'':'none';
  $('macbox').style.display=($('nmacm').value==='1')?'':'none';
}
function showNet(n){
  var t='In use: '+(n.active==='none'?'nothing connected yet':(n.active==='ethernet'?'LAN cable':'WiFi')+'  -  IP '+n.active_ip);
  if(n.running_mode!==0){
    t+='\nLAN: '+n.eth.state;
    if(n.eth.has_ip) t+='\nLAN address '+n.eth.ip+'  mask '+n.eth.mask+'  gateway '+n.eth.gw;
    t+='\nLAN MAC '+n.eth.mac;
  }
  if(n.running_mode!==1) t+='\nWiFi: '+(n.wifi.connected?'connected to "'+n.wifi.ssid+'"  -  IP '+n.wifi.ip:'not connected');
  if(!n.eth_built) t+='\n(This firmware was built without LAN support.)';
  $('nstat').textContent=t;
  $('probe').textContent=n.eth_built?('LAN chip: '+n.probe):'';
}
function loadNet(){
  fetch('/api/net').then(function(r){return r.json();}).then(function(n){
    $('nmode').value=n.mode; $('ipmode').value=n.ipmode;
    $('nip').value=n.ip; $('nmask').value=n.mask; $('ngw').value=n.gw; $('ndns').value=n.dns;
    var p=n.pins; $('psclk').value=p.sclk; $('pmiso').value=p.miso; $('pmosi').value=p.mosi; $('pcs').value=p.cs; $('pint').value=(p.int<0)?'':p.int;
    $('prst').value=(p.rst<0)?'':p.rst; $('pmhz').value=p.mhz; $('npre').value=presetOf(p); $('dhw').value=n.dhcpwait;
    $('nmacm').value=n.macmode; $('nmac').value=n.mac; $('refmac').textContent=n.refmac;
    if(!n.eth_built){ $('nmode').options[1].disabled=true; $('nmode').options[2].disabled=true; }
    netUi(); showNet(n);
  }).catch(function(){});
}
function netStatus(){ fetch('/api/net').then(function(r){return r.json();}).then(showNet).catch(function(){}); }
function saveNet(){
  if(!confirm('Save and reboot the device now?')) return;
  var b='mode='+$('nmode').value+'&ipmode='+$('ipmode').value+'&ip='+encodeURIComponent($('nip').value)+
        '&mask='+encodeURIComponent($('nmask').value)+'&gw='+encodeURIComponent($('ngw').value)+'&dns='+encodeURIComponent($('ndns').value)+
        '&sclk='+encodeURIComponent($('psclk').value)+'&miso='+encodeURIComponent($('pmiso').value)+'&mosi='+encodeURIComponent($('pmosi').value)+
        '&cs='+encodeURIComponent($('pcs').value)+'&int='+encodeURIComponent($('pint').value)+'&rst='+encodeURIComponent($('prst').value)+
        '&mhz='+encodeURIComponent($('pmhz').value)+'&dhcpwait='+encodeURIComponent($('dhw').value)+'&macmode='+$('nmacm').value+'&mac='+encodeURIComponent($('nmac').value.trim());
  post('/api/net',b,'nmsg');
}

/* ---------------- Find the LAN chip ---------------- */
function usePins(p){
  $('psclk').value=p[0]; $('pmiso').value=p[1]; $('pmosi').value=p[2]; $('pcs').value=p[3]; $('pint').value=(p[4]<0)?'':p[4]; $('prst').value=(p[5]<0)?'':p[5];
  $('npre').value=presetOf({sclk:p[0],miso:p[1],mosi:p[2],cs:p[3],int:p[4],rst:p[5]});
  say('nmsg',true,'Pins filled in. Press "Save & reboot" to use them.');
}
function scanLan(){
  if(!confirm('This briefly drives the SPI pins (and the reset pin) of each known wiring. Fine on a bench; check the schematic first if other hardware is wired to those pins. Continue?')) return;
  var o=$('scanout'); o.textContent='Scanning (about 5 seconds)...';
  fetch('/api/lan/scan',{method:'POST'}).then(function(r){return r.json();}).then(function(d){
    o.textContent='';
    if(!d.allowed){ o.textContent=d.message; return; }
    d.results.forEach(function(r){
      var row=document.createElement('div'); row.style.cssText='font-size:13px;padding:5px 0;border-bottom:1px solid #eee';
      var t=document.createElement('div'); t.style.color=r.ok?'#2e7d32':'#c62828';
      t.textContent=(r.ok?'FOUND  ':'no answer  ')+r.name+'  (SCLK '+r.pins[0]+', MISO '+r.pins[1]+', MOSI '+r.pins[2]+', CS '+r.pins[3]+', INT '+r.pins[4]+', RST '+r.pins[5]+')';
      row.appendChild(t);
      var x=document.createElement('div'); x.style.color='#555'; x.textContent=r.text; row.appendChild(x);
      if(r.ok){ var b=document.createElement('button'); b.textContent='Use these pins'; b.onclick=function(){usePins(r.pins);}; row.appendChild(b); }
      o.appendChild(row);
    });
    var any=d.results.some(function(r){return r.ok;});
    if(!any){ var n=document.createElement('div'); n.style.cssText='color:#c62828;margin-top:6px;font-size:13px';
      n.textContent='No W5500 answered on any known wiring. Your board uses other pins (see its schematic or the maker\'s pin table), or the chip has no 3.3 V / is not a W5500.'; o.appendChild(n); }
  }).catch(function(){ o.textContent='Scan failed'; });
}

/* ---------------- Connection test ---------------- */
var COL={o:'#2e7d32',f:'#c62828',w:'#b26a00',i:'#444'}, TAG={o:'OK    ',f:'FAIL  ',w:'NOTE  ',i:''};
function showTest(d){
  var o=$('tout'); o.textContent='';
  d.lines.forEach(function(l){
    var p=document.createElement('div'); p.style.cssText='font-size:14px;padding:3px 0;color:'+COL[l.k]+(l.k==='f'?';font-weight:600':'');
    p.textContent=TAG[l.k]+l.t; o.appendChild(p);
  });
  if(d.running){ var r=document.createElement('div'); r.style.color='#888'; r.textContent='...'; o.appendChild(r); }
}
function pollTest(){
  fetch('/api/diag').then(function(r){return r.json();}).then(function(d){
    showTest(d);
    if(d.running) setTimeout(pollTest,700); else $('tbtn').disabled=false;
  }).catch(function(){ $('tbtn').disabled=false; });
}
function runTest(){
  var b='host='+encodeURIComponent($('tth').value.trim())+'&port='+encodeURIComponent($('ttp').value)+'&post='+($('ttpost').checked?'1':'0');
  $('tbtn').disabled=true; $('tout').textContent='Starting...';
  fetch('/api/diag/run',{method:'POST',headers:HDR,body:b}).then(function(r){
    return r.text().then(function(t){ if(!r.ok){ $('tout').textContent=t; $('tbtn').disabled=false; } else pollTest(); });
  }).catch(function(){ $('tout').textContent='Request failed'; $('tbtn').disabled=false; });
}

/* ---------------- Pump serial input ---------------- */
function showPump(d){
  var l=d.live, t='Listening on GPIO'+l.rx+' at '+l.baud+' baud.\n';
  if(l.frames>0) t+='RECEIVING: '+l.frames+' valid frames, '+l.bytes+' bytes, '+l.crc+' CRC errors, last frame '+l.age_s+' s ago.';
  else if(l.bytes>0) t+='Bytes arrive ('+l.bytes+') but no valid frame yet: wrong baud rate, noise or RS-232 levels.';
  else t+='NO BYTES received yet on this pin: check the wiring (pump TX to the RX pin, common GND) or press "Find the pump line".';
  if(l.console_off) t+='\nSerial console is OFF (the pump uses GPIO43/44).'; else if(l.rx==44||l.rx==43) t+='\nSerial console is kept ON (debugging).';
  if(d.scan.note) t+='\n'+d.scan.note;
  $('ustat').textContent=t;
  showScan(d.scan);
}
function showScan(s){
  var o=$('uscan'); o.textContent='';
  if(s.running){ o.textContent='Listening on each pin for a few seconds ...'; }
  s.results.forEach(function(r){
    var row=document.createElement('div'); row.style.cssText='font-size:13px;padding:5px 0;border-bottom:1px solid #eee';
    var t=document.createElement('div'); t.style.color=r.frames>0?'#2e7d32':(r.bytes>0?'#b26a00':'#888');
    t.textContent=(r.frames>0?'PUMP FOUND  ':(r.bytes>0?'data, no valid frame  ':'nothing  '))+r.label+':  '+r.bytes+' bytes, '+r.frames+' valid frames, '+r.crc+' CRC errors'+(r.idle?'':'  (line held LOW)');
    row.appendChild(t);
    if(r.frames>0){ var b=document.createElement('button'); b.textContent='Use GPIO'+r.pin; b.onclick=function(){ $('urx').value=r.pin; $('utx').value=''; say('umsg',true,'Pin filled in. Press "Save & reboot" to use it.'); }; row.appendChild(b); }
    o.appendChild(row);
  });
}
function loadPump(){
  fetch('/api/pump').then(function(r){return r.json();}).then(function(d){
    $('urx').value=d.cfg.rx; $('utx').value=(d.cfg.tx<0)?'':d.cfg.tx; $('ubaud').value=d.cfg.baud; $('uauto').checked=d.cfg.auto; $('ucons').checked=d.cfg.console;
    showPump(d);
  }).catch(function(){});
}
function pumpStatus(){ fetch('/api/pump').then(function(r){return r.json();}).then(showPump).catch(function(){}); }
function scanPump(){
  if(!confirm('This listens on GPIO44, 43, 17 and 18 for about 15 seconds. Keep the pump connected and powered on. The serial console may stop for a moment. Continue?')) return;
  post('/api/pump/scan','','umsg').then(function(){ setTimeout(pumpStatus,1000); });
}
function savePump(){
  var b='rx='+encodeURIComponent($('urx').value)+'&tx='+encodeURIComponent($('utx').value)+'&baud='+$('ubaud').value+'&auto='+($('uauto').checked?'1':'0')+'&console='+($('ucons').checked?'1':'0');
  if(!confirm('Save and reboot the device now?')) return;
  post('/api/pump',b,'umsg');
}

/* ---------------- Connected device ---------------- */
function devUi(){ $('dbox').style.display=($('dman').value==='1')?'':'none'; }
function showDev(d){
  var e=d.effective, t;
  if(e.known) t=e.model+'   serial '+(e.serial||'-')+'\n'+e.cls_text+(e.manual?' (typed in)':(e.live?' (detected from the data it sends)':' (last seen '+Math.round(e.age_s/60)+' min ago - not live)'))+(e.version?'   device software '+e.version:'');
  else t='Not known yet: no frame from the device has been decoded. Power the device on, or type it in below.';
  $('dstat').textContent=t;
}
function loadDev(){
  fetch('/api/device').then(function(r){return r.json();}).then(function(d){
    $('dman').value=d.cfg.manual?'1':'0'; $('dcls').value=d.cfg.cls||'other'; $('dmod').value=d.cfg.model; $('dser').value=d.cfg.serial;
    devUi(); showDev(d);
  }).catch(function(){});
}
function devStatus(){ fetch('/api/device').then(function(r){return r.json();}).then(showDev).catch(function(){}); }
function saveDev(){
  var b='manual='+$('dman').value+'&cls='+encodeURIComponent($('dcls').value)+'&model='+encodeURIComponent($('dmod').value)+'&serial='+encodeURIComponent($('dser').value);
  post('/api/device',b,'dmsg').then(devStatus);
}

loadNet(); loadDev(); loadPump(); setInterval(netStatus,5000); setInterval(devStatus,5000); setInterval(pumpStatus,2500);
loadWifi(); loadServer(); setInterval(loadWifi,10000);
</script>
)HTML";

// ======================================================================
// Firmware update  (/ota)
// ======================================================================
static const char PAGE_OTA[] PROGMEM = R"HTML(
<div class="card"><h2>Connected device</h2>
 <div id="odev" style="font-size:14px;line-height:1.6;white-space:pre-line">loading...</div>
 <div class="hint">Firmware updates can be limited to a device class, model or serial number, so a gateway is only updated when it is connected to the device you mean.
  Change or type in the device on the Settings page.</div>
</div>

<div class="card"><h2>Firmware update (OTA) - upload a file</h2>
 <p style="font-size:13px;color:#555">Build the project in VS Code, then select <b>firmware.bin</b> from<br>
 <code>.pio/build/wroom1u_4mb/firmware.bin</code> (the folder is named after your PlatformIO environment)</p>
 <input type="file" id="fw" accept=".bin">
 <h3>Only install if the connected device matches (optional)</h3>
 <div class="grid2">
  <div><label>Device class</label>
   <select id="tcls"><option value="">any</option><option value="syringe_pump">Syringe pump</option><option value="infusion_pump">Infusion pump</option>
    <option value="patient_monitor">Patient monitor</option><option value="other">Other</option></select></div>
  <div><label>Model (for example Volumat*)</label><input type="text" id="tmodel" autocapitalize="off"></div>
 </div>
 <label>Serial number (for example 018090/22083105X  or  018090/*)</label>
 <input type="text" id="tserial" autocapitalize="off">
 <div class="hint">Empty = update whatever is connected. <b>*</b> = any text, <b>?</b> = one character, upper/lower case is ignored.
  If the connected device does not match, the gateway refuses the file and nothing is changed.</div>
 <button id="btn" onclick="upload()">Upload &amp; Update</button>
 <progress id="pr" value="0" max="100" style="display:none"></progress>
 <div id="msg" class="msg"></div>
 <div class="hint">The device reboots by itself when the update is finished. Settings (network, WiFi, server, device) are kept.</div>
</div>

<div class="card"><h2>Server-managed update (by device model / serial number)</h2>
 <div id="fstat" style="font-size:14px;line-height:1.6;white-space:pre-line">loading...</div>
 <label>Mode</label>
 <select id="fmode">
  <option value="0">Off</option>
  <option value="1">Check for updates, I press Install</option>
  <option value="2">Check and install automatically</option>
 </select>
 <label>Update server URL (empty = the server from the Settings page)</label>
 <input type="text" id="fbase" placeholder="http://192.168.50.1:8080" autocapitalize="off">
 <div class="grid2">
  <div><label>Check path</label><input type="text" id="fpath" autocapitalize="off"></div>
  <div><label>Check every (minutes)</label><input type="number" id="fint" min="1" max="1440"></div>
 </div>
 <div class="hint">The gateway tells the server its device class, model and serial number; the server answers which firmware (if any) this device should run.
  The file is checked against a SHA-256 before it is installed. Automatic install waits while the device reports an infusion in progress.</div>
 <button onclick="saveFleet()">Save</button><button onclick="fleetCheck()" class="green">Check now</button><button id="finst" onclick="fleetInstall()" class="red" style="display:none">Install now</button>
 <progress id="fpr" value="0" max="100" style="display:none"></progress>
 <div id="fmsg" class="msg"></div>
</div>

<script>
function $(i){return document.getElementById(i);}
var HDR={'Content-Type':'application/x-www-form-urlencoded'};
function say(id,ok,t){var m=$(id);m.style.color=ok?'#2e7d32':'#c62828';m.textContent=t;}
function post(url,body,id){
  return fetch(url,{method:'POST',headers:HDR,body:body}).then(function(r){
    return r.text().then(function(t){say(id,r.ok,t);return r.ok;});
  }).catch(function(){say(id,false,'Request failed');return false;});
}

/* ---- connected device ---- */
function showDev(){
  fetch('/api/device').then(function(r){return r.json();}).then(function(d){
    var e=d.effective, t;
    if(e.known) t=e.model+'   serial '+(e.serial||'-')+'\n'+e.cls_text+(e.manual?' (typed in)':(e.live?' (detected from the data it sends)':' (last seen '+Math.round(e.age_s/60)+' min ago - not live)'))+(e.infusing?'\nInfusion in progress':'');
    else t='Not known yet: no frame from the device has been decoded. Power the device on, or type it in on the Settings page.';
    $('odev').textContent=t;
  }).catch(function(){});
}

/* ---- upload with target guard ---- */
function upload(){
  var f=$('fw').files[0];
  if(!f){alert('Select firmware.bin first');return;}
  var q=[];
  if($('tcls').value) q.push('tcls='+encodeURIComponent($('tcls').value));
  if($('tmodel').value.trim()) q.push('tmodel='+encodeURIComponent($('tmodel').value.trim()));
  if($('tserial').value.trim()) q.push('tserial='+encodeURIComponent($('tserial').value.trim()));
  var fd=new FormData(); fd.append('firmware',f,f.name);
  var x=new XMLHttpRequest(), pr=$('pr'), m=$('msg');
  pr.style.display='block'; $('btn').disabled=true; m.style.color='#222'; m.textContent='';
  x.upload.onprogress=function(e){ if(e.lengthComputable) pr.value=e.loaded*100/e.total; };
  x.onload=function(){
    m.style.color=(x.status==200)?'#2e7d32':'#c62828';
    m.textContent=x.responseText;
    if(x.status==200){ m.textContent+=' - waiting for device to reboot...'; setTimeout(function(){location.href='/';},10000); }
    else $('btn').disabled=false;
  };
  x.onerror=function(){ m.style.color='#c62828'; m.textContent='Upload error'; $('btn').disabled=false; };
  x.open('POST','/update'+(q.length?'?'+q.join('&'):'')); x.send(fd);
}

/* ---- server-managed update ---- */
function loadFleetCfg(){
  fetch('/api/fleet').then(function(r){return r.json();}).then(function(d){
    $('fmode').value=d.cfg.mode; $('fbase').value=d.cfg.base; $('fpath').value=d.cfg.path; $('fint').value=d.cfg.interval;
    $('fbase').placeholder=d.cfg.derived_base?('empty = '+d.cfg.derived_base):'http://192.168.50.1:8080';
    showFleet(d.status);
  }).catch(function(){});
}
function showFleet(s){
  var t='Running firmware '+s.running+'\n';
  if(s.mode===0) t+='Server-managed update is OFF.';
  else {
    t+=s.status;
    if(s.checked) t+='\nLast check '+s.ago_s+' s ago'+(s.next_s?', next in '+s.next_s+' s':'');
    if(s.available) t+='\nOffered: '+s.version+' ('+Math.round(s.size/1024)+' kB)'+(s.rule?'  rule: '+s.rule:'')+(s.force?'  [forced]':'');
    if(s.last_result) t+='\nLast install result: '+s.last_result;
  }
  $('fstat').textContent=t;
  $('finst').style.display=(s.mode!==0&&s.available&&!s.installing)?'':'none';
  var pr=$('fpr'); pr.style.display=s.installing?'block':'none'; pr.value=s.progress;
}
function fleetStatus(){ fetch('/api/fleet').then(function(r){return r.json();}).then(function(d){showFleet(d.status);}).catch(function(){}); }
function saveFleet(){
  var b='mode='+$('fmode').value+'&base='+encodeURIComponent($('fbase').value)+'&path='+encodeURIComponent($('fpath').value)+'&interval='+encodeURIComponent($('fint').value);
  post('/api/fleet',b,'fmsg').then(fleetStatus);
}
function fleetCheck(){ post('/api/fleet/check','','fmsg').then(function(){setTimeout(fleetStatus,1500);}); }
function fleetInstall(){
  if(!confirm('Download and install the offered firmware now? The device reboots when it is done.')) return;
  post('/api/fleet/install','','fmsg');
}
showDev(); loadFleetCfg(); setInterval(showDev,5000); setInterval(fleetStatus,3000);
</script>
)HTML";
