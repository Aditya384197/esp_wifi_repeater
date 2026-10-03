/* Generated from page.html - dashboard served by web_ui.c */
#ifndef _WEB_PAGE_H_
#define _WEB_PAGE_H_

#define WEB_PAGE \
"HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" \
"<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>WiFi Repeater</title>\n" \
"<style>\n" \
"body{font-family:sans-serif;margin:0;background:#101418;color:#e8ecf0}\n" \
".w{max-width:480px;margin:auto;padding:10px}\n" \
"h1{font-size:19px;margin:6px 0}\n" \
".c{background:#1b222a;border-radius:10px;padding:12px;margin:10px 0}\n" \
"h2{font-size:15px;margin:0 0 8px;color:#8fc4ff}\n" \
"label{display:block;font-size:12px;color:#9aa7b4;margin-top:8px}\n" \
"input,select{width:100%;box-sizing:border-box;padding:9px;border-radius:6px;border:1px solid #33404d;background:#0d1217;color:#fff;font-size:15px}\n" \
"button{padding:10px 12px;margin:8px 6px 0 0;border:0;border-radius:6px;background:#2d7ff9;color:#fff;font-size:14px}\n" \
"button.g{background:#3a4652}button.r{background:#d64545}\n" \
".k{display:flex;justify-content:space-between;padding:4px 0;font-size:14px;border-bottom:1px solid #26303a}\n" \
".k b{font-weight:normal;color:#9aa7b4}\n" \
".b{height:8px;background:#26303a;border-radius:4px;margin:8px 0}\n" \
".b div{height:8px;border-radius:4px;background:#3ccf6e;width:0}\n" \
"#m{position:fixed;left:8px;right:8px;bottom:8px;padding:10px;border-radius:8px;background:#2d7ff9;display:none;text-align:center}\n" \
"</style></head><body><div class=\"w\">\n" \
"<h1>WiFi Repeater <small id=\"vr\"></small></h1>\n" \
"<div id=\"lk\" class=\"c\" style=\"display:none\"><h2>Config locked</h2>\n" \
"<label>Password</label><input id=\"up\" type=\"password\"><button onclick=\"UL()\">Unlock</button></div>\n" \
"<div id=\"mn\">\n" \
"<div class=\"c\"><h2>Status</h2>\n" \
"<div class=\"k\"><b>Uplink</b><span id=\"cn\">...</span></div>\n" \
"<div class=\"k\"><b>Router</b><span id=\"us\">-</span></div>\n" \
"<div class=\"k\"><b>Signal</b><span id=\"rs\">-</span></div>\n" \
"<div class=\"b\" id=\"bar\"><div></div></div>\n" \
"<div class=\"k\"><b>Quality</b><span id=\"qt\">-</span></div>\n" \
"<div class=\"k\"><b>Distance</b><span id=\"ds\">-</span></div>\n" \
"<div class=\"k\"><b>Channel</b><span id=\"ch\">-</span></div>\n" \
"<div class=\"k\"><b>IP / Gateway</b><span><span id=\"ip\">-</span> / <span id=\"gw\">-</span></span></div>\n" \
"<div class=\"k\"><b>Clients</b><span id=\"cl\">-</span></div>\n" \
"<div class=\"k\"><b>Traffic</b><span id=\"tr\">-</span></div>\n" \
"<div class=\"k\"><b>Uptime</b><span id=\"ut\">-</span></div>\n" \
"<div class=\"k\"><b>Free heap</b><span id=\"hp\">-</span></div>\n" \
"</div>\n" \
"<div class=\"c\"><h2>Uplink (your router)</h2>\n" \
"<label>SSID</label><input id=\"ss\" maxlength=\"31\" autocapitalize=\"off\">\n" \
"<select id=\"sl\" size=\"5\" style=\"display:none;margin-top:6px\"></select>\n" \
"<label>Password (empty = open network)</label><input id=\"sp\" type=\"password\" maxlength=\"63\">\n" \
"<label><input type=\"checkbox\" style=\"width:auto\" onclick=\"$('sp').type=this.checked?'text':'password'\"> Show password</label>\n" \
"<button class=\"g\" onclick=\"SC()\">Scan</button><button onclick=\"A('save')\">Save</button><button onclick=\"A('connect')\">Save &amp; Connect</button>\n" \
"<div style=\"font-size:12px;color:#9aa7b4;margin-top:8px\">Scanning briefly interrupts traffic. Connect may drop this page for a few seconds.</div></div>\n" \
"<div class=\"c\"><h2>Repeater WiFi (AP)</h2>\n" \
"<label>SSID</label><input id=\"as\" maxlength=\"31\">\n" \
"<label>Security</label><select id=\"ao\"><option value=\"wpa2\">WPA2</option><option value=\"open\">Open</option></select>\n" \
"<label>Password (8-63 chars)</label><input id=\"ap\" maxlength=\"63\">\n" \
"<button onclick=\"A('save')\">Save</button><button onclick=\"A('reboot')\">Save &amp; Restart</button></div>\n" \
"<div class=\"c\"><h2>Device</h2>\n" \
"<button onclick=\"RB()\">Restart</button><button class=\"g\" onclick=\"LKK()\">Lock config</button><button class=\"r\" onclick=\"FR()\">Factory reset</button></div>\n" \
"</div></div><div id=\"m\"></div>\n" \
"<script>\n" \
"var $=function(i){return document.getElementById(i)},F=0;\n" \
"function g(u,f){var x=new XMLHttpRequest();x.open('GET',u,true);x.timeout=9000;x.onload=function(){var j=null;try{j=JSON.parse(x.responseText)}catch(e){}f(j)};x.onerror=x.ontimeout=function(){f(null)};x.send()}\n" \
"function m(t,e){var d=$('m');d.textContent=t;d.style.background=e?'#d64545':'#2d7ff9';d.style.display='block';setTimeout(function(){d.style.display='none'},4000)}\n" \
"function T(i,v){$(i).textContent=v}\n" \
"function E(p){return encodeURIComponent(p)}\n" \
"var R={2:'Auth expired',4:'Assoc expired',8:'Left',15:'Wrong password',200:'Beacon timeout',201:'Router not found',202:'Auth failed',203:'Assoc failed',204:'Handshake timeout'};\n" \
"function S(){g('/status',function(s){\n" \
"if(!s){T('cn','No response');return}\n" \
"if(s.locked){$('lk').style.display='block';$('mn').style.display='none';return}\n" \
"$('lk').style.display='none';$('mn').style.display='block';\n" \
"if(!F){F=1;$('ss').value=s.ssid;$('sp').value=s.pw;$('as').value=s.ap_ssid;$('ap').value=s.ap_pw;$('ao').value=s.ap_open?'open':'wpa2'}\n" \
"T('vr',s.ver);T('cn',s.conn?'Connected':'Not connected'+(s.dr?' ('+(R[s.dr]||'code '+s.dr)+')':''));T('us',s.ssid);\n" \
"if(s.conn){var q=Math.max(0,Math.min(100,2*(s.rssi+100))),d=Math.pow(10,(-45-s.rssi)/25);\n" \
"T('rs',s.rssi+' dBm ('+q+'%)');$('bar').firstChild.style.width=q+'%';\n" \
"T('qt',s.rssi>-50?'Excellent':s.rssi>-60?'Good':s.rssi>-70?'Fair':s.rssi>-80?'Weak':'Very weak');\n" \
"T('ds','~'+(d<10?d.toFixed(1):Math.round(d))+' m (approx.)')}\n" \
"else{T('rs','-');T('qt','-');T('ds','-');$('bar').firstChild.style.width='0'}\n" \
"T('ch',s.ch);T('ip',s.ip);T('gw',s.gw);T('cl',s.clients);T('tr','down '+s.rx+' KB / up '+s.tx+' KB');\n" \
"var u=s.up;T('ut',Math.floor(u/3600)+'h '+Math.floor(u%3600/60)+'m '+u%60+'s');T('hp',s.heap+' B')})}\n" \
"function W(){m('Restarting... please wait');setTimeout(function(){var t=setInterval(function(){g('/status',function(s){if(s){clearInterval(t);location.reload()}})},3000)},4000)}\n" \
"function A(a){var u='/save?act='+a+'&sta_ssid='+E($('ss').value)+'&sta_pw='+E($('sp').value)+'&ap_ssid='+E($('as').value)+'&ap_pw='+E($('ap').value)+'&ap_sec='+$('ao').value;\n" \
"g(u,function(j){if(!j){if(a=='reboot')W();else m('No response',1);return}m(j.msg,!j.ok);if(j.ok&&j.reboot)W()})}\n" \
"function RB(){if(confirm('Restart now?'))g('/reboot',function(){W()})}\n" \
"function FR(){if(confirm('Reset ALL settings to default?'))g('/factory',function(){W()})}\n" \
"function LKK(){if(confirm('Lock config? Unlock password = uplink WiFi password.'))g('/save?act=save&lock=1',function(j){location.reload()})}\n" \
"function UL(){g('/unlock?pw='+E($('up').value),function(j){if(j&&j.ok)location.reload();else m('Wrong password',1)})}\n" \
"function LS(a){var s=$('sl');s.innerHTML='';s.style.display='block';s.size=Math.min(6,a.length+1);\n" \
"a.forEach(function(e){var o=document.createElement('option');o.value=e.s;o.textContent=e.s+'  ('+e.r+' dBm, ch'+e.c+(e.a?'':', open')+')';s.appendChild(o)});\n" \
"s.onchange=function(){$('ss').value=s.value}}\n" \
"function SC(){m('Scanning...');g('/scan?go=1',function(){var n=0,t=setInterval(function(){g('/scan',function(j){n++;if((j&&!j.busy)||n>8){clearInterval(t);if(j)LS(j.list);else m('Scan failed',1)}})},1500)})}\n" \
"S();setInterval(S,3000);\n" \
"</script></body></html>\n" 

#endif