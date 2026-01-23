// HvacTelnetServer.ino
// ESP32 example: Telnet JSON HVAC control + web configuration UI.

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>

#include <IRremoteESP8266.h>
#include <IRac.h>
#include <IRsend.h>
#include <IRutils.h>

// ---- User-tunable limits ----
static const uint16_t kTelnetPort = 4998;
static const uint8_t kMaxTelnetClients = 4;
static const uint8_t kMaxEmitters = 8;
static const uint8_t kMaxHvacs = 32;
static const uint8_t kMaxCustomTemps = 16;

static const char *kConfigPath = "/config.json";
static const char *kApSsid = "IR-HVAC-Setup";

struct CustomTempCode {
  int tempC;
  String code;
};

struct HvacConfig {
  String id;
  String protocol;
  int emitterIndex = -1;
  int model = -1;
  bool isCustom = false;
  String customEncoding;  // "pronto" or "gc"
  String customOff;
  CustomTempCode customTemps[kMaxCustomTemps];
  uint8_t customTempCount = 0;
};

struct WifiConfig {
  String ssid;
  String password;
  bool dhcp = true;
  IPAddress ip;
  IPAddress gateway;
  IPAddress subnet;
  IPAddress dns;
};

struct WebConfig {
  String password;
};

struct Config {
  WifiConfig wifi;
  WebConfig web;
  uint16_t emitterGpios[kMaxEmitters];
  uint8_t emitterCount = 0;
  HvacConfig hvacs[kMaxHvacs];
  uint8_t hvacCount = 0;
};

struct EmitterRuntime {
  uint16_t gpio = 0;
  IRsend *raw = nullptr;
  IRac *ac = nullptr;
};

Config config;
EmitterRuntime emitters[kMaxEmitters];
uint8_t emitterRuntimeCount = 0;

WebServer web(80);
WiFiServer telnetServer(kTelnetPort);
WiFiClient telnetClients[kMaxTelnetClients];
String telnetBuffers[kMaxTelnetClients];

// ---- Helpers ----

bool isAuthRequired() { return config.web.password.length() > 0; }

bool checkAuth() {
  if (!isAuthRequired()) return true;
  return web.authenticate("admin", config.web.password.c_str());
}

void requestAuth() {
  if (!isAuthRequired()) return;
  web.requestAuthentication();
}

String htmlEscape(const String &input) {
  String out;
  out.reserve(input.length() + 16);
  for (size_t i = 0; i < input.length(); i++) {
    char c = input[i];
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c; break;
    }
  }
  return out;
}

uint16_t countValuesInStr(const String str, char sep) {
  int16_t index = -1;
  uint16_t count = 1;
  do {
    index = str.indexOf(sep, index + 1);
    count++;
  } while (index != -1);
  return count;
}

uint16_t *newCodeArray(const uint16_t size) {
  uint16_t *result = reinterpret_cast<uint16_t*>(malloc(size * sizeof(uint16_t)));
  return result;
}

bool parseStringAndSendGC(IRsend *irsend, const String str) {
#if SEND_GLOBALCACHE
  String tmp_str = str;
  if (str.startsWith(PSTR("1:1,1,"))) tmp_str = str.substring(6);
  uint16_t count = countValuesInStr(tmp_str, ',');
  uint16_t *code_array = newCodeArray(count);
  if (!code_array) return false;
  count = 0;
  uint16_t start_from = 0;
  int16_t index = -1;
  do {
    index = tmp_str.indexOf(',', start_from);
    code_array[count] = tmp_str.substring(start_from, index).toInt();
    start_from = index + 1;
    count++;
  } while (index != -1);
  irsend->sendGC(code_array, count);
  free(code_array);
  return count > 0;
#else
  (void)irsend;
  (void)str;
  return false;
#endif
}

bool parseStringAndSendPronto(IRsend *irsend, const String str, uint16_t repeats) {
#if SEND_PRONTO
  uint16_t count = countValuesInStr(str, ',');
  int16_t index = -1;
  uint16_t start_from = 0;
  if (str.startsWith("R") || str.startsWith("r")) {
    index = str.indexOf(',', start_from);
    repeats = str.substring(start_from + 1, index).toInt();
    start_from = index + 1;
    count--;
  }
  if (count < kProntoMinLength) return false;
  uint16_t *code_array = newCodeArray(count);
  if (!code_array) return false;
  count = 0;
  do {
    index = str.indexOf(',', start_from);
    code_array[count] = strtoul(str.substring(start_from, index).c_str(), NULL, 16);
    start_from = index + 1;
    count++;
  } while (index != -1);
  irsend->sendPronto(code_array, count, repeats);
  free(code_array);
  return count > 0;
#else
  (void)irsend;
  (void)str;
  (void)repeats;
  return false;
#endif
}

String configToJsonString() {
  JsonDocument doc;
  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["ssid"] = config.wifi.ssid;
  wifi["password"] = config.wifi.password;
  wifi["dhcp"] = config.wifi.dhcp;
  wifi["ip"] = config.wifi.ip.toString();
  wifi["gateway"] = config.wifi.gateway.toString();
  wifi["subnet"] = config.wifi.subnet.toString();
  wifi["dns"] = config.wifi.dns.toString();

  JsonObject webc = doc["web"].to<JsonObject>();
  webc["password"] = config.web.password;

  JsonArray em = doc["emitters"].to<JsonArray>();
  for (uint8_t i = 0; i < config.emitterCount; i++) {
    JsonObject e = em.add<JsonObject>();
    e["gpio"] = config.emitterGpios[i];
  }

  JsonArray hv = doc["hvacs"].to<JsonArray>();
  for (uint8_t i = 0; i < config.hvacCount; i++) {
    const HvacConfig &h = config.hvacs[i];
    JsonObject o = hv.add<JsonObject>();
    o["id"] = h.id;
    o["protocol"] = h.protocol;
    o["emitter"] = h.emitterIndex;
    o["model"] = h.model;
    if (h.isCustom) {
      JsonObject c = o["custom"].to<JsonObject>();
      c["encoding"] = h.customEncoding;
      c["off"] = h.customOff;
      JsonObject temps = c["temps"].to<JsonObject>();
      for (uint8_t t = 0; t < h.customTempCount; t++) {
        temps[String(h.customTemps[t].tempC)] = h.customTemps[t].code;
      }
    }
  }

  String out;
  serializeJson(doc, out);
  return out;
}

void saveConfig() {
  File f = SPIFFS.open(kConfigPath, FILE_WRITE);
  if (!f) {
    Serial.println("config: failed to open for write");
    return;
  }
  String json = configToJsonString();
  f.print(json);
  f.close();
  Serial.println("config: saved");
}

void clearConfig() {
  config.wifi.ssid = "";
  config.wifi.password = "";
  config.wifi.dhcp = true;
  config.wifi.ip = IPAddress();
  config.wifi.gateway = IPAddress();
  config.wifi.subnet = IPAddress();
  config.wifi.dns = IPAddress();
  config.web.password = "";
  for (uint8_t i = 0; i < kMaxEmitters; i++) {
    config.emitterGpios[i] = 0;
  }
  config.emitterCount = 0;
  for (uint8_t i = 0; i < kMaxHvacs; i++) {
    HvacConfig &h = config.hvacs[i];
    h.id = "";
    h.protocol = "";
    h.emitterIndex = -1;
    h.model = -1;
    h.isCustom = false;
    h.customEncoding = "";
    h.customOff = "";
    for (uint8_t t = 0; t < kMaxCustomTemps; t++) {
      h.customTemps[t].tempC = 0;
      h.customTemps[t].code = "";
    }
    h.customTempCount = 0;
  }
  config.hvacCount = 0;
}

void loadConfig() {
  clearConfig();
  if (!SPIFFS.exists(kConfigPath)) {
    Serial.println("config: not found");
    return;
  }
  File f = SPIFFS.open(kConfigPath, FILE_READ);
  if (!f) {
    Serial.println("config: failed to open");
    return;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.print("config: parse error ");
    Serial.println(err.c_str());
    return;
  }

  JsonObject wifi = doc["wifi"];
  if (!wifi.isNull()) {
    config.wifi.ssid = wifi["ssid"] | "";
    config.wifi.password = wifi["password"] | "";
    config.wifi.dhcp = wifi["dhcp"] | true;
    config.wifi.ip.fromString(wifi["ip"] | "");
    config.wifi.gateway.fromString(wifi["gateway"] | "");
    config.wifi.subnet.fromString(wifi["subnet"] | "");
    config.wifi.dns.fromString(wifi["dns"] | "");
  }

  JsonObject webc = doc["web"];
  if (!webc.isNull()) {
    config.web.password = webc["password"] | "";
  }

  JsonArray em = doc["emitters"];
  if (!em.isNull()) {
    for (JsonObject e : em) {
      if (config.emitterCount >= kMaxEmitters) break;
      config.emitterGpios[config.emitterCount++] = e["gpio"] | 0;
    }
  }

  JsonArray hv = doc["hvacs"];
  if (!hv.isNull()) {
    for (JsonObject o : hv) {
      if (config.hvacCount >= kMaxHvacs) break;
      HvacConfig &h = config.hvacs[config.hvacCount++];
      h.id = o["id"] | "";
      h.protocol = o["protocol"] | "";
      h.emitterIndex = o["emitter"] | -1;
      h.model = o["model"] | -1;
      JsonObject c = o["custom"];
      if (!c.isNull()) {
        h.isCustom = true;
        h.customEncoding = c["encoding"] | "";
        h.customOff = c["off"] | "";
        JsonObject temps = c["temps"];
        if (!temps.isNull()) {
          for (JsonPair kv : temps) {
            if (h.customTempCount >= kMaxCustomTemps) break;
            h.customTemps[h.customTempCount].tempC = atoi(kv.key().c_str());
            h.customTemps[h.customTempCount].code = kv.value().as<String>();
            h.customTempCount++;
          }
        }
      }
    }
  }
}

void rebuildEmitters() {
  for (uint8_t i = 0; i < emitterRuntimeCount; i++) {
    delete emitters[i].raw;
    delete emitters[i].ac;
    emitters[i].raw = nullptr;
    emitters[i].ac = nullptr;
  }
  emitterRuntimeCount = 0;
  for (uint8_t i = 0; i < config.emitterCount && i < kMaxEmitters; i++) {
    emitters[i].gpio = config.emitterGpios[i];
    emitters[i].raw = new IRsend(emitters[i].gpio);
    emitters[i].raw->begin();
    emitters[i].ac = new IRac(emitters[i].gpio);
    emitterRuntimeCount++;
  }
  Serial.print("emitters: configured ");
  Serial.println(emitterRuntimeCount);
}

bool findHvacById(const String &id, HvacConfig *&out) {
  for (uint8_t i = 0; i < config.hvacCount; i++) {
    if (config.hvacs[i].id == id) {
      out = &config.hvacs[i];
      return true;
    }
  }
  return false;
}

EmitterRuntime* getEmitter(uint8_t idx) {
  if (idx >= emitterRuntimeCount) return nullptr;
  return &emitters[idx];
}

String protocolOptionsHtml(const String &selected) {
  String out;
  out.reserve(2048);
  out += "<option value='CUSTOM'";
  if (selected == "CUSTOM") out += " selected";
  out += ">CUSTOM</option>";
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
    out += "<option value='" + htmlEscape(ssid) + "'>" + htmlEscape(ssid) + "</option>";
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
  html += "table{border-collapse:collapse;width:100%;}th,td{border:1px solid #334155;padding:8px;text-align:left;}";
  html += "code,pre{background:#0b1220;border:1px solid #334155;border-radius:8px;padding:8px;display:block;white-space:pre-wrap;}";
  html += ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px;}";
  html += ".pill{display:inline-block;background:#1e293b;color:#e2e8f0;padding:2px 8px;border-radius:999px;font-size:12px;margin-left:6px;}";
  html += "</style></head><body><div class='wrap'>";
  html += "<nav><a href='/'>Home</a><a href='/config'>Config</a><a href='/emitters'>Emitters</a><a href='/hvacs'>HVACs</a><a href='/config/upload'>Upload</a><a href='/config/download'>Download</a></nav>";
  return html;
}

String pageFooter() { return "</div></body></html>"; }

// ---- Web Handlers ----

void handleHome() {
  if (!checkAuth()) { requestAuth(); return; }
  String html = pageHeader("IR HVAC Telnet");
  html += "<div class='card'><h2>IR HVAC Telnet Server</h2>";
  html += "<p>Telnet port: <strong>" + String(kTelnetPort) + "</strong></p>";
  html += "<p>WiFi mode: <strong>" + String(WiFi.isConnected() ? "STA" : "AP") + "</strong></p>";
  html += "<p>IP: <strong>" + WiFi.localIP().toString() + "</strong></p>";
  html += "<p>Emitters: <strong>" + String(config.emitterCount) + "</strong></p>";
  html += "<p>HVACs: <strong>" + String(config.hvacCount) + "</strong></p></div>";
  html += pageFooter();
  web.send(200, "text/html", html);
}

void handleConfigPage() {
  if (!checkAuth()) { requestAuth(); return; }
  String html = pageHeader("Config");
  html += "<div class='card'><h2>WiFi / Network</h2>";
  html += "<form method='POST' action='/config/save'>";
  html += "<label>WiFi SSID</label>";
  html += "<input name='ssid' value='" + htmlEscape(config.wifi.ssid) + "'>";
  html += "<label>Select from scan</label>";
  html += "<div class='grid'><div>";
  html += "<select name='ssid_scan'><option value=''>-- select --</option>" + networkListHtml() + "</select>";
  html += "</div><div>";
  html += "<a class='pill' href='/config?scan=1'>Rescan</a>";
  html += "</div></div>";
  html += "<label>WiFi Password</label>";
  html += "<input name='password' type='password' value='" + htmlEscape(config.wifi.password) + "'>";
  html += "<label><input type='checkbox' name='dhcp'" + String(config.wifi.dhcp ? " checked" : "") + "> DHCP</label>";
  html += "<label>Static IP</label><input name='ip' value='" + htmlEscape(config.wifi.ip.toString()) + "'>";
  html += "<label>Gateway</label><input name='gateway' value='" + htmlEscape(config.wifi.gateway.toString()) + "'>";
  html += "<label>Subnet</label><input name='subnet' value='" + htmlEscape(config.wifi.subnet.toString()) + "'>";
  html += "<label>DNS</label><input name='dns' value='" + htmlEscape(config.wifi.dns.toString()) + "'>";
  html += "<h3>Web Password</h3>";
  html += "<label>Admin password (blank = no auth)</label>";
  html += "<input name='webpass' type='password' value='" + htmlEscape(config.web.password) + "'>";
  html += "<button type='submit'>Save & Reboot</button>";
  html += "</form></div>";
  html += pageFooter();
  web.send(200, "text/html", html);
}

void handleConfigSave() {
  if (!checkAuth()) { requestAuth(); return; }
  String ssid = web.arg("ssid");
  String ssid_scan = web.arg("ssid_scan");
  if (ssid_scan.length()) ssid = ssid_scan;
  config.wifi.ssid = ssid;
  config.wifi.password = web.arg("password");
  config.wifi.dhcp = web.hasArg("dhcp");
  config.wifi.ip.fromString(web.arg("ip"));
  config.wifi.gateway.fromString(web.arg("gateway"));
  config.wifi.subnet.fromString(web.arg("subnet"));
  config.wifi.dns.fromString(web.arg("dns"));
  config.web.password = web.arg("webpass");
  saveConfig();
  Serial.println("web: config saved, rebooting");
  web.send(200, "text/html", "<html><body><p>Saved. Rebooting...</p></body></html>");
  delay(500);
  ESP.restart();
}

void handleEmittersPage() {
  if (!checkAuth()) { requestAuth(); return; }
  String html = pageHeader("Emitters");
  html += "<div class='card'><h2>Emitters</h2>";
  html += "<table><tr><th>#</th><th>GPIO</th><th>Action</th></tr>";
  for (uint8_t i = 0; i < config.emitterCount; i++) {
    html += "<tr><td>" + String(i) + "</td><td>" + String(config.emitterGpios[i]) + "</td>";
    html += "<td><a href='/emitters/delete?index=" + String(i) + "'>Delete</a></td></tr>";
  }
  html += "</table>";
  html += "<h3>Add Emitters</h3>";
  html += "<form method='POST' action='/emitters/add'>";
  html += "<label>GPIO selection (multi-select)</label>";
  html += "<select name='gpios' multiple size='8'>";
  html += "<option value='2'>GPIO 2</option>";
  html += "<option value='4'>GPIO 4</option>";
  html += "<option value='5'>GPIO 5</option>";
  html += "<option value='12'>GPIO 12</option>";
  html += "<option value='13'>GPIO 13</option>";
  html += "<option value='14'>GPIO 14</option>";
  html += "<option value='15'>GPIO 15</option>";
  html += "<option value='16'>GPIO 16</option>";
  html += "<option value='17'>GPIO 17</option>";
  html += "<option value='18'>GPIO 18</option>";
  html += "<option value='19'>GPIO 19</option>";
  html += "<option value='21'>GPIO 21</option>";
  html += "<option value='22'>GPIO 22</option>";
  html += "<option value='23'>GPIO 23</option>";
  html += "<option value='25'>GPIO 25</option>";
  html += "<option value='26'>GPIO 26</option>";
  html += "<option value='27'>GPIO 27</option>";
  html += "<option value='32'>GPIO 32</option>";
  html += "<option value='33'>GPIO 33</option>";
  html += "</select>";
  html += "<small class='pill'>Ctrl/Shift for multi-select</small>";
  html += "<button type='submit'>Add</button></form></div>";
  html += pageFooter();
  web.send(200, "text/html", html);
}

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
  web.sendHeader("Location", "/emitters");
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
  web.sendHeader("Location", "/emitters");
  web.send(302, "text/plain", "");
}

void handleHvacsPage() {
  if (!checkAuth()) { requestAuth(); return; }
  String html = pageHeader("HVACs");
  html += "<div class='card'><h2>HVACs</h2>";
  html += "<table><tr><th>#</th><th>ID</th><th>Protocol</th><th>Emitter</th><th>Action</th></tr>";
  for (uint8_t i = 0; i < config.hvacCount; i++) {
    const HvacConfig &h = config.hvacs[i];
    html += "<tr><td>" + String(i) + "</td><td>" + htmlEscape(h.id) + "</td><td>" + htmlEscape(h.protocol) + "</td>";
    html += "<td>" + String(h.emitterIndex) + "</td>";
    html += "<td><a href='/hvacs/delete?index=" + String(i) + "'>Delete</a></td></tr>";
  }
  html += "</table>";

  if (config.emitterCount == 0) {
    html += "<p><strong>Add at least one emitter before registering HVACs.</strong></p>";
  } else {
    html += "<h3>Add HVAC</h3>";
    html += "<form method='POST' action='/hvacs/add'>";
    html += "<label>ID</label><input value='auto' disabled>";
    html += "<small class='pill'>auto 1-99</small>";
    html += "<label>Protocol</label><select name='protocol'>" + protocolOptionsHtml("") + "</select>";
    html += "<label>Emitter</label><select name='emitter'>" + emitterOptionsHtml(0) + "</select>";
    html += "<label>Model (optional)</label><input name='model' value='-1'>";
    html += "<h4>Custom Protocol Fields (only for CUSTOM)</h4>";
    html += "<label>Encoding</label><select name='custom_encoding'><option value='pronto'>pronto</option><option value='gc'>gc</option></select>";
    html += "<label>OFF code</label><textarea name='custom_off'></textarea>";
    html += "<label>Temperature codes JSON (e.g. {\"18\":\"0000,...\",\"19\":\"0000,...\"})</label>";
    html += "<textarea name='custom_temps'></textarea>";
    html += "<button type='submit'>Add</button></form>";
  }
  html += "</div>";

  html += "<div class='card'><h3>Test HVAC</h3>";
  if (config.hvacCount == 0) {
    html += "<p>No HVACs registered.</p>";
  } else {
    html += "<form id='testForm' method='POST' action='/hvacs/test'>";
    html += "<div class='grid'>";
    html += "<div><label>HVAC</label><select name='id'>";
    for (uint8_t i = 0; i < config.hvacCount; i++) {
      html += "<option value='" + htmlEscape(config.hvacs[i].id) + "'>" + htmlEscape(config.hvacs[i].id) + " (" + htmlEscape(config.hvacs[i].protocol) + ")</option>";
    }
    html += "</select></div>";
    html += "<div><label>Power</label><select name='power'><option value='on'>on</option><option value='off'>off</option></select></div>";
    html += "<div><label>Mode</label><select name='mode'><option>auto</option><option>cool</option><option>heat</option><option>dry</option><option>fan</option></select></div>";
    html += "<div><label>Temp (C)</label><input name='temp' value='24'></div>";
    html += "<div><label>Fan</label><select name='fan'><option>auto</option><option>low</option><option>medium</option><option>high</option></select></div>";
    html += "<div><label>Swing V</label><select name='swingv'><option>off</option><option>auto</option><option>low</option><option>middle</option><option>high</option></select></div>";
    html += "<div><label>Swing H</label><select name='swingh'><option>off</option><option>auto</option><option>left</option><option>middle</option><option>right</option></select></div>";
    html += "<div><label>Encoding (custom)</label><select name='encoding'><option value=''>default</option><option value='pronto'>pronto</option><option value='gc'>gc</option></select></div>";
    html += "<div><label>Custom code (optional)</label><input name='code' placeholder='pronto/gc code'></div>";
    html += "</div>";
    html += "<button type='submit'>Send Test</button></form>";
  }
  html += "<h4>Generated JSON</h4><pre id='jsonPreview'>{}</pre>";
  html += "<script>";
  html += "const form=document.getElementById('testForm');";
  html += "if(form){const preview=document.getElementById('jsonPreview');";
  html += "const update=()=>{const data={cmd:'send'};const fd=new FormData(form);";
  html += "for(const [k,v] of fd.entries()){if(v==='')continue;data[k]=v;}preview.textContent=JSON.stringify(data,null,2);};";
  html += "form.addEventListener('input',update);update();}";
  html += "</script></div>";

  html += pageFooter();
  web.send(200, "text/html", html);
}

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
  h.emitterIndex = web.arg("emitter").toInt();
  h.model = web.arg("model").toInt();
  if (h.protocol == "CUSTOM") {
    h.isCustom = true;
    h.customEncoding = web.arg("custom_encoding");
    h.customOff = web.arg("custom_off");
    String tempsJson = web.arg("custom_temps");
    if (tempsJson.length()) {
      JsonDocument doc;
      if (deserializeJson(doc, tempsJson) == DeserializationError::Ok) {
        for (JsonPair kv : doc.as<JsonObject>()) {
          if (h.customTempCount >= kMaxCustomTemps) break;
          h.customTemps[h.customTempCount].tempC = atoi(kv.key().c_str());
          h.customTemps[h.customTempCount].code = kv.value().as<String>();
          h.customTempCount++;
        }
      }
    }
  }
  saveConfig();
  Serial.print("web: hvac added id=");
  Serial.println(id);
  web.sendHeader("Location", "/hvacs");
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
  saveConfig();
  Serial.print("web: hvac deleted index ");
  Serial.println(idx);
  web.sendHeader("Location", "/hvacs");
  web.send(302, "text/plain", "");
}

void handleConfigDownload() {
  if (!checkAuth()) { requestAuth(); return; }
  if (!SPIFFS.exists(kConfigPath)) {
    web.send(404, "text/plain", "No config file");
    return;
  }
  File f = SPIFFS.open(kConfigPath, FILE_READ);
  web.streamFile(f, "application/json");
  f.close();
}

File uploadFile;
void handleConfigUploadPage() {
  if (!checkAuth()) { requestAuth(); return; }
  String html = pageHeader("Upload Config");
  html += "<div class='card'><h2>Upload Config</h2>";
  html += "<form method='POST' action='/config/upload' enctype='multipart/form-data'>";
  html += "<input type='file' name='config'>";
  html += "<button type='submit'>Upload</button></form></div>";
  html += pageFooter();
  web.send(200, "text/html", html);
}

void handleConfigUpload() {
  if (!checkAuth()) { requestAuth(); return; }
  HTTPUpload &up = web.upload();
  if (up.status == UPLOAD_FILE_START) {
    uploadFile = SPIFFS.open(kConfigPath, FILE_WRITE);
    Serial.println("web: upload start");
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) uploadFile.write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    if (uploadFile) uploadFile.close();
    Serial.println("web: upload complete");
  }
}

void handleConfigUploadDone() {
  if (!checkAuth()) { requestAuth(); return; }
  loadConfig();
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

bool processCommand(JsonDocument &doc, JsonDocument &resp) {
  String cmd = doc["cmd"] | "send";
  if (cmd == "help") {
    resp["ok"] = true;
    JsonObject help = resp["help"].to<JsonObject>();
    JsonArray cmds = help["commands"].to<JsonArray>();
    cmds.add("list");
    cmds.add("send");
    cmds.add("raw");
    cmds.add("help");
    JsonArray examples = help["examples"].to<JsonArray>();
    examples.add("{\"cmd\":\"list\"}");
    examples.add("{\"cmd\":\"send\",\"id\":\"1\",\"power\":\"on\",\"mode\":\"cool\",\"temp\":24,\"fan\":\"auto\"}");
    examples.add("{\"cmd\":\"raw\",\"emitter\":0,\"encoding\":\"pronto\",\"code\":\"0000,0067,...\"}");
    return true;
  }

  if (cmd == "list") {
    resp["ok"] = true;
    JsonArray em = resp["emitters"].to<JsonArray>();
    for (uint8_t i = 0; i < config.emitterCount; i++) {
      JsonObject o = em.add<JsonObject>();
      o["index"] = i;
      o["gpio"] = config.emitterGpios[i];
    }
    JsonArray hv = resp["hvacs"].to<JsonArray>();
    for (uint8_t i = 0; i < config.hvacCount; i++) {
      JsonObject o = hv.add<JsonObject>();
      o["id"] = config.hvacs[i].id;
      o["protocol"] = config.hvacs[i].protocol;
      o["emitter"] = config.hvacs[i].emitterIndex;
      o["model"] = config.hvacs[i].model;
      o["custom"] = config.hvacs[i].isCustom;
    }
    return true;
  }

  if (cmd == "raw") {
    int emitterIndex = doc["emitter"] | 0;
    String encoding = doc["encoding"] | "pronto";
    String code = doc["code"] | "";
    EmitterRuntime *em = getEmitter(emitterIndex);
    if (!em) { resp["ok"] = false; resp["error"] = "invalid_emitter"; return false; }
    bool ok = sendCustomCode(HvacConfig(), em, code, encoding);
    resp["ok"] = ok;
    if (!ok) resp["error"] = "send_failed";
    return ok;
  }

  if (cmd != "send") {
    resp["ok"] = false;
    resp["error"] = "unknown_cmd";
    return false;
  }

  String id = doc["id"] | "";
  if (!id.length()) { resp["ok"] = false; resp["error"] = "missing_id"; return false; }
  HvacConfig *hvac = nullptr;
  if (!findHvacById(id, hvac) || !hvac) {
    resp["ok"] = false;
    resp["error"] = "unknown_id";
    return false;
  }
  EmitterRuntime *em = getEmitter(hvac->emitterIndex);
  if (!em || !em->ac) { resp["ok"] = false; resp["error"] = "invalid_emitter"; return false; }

  if (hvac->isCustom || hvac->protocol == "CUSTOM") {
    String encoding = doc["encoding"] | hvac->customEncoding;
    bool power = IRac::strToBool((const char*)(doc["power"] | "on"));
    String command = doc["command"] | "";
    if (command == "off") power = false;
    String code = doc["code"] | "";
    if (!power) {
      if (!hvac->customOff.length()) { resp["ok"] = false; resp["error"] = "missing_custom_off"; return false; }
      code = hvac->customOff;
    } else if (code.length() == 0 && doc["temp"].is<int>()) {
      int temp = doc["temp"] | 0;
      code = findCustomTempCode(*hvac, temp);
      if (!code.length()) { resp["ok"] = false; resp["error"] = "missing_temp_code"; return false; }
    }
    if (!code.length()) { resp["ok"] = false; resp["error"] = "missing_code"; return false; }
    bool ok = sendCustomCode(*hvac, em, code, encoding);
    resp["ok"] = ok;
    if (!ok) resp["error"] = "send_failed";
    return ok;
  }

  decode_type_t proto = strToDecodeType(hvac->protocol.c_str());
  if (!IRac::isProtocolSupported(proto)) {
    resp["ok"] = false;
    resp["error"] = "unsupported_protocol";
    return false;
  }
  bool power = IRac::strToBool((const char*)(doc["power"] | "on"));
  String command = doc["command"] | "";
  if (command == "off") power = false;
  stdAc::opmode_t mode = IRac::strToOpmode((const char*)(doc["mode"] | "auto"));
  if (!power) mode = stdAc::opmode_t::kOff;
  float temp = doc["temp"] | 24.0;
  bool celsius = IRac::strToBool((const char*)(doc["celsius"] | "true"), true);
  stdAc::fanspeed_t fan = IRac::strToFanspeed((const char*)(doc["fan"] | "auto"));
  stdAc::swingv_t swingv = IRac::strToSwingV((const char*)(doc["swingv"] | "off"));
  stdAc::swingh_t swingh = IRac::strToSwingH((const char*)(doc["swingh"] | "off"));
  bool quiet = IRac::strToBool((const char*)(doc["quiet"] | "false"));
  bool turbo = IRac::strToBool((const char*)(doc["turbo"] | "false"));
  bool econo = IRac::strToBool((const char*)(doc["econo"] | "false"));
  bool light = IRac::strToBool((const char*)(doc["light"] | "false"));
  bool filter = IRac::strToBool((const char*)(doc["filter"] | "false"));
  bool clean = IRac::strToBool((const char*)(doc["clean"] | "false"));
  bool beep = IRac::strToBool((const char*)(doc["beep"] | "false"));
  int16_t sleep = doc["sleep"] | -1;
  int16_t clock = doc["clock"] | -1;
  int16_t model = doc["model"].is<int>() ? (int16_t)(doc["model"] | -1) : hvac->model;

  bool ok = em->ac->sendAc(proto, model, power, mode, temp, celsius,
                           fan, swingv, swingh, quiet, turbo, econo,
                           light, filter, clean, beep, sleep, clock);
  resp["ok"] = ok;
  if (!ok) resp["error"] = "send_failed";
  return ok;
}

void handleHvacTest() {
  if (!checkAuth()) { requestAuth(); return; }
  JsonDocument cmd;
  cmd["cmd"] = "send";
  cmd["id"] = web.arg("id");
  if (web.arg("power").length()) cmd["power"] = web.arg("power");
  if (web.arg("mode").length()) cmd["mode"] = web.arg("mode");
  if (web.arg("temp").length()) cmd["temp"] = web.arg("temp").toFloat();
  if (web.arg("fan").length()) cmd["fan"] = web.arg("fan");
  if (web.arg("swingv").length()) cmd["swingv"] = web.arg("swingv");
  if (web.arg("swingh").length()) cmd["swingh"] = web.arg("swingh");
  if (web.arg("encoding").length()) cmd["encoding"] = web.arg("encoding");
  if (web.arg("code").length()) cmd["code"] = web.arg("code");

  JsonDocument resp;
  processCommand(cmd, resp);

  String reqStr;
  String respStr;
  serializeJsonPretty(cmd, reqStr);
  serializeJsonPretty(resp, respStr);

  String html = pageHeader("HVAC Test");
  html += "<div class='card'><h2>Test Result</h2>";
  html += "<h4>Request JSON</h4><pre>" + htmlEscape(reqStr) + "</pre>";
  html += "<h4>Response JSON</h4><pre>" + htmlEscape(respStr) + "</pre>";
  html += "<a href='/hvacs'>Back</a></div>";
  html += pageFooter();
  web.send(200, "text/html", html);
}

void setupWeb() {
  web.on("/", handleHome);
  web.on("/config", handleConfigPage);
  web.on("/config/save", HTTP_POST, handleConfigSave);
  web.on("/emitters", handleEmittersPage);
  web.on("/emitters/add", HTTP_POST, handleEmittersAdd);
  web.on("/emitters/delete", HTTP_GET, handleEmittersDelete);
  web.on("/hvacs", handleHvacsPage);
  web.on("/hvacs/add", HTTP_POST, handleHvacsAdd);
  web.on("/hvacs/test", HTTP_POST, handleHvacTest);
  web.on("/hvacs/delete", HTTP_GET, handleHvacsDelete);
  web.on("/config/download", HTTP_GET, handleConfigDownload);
  web.on("/config/upload", HTTP_GET, handleConfigUploadPage);
  web.on("/config/upload", HTTP_POST, handleConfigUploadDone, handleConfigUpload);
  web.on("/api/config", HTTP_GET, handleApiConfig);
  web.begin();
}

// ---- Telnet handling ----

void sendTelnetJson(WiFiClient &client, JsonDocument &doc) {
  serializeJson(doc, client);
  client.print("\r\n");
}

void respondTelnetError(WiFiClient &client, const String &message) {
  JsonDocument doc;
  doc["ok"] = false;
  doc["error"] = message;
  sendTelnetJson(client, doc);
}

bool sendCustomCode(const HvacConfig &hvac, EmitterRuntime *em, const String &code, const String &encoding) {
  if (!em || !em->raw) return false;
  if (encoding == "pronto") {
    return parseStringAndSendPronto(em->raw, code, 0);
  }
  if (encoding == "gc") {
    return parseStringAndSendGC(em->raw, code);
  }
  return false;
}

String findCustomTempCode(const HvacConfig &hvac, int tempC) {
  for (uint8_t i = 0; i < hvac.customTempCount; i++) {
    if (hvac.customTemps[i].tempC == tempC) return hvac.customTemps[i].code;
  }
  return "";
}

void handleTelnetLine(WiFiClient &client, const String &line) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) {
    respondTelnetError(client, "invalid_json");
    return;
  }
  JsonDocument resp;
  String cmd = doc["cmd"] | "send";
  processCommand(doc, resp);
  sendTelnetJson(client, resp);
  Serial.print("telnet: ");
  Serial.println(cmd);
}

void handleTelnet() {
  WiFiClient incoming = telnetServer.available();
  if (incoming) {
    bool assigned = false;
    for (uint8_t i = 0; i < kMaxTelnetClients; i++) {
      if (!telnetClients[i] || !telnetClients[i].connected()) {
        telnetClients[i].stop();
        telnetClients[i] = incoming;
        telnetBuffers[i] = "";
        assigned = true;
        Serial.print("telnet: client connected slot ");
        Serial.print(i);
        Serial.print(" from ");
        Serial.print(incoming.remoteIP());
        Serial.print(":");
        Serial.println(incoming.remotePort());
        break;
      }
    }
    if (!assigned) {
      incoming.stop();
      Serial.println("telnet: client rejected (full)");
    }
  }

  for (uint8_t i = 0; i < kMaxTelnetClients; i++) {
    WiFiClient &c = telnetClients[i];
    if (!c || !c.connected()) {
      if (c) c.stop();
      continue;
    }
    while (c.available()) {
      char ch = static_cast<char>(c.read());
      if (ch == '\r') continue;
      if (ch == '\n') {
        String line = telnetBuffers[i];
        telnetBuffers[i] = "";
        if (line.length()) handleTelnetLine(c, line);
      } else {
        telnetBuffers[i] += ch;
      }
    }
  }
}

void startWifi() {
  if (config.wifi.ssid.length() == 0) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(kApSsid);
    Serial.print("wifi: AP mode SSID=");
    Serial.println(kApSsid);
    Serial.print("wifi: AP IP=");
    Serial.println(WiFi.softAPIP());
    return;
  }
  WiFi.mode(WIFI_STA);
  if (!config.wifi.dhcp) {
    WiFi.config(config.wifi.ip, config.wifi.gateway, config.wifi.subnet, config.wifi.dns);
  }
  WiFi.begin(config.wifi.ssid.c_str(), config.wifi.password.c_str());
  Serial.print("wifi: connecting to ");
  Serial.println(config.wifi.ssid);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(200);
  }
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(kApSsid);
    Serial.println("wifi: connect failed, fallback to AP");
    Serial.print("wifi: AP IP=");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.print("wifi: connected IP=");
    Serial.println(WiFi.localIP());
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("IR HVAC Telnet Server boot");

  if (!SPIFFS.begin(true)) {
    Serial.println("fs: SPIFFS mount failed");
  } else {
    Serial.println("fs: SPIFFS mounted");
  }
  loadConfig();
  rebuildEmitters();
  startWifi();
  setupWeb();
  telnetServer.begin();
  telnetServer.setNoDelay(true);
  Serial.print("telnet: listening on ");
  Serial.println(kTelnetPort);
}

void loop() {
  web.handleClient();
  handleTelnet();
}
