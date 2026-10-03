#ifndef _WEB_H_
#define _WEB_H_

#include "user_config.h"

/*
 * Web UI response template.  The firmware formats this page with runtime
 * status/configuration values.  Password fields intentionally remain blank
 * so credentials are never echoed back into the browser source.
 */
#define CONFIG_PAGE "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n\
<!doctype html>\
<html><head><meta name='viewport' content='width=device-width,initial-scale=1'>\
<title>ESP WiFi Repeater</title>\
<style>body{font-family:Arial,sans-serif;max-width:760px;margin:0 auto;padding:14px;line-height:1.4}input,select{font-size:16px;padding:7px;margin:3px 0;width:100%;box-sizing:border-box}button{font-size:16px;padding:9px 14px;margin-top:5px}table{width:auto;border-collapse:collapse}td{padding:5px;border-bottom:1px solid #ddd}td:first-child{width:240px;font-weight:bold}.ok{padding:8px;background:#e8f5e9}.warn{padding:8px;background:#fff3cd}.mono{font-family:monospace}</style>\
</head><body>\
<h1>ESP WiFi Repeater</h1>\
<div class='ok'><b>%s</b></div>\
<h2>Uplink Status</h2>\
<table>\
<tr><td>Status</td><td>%s</td></tr>\
<tr><td>Signal</td><td>%s</td></tr>\
<tr><td>Estimated distance</td><td>%s</td></tr>\
<tr><td>Channel</td><td>%d</td></tr>\
<tr><td>Uplink IP</td><td class='mono'>%s</td></tr>\
<tr><td>Gateway</td><td class='mono'>%s</td></tr>\
<tr><td>Uplink BSSID</td><td class='mono'>%s</td></tr>\
<tr><td>AP clients</td><td>%d</td></tr>\
<tr><td>Free heap</td><td>%d bytes</td></tr>\
</table>\
\
<h2>Uplink Wi-Fi</h2>\
<form action='/' method='GET'>\
<input type='hidden' name='action' value='connect'>\
<label>Router SSID</label>\
<input type='text' name='ssid' value='%s' maxlength='31' autocomplete='off' required>\
<label>Router password</label>\
<input type='password' name='password' value='' maxlength='64' autocomplete='off' placeholder='Leave blank to keep current password'>\
<button type='submit'>Save &amp; Connect</button>\
</form>\
<form action='/' method='GET'>\
<input type='hidden' name='action' value='save'>\
<input type='hidden' name='ssid' value='%s'>\
<label>Router password (optional)</label>\
<input type='password' name='password' value='' maxlength='64' autocomplete='off' placeholder='Leave blank to keep current password'>\
<button type='submit'>Save Only</button>\
</form>\
\
<h2>Repeater AP</h2>\
<form action='/' method='GET'>\
<input type='hidden' name='action' value='save'>\
<label>AP SSID</label>\
<input type='text' name='ap_ssid' value='%s' maxlength='31' required>\
<label>AP password</label>\
<input type='password' name='ap_password' value='' maxlength='63' autocomplete='off' placeholder='Leave blank to keep current password'>\
<label>Security</label>\
<select name='ap_open'><option value='open'%s>Open</option><option value='wpa2'%s>WPA2</option></select>\
<label>AP network</label>\
<input type='text' name='network' value='%s' maxlength='15'>\
<button type='submit'>Save AP Settings</button>\
</form>\
\
<h2>Configuration Lock</h2>\
<form action='/' method='GET'>\
<input type='hidden' name='action' value='lock'>\
<button type='submit'>Lock Configuration</button>\
</form>\
\
<h2>Device Management</h2>\
<form action='/' method='GET'>\
<input type='hidden' name='action' value='restart'>\
<button type='submit'>Restart Device</button>\
</form>\
<p><a href='/'>Refresh status</a></p>\
<div class='warn'><b>Distance note:</b> the distance shown is only a rough RSSI-based estimate. Walls, antenna position, router transmit power and interference can change the real distance substantially.</div>\
</body></html>"

#define LOCK_PAGE "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n\
<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>ESP WiFi Repeater</title></head><body>\
<h1>ESP WiFi Repeater</h1><h2>Configuration Locked</h2>\
<form autocomplete='off' action='/' method='GET'>\
<label>Unlock password</label><input type='password' name='unlock_password' maxlength='63'>\
<button type='submit'>Unlock</button>\
</form><p>The legacy default unlock password is the configured STA password.</p>\
</body></html>"

#endif /* _WEB_H_ */
