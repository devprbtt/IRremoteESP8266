#include "web_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "config_store.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "telnet_server.h"
#include "wifi_manager.h"

static const char *TAG = "web_server";
static const char *DEFAULT_HOSTNAME = "daikin-d3net";
static const uint16_t DEFAULT_TELNET_PORT = 23;

static void copy_string(char *dst, size_t dst_len, const char *src) {
    if (dst == NULL || dst_len == 0U) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_len - 1U);
    dst[dst_len - 1U] = '\0';
}

static bool reg_is_set(const app_context_t *app, uint8_t index) {
    if (index >= D3NET_MAX_UNITS) {
        return false;
    }
    return (app->config.registered_mask & (1ULL << index)) != 0ULL;
}

static void reg_set(app_context_t *app, uint8_t index, bool on, const char *unit_id) {
    if (index >= D3NET_MAX_UNITS) {
        return;
    }
    if (on) {
        app->config.registered_mask |= (1ULL << index);
        if (unit_id != NULL) {
            copy_string(app->config.registered_ids[index], sizeof(app->config.registered_ids[index]), unit_id);
        }
    } else {
        app->config.registered_mask &= ~(1ULL << index);
        memset(app->config.registered_ids[index], 0, sizeof(app->config.registered_ids[index]));
    }
}

static const char *PAGE_COMMON_HEAD =
    "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>"
    "body{font-family:Segoe UI,Tahoma,Arial,sans-serif;margin:0;background:#0f172a;color:#e2e8f0;}"
    ".wrap{max-width:1100px;margin:0 auto;padding:24px;}"
    "nav{display:flex;flex-wrap:wrap;gap:12px;margin-bottom:16px;}"
    "nav a{color:#0f172a;background:#e2e8f0;padding:8px 12px;border-radius:8px;text-decoration:none;font-weight:600;}"
    "h2,h3{color:#f8fafc;margin:14px 0 8px;}"
    ".card{background:#111827;border:1px solid #1f2937;border-radius:12px;padding:16px;margin:12px 0;}"
    "label{font-size:12px;color:#94a3b8;display:block;margin-top:6px;}"
    "input,select,textarea{width:100%;padding:8px;margin:4px 0;background:#0b1220;color:#e2e8f0;border:1px solid #334155;border-radius:8px;box-sizing:border-box;}"
    "button{background:#22c55e;border:0;color:#0b1220;font-weight:700;padding:8px 14px;border-radius:8px;cursor:pointer;}"
    "button.secondary{background:#38bdf8;}"
    ".row{display:grid;grid-template-columns:1fr 1fr;gap:12px}"
    ".term{background:#000;color:#0f0;font-family:monospace;height:260px;overflow:auto;padding:8px;border-radius:8px}"
    "pre{background:#0b1220;border:1px solid #334155;padding:8px;border-radius:8px;overflow:auto}"
    ".meter{width:100%;height:12px;background:#1f2937;border-radius:8px;overflow:hidden}"
    ".bar{height:12px;background:#25a16b;width:0%}"
    ".badge{display:inline-block;padding:4px 8px;border-radius:10px;font-size:12px}.on{background:#d6f5e3;color:#155724}.off{background:#fde0e0;color:#7b1c1c}"
    "</style></head><body><div class='wrap'>"
    "<nav><a href='/d3net'>D3Net</a><a href='/config'>Config</a><a href='/rtu'>RTU</a><a href='/monitor'>Monitor</a><a href='/firmware'>Firmware</a></nav>";

static const char *PAGE_FOOTER = "</div></body></html>";

static const char *PAGE_D3NET =
    "<h2>Daikin D3Net Controller</h2>"
    "<div class='card'><b>Status</b><div id='wifi_status'>loading...</div></div>"
    "<div class='card'><b>Discovery & Registry</b><button onclick='discover()'>Scan Units</button> <button onclick='loadRegistry()'>Refresh Registry</button><div id='registry'></div></div>"
    "<script>"
    "const modes=[[0,'FAN'],[1,'HEAT'],[2,'COOL'],[3,'AUTO'],[4,'VENT'],[7,'DRY']];const fanSpeeds=[[0,'AUTO'],[1,'LOW'],[2,'LOW_MED'],[3,'MED'],[4,'HI_MED'],[5,'HIGH']];const fanDirs=[[0,'P0'],[1,'P1'],[2,'P2'],[3,'P3'],[4,'P4'],[6,'STOP'],[7,'SWING']];"
    "function badge(on){return `<span class='badge ${on?'on':'off'}'>${on?'ONLINE':'OFFLINE'}</span>`}"
    "async function j(u,o){let r=await fetch(u,o);return r.json()}"
    "async function refresh(){let s=await j('/api/status');document.getElementById('wifi_status').innerText=`STA:${s.wifi.connected?'connected':'disconnected'} ${s.wifi.ip||''} | Hostname:${s.hostname||''}.local | Telnet:${s.telnet_port||23}`;}"
    "function opt(list){return list.map(([v,l])=>`<option value='${v}'>${l}</option>`).join('')}"
    "function renderCard(u){const dis=!u.online;return `<div class='card'><div style='display:flex;justify-content:space-between;align-items:center'><b>${u.unit_id||'unit '+u.index}</b>${badge(u.online)}</div>`+`<div>Idx ${u.index} | Registered: ${u.registered?'yes':'no'} <button onclick=\\\"${u.registered?'unreg':'reg'}(${u.index})\\\">${u.registered?'Unregister':'Register'}</button></div>`+`<div>Power ${u.power?'ON':'OFF'} | Mode ${u.mode_name||u.mode} | Cur ${(u.temp_current==null?'-':u.temp_current)} C | Set ${(u.temp_setpoint==null?'-':u.temp_setpoint)} C</div>`+`<div class='row'><select id='p_${u.index}' ${dis?'disabled':''} onchange='setPreview(${u.index})'><option value='1'>Power ON</option><option value='0'>Power OFF</option></select><input id='sp_${u.index}' type='number' step='0.5' placeholder='Setpoint C' ${dis?'disabled':''} oninput='setPreview(${u.index})'></div>`+`<div class='row'><select id='m_${u.index}' ${dis?'disabled':''} onchange='setPreview(${u.index})'>${opt(modes)}</select><select id='fs_${u.index}' ${dis?'disabled':''} onchange='setPreview(${u.index})'>${opt(fanSpeeds)}</select></div>`+`<div class='row'><select id='fd_${u.index}' ${dis?'disabled':''} onchange='setPreview(${u.index})'>${opt(fanDirs)}</select><button ${dis?'disabled':''} onclick='sendCmd(${u.index})'>Send</button></div>`+`<div><button ${dis?'disabled':''} onclick='filterReset(${u.index})'>Filter Reset</button></div>`+`<pre id='json_${u.index}'></pre></div>`;}"
    "async function loadRegistry(){let r=await j('/api/registry');let h='';for(let u of r.units){h+=renderCard(u);setPreview(u.index);}document.getElementById('registry').innerHTML=h||'No units';}"
    "async function discover(){await fetch('/api/discover',{method:'POST'});setTimeout(loadRegistry,500)}"
    "async function reg(i){await fetch('/api/registry',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({index:i,action:'add'})});loadRegistry()}"
    "async function unreg(i){await fetch('/api/registry',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({index:i,action:'remove'})});loadRegistry()}"
    "function buildPayload(i){return {index:i,power:parseInt(document.getElementById('p_'+i).value),mode:parseInt(document.getElementById('m_'+i).value),setpoint:parseFloat(document.getElementById('sp_'+i).value),fan_speed:parseInt(document.getElementById('fs_'+i).value),fan_dir:parseInt(document.getElementById('fd_'+i).value)}}"
    "function setPreview(i){let p=buildPayload(i);document.getElementById('json_'+i).innerText=JSON.stringify(p,null,2)}"
    "async function sendCmd(i){let p=buildPayload(i);let body=[{cmd:'power',value:p.power},{cmd:'mode',value:p.mode},{cmd:'setpoint',value:p.setpoint},{cmd:'fan_speed',value:p.fan_speed},{cmd:'fan_dir',value:p.fan_dir}];document.getElementById('json_'+i).innerText=JSON.stringify(body,null,2);for (let c of body){if(isNaN(c.value)) continue;await fetch('/api/hvac/cmd',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({index:i,cmd:c.cmd,value:c.value})});}setTimeout(loadRegistry,400)}"
    "async function filterReset(i){await fetch('/api/hvac/cmd',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({index:i,cmd:'filter_reset'})});setTimeout(loadRegistry,400)}"
    "setInterval(loadRegistry,3000);refresh();loadRegistry();"
    "</script>";

static const char *PAGE_CONFIG =
    "<h2>WiFi / Network</h2>"
    "<div class='card'><label>WiFi SSID</label><input id='ssid' placeholder='SSID'>"
    "<label>Select from scan</label><div class='row'><select id='ssid_select' onchange='ssid.value=this.value'><option value=''>-- scan to load --</option></select><button type='button' class='secondary' onclick='scanWifi()'>Scan Networks</button></div>"
    "<label>WiFi Password</label><input id='pass' placeholder='Password' type='password'>"
    "<div><label><input id='dhcp' type='checkbox' checked style='width:auto'> DHCP</label></div>"
    "<div class='row'><div><label>Static IP</label><input id='ip'></div><div><label>Gateway</label><input id='gateway'></div></div>"
    "<div class='row'><div><label>Subnet</label><input id='subnet'></div><div><label>DNS</label><input id='dns'></div></div>"
    "<div class='row'><div><label>Hostname (.local)</label><input id='hostname'></div><div><label>Telnet Port</label><input id='telnet_port' type='number' min='1' max='65535'></div></div>"
    "<button onclick='saveNetwork()'>Save & Reboot</button></div>"
    "<script>"
    "async function j(u,o){let r=await fetch(u,o);return r.json()}"
    "function setDhcpState(){const off=!document.getElementById('dhcp').checked;['ip','gateway','subnet','dns'].forEach(id=>{let el=document.getElementById(id);if(el) el.disabled=off;});}"
    "async function loadConfig(){let c=await j('/api/config');ssid.value=c.wifi.ssid||'';pass.value=c.wifi.password||'';dhcp.checked=(c.wifi.dhcp!==false);ip.value=c.wifi.ip||'';gateway.value=c.wifi.gateway||'';subnet.value=c.wifi.subnet||'';dns.value=c.wifi.dns||'';hostname.value=c.hostname||'daikin-d3net';telnet_port.value=c.telnet_port||23;setDhcpState();}"
    "async function scanWifi(){let r=await j('/api/wifi/scan');let list=r.networks||r.items||[];let s=document.getElementById('ssid_select');s.innerHTML='';if(!list.length){let o=document.createElement('option');o.value='';o.text='No APs found';s.appendChild(o);return;}let p=document.createElement('option');p.value='';p.text='Select scanned AP';s.appendChild(p);for(let ap of list){let o=document.createElement('option');o.value=ap.ssid;o.text=`${ap.ssid} (RSSI ${ap.rssi})`;s.appendChild(o);}}"
    "async function saveNetwork(){let selected=document.getElementById('ssid_select').value;let chosen=ssid.value||selected;let body={ssid:chosen,password:pass.value,dhcp:dhcp.checked,ip:ip.value,gateway:gateway.value,subnet:subnet.value,dns:dns.value,hostname:hostname.value,telnet_port:parseInt(telnet_port.value||'23')};await fetch('/api/config/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});alert('Saved. Device will reboot.');}"
    "document.getElementById('dhcp').addEventListener('change',setDhcpState);loadConfig();"
    "</script>";

static const char *PAGE_RTU =
    "<h2>RS485 / Modbus RTU</h2>"
    "<div class='card'>"
    "<div class='row'><div><label>TX pin</label><input id='rtu_tx' type='number' placeholder='17'></div><div><label>RX pin</label><input id='rtu_rx' type='number' placeholder='16'></div></div>"
    "<div class='row'><div><label>DE pin</label><input id='rtu_de' type='number' placeholder='4'></div><div><label>RE pin</label><input id='rtu_re' type='number' placeholder='5'></div></div>"
    "<div class='row'><div><label>Baud rate</label><input id='rtu_baud' type='number' placeholder='19200'></div><div><label>Parity</label><select id='rtu_parity'><option value='N'>Parity None</option><option value='E'>Parity Even</option><option value='O'>Parity Odd</option></select></div></div>"
    "<div class='row'><div><label>Stop bits</label><select id='rtu_stop'><option value='1'>Stop 1</option><option value='2'>Stop 2</option></select></div><div><label>Data bits</label><select id='rtu_bits'><option value='8'>Data 8</option><option value='7'>Data 7</option></select></div></div>"
    "<div class='row'><div><label>Slave ID</label><input id='rtu_slave' type='number' placeholder='1'></div><div><label>Timeout (ms)</label><input id='rtu_timeout' type='number' placeholder='3000'></div></div>"
    "<div class='row'><button class='secondary' onclick='applyDefaults()'>Load D3Net Defaults</button><button onclick='saveRtu()'>Save & Reboot</button></div></div>"
    "<script>"
    "let defaults={tx_pin:17,rx_pin:16,de_pin:4,re_pin:5,baud_rate:19200,parity:'N',stop_bits:2,data_bits:8,slave_id:1,timeout_ms:3000};"
    "async function j(u,o){let r=await fetch(u,o);return r.json()}"
    "function setVals(r){const m=(id,v)=>{let el=document.getElementById(id);if(el) el.value=v};m('rtu_tx',r.tx_pin);m('rtu_rx',r.rx_pin);m('rtu_de',r.de_pin);m('rtu_re',r.re_pin);m('rtu_baud',r.baud_rate);m('rtu_parity',r.parity);m('rtu_stop',r.stop_bits);m('rtu_bits',r.data_bits);m('rtu_slave',r.slave_id);m('rtu_timeout',r.timeout_ms);}"
    "async function loadRtu(){let r=await j('/api/rtu');if(r.defaults) defaults=r.defaults;setVals(r);}"
    "function applyDefaults(){setVals(defaults);}"
    "async function saveRtu(){let body={tx_pin:parseInt(rtu_tx.value),rx_pin:parseInt(rtu_rx.value),de_pin:parseInt(rtu_de.value),re_pin:parseInt(rtu_re.value),baud_rate:parseInt(rtu_baud.value),parity:rtu_parity.value,stop_bits:parseInt(rtu_stop.value),data_bits:parseInt(rtu_bits.value),slave_id:parseInt(rtu_slave.value),timeout_ms:parseInt(rtu_timeout.value)};await fetch('/api/rtu',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});alert('Saved. Device will reboot.');}"
    "loadRtu();"
    "</script>";

static const char *PAGE_MONITOR =
    "<h2>Terminal Monitor</h2>"
    "<div class='card'><div class='row'><button id='toggleBtn' class='secondary' onclick='toggleMon()'>Toggle</button><button onclick='clearMon()'>Clear</button></div>"
    "<p id='monStatus'>status...</p><div class='term' id='term'></div></div>"
    "<script>"
    "let logSeq=0;let monEnabled=true;"
    "async function j(u,o){let r=await fetch(u,o);return r.json()}"
    "function setLabel(){document.getElementById('monStatus').innerText='Monitor: '+(monEnabled?'enabled':'disabled');document.getElementById('toggleBtn').innerText=monEnabled?'Disable Monitor':'Enable Monitor';}"
    "async function loadMon(){let s=await j('/api/monitor');monEnabled=!!s.enabled;setLabel();}"
    "async function toggleMon(){await fetch('/api/monitor/toggle',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({enabled:!monEnabled})});await loadMon();}"
    "async function clearMon(){await fetch('/api/monitor/clear',{method:'POST'});document.getElementById('term').innerText='';logSeq=0;}"
    "async function pollLogs(){let r=await j(`/api/logs?since=${logSeq}`);logSeq=r.latest||logSeq;let t=document.getElementById('term');for(let ln of r.lines){t.innerText+=ln.text;}t.scrollTop=t.scrollHeight;}"
    "setInterval(pollLogs,2000);loadMon();pollLogs();"
    "</script>";

static const char *PAGE_FIRMWARE =
    "<h2>OTA Firmware Update</h2>"
    "<div class='card'><p>Upload firmware binary (.bin).</p>"
    "<input id='fw' type='file' accept='.bin,application/octet-stream'><button id='fwBtn' onclick='uploadFw()'>Upload & Flash</button>"
    "<div class='meter'><div id='ota_bar' class='bar'></div></div><div id='ota_status'>idle</div></div>"
    "<script>"
    "async function uploadFw(){let f=document.getElementById('fw').files[0];if(!f)return;const btn=document.getElementById('fwBtn');btn.disabled=true;const xhr=new XMLHttpRequest();xhr.open('POST','/firmware/update',true);xhr.upload.onprogress=(ev)=>{if(!ev.lengthComputable)return;const p=Math.round((ev.loaded/ev.total)*100);document.getElementById('ota_bar').style.width=p+'%';document.getElementById('ota_status').innerText='Uploading '+p+'%';};xhr.onerror=()=>{btn.disabled=false;document.getElementById('ota_status').innerText='Upload failed';};xhr.onload=()=>{btn.disabled=false;if(xhr.status>=200&&xhr.status<300){document.getElementById('ota_status').innerText='Upload complete. Rebooting...';}else{document.getElementById('ota_status').innerText='Update failed ('+xhr.status+')';}};xhr.send(f);}"
    "</script>";

static esp_err_t http_reply_json(httpd_req_t *req, cJSON *root) {
    char *body = cJSON_PrintUnformatted(root);
    if (body == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    cJSON_Delete(root);
    return err;
}

static int recv_body(httpd_req_t *req, char **out, size_t *out_len) {
    if (req->content_len <= 0) {
        *out = NULL;
        *out_len = 0;
        return ESP_OK;
    }
    char *buf = calloc(1, (size_t)req->content_len + 1U);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int total = 0;
    while (total < req->content_len) {
        int r = httpd_req_recv(req, buf + total, req->content_len - total);
        if (r <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        total += r;
    }
    *out = buf;
    *out_len = (size_t)total;
    return ESP_OK;
}

static esp_err_t send_full_page(httpd_req_t *req, const char *title, const char *body) {
    (void)title;
    httpd_resp_set_type(req, "text/html");
    if (httpd_resp_sendstr_chunk(req, PAGE_COMMON_HEAD) != ESP_OK) return ESP_FAIL;
    if (httpd_resp_sendstr_chunk(req, body) != ESP_OK) return ESP_FAIL;
    if (httpd_resp_sendstr_chunk(req, PAGE_FOOTER) != ESP_OK) return ESP_FAIL;
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t handle_index_get(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/d3net");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t handle_d3net_page_get(httpd_req_t *req) {
    return send_full_page(req, "D3Net", PAGE_D3NET);
}

static esp_err_t handle_config_page_get(httpd_req_t *req) {
    return send_full_page(req, "Config", PAGE_CONFIG);
}

static esp_err_t handle_rtu_page_get(httpd_req_t *req) {
    return send_full_page(req, "RTU", PAGE_RTU);
}

static esp_err_t handle_monitor_page_get(httpd_req_t *req) {
    return send_full_page(req, "Monitor", PAGE_MONITOR);
}

static esp_err_t handle_firmware_page_get(httpd_req_t *req) {
    return send_full_page(req, "Firmware", PAGE_FIRMWARE);
}

static esp_err_t handle_status_get(httpd_req_t *req) {
    app_context_t *app = (app_context_t *)req->user_ctx;
    char ip[32] = {0};
    wifi_manager_sta_ip(ip, sizeof(ip));

    cJSON *root = cJSON_CreateObject();
    cJSON *wifi = cJSON_CreateObject();
    cJSON_AddBoolToObject(wifi, "connected", wifi_manager_sta_connected());
    cJSON_AddStringToObject(wifi, "ip", ip);
    cJSON_AddItemToObject(root, "wifi", wifi);
    cJSON_AddStringToObject(root, "hostname", app->config.hostname[0] ? app->config.hostname : DEFAULT_HOSTNAME);
    cJSON_AddNumberToObject(root, "telnet_port", app->config.telnet_port ? app->config.telnet_port : DEFAULT_TELNET_PORT);

    cJSON *ota = cJSON_CreateObject();
    cJSON_AddBoolToObject(ota, "active", app->ota.active);
    cJSON_AddBoolToObject(ota, "success", app->ota.success);
    cJSON_AddNumberToObject(ota, "bytes_received", (double)app->ota.bytes_received);
    cJSON_AddNumberToObject(ota, "total_bytes", (double)app->ota.total_bytes);
    cJSON_AddStringToObject(ota, "message", app->ota.message);
    cJSON_AddItemToObject(root, "ota", ota);

    return http_reply_json(req, root);
}

static esp_err_t handle_wifi_scan_get(httpd_req_t *req) {
    wifi_scan_item_t items[20];
    size_t count = 0;
    esp_err_t err = wifi_manager_scan(items, 20, &count);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
        return err;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", items[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", items[i].rssi);
        cJSON_AddNumberToObject(item, "auth", items[i].authmode);
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddItemToObject(root, "items", arr);
    cJSON *arr2 = cJSON_Duplicate(arr, 1);
    if (arr2 != NULL) {
        cJSON_AddItemToObject(root, "networks", arr2);
    }
    return http_reply_json(req, root);
}

static esp_err_t handle_wifi_connect_post(httpd_req_t *req) {
    app_context_t *app = (app_context_t *)req->user_ctx;
    char *body = NULL;
    size_t body_len = 0;
    if (recv_body(req, &body, &body_len) != ESP_OK || body == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
        return ESP_FAIL;
    }
    cJSON *json = cJSON_ParseWithLength(body, body_len);
    free(body);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
        return ESP_FAIL;
    }

    cJSON *ssid = cJSON_GetObjectItem(json, "ssid");
    cJSON *pass = cJSON_GetObjectItem(json, "password");
    if (!cJSON_IsString(ssid) || ssid->valuestring == NULL) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid");
        return ESP_FAIL;
    }

    esp_err_t err = wifi_manager_set_sta_network(
        app->config.sta_dhcp, app->config.sta_ip, app->config.sta_gateway, app->config.sta_subnet, app->config.sta_dns);
    if (err == ESP_OK) {
        err = wifi_manager_connect_sta(ssid->valuestring, cJSON_IsString(pass) ? pass->valuestring : "");
    }
    if (err == ESP_OK) {
        copy_string(app->config.sta_ssid, sizeof(app->config.sta_ssid), ssid->valuestring);
        if (cJSON_IsString(pass) && pass->valuestring != NULL) {
            copy_string(app->config.sta_password, sizeof(app->config.sta_password), pass->valuestring);
        } else {
            app->config.sta_password[0] = '\0';
        }
        app->config.sta_configured = true;
        config_store_save(&app->config);
    }
    cJSON_Delete(json);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "connect failed");
        return err;
    }
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t handle_api_config_get(httpd_req_t *req) {
    app_context_t *app = (app_context_t *)req->user_ctx;
    cJSON *root = cJSON_CreateObject();
    cJSON *wifi = cJSON_CreateObject();
    cJSON_AddStringToObject(wifi, "ssid", app->config.sta_ssid);
    cJSON_AddStringToObject(wifi, "password", app->config.sta_password);
    cJSON_AddBoolToObject(wifi, "dhcp", app->config.sta_dhcp);
    cJSON_AddStringToObject(wifi, "ip", app->config.sta_ip);
    cJSON_AddStringToObject(wifi, "gateway", app->config.sta_gateway);
    cJSON_AddStringToObject(wifi, "subnet", app->config.sta_subnet);
    cJSON_AddStringToObject(wifi, "dns", app->config.sta_dns);
    cJSON_AddItemToObject(root, "wifi", wifi);
    cJSON_AddStringToObject(root, "hostname", app->config.hostname[0] ? app->config.hostname : DEFAULT_HOSTNAME);
    cJSON_AddNumberToObject(root, "telnet_port", app->config.telnet_port ? app->config.telnet_port : DEFAULT_TELNET_PORT);
    return http_reply_json(req, root);
}

static esp_err_t handle_api_config_save_post(httpd_req_t *req) {
    app_context_t *app = (app_context_t *)req->user_ctx;
    char *body = NULL;
    size_t body_len = 0;
    if (recv_body(req, &body, &body_len) != ESP_OK || body == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
        return ESP_FAIL;
    }
    cJSON *json = cJSON_ParseWithLength(body, body_len);
    free(body);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
        return ESP_FAIL;
    }

    cJSON *ssid = cJSON_GetObjectItem(json, "ssid");
    cJSON *pass = cJSON_GetObjectItem(json, "password");
    cJSON *dhcp = cJSON_GetObjectItem(json, "dhcp");
    cJSON *ip = cJSON_GetObjectItem(json, "ip");
    cJSON *gateway = cJSON_GetObjectItem(json, "gateway");
    cJSON *subnet = cJSON_GetObjectItem(json, "subnet");
    cJSON *dns = cJSON_GetObjectItem(json, "dns");
    cJSON *hostname = cJSON_GetObjectItem(json, "hostname");
    cJSON *telnet_port = cJSON_GetObjectItem(json, "telnet_port");

    if (cJSON_IsString(ssid) && ssid->valuestring != NULL) {
        copy_string(app->config.sta_ssid, sizeof(app->config.sta_ssid), ssid->valuestring);
    }
    if (cJSON_IsString(pass) && pass->valuestring != NULL) {
        copy_string(app->config.sta_password, sizeof(app->config.sta_password), pass->valuestring);
    }
    app->config.sta_configured = app->config.sta_ssid[0] != '\0';
    if (cJSON_IsTrue(dhcp) || cJSON_IsFalse(dhcp)) {
        app->config.sta_dhcp = cJSON_IsTrue(dhcp);
    }
    if (cJSON_IsString(ip) && ip->valuestring != NULL) {
        copy_string(app->config.sta_ip, sizeof(app->config.sta_ip), ip->valuestring);
    }
    if (cJSON_IsString(gateway) && gateway->valuestring != NULL) {
        copy_string(app->config.sta_gateway, sizeof(app->config.sta_gateway), gateway->valuestring);
    }
    if (cJSON_IsString(subnet) && subnet->valuestring != NULL) {
        copy_string(app->config.sta_subnet, sizeof(app->config.sta_subnet), subnet->valuestring);
    }
    if (cJSON_IsString(dns) && dns->valuestring != NULL) {
        copy_string(app->config.sta_dns, sizeof(app->config.sta_dns), dns->valuestring);
    }
    if (cJSON_IsString(hostname) && hostname->valuestring != NULL && hostname->valuestring[0] != '\0') {
        copy_string(app->config.hostname, sizeof(app->config.hostname), hostname->valuestring);
    } else if (app->config.hostname[0] == '\0') {
        copy_string(app->config.hostname, sizeof(app->config.hostname), DEFAULT_HOSTNAME);
    }
    if (cJSON_IsNumber(telnet_port) && telnet_port->valueint > 0 && telnet_port->valueint <= 65535) {
        app->config.telnet_port = (uint16_t)telnet_port->valueint;
    } else if (app->config.telnet_port == 0U) {
        app->config.telnet_port = DEFAULT_TELNET_PORT;
    }
    cJSON_Delete(json);

    esp_err_t err = wifi_manager_set_sta_network(
        app->config.sta_dhcp, app->config.sta_ip, app->config.sta_gateway, app->config.sta_subnet, app->config.sta_dns);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid ip config");
        return err;
    }
    if (app->config.sta_configured) {
        err = wifi_manager_connect_sta(app->config.sta_ssid, app->config.sta_password);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "STA connect failed after save: %s", esp_err_to_name(err));
        }
    }
    config_store_save(&app->config);
    httpd_resp_sendstr(req, "{\"ok\":true,\"reboot\":true}");
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
    return ESP_OK;
}

static esp_err_t handle_hvac_get(httpd_req_t *req) {
    app_context_t *app = (app_context_t *)req->user_ctx;
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();

    if (xSemaphoreTake(app->gateway_lock, pdMS_TO_TICKS(2000)) == pdTRUE) {
        for (uint8_t i = 0; i < D3NET_MAX_UNITS; i++) {
            d3net_unit_t *u = &app->gateway.units[i];
            if (!u->present) {
                continue;
            }
            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "index", u->index);
            cJSON_AddStringToObject(item, "unit_id", u->unit_id);
            cJSON_AddBoolToObject(item, "power", d3net_status_power_get(&u->status));
            cJSON_AddNumberToObject(item, "mode", d3net_status_oper_mode_get(&u->status));
            cJSON_AddNumberToObject(item, "temp_current", d3net_status_temp_current_get(&u->status));
            cJSON_AddNumberToObject(item, "temp_setpoint", d3net_status_temp_setpoint_get(&u->status));
            cJSON_AddItemToArray(arr, item);
        }
        xSemaphoreGive(app->gateway_lock);
    }

    cJSON_AddItemToObject(root, "units", arr);
    return http_reply_json(req, root);
}

static esp_err_t handle_discover_post(httpd_req_t *req) {
    app_context_t *app = (app_context_t *)req->user_ctx;
    esp_err_t err = ESP_FAIL;
    if (xSemaphoreTake(app->gateway_lock, pdMS_TO_TICKS(5000)) == pdTRUE) {
        err = d3net_gateway_discover_units(&app->gateway);
        xSemaphoreGive(app->gateway_lock);
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "discover failed");
        return err;
    }
    telnet_server_logf("discovery complete: units=%u", app->gateway.discovered_count);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t handle_hvac_cmd_post(httpd_req_t *req) {
    app_context_t *app = (app_context_t *)req->user_ctx;
    char *body = NULL;
    size_t body_len = 0;
    if (recv_body(req, &body, &body_len) != ESP_OK || body == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
        return ESP_FAIL;
    }
    cJSON *json = cJSON_ParseWithLength(body, body_len);
    free(body);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
        return ESP_FAIL;
    }

    cJSON *idx = cJSON_GetObjectItem(json, "index");
    cJSON *cmd = cJSON_GetObjectItem(json, "cmd");
    cJSON *val = cJSON_GetObjectItem(json, "value");
    if (!cJSON_IsNumber(idx) || !cJSON_IsString(cmd)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "fields");
        return ESP_FAIL;
    }
    int index = idx->valueint;
    if (index < 0 || index >= D3NET_MAX_UNITS) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "index");
        return ESP_FAIL;
    }

    esp_err_t err = ESP_FAIL;
    const char *cmd_text = cmd->valuestring;
    if (xSemaphoreTake(app->gateway_lock, pdMS_TO_TICKS(5000)) == pdTRUE) {
        d3net_unit_t *u = &app->gateway.units[index];
        if (!u->present) {
            err = ESP_ERR_NOT_FOUND;
        } else {
            uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
            if (strcmp(cmd->valuestring, "power") == 0 && cJSON_IsNumber(val)) {
                err = d3net_unit_set_power(&app->gateway, u, val->valuedouble > 0.5, now_ms);
            } else if (strcmp(cmd->valuestring, "mode") == 0 && cJSON_IsNumber(val)) {
                err = d3net_unit_set_mode(&app->gateway, u, (d3net_mode_t)val->valueint, now_ms);
            } else if (strcmp(cmd->valuestring, "setpoint") == 0 && cJSON_IsNumber(val)) {
                err = d3net_unit_set_setpoint(&app->gateway, u, (float)val->valuedouble, now_ms);
            } else if (strcmp(cmd->valuestring, "fan_speed") == 0 && cJSON_IsNumber(val)) {
                err = d3net_unit_set_fan_speed(&app->gateway, u, (d3net_fan_speed_t)val->valueint, now_ms);
            } else if (strcmp(cmd->valuestring, "fan_dir") == 0 && cJSON_IsNumber(val)) {
                err = d3net_unit_set_fan_dir(&app->gateway, u, (d3net_fan_dir_t)val->valueint, now_ms);
            } else if (strcmp(cmd->valuestring, "filter_reset") == 0) {
                err = d3net_unit_filter_reset(&app->gateway, u, now_ms);
            } else {
                err = ESP_ERR_INVALID_ARG;
            }
        }
        xSemaphoreGive(app->gateway_lock);
    }
    cJSON_Delete(json);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "command failed");
        return err;
    }
    telnet_server_logf("hvac cmd idx=%d %s", index, cmd_text);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t handle_registry_get(httpd_req_t *req) {
    app_context_t *app = (app_context_t *)req->user_ctx;
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();

    bool present_map[D3NET_MAX_UNITS] = {0};

    if (xSemaphoreTake(app->gateway_lock, pdMS_TO_TICKS(2000)) == pdTRUE) {
        for (uint8_t i = 0; i < D3NET_MAX_UNITS; i++) {
            d3net_unit_t *u = &app->gateway.units[i];
            if (!u->present) {
                continue;
            }
            present_map[i] = true;
            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "index", u->index);
            cJSON_AddStringToObject(item, "unit_id", u->unit_id);
            cJSON_AddBoolToObject(item, "registered", reg_is_set(app, i));
            cJSON_AddBoolToObject(item, "online", true);
            cJSON_AddBoolToObject(item, "power", d3net_status_power_get(&u->status));
            int mode = d3net_status_oper_mode_get(&u->status);
            cJSON_AddNumberToObject(item, "mode", mode);
            cJSON_AddStringToObject(item, "mode_name", "");
            cJSON_AddNumberToObject(item, "temp_current", d3net_status_temp_current_get(&u->status));
            cJSON_AddNumberToObject(item, "temp_setpoint", d3net_status_temp_setpoint_get(&u->status));
            cJSON_AddItemToArray(arr, item);
        }
        xSemaphoreGive(app->gateway_lock);
    }

    for (uint8_t i = 0; i < D3NET_MAX_UNITS; i++) {
        if (!reg_is_set(app, i)) {
            continue;
        }
        if (present_map[i]) {
            continue;
        }
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "index", i);
        cJSON_AddStringToObject(item, "unit_id", app->config.registered_ids[i]);
        cJSON_AddBoolToObject(item, "registered", true);
        cJSON_AddBoolToObject(item, "online", false);
        cJSON_AddItemToArray(arr, item);
    }

    cJSON_AddItemToObject(root, "units", arr);
    return http_reply_json(req, root);
}

static esp_err_t handle_registry_post(httpd_req_t *req) {
    app_context_t *app = (app_context_t *)req->user_ctx;
    char *body = NULL;
    size_t body_len = 0;
    if (recv_body(req, &body, &body_len) != ESP_OK || body == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
        return ESP_FAIL;
    }
    cJSON *json = cJSON_ParseWithLength(body, body_len);
    free(body);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
        return ESP_FAIL;
    }
    cJSON *idx = cJSON_GetObjectItem(json, "index");
    cJSON *action = cJSON_GetObjectItem(json, "action");
    if (!cJSON_IsNumber(idx) || !cJSON_IsString(action)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "fields");
        return ESP_FAIL;
    }
    int index = idx->valueint;
    if (index < 0 || index >= D3NET_MAX_UNITS) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "index");
        return ESP_FAIL;
    }

    esp_err_t err = ESP_OK;
    if (strcmp(action->valuestring, "add") == 0) {
        if (xSemaphoreTake(app->gateway_lock, pdMS_TO_TICKS(2000)) == pdTRUE) {
            d3net_unit_t *u = &app->gateway.units[index];
            if (!u->present) {
                err = ESP_ERR_NOT_FOUND;
            } else {
                reg_set(app, index, true, u->unit_id);
                config_store_save(&app->config);
                telnet_server_logf("registered unit idx=%d id=%s", index, u->unit_id);
            }
            xSemaphoreGive(app->gateway_lock);
        }
    } else if (strcmp(action->valuestring, "remove") == 0) {
        reg_set(app, index, false, NULL);
        config_store_save(&app->config);
        telnet_server_logf("unregistered unit idx=%d", index);
    } else {
        err = ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(json);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "registry");
        return err;
    }
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t handle_logs_get(httpd_req_t *req) {
    char query[64] = {0};
    uint32_t since = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char buf[16] = {0};
        if (httpd_query_key_value(query, "since", buf, sizeof(buf)) == ESP_OK) {
            since = (uint32_t)strtoul(buf, NULL, 10);
        }
    }
    telnet_log_line_t lines[64];
    size_t count = telnet_server_get_logs(since, lines, 64);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    uint32_t latest = since;
    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "seq", lines[i].seq);
        cJSON_AddStringToObject(item, "text", lines[i].line);
        cJSON_AddItemToArray(arr, item);
        if (lines[i].seq > latest) {
            latest = lines[i].seq;
        }
    }
    cJSON_AddItemToObject(root, "lines", arr);
    cJSON_AddNumberToObject(root, "latest", latest);
    cJSON_AddBoolToObject(root, "enabled", telnet_server_monitor_enabled());
    return http_reply_json(req, root);
}

static esp_err_t handle_monitor_get(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "enabled", telnet_server_monitor_enabled());
    return http_reply_json(req, root);
}

static esp_err_t handle_monitor_toggle_post(httpd_req_t *req) {
    char *body = NULL;
    size_t body_len = 0;
    if (recv_body(req, &body, &body_len) != ESP_OK || body == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
        return ESP_FAIL;
    }
    cJSON *json = cJSON_ParseWithLength(body, body_len);
    free(body);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
        return ESP_FAIL;
    }
    cJSON *enabled = cJSON_GetObjectItem(json, "enabled");
    if (!cJSON_IsTrue(enabled) && !cJSON_IsFalse(enabled)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "enabled");
        return ESP_FAIL;
    }
    telnet_server_set_monitor_enabled(cJSON_IsTrue(enabled));
    cJSON_Delete(json);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t handle_monitor_clear_post(httpd_req_t *req) {
    telnet_server_clear_logs();
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t handle_rtu_get(httpd_req_t *req) {
    app_context_t *app = (app_context_t *)req->user_ctx;
    cJSON *root = cJSON_CreateObject();
    const modbus_rtu_config_t *cfg = &app->config.rtu_cfg;
    cJSON_AddNumberToObject(root, "tx_pin", cfg->tx_pin);
    cJSON_AddNumberToObject(root, "rx_pin", cfg->rx_pin);
    cJSON_AddNumberToObject(root, "de_pin", cfg->de_pin);
    cJSON_AddNumberToObject(root, "re_pin", cfg->re_pin);
    cJSON_AddNumberToObject(root, "baud_rate", cfg->baud_rate);
    cJSON_AddNumberToObject(root, "data_bits", cfg->data_bits);
    cJSON_AddNumberToObject(root, "stop_bits", cfg->stop_bits);
    cJSON_AddStringToObject(root, "parity", (char[]){cfg->parity, 0});
    cJSON_AddNumberToObject(root, "slave_id", cfg->slave_id);
    cJSON_AddNumberToObject(root, "timeout_ms", cfg->timeout_ms);
    cJSON *defaults = cJSON_CreateObject();
    cJSON_AddNumberToObject(defaults, "tx_pin", 17);
    cJSON_AddNumberToObject(defaults, "rx_pin", 16);
    cJSON_AddNumberToObject(defaults, "de_pin", 4);
    cJSON_AddNumberToObject(defaults, "re_pin", 5);
    cJSON_AddNumberToObject(defaults, "baud_rate", 19200);
    cJSON_AddNumberToObject(defaults, "data_bits", 8);
    cJSON_AddNumberToObject(defaults, "stop_bits", 2);
    cJSON_AddStringToObject(defaults, "parity", "N");
    cJSON_AddNumberToObject(defaults, "slave_id", 1);
    cJSON_AddNumberToObject(defaults, "timeout_ms", 3000);
    cJSON_AddItemToObject(root, "defaults", defaults);
    return http_reply_json(req, root);
}

static esp_err_t handle_rtu_post(httpd_req_t *req) {
    app_context_t *app = (app_context_t *)req->user_ctx;
    char *body = NULL;
    size_t body_len = 0;
    if (recv_body(req, &body, &body_len) != ESP_OK || body == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
        return ESP_FAIL;
    }
    cJSON *json = cJSON_ParseWithLength(body, body_len);
    free(body);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
        return ESP_FAIL;
    }
    modbus_rtu_config_t *cfg = &app->config.rtu_cfg;
    cfg->tx_pin = cJSON_GetObjectItem(json, "tx_pin")->valueint;
    cfg->rx_pin = cJSON_GetObjectItem(json, "rx_pin")->valueint;
    cfg->de_pin = cJSON_GetObjectItem(json, "de_pin")->valueint;
    cfg->re_pin = cJSON_GetObjectItem(json, "re_pin")->valueint;
    cfg->baud_rate = cJSON_GetObjectItem(json, "baud_rate")->valueint;
    cfg->data_bits = cJSON_GetObjectItem(json, "data_bits")->valueint;
    cfg->stop_bits = cJSON_GetObjectItem(json, "stop_bits")->valueint;
    cfg->parity = (char)cJSON_GetObjectItem(json, "parity")->valuestring[0];
    cfg->slave_id = cJSON_GetObjectItem(json, "slave_id")->valueint;
    cfg->timeout_ms = cJSON_GetObjectItem(json, "timeout_ms")->valueint;
    cJSON_Delete(json);

    config_store_save(&app->config);
    httpd_resp_sendstr(req, "{\"ok\":true,\"reboot\":true}");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

static esp_err_t handle_ota_post(httpd_req_t *req) {
    app_context_t *app = (app_context_t *)req->user_ctx;
    app->ota.active = true;
    app->ota.success = false;
    app->ota.bytes_received = 0;
    app->ota.total_bytes = (size_t)req->content_len;
    strncpy(app->ota.message, "OTA receiving", sizeof(app->ota.message) - 1U);

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        strncpy(app->ota.message, "No OTA partition", sizeof(app->ota.message) - 1U);
        app->ota.active = false;
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "partition");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        strncpy(app->ota.message, "OTA begin failed", sizeof(app->ota.message) - 1U);
        app->ota.active = false;
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin");
        return err;
    }

    char buf[1024];
    int remaining = req->content_len;
    while (remaining > 0) {
        int to_read = remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining;
        int r = httpd_req_recv(req, buf, to_read);
        if (r <= 0) {
            esp_ota_abort(ota_handle);
            app->ota.active = false;
            strncpy(app->ota.message, "OTA read failed", sizeof(app->ota.message) - 1U);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota read");
            return ESP_FAIL;
        }
        err = esp_ota_write(ota_handle, buf, (size_t)r);
        if (err != ESP_OK) {
            esp_ota_abort(ota_handle);
            app->ota.active = false;
            strncpy(app->ota.message, "OTA write failed", sizeof(app->ota.message) - 1U);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota write");
            return err;
        }
        remaining -= r;
        app->ota.bytes_received += (size_t)r;
    }

    err = esp_ota_end(ota_handle);
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(update_partition);
    }
    app->ota.active = false;
    app->ota.success = (err == ESP_OK);
    strncpy(app->ota.message, err == ESP_OK ? "OTA complete, rebooting" : "OTA finalize failed", sizeof(app->ota.message) - 1U);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota end");
        return err;
    }

    httpd_resp_sendstr(req, "{\"ok\":true,\"reboot\":true}");
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
    return ESP_OK;
}

esp_err_t web_server_start(app_context_t *app, httpd_handle_t *out_handle) {
    if (app == NULL || out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 32;
    config.stack_size = 8192;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = handle_index_get, .user_ctx = app},
        {.uri = "/d3net", .method = HTTP_GET, .handler = handle_d3net_page_get, .user_ctx = app},
        {.uri = "/config", .method = HTTP_GET, .handler = handle_config_page_get, .user_ctx = app},
        {.uri = "/rtu", .method = HTTP_GET, .handler = handle_rtu_page_get, .user_ctx = app},
        {.uri = "/monitor", .method = HTTP_GET, .handler = handle_monitor_page_get, .user_ctx = app},
        {.uri = "/firmware", .method = HTTP_GET, .handler = handle_firmware_page_get, .user_ctx = app},
        {.uri = "/api/status", .method = HTTP_GET, .handler = handle_status_get, .user_ctx = app},
        {.uri = "/api/config", .method = HTTP_GET, .handler = handle_api_config_get, .user_ctx = app},
        {.uri = "/api/config/save", .method = HTTP_POST, .handler = handle_api_config_save_post, .user_ctx = app},
        {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = handle_wifi_scan_get, .user_ctx = app},
        {.uri = "/api/wifi/connect", .method = HTTP_POST, .handler = handle_wifi_connect_post, .user_ctx = app},
        {.uri = "/api/hvac", .method = HTTP_GET, .handler = handle_hvac_get, .user_ctx = app},
        {.uri = "/api/discover", .method = HTTP_POST, .handler = handle_discover_post, .user_ctx = app},
        {.uri = "/api/hvac/cmd", .method = HTTP_POST, .handler = handle_hvac_cmd_post, .user_ctx = app},
        {.uri = "/api/registry", .method = HTTP_GET, .handler = handle_registry_get, .user_ctx = app},
        {.uri = "/api/registry", .method = HTTP_POST, .handler = handle_registry_post, .user_ctx = app},
        {.uri = "/api/logs", .method = HTTP_GET, .handler = handle_logs_get, .user_ctx = app},
        {.uri = "/api/monitor", .method = HTTP_GET, .handler = handle_monitor_get, .user_ctx = app},
        {.uri = "/api/monitor/toggle", .method = HTTP_POST, .handler = handle_monitor_toggle_post, .user_ctx = app},
        {.uri = "/api/monitor/clear", .method = HTTP_POST, .handler = handle_monitor_clear_post, .user_ctx = app},
        {.uri = "/api/rtu", .method = HTTP_GET, .handler = handle_rtu_get, .user_ctx = app},
        {.uri = "/api/rtu", .method = HTTP_POST, .handler = handle_rtu_post, .user_ctx = app},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = handle_ota_post, .user_ctx = app},
        {.uri = "/firmware/update", .method = HTTP_POST, .handler = handle_ota_post, .user_ctx = app},
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "handler register failed for %s", routes[i].uri);
        }
    }

    *out_handle = server;
    ESP_LOGI(TAG, "web server started");
    return ESP_OK;
}
