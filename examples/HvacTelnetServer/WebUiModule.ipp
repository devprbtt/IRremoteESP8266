String protocolOptionsHtml(const String &selected) {
  String out;
  out += "<option value='CUSTOM'";
  if (selected == "CUSTOM") out += " selected";
  out += ">CUSTOM</option>";
  out.reserve(2048);
  for (uint16_t i = 0; i <= kLastDecodeType; i++) {
    decode_type_t proto = static_cast<decode_type_t>(i);
    if (!IRac::isProtocolSupported(proto)) continue;
    String name = typeToString(proto);
    out += "<option value='" + htmlEscape(name) + "'";
    if (selected == name) out += " selected";
    out += ">" + htmlEscape(name) + "</option>";
  }
  return out;
}

String emitterOptionsHtml(int selectedIndex) {
  String out;
  for (uint8_t i = 0; i < config.emitterCount; i++) {
    out += "<option value='" + String(i) + "'";
    if (static_cast<int>(i) == selectedIndex) out += " selected";
    out += ">" + String(i) + " (GPIO " + String(config.emitterGpios[i]) + ")</option>";
  }
  return out;
}

String dinActionOptionsHtml(const String &selected) {
  const char *actions[] = {
      "none", "temp_up", "temp_down", "set_temp",
      "power_on", "power_off", "toggle_power",
      "mode_heat", "mode_cool", "mode_fan", "mode_auto", "mode_off",
      "light_on", "light_off", "toggle_light"};
  const size_t count = sizeof(actions) / sizeof(actions[0]);
  String out;
  for (size_t i = 0; i < count; i++) {
    String a = actions[i];
    out += "<option value='" + a + "'";
    if (selected == a) out += " selected";
    out += ">" + a + "</option>";
  }
  return out;
}

String nextHvacId() {
  bool used[100];
  for (int i = 0; i < 100; i++) used[i] = false;
  for (uint8_t i = 0; i < config.hvacCount; i++) {
    int id = config.hvacs[i].id.toInt();
    if (id >= 1 && id <= 99) used[id] = true;
  }
  for (int id = 1; id <= 99; id++) {
    if (!used[id]) return String(id);
  }
  return "";
}

String networkListHtml() {
  String out;
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  int n = WiFi.scanNetworks(false, true);
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    int32_t rssi = WiFi.RSSI(i);
    out += "<option value='" + htmlEscape(ssid) + "'>" + htmlEscape(ssid) +
           " (" + String(rssi) + " dBm)</option>";
  }
  return out;
}

String pageHeader(const String &title) {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>" + htmlEscape(title) + "</title>";
  html += "<style>";
  html += "body{font-family:Segoe UI,Tahoma,Arial,sans-serif;margin:0;background:#0f172a;color:#e2e8f0;}";
  html += ".wrap{max-width:1100px;margin:0 auto;padding:24px;}";
  html += "nav{display:flex;flex-wrap:wrap;gap:12px;margin-bottom:16px;}";
  html += "nav a{color:#0f172a;background:#e2e8f0;padding:8px 12px;border-radius:8px;text-decoration:none;font-weight:600;}";
  html += "h2,h3,h4{color:#f8fafc;margin:16px 0 8px;}";
  html += ".card{background:#111827;border:1px solid #1f2937;border-radius:12px;padding:16px;margin:12px 0;}";
  html += "label{font-size:12px;color:#94a3b8;display:block;margin-top:6px;}";
  html += "input,select,textarea{width:100%;padding:8px;margin:4px 0;background:#0b1220;color:#e2e8f0;border:1px solid #334155;border-radius:8px;}";
  html += "button{background:#22c55e;border:0;color:#0b1220;font-weight:700;padding:8px 14px;border-radius:8px;cursor:pointer;}";
  html += "button.secondary{background:#38bdf8;}";
  html += "input:disabled{opacity:0.6;cursor:not-allowed;}";
  html += ".row{display:flex;align-items:center;gap:8px;}";
  html += ".row input[type='checkbox']{width:auto;margin:0;}";
  html += "table{border-collapse:collapse;width:100%;}th,td{border:1px solid #334155;padding:8px;text-align:left;}";
  html += ".din-actions{table-layout:fixed;}";
  html += ".din-actions th,.din-actions td{vertical-align:middle;}";
  html += ".din-actions input,.din-actions select{width:100%;min-width:0;box-sizing:border-box;}";
  html += ".din-actions .din-id{max-width:90px;}";
  html += ".din-actions .din-val{max-width:90px;}";
  html += ".din-actions .din-action{max-width:170px;}";
  html += ".din-actions .din-led{width:auto;}";
  html += "code,pre{background:#0b1220;border:1px solid #334155;border-radius:8px;padding:8px;display:block;white-space:pre-wrap;}";
  html += ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px;}";
  html += ".pill{display:inline-block;background:#1e293b;color:#e2e8f0;padding:2px 8px;border-radius:999px;font-size:12px;margin-left:6px;}";
  html += "</style></head><body><div class='wrap'>";
  html += "<nav><a href='/'>Home</a><a href='/config'>Config</a><a href='/emitters'>Emitters</a><a href='/devices'>Devices</a><a href='/devices/test'>Test Device</a><a href='/dinplug'>DINplug</a><a href='/system#monitor'>Monitor</a><a href='/system#firmware'>Firmware</a><a href='/system#backup'>Upload</a><a href='/config/download'>Download</a></nav>";
  return html;
}

String pageFooter() { return "</div></body></html>"; }

bool streamSpiffsFile(const char *path, const char *contentType) {
  if (!SPIFFS.exists(path)) return false;
  File f = SPIFFS.open(path, FILE_READ);
  if (!f) return false;
  web.streamFile(f, contentType);
  f.close();
  return true;
}

void sendSpiffsFallbackPage(const String &title, const String &spiffsPath) {
  if (!checkAuth()) { requestAuth(); return; }
  String html = pageHeader(title);
  html += "<div class='card'><h2>" + htmlEscape(title) + "</h2>";
  html += "<p>The web UI file <code>" + htmlEscape(spiffsPath) + "</code> is missing from SPIFFS.</p>";
  html += "<p>Upload the latest <code>spiffs.bin</code> from the firmware build to restore this page.</p>";
  html += "<p>The device services are still running. Only this route's embedded web UI is unavailable.</p>";
  html += "</div>";
  html += pageFooter();
  web.send(503, "text/html", html);
}

void handleHome() {
  if (!checkAuth()) { requestAuth(); return; }
  String html = pageHeader("IR Server Telnet");
  html += "<div class='card'><h2>IR Server Telnet</h2>";
  html += "<p>Telnet port: <strong>" + String(config.telnetPort) + "</strong></p>";
  html += "<p>Network mode: <strong>" + networkModeString() + "</strong></p>";
  html += "<p>IP: <strong>" + networkLocalIp().toString() + "</strong></p>";
  html += "<p>Hostname: <strong>" + htmlEscape(config.hostname.length() ? config.hostname : kDefaultHostname) + ".local</strong></p>";
  html += "<p>DINplug: <strong>" + htmlEscape(dinplugConnectionStatus()) + "</strong></p>";
  html += "<p>Emitters: <strong>" + String(config.emitterCount) + "</strong></p>";
  html += "<p>IR receiver: <strong>" + String(config.irReceiver.enabled ? "enabled" : "disabled") + "</strong></p>";
  if (config.irReceiver.enabled) {
    html += "<p>IR RX GPIO/mode: <strong>" + String(config.irReceiver.gpio) + " / " +
            htmlEscape(normalizeIrReceiverMode(config.irReceiver.mode)) + "</strong></p>";
  }
  html += "<p>Devices: <strong>" + String(config.hvacCount) + "</strong></p></div>";
  html += "<div class='card'><h3>Telnet Connections</h3>";
  uint8_t activeTelnet = activeTelnetClientCount();
  html += "<p>Active clients: <strong>" + String(activeTelnet) + "</strong></p>";
  if (activeTelnet > 0) {
    html += "<table><tr><th>Slot</th><th>Remote IP</th><th>Remote Port</th></tr>";
    for (uint8_t i = 0; i < kMaxTelnetClients; i++) {
      WiFiClient &c = telnetClients[i];
      if (!c || !c.connected()) continue;
      html += "<tr><td>" + String(i) + "</td><td>" + c.remoteIP().toString() + "</td><td>" + String(c.remotePort()) + "</td></tr>";
    }
    html += "</table>";
  }
  html += "</div>";
  if (config.tempSensors.enabled) {
    html += "<div class='card'><h3>Detected Temperature Sensors</h3>";
    if (tempSensorCount == 0) {
      html += "<p>No sensors detected.</p>";
    } else {
      html += "<table><tr><th>Index</th><th>Name</th><th>Address</th><th>Current Temp (C)</th></tr>";
      for (uint8_t i = 0; i < tempSensorCount; i++) {
        html += "<tr><td>" + String(i) + "</td><td>" + htmlEscape(sensorNameForIndex(i)) + "</td><td>" +
                htmlEscape(sensorAddressToString(tempSensorAddresses[i])) + "</td><td>";
        if (tempSensorValid[i]) html += String(tempSensorReadings[i], static_cast<unsigned int>(tempSensorPrecision()));
        else html += "N/A";
        html += "</td></tr>";
      }
      html += "</table>";
    }
    html += "</div>";
  }
  html += pageFooter();
  web.send(200, "text/html", html);
}

void handleConfigPage() { sendSpiffsFallbackPage("Config", "/config.html"); }

void handleConfigSave() {
  if (!checkAuth()) { requestAuth(); return; }
  String ssid = web.arg("ssid");
  String ssidScan = web.arg("ssid_scan");
  if (ssidScan.length()) ssid = ssidScan;
  config.wifi.ssid = ssid;
  config.wifi.password = web.arg("password");
  config.wifi.dhcp = web.hasArg("dhcp");
  config.wifi.ip.fromString(web.arg("ip"));
  config.wifi.gateway.fromString(web.arg("gateway"));
  config.wifi.subnet.fromString(web.arg("subnet"));
  config.wifi.dns.fromString(web.arg("dns"));
  config.web.password = web.arg("webpass");
  config.tempSensors.enabled = web.hasArg("ts_enabled");
  int tsGpio = web.arg("ts_gpio").toInt();
  if (tsGpio < 0 || tsGpio > 39) tsGpio = 4;
  config.tempSensors.gpio = static_cast<uint8_t>(tsGpio);
  uint16_t tsInterval = web.arg("ts_interval").toInt();
  if (tsInterval == 0) tsInterval = 10;
  config.tempSensors.readIntervalSec = tsInterval;
  uint8_t tsPrecision = static_cast<uint8_t>(web.arg("ts_precision").toInt());
  if (tsPrecision > 2) tsPrecision = 2;
  config.tempSensors.precision = tsPrecision;
  for (uint8_t i = 0; i < kMaxTempSensors; i++) {
    String key = "ts_name_" + String(i);
    String name = web.arg(key);
    name.trim();
    config.tempSensors.names[i] = name;
  }
  config.eth.enabled = web.hasArg("eth_enabled");
  config.irReceiver.enabled = web.hasArg("irrx_enabled");
  int irrxGpio = web.arg("irrx_gpio").toInt();
  if (irrxGpio < 0 || irrxGpio > 39) irrxGpio = 14;
  config.irReceiver.gpio = static_cast<uint8_t>(irrxGpio);
  config.irReceiver.mode = normalizeIrReceiverMode(web.arg("irrx_mode"));
  String hostname = web.arg("hostname");
  hostname.trim();
  if (hostname.length() == 0) hostname = kDefaultHostname;
  config.hostname = hostname;
  config.timezone = normalizeTimezoneOffset(web.arg("timezone"));
  String gatewayHost = web.arg("gateway_host");
  if (gatewayHost.length() == 0) gatewayHost = web.arg("gateway_ip");
  gatewayHost.trim();
  config.dinplug.gatewayHost = gatewayHost;
  config.dinplug.autoConnect = web.hasArg("auto_connect");
  uint16_t port = web.arg("telnet_port").toInt();
  if (port == 0) port = kDefaultTelnetPort;
  config.telnetPort = port;
  saveConfig();
  Serial.println("web: config saved, rebooting");

  String mdnsUrl = "http://" + htmlEscape(config.hostname.length() ? config.hostname : kDefaultHostname) + ".local/";
  String staticIpUrl = "";
  if (!config.eth.enabled && config.wifi.ssid.length() > 0 && !config.wifi.dhcp && config.wifi.ip != IPAddress()) {
    staticIpUrl = "http://" + config.wifi.ip.toString() + "/";
  }
  String rebootHtml = "<!doctype html><html><head><meta charset='utf-8'><title>Rebooting</title>";
  rebootHtml += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  rebootHtml += "<style>body{font-family:Arial,sans-serif;padding:24px;max-width:720px;margin:auto;}code{background:#f1f5f9;padding:2px 6px;border-radius:6px;}a{word-break:break-all;}</style>";
  rebootHtml += "</head><body><h2>Saved. Rebooting device...</h2>";
  rebootHtml += "<p>This page will try to reconnect automatically.</p>";
  rebootHtml += "<p>Fallback links:</p><ul>";
  rebootHtml += "<li><a href='" + mdnsUrl + "'>" + mdnsUrl + "</a></li>";
  if (staticIpUrl.length()) rebootHtml += "<li><a href='" + staticIpUrl + "'>" + staticIpUrl + "</a></li>";
  rebootHtml += "<li><a href='http://192.168.4.1/'>http://192.168.4.1/</a></li></ul>";
  rebootHtml += "<script>";
  rebootHtml += "const targets=[];";
  rebootHtml += "if(location&&location.origin) targets.push(location.origin+'/');";
  rebootHtml += "targets.push('" + mdnsUrl + "');";
  if (staticIpUrl.length()) rebootHtml += "targets.push('" + staticIpUrl + "');";
  rebootHtml += "targets.push('http://192.168.4.1/');";
  rebootHtml += "let i=0;const tryNext=()=>{if(!targets.length)return;location.href=targets[i%targets.length];i++;setTimeout(tryNext,3500);};";
  rebootHtml += "setTimeout(tryNext,2200);";
  rebootHtml += "</script></body></html>";
  web.sendHeader("Cache-Control", "no-store");
  web.sendHeader("Connection", "close");
  web.send(200, "text/html", rebootHtml);
  delay(1200);
  ESP.restart();
}

void handleEmittersPage() { sendSpiffsFallbackPage("Emitters", "/emitters.html"); }

void handleEmittersAdd() {
  if (!checkAuth()) { requestAuth(); return; }
  int added = 0;
  for (int i = 0; i < web.args(); i++) {
    if (web.argName(i) != "gpios") continue;
    if (config.emitterCount >= kMaxEmitters) break;
    int gpio = web.arg(i).toInt();
    if (gpio <= 0) continue;
    config.emitterGpios[config.emitterCount++] = static_cast<uint16_t>(gpio);
    added++;
  }
  if (added == 0) {
    web.send(400, "text/plain", "No valid GPIOs");
    return;
  }
  saveConfig();
  rebuildEmitters();
  Serial.print("web: emitters added ");
  Serial.println(added);
  web.sendHeader("Location", "/devices?tab=emitters");
  web.send(302, "text/plain", "");
}

void handleEmittersDelete() {
  if (!checkAuth()) { requestAuth(); return; }
  int idx = web.arg("index").toInt();
  if (idx < 0 || idx >= config.emitterCount) {
    web.send(400, "text/plain", "Invalid index");
    return;
  }
  for (int i = idx; i < config.emitterCount - 1; i++) {
    config.emitterGpios[i] = config.emitterGpios[i + 1];
  }
  config.emitterCount--;
  saveConfig();
  rebuildEmitters();
  Serial.print("web: emitter deleted index ");
  Serial.println(idx);
  web.sendHeader("Location", "/devices?tab=emitters");
  web.send(302, "text/plain", "");
}

void handleHvacsPage() { sendSpiffsFallbackPage("Devices", "/hvacs.html"); }

void handleHvacsAdd() {
  if (!checkAuth()) { requestAuth(); return; }
  if (config.emitterCount == 0) {
    web.send(400, "text/plain", "Add an emitter first");
    return;
  }
  if (config.hvacCount >= kMaxHvacs) {
    web.send(400, "text/plain", "Too many HVACs");
    return;
  }
  String id = nextHvacId();
  if (!id.length()) {
    web.send(400, "text/plain", "No IDs left (1-99)");
    return;
  }
  HvacConfig &h = config.hvacs[config.hvacCount++];
  h.id = id;
  h.protocol = web.arg("protocol");
  if (!h.protocol.length()) h.protocol = "CUSTOM";
  h.profileName = web.arg("profile_name");
  h.profileName.trim();
  if (h.protocol != "CUSTOM") {
    decode_type_t proto = strToDecodeType(h.protocol.c_str());
    if (!IRac::isProtocolSupported(proto)) {
      config.hvacCount--;
      web.send(400, "text/plain", "Unsupported protocol");
      return;
    }
  }
  h.emitterIndex = web.arg("emitter").toInt();
  h.model = web.arg("model").toInt();
  h.currentTempSource = web.arg("current_temp_source");
  h.currentTempSource.toLowerCase();
  if (h.currentTempSource != "sensor") h.currentTempSource = "setpoint";
  uint16_t sensorIndex = web.arg("temp_sensor_index").toInt();
  if (sensorIndex >= kMaxTempSensors) sensorIndex = 0;
  h.tempSensorIndex = static_cast<uint8_t>(sensorIndex);
  h.isCustom = (h.protocol == "CUSTOM");
  if (h.isCustom) h.currentTempSource = "setpoint";
  if (h.currentTempSource == "sensor" && (tempSensorCount == 0 || h.tempSensorIndex >= tempSensorCount)) {
    h.currentTempSource = "setpoint";
  }
  if (h.isCustom) loadCustomCommandsFromRequest(h);
  else h.customCommandCount = 0;
  initHvacRuntimeStates();
  saveConfig();
  Serial.print("web: hvac added id=");
  Serial.println(id);
  web.sendHeader("Location", "/devices");
  web.send(302, "text/plain", "");
}

void handleHvacsClone() {
  if (!checkAuth()) { requestAuth(); return; }
  if (config.emitterCount == 0) {
    web.send(400, "text/plain", "Add an emitter first");
    return;
  }
  if (config.hvacCount >= kMaxHvacs) {
    web.send(400, "text/plain", "Too many HVACs");
    return;
  }
  int srcIdx = web.arg("source_index").toInt();
  if (srcIdx < 0 || srcIdx >= config.hvacCount) {
    web.send(400, "text/plain", "Invalid source HVAC");
    return;
  }
  HvacConfig &src = config.hvacs[srcIdx];
  if (src.protocol != "CUSTOM" && !src.isCustom) {
    web.send(400, "text/plain", "Source HVAC is not custom");
    return;
  }
  int emitterIndex = web.arg("emitter").toInt();
  if (emitterIndex < 0 || emitterIndex >= config.emitterCount) {
    web.send(400, "text/plain", "Invalid emitter");
    return;
  }
  String id = nextHvacId();
  if (!id.length()) {
    web.send(400, "text/plain", "No IDs left (1-99)");
    return;
  }

  HvacConfig &dst = config.hvacs[config.hvacCount++];
  dst = src;
  dst.id = id;
  dst.protocol = "CUSTOM";
  dst.isCustom = true;
  dst.emitterIndex = emitterIndex;
  dst.model = -1;
  dst.currentTempSource = "setpoint";
  dst.tempSensorIndex = 0;
  dst.dinKeypadCount = 0;
  dst.dinButtonStart = 0;
  dst.dinButtonCount = 0;

  initHvacRuntimeStates();
  saveConfig();
  Serial.print("web: hvac cloned from index ");
  Serial.print(srcIdx);
  Serial.print(" to id=");
  Serial.println(id);
  web.sendHeader("Location", "/devices");
  web.send(302, "text/plain", "");
}

void handleHvacsUpdate() {
  if (!checkAuth()) { requestAuth(); return; }
  int idx = web.arg("index").toInt();
  if (idx < 0 || idx >= config.hvacCount) {
    web.send(400, "text/plain", "Invalid index");
    return;
  }
  if (config.emitterCount == 0) {
    web.send(400, "text/plain", "Add an emitter first");
    return;
  }
  String protocol = web.arg("protocol");
  if (!protocol.length()) {
    web.send(400, "text/plain", "Missing protocol");
    return;
  }
  if (protocol != "CUSTOM") {
    decode_type_t proto = strToDecodeType(protocol.c_str());
    if (!IRac::isProtocolSupported(proto)) {
      web.send(400, "text/plain", "Unsupported protocol");
      return;
    }
  }
  int emitterIndex = web.arg("emitter").toInt();
  if (emitterIndex < 0 || emitterIndex >= config.emitterCount) {
    web.send(400, "text/plain", "Invalid emitter");
    return;
  }
  int model = web.arg("model").toInt();
  HvacConfig &h = config.hvacs[idx];
  h.protocol = protocol;
  h.profileName = web.arg("profile_name");
  h.profileName.trim();
  h.emitterIndex = emitterIndex;
  h.model = model;
  h.currentTempSource = web.arg("current_temp_source");
  h.currentTempSource.toLowerCase();
  if (h.currentTempSource != "sensor") h.currentTempSource = "setpoint";
  if (protocol == "CUSTOM") h.currentTempSource = "setpoint";
  uint16_t sensorIndex = web.arg("temp_sensor_index").toInt();
  if (sensorIndex >= kMaxTempSensors) sensorIndex = 0;
  h.tempSensorIndex = static_cast<uint8_t>(sensorIndex);
  if (h.currentTempSource == "sensor" && (tempSensorCount == 0 || h.tempSensorIndex >= tempSensorCount)) {
    h.currentTempSource = "setpoint";
  }
  h.isCustom = (protocol == "CUSTOM");
  if (h.isCustom) loadCustomCommandsFromRequest(h);
  else h.customCommandCount = 0;
  String keypadCsv = web.arg("din_keypad_ids");
  if (keypadCsv.length() == 0) keypadCsv = web.arg("din_keypad_id");
  parseDinKeypadsCsv(keypadCsv, h);
  DinplugButtonBinding nextBindings[kMaxDinplugBindingsTotal];
  uint8_t nextBindingCount = 0;
  for (uint8_t i = 0; i < kMaxDinplugBindingsTotal; i++) {
    String base = "btn" + String(i) + "_";
    uint16_t keypadId = web.arg(base + "keypad_id").toInt();
    uint16_t btnId = web.arg(base + "id").toInt();
    if (btnId == 0) continue;
    DinplugButtonBinding &b = nextBindings[nextBindingCount++];
    b.keypadId = keypadId;
    b.buttonId = btnId;
    b.pressAction = web.arg(base + "press_action");
    if (!b.pressAction.length()) b.pressAction = "none";
    b.pressValue = dinActionUsesValue(b.pressAction) ? web.arg(base + "press_value").toFloat() : 1.0f;
    if (b.pressValue == 0) b.pressValue = 1.0f;
    b.pressModeOverride = normalizeDinplugModeOverride(web.arg(base + "press_mode_override"));
    b.pressLightMode = normalizeDinplugLightMode(web.arg(base + "press_light_mode"));
    b.holdAction = web.arg(base + "hold_action");
    if (!b.holdAction.length()) b.holdAction = "none";
    b.holdValue = dinActionUsesValue(b.holdAction) ? web.arg(base + "hold_value").toFloat() : 1.0f;
    if (b.holdValue == 0) b.holdValue = 1.0f;
    b.holdModeOverride = normalizeDinplugModeOverride(web.arg(base + "hold_mode_override"));
    b.holdLightMode = normalizeDinplugLightMode(web.arg(base + "hold_light_mode"));
    b.togglePowerMode = web.arg(base + "toggle_power_mode");
    b.togglePowerMode.toLowerCase();
    if (b.togglePowerMode != "auto" && b.togglePowerMode != "cool" && b.togglePowerMode != "heat" &&
        b.togglePowerMode != "dry" && b.togglePowerMode != "fan") {
      b.togglePowerMode = "auto";
    }
    uint8_t ledMode = static_cast<uint8_t>(web.arg(base + "led_follow_mode").toInt());
    if (ledMode > 2) ledMode = 0;
    b.ledFollowMode = ledMode;
    if (h.isCustom) {
      if (!b.pressAction.startsWith("custom:") && b.pressAction != "none") b.pressAction = "none";
      if (!b.holdAction.startsWith("custom:") && b.holdAction != "none") b.holdAction = "none";
      b.pressValue = 1.0f;
      b.holdValue = 1.0f;
      b.pressModeOverride = kDinplugModeOverrideKeep;
      b.holdModeOverride = kDinplugModeOverrideKeep;
      b.togglePowerMode = "auto";
    }
    if (nextBindingCount >= kMaxDinplugBindingsTotal) break;
  }
  if (!setHvacDinplugBindings(static_cast<uint8_t>(idx), nextBindings, nextBindingCount)) {
    web.send(400, "text/plain", "Too many DINplug bindings in use");
    return;
  }
  resetHvacRuntimeState(static_cast<uint8_t>(idx));
  saveConfig();
  Serial.print("web: hvac updated index ");
  Serial.println(idx);
  web.sendHeader("Location", "/devices");
  web.send(302, "text/plain", "");
}

void handleHvacsDelete() {
  if (!checkAuth()) { requestAuth(); return; }
  int idx = web.arg("index").toInt();
  if (idx < 0 || idx >= config.hvacCount) {
    web.send(400, "text/plain", "Invalid index");
    return;
  }
  for (int i = idx; i < config.hvacCount - 1; i++) {
    config.hvacs[i] = config.hvacs[i + 1];
  }
  config.hvacCount--;
  compactDinplugBindingPool();
  initHvacRuntimeStates();
  saveConfig();
  Serial.print("web: hvac deleted index ");
  Serial.println(idx);
  web.sendHeader("Location", "/devices");
  web.send(302, "text/plain", "");
}

void handleHvacTestPage() { sendSpiffsFallbackPage("Device Test", "/hvacs_test.html"); }

void handleConfigDownload() {
  if (!checkAuth()) { requestAuth(); return; }
  String json = configToJsonString();
  web.sendHeader("Content-Disposition", "attachment; filename=\"config.json\"");
  web.send(200, "application/json", json);
}

String configUploadBuffer;

void handleConfigUploadPage() { sendSpiffsFallbackPage("Upload Config", "/system.html"); }

void handleConfigUpload() {
  if (!checkAuth()) { requestAuth(); return; }
  HTTPUpload &up = web.upload();
  if (up.status == UPLOAD_FILE_START) {
    configUploadBuffer = "";
    configUploadBuffer.reserve(up.totalSize > 0 ? up.totalSize : 1024);
    Serial.println("web: upload start");
  } else if (up.status == UPLOAD_FILE_WRITE) {
    configUploadBuffer.concat(reinterpret_cast<const char *>(up.buf), up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    Serial.println("web: upload complete");
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    configUploadBuffer = "";
    Serial.println("web: upload aborted");
  }
}

void handleConfigUploadDone() {
  if (!checkAuth()) { requestAuth(); return; }
  if (!configUploadBuffer.length()) {
    web.send(400, "text/html", "<html><body><p>Upload failed: empty config.</p></body></html>");
    return;
  }
  if (!loadConfigFromJson(configUploadBuffer, true)) {
    configUploadBuffer = "";
    web.send(400, "text/html", "<html><body><p>Upload failed: invalid config JSON.</p></body></html>");
    return;
  }
  if (!saveConfigJson(configUploadBuffer)) {
    configUploadBuffer = "";
    web.send(500, "text/html", "<html><body><p>Upload failed: could not persist config.</p></body></html>");
    return;
  }
  configUploadBuffer = "";
  rebuildEmitters();
  Serial.println("web: upload applied, rebooting");
  web.send(200, "text/html", "<html><body><p>Uploaded. Rebooting...</p></body></html>");
  delay(500);
  ESP.restart();
}

void handleApiConfig() {
  if (!checkAuth()) { requestAuth(); return; }
  String json = configToJsonString();
  web.send(200, "application/json", json);
}

void handleApiConfigSave() {
  if (!checkAuth()) { requestAuth(); return; }
  String body = web.arg("plain");
  if (!body.length()) {
    web.send(400, "application/json", "{\"ok\":false,\"error\":\"missing_body\"}");
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    String out = "{\"ok\":false,\"error\":\"invalid_json\",\"detail\":\"";
    out += err.c_str();
    out += "\"}";
    web.send(400, "application/json", out);
    return;
  }

  if (!saveConfigJson(body)) {
    web.send(500, "application/json", "{\"ok\":false,\"error\":\"config_write_failed\"}");
    return;
  }

  web.send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
  delay(300);
  ESP.restart();
}

void handleMonitorPage() { sendSpiffsFallbackPage("Monitor", "/system.html"); }

bool isApPortalMode() {
  wifi_mode_t mode = WiFi.getMode();
  return (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA);
}

String captivePortalUrl() {
  IPAddress apIp = WiFi.softAPIP();
  if (apIp == IPAddress()) apIp = IPAddress(192, 168, 4, 1);
  return "http://" + apIp.toString() + "/";
}

void sendPortalRedirect() {
  String url = captivePortalUrl();
  web.sendHeader("Cache-Control", "no-store");
  web.sendHeader("Location", url, true);
  web.send(302, "text/html", "<html><body><a href='" + url + "'>Redirecting...</a></body></html>");
}

void handleCaptive204() {
  if (isApPortalMode()) {
    sendPortalRedirect();
    return;
  }
  web.send(204);
}

void handleCaptiveRedirect() {
  if (isApPortalMode()) {
    sendPortalRedirect();
    return;
  }
  web.sendHeader("Location", "/", true);
  web.send(302, "text/plain", "");
}

void setupWeb() {
  web.on("/", HTTP_GET, []() {
    if (!checkAuth()) { requestAuth(); return; }
    if (!streamSpiffsFile("/index.html", "text/html; charset=utf-8")) handleHome();
  });
  web.on("/config", HTTP_GET, []() {
    if (!checkAuth()) { requestAuth(); return; }
    if (!streamSpiffsFile("/config.html", "text/html; charset=utf-8")) handleConfigPage();
  });
  web.on("/config/save", HTTP_POST, handleConfigSave);
  web.on("/emitters", HTTP_GET, []() {
    if (!checkAuth()) { requestAuth(); return; }
    if (!streamSpiffsFile("/emitters.html", "text/html; charset=utf-8")) handleEmittersPage();
  });
  web.on("/emitters/add", HTTP_POST, handleEmittersAdd);
  web.on("/emitters/delete", HTTP_GET, handleEmittersDelete);
  web.on("/devices", HTTP_GET, []() {
    if (!checkAuth()) { requestAuth(); return; }
    if (!streamSpiffsFile("/hvacs.html", "text/html; charset=utf-8")) handleHvacsPage();
  });
  web.on("/devices/add", HTTP_POST, handleHvacsAdd);
  web.on("/devices/clone", HTTP_POST, handleHvacsClone);
  web.on("/devices/test", HTTP_GET, []() {
    if (!checkAuth()) { requestAuth(); return; }
    if (!streamSpiffsFile("/hvacs_test.html", "text/html; charset=utf-8")) handleHvacTestPage();
  });
  web.on("/devices/test", HTTP_POST, handleHvacTest);
  web.on("/devices/update", HTTP_POST, handleHvacsUpdate);
  web.on("/devices/delete", HTTP_GET, handleHvacsDelete);
  web.on("/dinplug", HTTP_GET, []() {
    if (!checkAuth()) { requestAuth(); return; }
    if (!streamSpiffsFile("/dinplug.html", "text/html; charset=utf-8")) handleDinplugPage();
  });
  web.on("/dinplug/save", HTTP_POST, handleDinplugSave);
  web.on("/dinplug/test", HTTP_POST, handleDinplugTest);
  web.on("/raw/test", HTTP_POST, handleRawTest);
  web.on("/monitor", HTTP_GET, []() {
    if (!checkAuth()) { requestAuth(); return; }
    web.sendHeader("Location", "/system#monitor", true);
    web.send(302, "text/plain", "");
  });
  web.on("/api/monitor", HTTP_GET, handleApiMonitor);
  web.on("/api/diagnostics", HTTP_GET, handleApiDiagnostics);
  web.on("/monitor/clear", HTTP_POST, handleMonitorClear);
  web.on("/monitor/toggle", HTTP_POST, handleMonitorToggle);
  web.on("/api/ir/learn/start", HTTP_POST, handleIrLearnStart);
  web.on("/api/ir/learn/poll", HTTP_GET, handleIrLearnPoll);
  web.on("/api/ir/learn/cancel", HTTP_POST, handleIrLearnCancel);
  web.on("/system", HTTP_GET, []() {
    if (!checkAuth()) { requestAuth(); return; }
    if (!streamSpiffsFile("/system.html", "text/html; charset=utf-8")) handleFirmwarePage();
  });
  web.on("/system/factory-reset", HTTP_POST, handleFactoryReset);
  web.on("/generate_204", HTTP_ANY, handleCaptive204);
  web.on("/gen_204", HTTP_ANY, handleCaptive204);
  web.on("/hotspot-detect.html", HTTP_ANY, handleCaptiveRedirect);
  web.on("/success.txt", HTTP_ANY, handleCaptiveRedirect);
  web.on("/canonical.html", HTTP_ANY, handleCaptiveRedirect);
  web.on("/redirect", HTTP_ANY, handleCaptiveRedirect);
  web.on("/fwlink", HTTP_ANY, handleCaptiveRedirect);
  web.on("/connecttest.txt", HTTP_ANY, handleCaptiveRedirect);
  web.on("/ncsi.txt", HTTP_ANY, handleCaptiveRedirect);
  web.on("/library/test/success.html", HTTP_ANY, handleCaptiveRedirect);
  web.on("/config/download", HTTP_GET, handleConfigDownload);
  web.on("/config/upload", HTTP_GET, []() {
    if (!checkAuth()) { requestAuth(); return; }
    web.sendHeader("Location", "/system#backup", true);
    web.send(302, "text/plain", "");
  });
  web.on("/config/upload", HTTP_POST, handleConfigUploadDone, handleConfigUpload);
  web.on("/firmware", HTTP_GET, []() {
    if (!checkAuth()) { requestAuth(); return; }
    web.sendHeader("Location", "/system#firmware", true);
    web.send(302, "text/plain", "");
  });
  web.on("/firmware/update", HTTP_POST, handleFirmwareUpdate, handleFirmwareUpload);
  web.on("/spiffs/update", HTTP_POST, handleFilesystemUpdate, handleFilesystemUpload);
  web.on("/api/config", HTTP_GET, handleApiConfig);
  web.on("/api/config/save", HTTP_POST, handleApiConfigSave);
  web.on("/api/device/get", HTTP_GET, handleApiDeviceGet);
  web.on("/api/device/get_all", HTTP_GET, handleApiDeviceGetAll);
  web.on("/api/device/send", HTTP_POST, handleApiDeviceSend);
  web.on("/api/device/raw", HTTP_POST, handleApiDeviceRaw);
  web.on("/api/status", HTTP_GET, handleApiStatus);
  web.on("/api/meta", HTTP_GET, handleApiMeta);
  web.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
  web.on("/version.js", HTTP_GET, []() {
    if (!checkAuth()) { requestAuth(); return; }
    if (!streamSpiffsFile("/version.js", "application/javascript; charset=utf-8")) {
      web.send(404, "text/plain", "missing version.js");
    }
  });
  web.on("/version.json", HTTP_GET, []() {
    if (!checkAuth()) { requestAuth(); return; }
    if (!streamSpiffsFile("/version.json", "application/json; charset=utf-8")) {
      web.send(404, "application/json", "{\"ok\":false,\"error\":\"missing_version_json\"}");
    }
  });
  web.on("/favicon.ico", HTTP_ANY, []() { web.send(204, "text/plain", ""); });
  web.on("/apple-touch-icon.png", HTTP_ANY, []() { web.send(204, "text/plain", ""); });
  web.on("/apple-touch-icon-precomposed.png", HTTP_ANY, []() { web.send(204, "text/plain", ""); });
  web.onNotFound([]() {
    if (isApPortalMode()) {
      sendPortalRedirect();
      return;
    }
    web.sendHeader("Location", "/", true);
    web.send(302, "text/plain", "");
  });
  web.begin();
}
