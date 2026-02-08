# 1 "C:\\Users\\Paulo\\AppData\\Local\\Temp\\tmpp78aja6j"
#include <Arduino.h>
# 1 "I:/irhvactelnet/IRremoteESP8266/examples/HvacTelnetServer/HvacTelnetServer.ino"



#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <Update.h>

#include <IRremoteESP8266.h>
#include <IRac.h>
#include <IRsend.h>
#include <IRutils.h>


static const uint8_t kMaxTelnetClients = 4;
static const uint8_t kMaxEmitters = 8;
static const uint8_t kMaxHvacs = 32;
static const uint8_t kMaxCustomTemps = 16;
static const uint16_t kDefaultTelnetPort = 4998;

static const char *kConfigPath = "/config.json";
static const char *kApSsid = "IR-HVAC-Setup";
static const char *kDefaultHostname = "ir-server";

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
  String customEncoding;
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
  String hostname;
  uint16_t telnetPort = kDefaultTelnetPort;
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

struct HvacRuntimeState {
  bool initialized = false;
  bool power = false;
  String mode = "off";
  float setpoint = 24.0f;
  float currentTemp = 24.0f;
  String fan = "auto";
  bool light = false;
};

Config config;
EmitterRuntime emitters[kMaxEmitters];
HvacRuntimeState hvacStates[kMaxHvacs];
uint8_t emitterRuntimeCount = 0;

WebServer web(80);
WiFiServer *telnetServer = nullptr;
WiFiClient telnetClients[kMaxTelnetClients];
String telnetBuffers[kMaxTelnetClients];
DNSServer dnsServer;

void initHvacRuntimeStates();
bool processCommand(JsonDocument &doc, JsonDocument &resp, int8_t sourceTelnetSlot = -1);
bool sendCustomCode(const HvacConfig &hvac, EmitterRuntime *em, const String &code, const String &encoding);
String findCustomTempCode(const HvacConfig &hvac, int tempC);
bool isAuthRequired();
bool checkAuth();
void requestAuth();
String htmlEscape(const String &input);
uint16_t countValuesInStr(const String str, char sep);
bool isTokenSep(char c);
bool isHexDigitChar(char c);
uint16_t countTokensFlexible(const String &str);
bool nextTokenFlexible(const String &str, size_t &pos, String &token);
bool parseStringAndSendGC(IRsend *irsend, const String str);
bool parseStringAndSendPronto(IRsend *irsend, const String str, uint16_t repeats);
bool parseStringAndSendRacepoint(IRsend *irsend, const String str);
String configToJsonString();
void saveConfig();
void clearConfig();
void loadConfig();
void rebuildEmitters();
bool findHvacById(const String &id, HvacConfig *&out);
EmitterRuntime* getEmitter(uint8_t idx);
void resetHvacRuntimeState(uint8_t idx);
int8_t findHvacIndexById(const String &id);
String normalizeMode(const String &in);
String normalizeFan(const String &in);
bool floatChanged(float a, float b);
bool hvacStateChanged(const HvacRuntimeState &a, const HvacRuntimeState &b);
void ensureHvacStateInitialized(uint8_t idx);
void writeStateJson(JsonObject state, const String &id, const HvacRuntimeState &hvacState);
void sendTelnetJson(WiFiClient &client, JsonDocument &doc);
void broadcastStateToTelnetClients(const String &id, const HvacRuntimeState &state, int8_t excludeSlot);
void sendAllStatesToTelnetClient(WiFiClient &client);
String protocolOptionsHtml(const String &selected);
String emitterOptionsHtml(int selectedIndex);
String nextHvacId();
String networkListHtml();
String pageHeader(const String &title);
String pageFooter();
void handleHome();
void handleConfigPage();
void handleConfigSave();
void handleEmittersPage();
void handleEmittersAdd();
void handleEmittersDelete();
void handleHvacsPage();
void handleHvacsAdd();
void handleHvacsUpdate();
void handleHvacsDelete();
void handleHvacTestPage();
void handleConfigDownload();
void handleConfigUploadPage();
void handleConfigUpload();
void handleConfigUploadDone();
void handleFirmwarePage();
void handleFirmwareUpdate();
void handleFirmwareUpload();
void handleApiConfig();
void handleWifiScan();
bool processCommand(JsonDocument &doc, JsonDocument &resp, int8_t sourceTelnetSlot);
void handleHvacTest();
void handleRawTest();
void handleCaptive204();
void handleCaptiveRedirect();
void setupWeb();
void respondTelnetError(WiFiClient &client, const String &message);
void handleTelnetLine(WiFiClient &client, const String &line, int8_t sourceTelnetSlot);
void handleTelnet();
void setupArduinoOta();
void startMdns();
void startWifi();
void setup();
void loop();
#line 106 "I:/irhvactelnet/IRremoteESP8266/examples/HvacTelnetServer/HvacTelnetServer.ino"
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

bool isTokenSep(char c) {
  return c == ',' || c == ';' || c == ' ' || c == '\t';
}

bool isHexDigitChar(char c) {
  return (c >= '0' && c <= '9') ||
         (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

uint16_t countTokensFlexible(const String &str) {
  uint16_t count = 0;
  bool in_token = false;
  for (size_t i = 0; i < str.length(); i++) {
    if (isTokenSep(str[i])) {
      if (in_token) {
        count++;
        in_token = false;
      }
    } else {
      in_token = true;
    }
  }
  if (in_token) count++;
  return count;
}

bool nextTokenFlexible(const String &str, size_t &pos, String &token) {
  const size_t len = str.length();
  while (pos < len && isTokenSep(str[pos])) pos++;
  if (pos >= len) return false;
  size_t start = pos;
  while (pos < len && !isTokenSep(str[pos])) pos++;
  token = str.substring(start, pos);
  return true;
}

uint16_t *newCodeArray(const uint16_t size) {
  uint16_t *result = reinterpret_cast<uint16_t*>(malloc(size * sizeof(uint16_t)));
  return result;
}

bool parseStringAndSendGC(IRsend *irsend, const String str) {
#if SEND_GLOBALCACHE
  String tmp_str = str;
  tmp_str.trim();

  if (tmp_str.startsWith(PSTR("sendir,"))) tmp_str = tmp_str.substring(7);
  if (tmp_str.startsWith(PSTR("1:1,1,"))) tmp_str = tmp_str.substring(6);
  uint16_t count = countTokensFlexible(tmp_str);
  uint16_t *code_array = newCodeArray(count);
  if (!code_array) return false;
  uint16_t filled = 0;
  size_t pos = 0;
  String token;
  while (nextTokenFlexible(tmp_str, pos, token) && filled < count) {
    code_array[filled++] = token.toInt();
  }
  irsend->sendGC(code_array, filled);
  free(code_array);
  return filled > 0;
#else
  (void)irsend;
  (void)str;
  return false;
#endif
}

bool parseStringAndSendPronto(IRsend *irsend, const String str, uint16_t repeats) {
#if SEND_PRONTO
  String tmp_str = str;
  tmp_str.trim();


  uint16_t count = countTokensFlexible(tmp_str);
  size_t pos = 0;
  String token;


  if (nextTokenFlexible(tmp_str, pos, token)) {
    if (token.length() > 1 && (token[0] == 'R' || token[0] == 'r')) {
      repeats = token.substring(1).toInt();
      count--;
    } else {

      pos = 0;
    }
  }

  if (count < kProntoMinLength) return false;
  uint16_t *code_array = newCodeArray(count);
  if (!code_array) return false;
  uint16_t filled = 0;

  while (nextTokenFlexible(tmp_str, pos, token) && filled < count) {
    code_array[filled++] = strtoul(token.c_str(), NULL, 16);
  }
  irsend->sendPronto(code_array, filled, repeats);
  free(code_array);
  return filled > 0;
#else
  (void)irsend;
  (void)str;
  (void)repeats;
  return false;
#endif
}

bool parseStringAndSendRacepoint(IRsend *irsend, const String str) {
#if SEND_RAW
  if (!irsend) return false;
  String hex;
  hex.reserve(str.length());
  for (size_t i = 0; i < str.length(); i++) {
    char c = str[i];
    if (isHexDigitChar(c)) hex += c;
  }
  if (hex.length() < 8 || (hex.length() % 4) != 0) return false;

  const uint16_t wordCount = hex.length() / 4;
  uint16_t *words = newCodeArray(wordCount);
  if (!words) return false;

  char buf[5];
  buf[4] = '\0';
  for (uint16_t i = 0; i < wordCount; i++) {
    buf[0] = hex[i * 4];
    buf[1] = hex[i * 4 + 1];
    buf[2] = hex[i * 4 + 2];
    buf[3] = hex[i * 4 + 3];
    words[i] = static_cast<uint16_t>(strtoul(buf, NULL, 16));
  }

  uint16_t freq = 0;
  uint16_t start = 0;
  for (uint16_t i = 0; i < wordCount; i++) {
    if (words[i] >= 20000 && words[i] <= 60000) {
      freq = words[i];
      start = i + 1;
      break;
    }
  }
  if (freq == 0 || start >= wordCount) {
    free(words);
    return false;
  }

  uint16_t end = wordCount;
  while (end > start && words[end - 1] == 0) end--;
  uint16_t pulseCount = end - start;
  if (pulseCount == 0) {
    free(words);
    return false;
  }

  irsend->enableIROut(freq);
  for (uint16_t i = 0; i < pulseCount; i++) {
    uint32_t duration = (static_cast<uint32_t>(words[start + i]) * 1000000UL + (freq / 2)) / freq;
    if ((i & 1) == 0) {
      while (duration > 0) {
        uint16_t chunk = duration > 65535 ? 65535 : static_cast<uint16_t>(duration);
        irsend->mark(chunk);
        duration -= chunk;
      }
    } else {
      irsend->space(duration);
    }
  }
  irsend->space(0);
  free(words);
  return true;
#else
  (void)irsend;
  (void)str;
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

  doc["hostname"] = config.hostname.length() ? config.hostname : kDefaultHostname;
  doc["telnet_port"] = config.telnetPort;

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
  config.hostname = kDefaultHostname;
  config.telnetPort = kDefaultTelnetPort;
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
  initHvacRuntimeStates();
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

  config.hostname = doc["hostname"] | kDefaultHostname;

  config.telnetPort = doc["telnet_port"] | kDefaultTelnetPort;

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

void initHvacRuntimeStates() {
  for (uint8_t i = 0; i < kMaxHvacs; i++) {
    hvacStates[i] = HvacRuntimeState();
  }
}

void resetHvacRuntimeState(uint8_t idx) {
  if (idx >= kMaxHvacs) return;
  hvacStates[idx] = HvacRuntimeState();
}

int8_t findHvacIndexById(const String &id) {
  for (uint8_t i = 0; i < config.hvacCount; i++) {
    if (config.hvacs[i].id == id) return static_cast<int8_t>(i);
  }
  return -1;
}

String normalizeMode(const String &in) {
  String out = in;
  out.toLowerCase();
  if (out == "cool" || out == "heat" || out == "dry" || out == "fan" ||
      out == "off") return out;
  return "auto";
}

String normalizeFan(const String &in) {
  String out = in;
  out.toLowerCase();
  if (out == "min" || out == "low" || out == "medium" || out == "high" ||
      out == "max") return out;
  return "auto";
}

String opmodeToString(stdAc::opmode_t mode) {
  switch (mode) {
    case stdAc::opmode_t::kCool: return "cool";
    case stdAc::opmode_t::kHeat: return "heat";
    case stdAc::opmode_t::kDry: return "dry";
    case stdAc::opmode_t::kFan: return "fan";
    case stdAc::opmode_t::kOff: return "off";
    default: return "auto";
  }
}

String fanToString(stdAc::fanspeed_t fan) {
  switch (fan) {
    case stdAc::fanspeed_t::kMin: return "min";
    case stdAc::fanspeed_t::kLow: return "low";
    case stdAc::fanspeed_t::kMedium: return "medium";
    case stdAc::fanspeed_t::kHigh: return "high";
    case stdAc::fanspeed_t::kMax: return "max";
    default: return "auto";
  }
}

bool floatChanged(float a, float b) {
  return (a > b + 0.05f) || (b > a + 0.05f);
}

bool hvacStateChanged(const HvacRuntimeState &a, const HvacRuntimeState &b) {
  if (a.initialized != b.initialized) return true;
  if (!a.initialized && !b.initialized) return false;
  if (a.power != b.power) return true;
  if (a.mode != b.mode) return true;
  if (a.fan != b.fan) return true;
  if (a.light != b.light) return true;
  if (floatChanged(a.setpoint, b.setpoint)) return true;
  if (floatChanged(a.currentTemp, b.currentTemp)) return true;
  return false;
}

void ensureHvacStateInitialized(uint8_t idx) {
  if (idx >= kMaxHvacs) return;
  if (hvacStates[idx].initialized) return;
  hvacStates[idx].initialized = true;
  hvacStates[idx].power = false;
  hvacStates[idx].mode = "off";
  hvacStates[idx].setpoint = 24.0f;
  hvacStates[idx].currentTemp = 24.0f;
  hvacStates[idx].fan = "auto";
  hvacStates[idx].light = false;
}

void writeStateJson(JsonObject state, const String &id, const HvacRuntimeState &hvacState) {
  state["type"] = "state";
  state["id"] = id;
  state["power"] = hvacState.power ? "on" : "off";
  state["mode"] = hvacState.mode;
  state["setpoint"] = hvacState.setpoint;

  state["current_temp"] = hvacState.setpoint;
  state["fan"] = hvacState.fan;
  state["light"] = hvacState.light ? "on" : "off";
}

void sendTelnetJson(WiFiClient &client, JsonDocument &doc) {
  String payload;
  payload.reserve(512);
  serializeJson(doc, payload);
  payload += '\n';
  client.print(payload);
}

void broadcastStateToTelnetClients(const String &id, const HvacRuntimeState &state, int8_t excludeSlot) {
  JsonDocument msg;
  writeStateJson(msg.to<JsonObject>(), id, state);
  for (uint8_t i = 0; i < kMaxTelnetClients; i++) {
    if (static_cast<int8_t>(i) == excludeSlot) continue;
    WiFiClient &c = telnetClients[i];
    if (!c || !c.connected()) continue;
    sendTelnetJson(c, msg);
  }
}

void sendAllStatesToTelnetClient(WiFiClient &client) {
  for (uint8_t i = 0; i < config.hvacCount; i++) {
    ensureHvacStateInitialized(i);
    JsonDocument msg;
    writeStateJson(msg.to<JsonObject>(), config.hvacs[i].id, hvacStates[i]);
    sendTelnetJson(client, msg);
  }
}

String protocolOptionsHtml(const String &selected) {
  String out;
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
  html += "code,pre{background:#0b1220;border:1px solid #334155;border-radius:8px;padding:8px;display:block;white-space:pre-wrap;}";
  html += ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px;}";
  html += ".pill{display:inline-block;background:#1e293b;color:#e2e8f0;padding:2px 8px;border-radius:999px;font-size:12px;margin-left:6px;}";
  html += "</style></head><body><div class='wrap'>";
  html += "<nav><a href='/'>Home</a><a href='/config'>Config</a><a href='/emitters'>Emitters</a><a href='/hvacs'>HVACs</a><a href='/hvacs/test'>Test HVAC</a><a href='/firmware'>Firmware</a><a href='/config/upload'>Upload</a><a href='/config/download'>Download</a></nav>";
  return html;
}

String pageFooter() { return "</div></body></html>"; }



void handleHome() {
  if (!checkAuth()) { requestAuth(); return; }
  String html = pageHeader("IR HVAC Telnet");
  html += "<div class='card'><h2>IR HVAC Telnet Server</h2>";
  html += "<p>Telnet port: <strong>" + String(config.telnetPort) + "</strong></p>";
  html += "<p>WiFi mode: <strong>" + String(WiFi.isConnected() ? "STA" : "AP") + "</strong></p>";
  html += "<p>IP: <strong>" + WiFi.localIP().toString() + "</strong></p>";
  html += "<p>Hostname: <strong>" + htmlEscape(config.hostname.length() ? config.hostname : kDefaultHostname) + ".local</strong></p>";
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
  html += "<select id='ssidScan' name='ssid_scan'><option value=''>-- scan to load --</option></select>";
  html += "</div><div>";
  html += "<button class='secondary' type='button' id='scanBtn'>Scan Networks</button>";
  html += "</div></div>";
  html += "<label>WiFi Password</label>";
  html += "<input name='password' type='password' value='" + htmlEscape(config.wifi.password) + "'>";
  html += "<div class='row'><input type='checkbox' id='dhcpToggle' name='dhcp'" +
          String(config.wifi.dhcp ? " checked" : "") + "><label for='dhcpToggle'>DHCP</label></div>";
  html += "<label>Static IP</label><input name='ip' value='" + htmlEscape(config.wifi.ip.toString()) + "'>";
  html += "<label>Gateway</label><input name='gateway' value='" + htmlEscape(config.wifi.gateway.toString()) + "'>";
  html += "<label>Subnet</label><input name='subnet' value='" + htmlEscape(config.wifi.subnet.toString()) + "'>";
  html += "<label>DNS</label><input name='dns' value='" + htmlEscape(config.wifi.dns.toString()) + "'>";
  html += "<label>Hostname (mDNS .local)</label>";
  html += "<input name='hostname' maxlength='32' value='" +
          htmlEscape(config.hostname.length() ? config.hostname : kDefaultHostname) + "'>";
  html += "<label>Telnet Port</label>";
  html += "<input name='telnet_port' type='number' min='1' max='65535' value='" + String(config.telnetPort) + "'>";
  html += "<h3>Web Password</h3>";
  html += "<label>Admin password (blank = no auth)</label>";
  html += "<input name='webpass' type='password' value='" + htmlEscape(config.web.password) + "'>";
  html += "<button type='submit'>Save & Reboot</button>";
  html += "</form></div>";
  html += "<script>";
  html += "const scanBtn=document.getElementById('scanBtn');";
  html += "const ssidScan=document.getElementById('ssidScan');";
  html += "const dhcpToggle=document.getElementById('dhcpToggle');";
  html += "const ipFields=['ip','gateway','subnet','dns'].map(id=>document.querySelector(`input[name=${id}]`));";
  html += "const updateDhcp=()=>{const disabled=dhcpToggle.checked;ipFields.forEach(f=>{f.disabled=disabled;});};";
  html += "if(dhcpToggle){dhcpToggle.addEventListener('change',updateDhcp);updateDhcp();}";
  html += "if(scanBtn){scanBtn.addEventListener('click',async()=>{scanBtn.disabled=true;scanBtn.textContent='Scanning...';";
  html += "try{const res=await fetch('/api/wifi/scan');const data=await res.json();";
  html += "ssidScan.innerHTML='<option value=\"\">-- select --</option>';data.networks.forEach(n=>{";
  html += "const opt=document.createElement('option');opt.value=n.ssid;opt.textContent=`${n.ssid} (${n.rssi} dBm)`;ssidScan.appendChild(opt);});";
  html += "}catch(e){alert('Scan failed');}finally{scanBtn.disabled=false;scanBtn.textContent='Scan Networks';}});}";
  html += "</script>";
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
  String hostname = web.arg("hostname");
  hostname.trim();
  if (hostname.length() == 0) hostname = kDefaultHostname;
  config.hostname = hostname;
  uint16_t port = web.arg("telnet_port").toInt();
  if (port == 0) port = kDefaultTelnetPort;
  config.telnetPort = port;
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
  html += "<label>GPIO selection</label>";
  html += "<select name='gpios'>";
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
    html += "<label>Protocol</label><select name='protocol'>" + protocolOptionsHtml("") + "</select>";
    html += "<label>Emitter</label><select name='emitter'>" + emitterOptionsHtml(0) + "</select>";
    html += "<label>Model (optional)</label><input name='model' value='-1'>";
    html += "<button type='submit'>Add</button></form>";
  }
  if (config.hvacCount > 0 && config.emitterCount > 0) {
    html += "<h3>Edit HVAC</h3>";
    html += "<form method='POST' action='/hvacs/update' id='editHvacForm'>";
    html += "<label>HVAC</label><select name='index' id='editHvacIndex'>";
    for (uint8_t i = 0; i < config.hvacCount; i++) {
      const HvacConfig &h = config.hvacs[i];
      html += "<option value='" + String(i) + "' data-protocol='" + htmlEscape(h.protocol) +
              "' data-emitter='" + String(h.emitterIndex) + "' data-model='" + String(h.model) + "'>";
      html += htmlEscape(h.id) + " (" + htmlEscape(h.protocol) + ")</option>";
    }
    html += "</select>";
    html += "<label>Protocol</label><select name='protocol' id='editHvacProtocol'>" + protocolOptionsHtml("") + "</select>";
    html += "<label>Emitter</label><select name='emitter' id='editHvacEmitter'>" + emitterOptionsHtml(0) + "</select>";
    html += "<label>Model (optional)</label><input name='model' id='editHvacModel' value='-1'>";
    html += "<button type='submit' class='secondary'>Update</button></form>";
    html += "<script>";
    html += "const hvacSel=document.getElementById('editHvacIndex');";
    html += "const protoSel=document.getElementById('editHvacProtocol');";
    html += "const emSel=document.getElementById('editHvacEmitter');";
    html += "const modelInput=document.getElementById('editHvacModel');";
    html += "const sync=()=>{if(!hvacSel)return;const opt=hvacSel.selectedOptions[0];";
    html += "if(!opt)return;const p=opt.dataset.protocol||'';";
    html += "const e=opt.dataset.emitter||'0';const m=opt.dataset.model||'-1';";
    html += "if(protoSel){for(const o of protoSel.options){o.selected=(o.value===p);} }";
    html += "if(emSel){for(const o of emSel.options){o.selected=(o.value===e);} }";
    html += "if(modelInput){modelInput.value=m;}};";
    html += "if(hvacSel){hvacSel.addEventListener('change',sync);sync();}";
    html += "</script>";
  }
  html += "</div>";

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
  initHvacRuntimeStates();
  saveConfig();
  Serial.print("web: hvac added id=");
  Serial.println(id);
  web.sendHeader("Location", "/hvacs");
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
  h.emitterIndex = emitterIndex;
  h.model = model;
  h.isCustom = (protocol == "CUSTOM");
  resetHvacRuntimeState(static_cast<uint8_t>(idx));
  saveConfig();
  Serial.print("web: hvac updated index ");
  Serial.println(idx);
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
  initHvacRuntimeStates();
  saveConfig();
  Serial.print("web: hvac deleted index ");
  Serial.println(idx);
  web.sendHeader("Location", "/hvacs");
  web.send(302, "text/plain", "");
}

void handleHvacTestPage() {
  if (!checkAuth()) { requestAuth(); return; }
  String html = pageHeader("HVAC Test");
  html += "<div class='card'><h2>Test HVAC</h2>";
  if (config.hvacCount == 0) {
    html += "<p>No HVACs registered.</p>";
  } else {
    html += "<form id='testForm'>";
    html += "<div class='grid'>";
    html += "<div><label>HVAC</label><select name='id'>";
    for (uint8_t i = 0; i < config.hvacCount; i++) {
      ensureHvacStateInitialized(i);
      String lightValue = hvacStates[i].light ? "true" : "false";
      html += "<option value='" + htmlEscape(config.hvacs[i].id) + "' data-light='" + lightValue + "'>" +
              htmlEscape(config.hvacs[i].id) + " (" + htmlEscape(config.hvacs[i].protocol) + ")</option>";
    }
    html += "</select></div>";
    html += "<div><label>Power</label><select name='power'><option value='on'>on</option><option value='off'>off</option></select></div>";
    html += "<div><label>Mode</label><select name='mode'><option>auto</option><option>cool</option><option>heat</option><option>dry</option><option>fan</option></select></div>";
    html += "<div><label>Temp (C)</label><input name='temp' value='24'></div>";
    html += "<div><label>Fan</label><select name='fan'><option>auto</option><option>low</option><option>medium</option><option>high</option></select></div>";
    html += "<div><label>Swing V</label><select name='swingv'><option>off</option><option>auto</option><option>low</option><option>middle</option><option>high</option></select></div>";
    html += "<div><label>Swing H</label><select name='swingh'><option>off</option><option>auto</option><option>left</option><option>middle</option><option>right</option></select></div>";
    html += "<div><label>Light</label><select name='light'><option value=''>default</option><option value='true'>on</option><option value='false'>off</option></select></div>";
    html += "<div><label>Encoding (custom)</label><select name='encoding'><option value=''>default</option><option value='pronto'>pronto</option><option value='gc'>gc</option><option value='racepoint'>racepoint</option></select></div>";
    html += "<div><label>Custom code (optional)</label><input name='code' placeholder='pronto/gc code'></div>";
    html += "</div>";
    html += "<button type='submit'>Send Test</button></form>";
  }
  html += "<h4>Generated JSON</h4><pre id='jsonPreview'>{}</pre>";
  html += "<h4>Response</h4><pre id='jsonResponse'>-</pre></div>";
  html += "<div class='card'><h3>Send Raw Code</h3>";
  html += "<form id='rawForm'>";
  html += "<div class='grid'>";
  html += "<div><label>Emitter</label><select name='emitter'>" + emitterOptionsHtml(0) + "</select></div>";
  html += "<div><label>Encoding</label><select name='encoding'><option value='pronto'>pronto</option><option value='gc'>gc</option><option value='racepoint'>racepoint</option></select></div>";
  html += "<div><label>Code</label><input name='code' placeholder='0000,0067,...'></div>";
  html += "</div>";
  html += "<button type='submit'>Send Raw</button></form>";
  html += "<h4>Raw Response</h4><pre id='rawResponse'>-</pre></div>";
  html += "<script>";
  html += "const form=document.getElementById('testForm');";
  html += "const preview=document.getElementById('jsonPreview');";
  html += "const response=document.getElementById('jsonResponse');";
  html += "const hvacSelect=form?form.querySelector(\"select[name='id']\"):null;";
  html += "const lightSelect=form?form.querySelector(\"select[name='light']\"):null;";
  html += "const syncLight=()=>{if(!hvacSelect||!lightSelect)return;const opt=hvacSelect.selectedOptions[0];";
  html += "if(!opt)return;const val=opt.dataset.light;if(val==='true'||val==='false'){lightSelect.value=val;}else{lightSelect.value='';}};";
  html += "const update=()=>{if(!form)return;const data={cmd:'send'};const fd=new FormData(form);";
  html += "for(const [k,v] of fd.entries()){if(v==='')continue;data[k]=v;}preview.textContent=JSON.stringify(data,null,2);};";
  html += "if(hvacSelect){hvacSelect.addEventListener('change',()=>{syncLight();update();});}";
  html += "if(form){form.addEventListener('input',update);syncLight();update();";
  html += "form.addEventListener('submit',async(e)=>{e.preventDefault();response.textContent='Sending...';";
  html += "const fd=new FormData(form);const params=new URLSearchParams(fd);";
  html += "const res=await fetch('/hvacs/test',{method:'POST',body:params});";
  html += "const data=await res.json();response.textContent=JSON.stringify(data,null,2);});}";
  html += "const rawForm=document.getElementById('rawForm');const rawResponse=document.getElementById('rawResponse');";
  html += "if(rawForm){rawForm.addEventListener('submit',async(e)=>{e.preventDefault();rawResponse.textContent='Sending...';";
  html += "const fd=new FormData(rawForm);const params=new URLSearchParams(fd);";
  html += "const res=await fetch('/raw/test',{method:'POST',body:params});";
  html += "const data=await res.json();rawResponse.textContent=JSON.stringify(data,null,2);});}";
  html += "</script>";
  html += pageFooter();
  web.send(200, "text/html", html);
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

void handleFirmwarePage() {
  if (!checkAuth()) { requestAuth(); return; }
  String html = pageHeader("Firmware Update");
  html += "<div class='card'><h2>OTA Firmware Update</h2>";
  html += "<p>Upload a compiled ESP32 firmware binary (.bin) to flash over WiFi.</p>";
  html += "<form method='POST' action='/firmware/update' enctype='multipart/form-data'>";
  html += "<input type='file' name='firmware' accept='.bin,application/octet-stream' required>";
  html += "<button type='submit'>Upload & Flash</button></form>";
  html += "<p>Alternative OTA endpoint: ArduinoOTA on hostname <code>" +
          htmlEscape(config.hostname.length() ? config.hostname : kDefaultHostname) +
          ".local</code>.</p>";
  html += "</div>";
  html += pageFooter();
  web.send(200, "text/html", html);
}

void handleFirmwareUpdate() {
  if (!checkAuth()) { requestAuth(); return; }
  bool ok = !Update.hasError();
  String html = "<html><body><h3>";
  html += ok ? "Firmware updated. Rebooting..." : "Firmware update failed.";
  html += "</h3></body></html>";
  web.send(ok ? 200 : 500, "text/html", html);
  if (ok) {
    delay(500);
    ESP.restart();
  }
}

void handleFirmwareUpload() {
  if (!checkAuth()) { requestAuth(); return; }
  HTTPUpload &up = web.upload();
  if (up.status == UPLOAD_FILE_START) {
    Serial.printf("ota-web: start %s\n", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) {
      Update.printError(Serial);
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("ota-web: success %u bytes\n", up.totalSize);
    } else {
      Update.printError(Serial);
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    Serial.println("ota-web: aborted");
  }
}

void handleApiConfig() {
  if (!checkAuth()) { requestAuth(); return; }
  String json = configToJsonString();
  web.send(200, "application/json", json);
}

void handleWifiScan() {
  if (!checkAuth()) { requestAuth(); return; }
  JsonDocument doc;
  JsonArray networks = doc["networks"].to<JsonArray>();
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  int n = WiFi.scanNetworks(false, true);
  for (int i = 0; i < n; i++) {
    JsonObject o = networks.add<JsonObject>();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
  }
  String out;
  serializeJson(doc, out);
  web.send(200, "application/json", out);
}

bool processCommand(JsonDocument &doc, JsonDocument &resp, int8_t sourceTelnetSlot) {
  String cmd = doc["cmd"] | "send";
  if (cmd == "help") {
    resp["ok"] = true;
    JsonObject help = resp["help"].to<JsonObject>();
    JsonArray cmds = help["commands"].to<JsonArray>();
    cmds.add("list");
    cmds.add("send");
    cmds.add("get");
    cmds.add("get_all");
    cmds.add("raw");
    cmds.add("help");
    JsonArray examples = help["examples"].to<JsonArray>();
    examples.add("{\"cmd\":\"list\"}");
    examples.add("{\"cmd\":\"send\",\"id\":\"1\",\"power\":\"on\",\"mode\":\"cool\",\"temp\":24,\"fan\":\"auto\"}");
    examples.add("{\"cmd\":\"get\",\"id\":\"1\"}");
    examples.add("{\"cmd\":\"get_all\"}");
    examples.add("{\"cmd\":\"raw\",\"emitter\":0,\"encoding\":\"pronto\",\"code\":\"0000 006D 0000 ...\"}");
    examples.add("{\"cmd\":\"raw\",\"emitter\":0,\"encoding\":\"gc\",\"code\":\"sendir,1:1,1,38000,1,1,172,172,...\"}");
    examples.add("{\"cmd\":\"raw\",\"emitter\":0,\"encoding\":\"racepoint\",\"code\":\"0000000000009470...\"}");
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

  if (cmd == "get") {
    String id = doc["id"] | "";
    int8_t hvacIndex = findHvacIndexById(id);
    if (!id.length()) { resp["ok"] = false; resp["error"] = "missing_id"; return false; }
    if (hvacIndex < 0) { resp["ok"] = false; resp["error"] = "unknown_id"; return false; }
    ensureHvacStateInitialized(hvacIndex);
    writeStateJson(resp.to<JsonObject>(), id, hvacStates[hvacIndex]);
    return true;
  }

  if (cmd == "get_all") {
    JsonArray states = resp.to<JsonArray>();
    for (uint8_t i = 0; i < config.hvacCount; i++) {
      ensureHvacStateInitialized(i);
      writeStateJson(states.add<JsonObject>(), config.hvacs[i].id, hvacStates[i]);
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
  int8_t hvacIndex = findHvacIndexById(id);
  if (hvacIndex < 0) {
    resp["ok"] = false;
    resp["error"] = "unknown_id";
    return false;
  }
  EmitterRuntime *em = getEmitter(hvac->emitterIndex);
  if (!em || !em->ac) { resp["ok"] = false; resp["error"] = "invalid_emitter"; return false; }

  HvacRuntimeState previous = hvacStates[hvacIndex];
  HvacRuntimeState nextState = previous;
  if (!nextState.initialized) {
    ensureHvacStateInitialized(hvacIndex);
    nextState = hvacStates[hvacIndex];
    previous = hvacStates[hvacIndex];
  }
  bool hasCurrentTemp = doc["current_temp"].is<float>() ||
                        doc["current_temp"].is<double>() ||
                        doc["current_temp"].is<int>();

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

    String modeStr = normalizeMode(doc["mode"] | nextState.mode);
    String fanStr = normalizeFan(doc["fan"] | nextState.fan);
    float temp = doc["temp"] | nextState.setpoint;
    bool light = previous.light;
    if (!doc["light"].isNull()) {
      if (doc["light"].is<bool>()) {
        light = doc["light"].as<bool>();
      } else {
        light = IRac::strToBool((const char*)(doc["light"] | (light ? "true" : "false")));
      }
    }
    nextState.initialized = true;
    nextState.power = power;
    nextState.mode = power ? modeStr : "off";
    nextState.setpoint = temp;
    nextState.fan = fanStr;
    nextState.light = light;
    if (hasCurrentTemp) {
      nextState.currentTemp = doc["current_temp"].as<float>();
    } else if (!previous.initialized) {
      nextState.currentTemp = temp;
    }

    bool ok = sendCustomCode(*hvac, em, code, encoding);
    if (!ok) {
      resp["ok"] = false;
      resp["error"] = "send_failed";
      return false;
    }
    hvacStates[hvacIndex] = nextState;
    writeStateJson(resp.to<JsonObject>(), id, hvacStates[hvacIndex]);
    if (hvacStateChanged(previous, hvacStates[hvacIndex])) {
      broadcastStateToTelnetClients(id, hvacStates[hvacIndex], sourceTelnetSlot);
    }
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
  bool light = previous.light;
  if (!doc["light"].isNull()) {
    if (doc["light"].is<bool>()) {
      light = doc["light"].as<bool>();
    } else {
      light = IRac::strToBool((const char*)(doc["light"] | (light ? "true" : "false")));
    }
  }
  bool filter = IRac::strToBool((const char*)(doc["filter"] | "false"));
  bool clean = IRac::strToBool((const char*)(doc["clean"] | "false"));
  bool beep = IRac::strToBool((const char*)(doc["beep"] | "false"));
  int16_t sleep = doc["sleep"] | -1;
  int16_t clock = doc["clock"] | -1;
  int16_t model = doc["model"].is<int>() ? (int16_t)(doc["model"] | -1) : hvac->model;

  nextState.initialized = true;
  nextState.power = power;
  nextState.mode = opmodeToString(mode);
  nextState.setpoint = temp;
  nextState.fan = fanToString(fan);
  nextState.light = light;
  if (hasCurrentTemp) {
    nextState.currentTemp = doc["current_temp"].as<float>();
  } else if (!previous.initialized) {
    nextState.currentTemp = temp;
  }

  bool ok = em->ac->sendAc(proto, model, power, mode, temp, celsius,
                           fan, swingv, swingh, quiet, turbo, econo,
                           light, filter, clean, beep, sleep, clock);
  if (!ok) {
    resp["ok"] = false;
    resp["error"] = "send_failed";
    return false;
  }
  hvacStates[hvacIndex] = nextState;
  writeStateJson(resp.to<JsonObject>(), id, hvacStates[hvacIndex]);
  if (hvacStateChanged(previous, hvacStates[hvacIndex])) {
    broadcastStateToTelnetClients(id, hvacStates[hvacIndex], sourceTelnetSlot);
  }
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
  if (web.arg("light").length()) cmd["light"] = web.arg("light");
  if (web.arg("encoding").length()) cmd["encoding"] = web.arg("encoding");
  if (web.arg("code").length()) cmd["code"] = web.arg("code");

  JsonDocument resp;
  processCommand(cmd, resp);
  String respStr;
  serializeJson(resp, respStr);
  web.send(200, "application/json", respStr);
}

void handleRawTest() {
  if (!checkAuth()) { requestAuth(); return; }
  JsonDocument cmd;
  cmd["cmd"] = "raw";
  cmd["emitter"] = web.arg("emitter").toInt();
  if (web.arg("encoding").length()) cmd["encoding"] = web.arg("encoding");
  cmd["code"] = web.arg("code");
  JsonDocument resp;
  processCommand(cmd, resp);
  String respStr;
  serializeJson(resp, respStr);
  web.send(200, "application/json", respStr);
}


void handleCaptive204() { web.send(204); }
void handleCaptiveRedirect() {
  web.sendHeader("Location", "/");
  web.send(302, "text/plain", "");
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
  web.on("/hvacs/test", HTTP_GET, handleHvacTestPage);
  web.on("/hvacs/test", HTTP_POST, handleHvacTest);
  web.on("/hvacs/update", HTTP_POST, handleHvacsUpdate);
  web.on("/hvacs/delete", HTTP_GET, handleHvacsDelete);
  web.on("/raw/test", HTTP_POST, handleRawTest);


  web.on("/generate_204", HTTP_ANY, handleCaptive204);
  web.on("/gen_204", HTTP_ANY, handleCaptive204);
  web.on("/hotspot-detect.html", HTTP_ANY, handleCaptiveRedirect);
  web.on("/fwlink", HTTP_ANY, handleCaptiveRedirect);
  web.on("/connecttest.txt", HTTP_ANY, handleCaptiveRedirect);
  web.on("/ncsi.txt", HTTP_ANY, handleCaptiveRedirect);
  web.on("/library/test/success.html", HTTP_ANY, handleCaptiveRedirect);

  web.on("/config/download", HTTP_GET, handleConfigDownload);
  web.on("/config/upload", HTTP_GET, handleConfigUploadPage);
  web.on("/config/upload", HTTP_POST, handleConfigUploadDone, handleConfigUpload);
  web.on("/firmware", HTTP_GET, handleFirmwarePage);
  web.on("/firmware/update", HTTP_POST, handleFirmwareUpdate, handleFirmwareUpload);
  web.on("/api/config", HTTP_GET, handleApiConfig);
  web.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
  web.onNotFound([]() {
    web.sendHeader("Location", "/");
    web.send(302, "text/plain", "");
  });
  web.begin();
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
  if (encoding == "racepoint") {
    return parseStringAndSendRacepoint(em->raw, code);
  }
  return false;
}

String findCustomTempCode(const HvacConfig &hvac, int tempC) {
  for (uint8_t i = 0; i < hvac.customTempCount; i++) {
    if (hvac.customTemps[i].tempC == tempC) return hvac.customTemps[i].code;
  }
  return "";
}

void handleTelnetLine(WiFiClient &client, const String &line, int8_t sourceTelnetSlot) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) {
    respondTelnetError(client, "invalid_json");
    return;
  }
  JsonDocument resp;
  String cmd = doc["cmd"] | "send";
  processCommand(doc, resp, sourceTelnetSlot);
  sendTelnetJson(client, resp);
  Serial.print("telnet: ");
  Serial.println(cmd);
}

void handleTelnet() {
  if (!telnetServer) return;
  WiFiClient incoming = telnetServer->available();
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
        sendAllStatesToTelnetClient(telnetClients[i]);
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
        if (line.length()) handleTelnetLine(c, line, static_cast<int8_t>(i));
      } else {
        telnetBuffers[i] += ch;
      }
    }
  }
}

void setupArduinoOta() {
  const char *host = (config.hostname.length() ? config.hostname.c_str() : kDefaultHostname);
  ArduinoOTA.setHostname(host);
  if (config.web.password.length()) {
    ArduinoOTA.setPassword(config.web.password.c_str());
  }
  ArduinoOTA.onStart([]() {
    Serial.println("ota: start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nota: end");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("ota: %u%%\r", (progress * 100U) / total);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("ota: error[%u]\n", error);
  });
  ArduinoOTA.begin();
  Serial.print("ota: ready on ");
  Serial.print(host);
  Serial.println(".local");
}

void startMdns() {
#if ARDUINO_ARCH_ESP32
  const char *host = (config.hostname.length() ? config.hostname.c_str() : kDefaultHostname);
  if (!MDNS.begin(host)) {
    Serial.println("mdns: start failed");
    return;
  }
  MDNS.addService("http", "tcp", 80);
  Serial.print("mdns: responding for ");
  Serial.print(host);
  Serial.println(".local");
#endif
}

void startWifi() {
  if (config.wifi.ssid.length() == 0) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(kApSsid);
    WiFi.softAPsetHostname(config.hostname.length() ? config.hostname.c_str() : kDefaultHostname);
    Serial.print("wifi: AP mode SSID=");
    Serial.println(kApSsid);
    Serial.print("wifi: AP IP=");
    Serial.println(WiFi.softAPIP());
    dnsServer.start(53, "*", WiFi.softAPIP());
    startMdns();
    return;
  }
  dnsServer.stop();
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(config.hostname.length() ? config.hostname.c_str() : kDefaultHostname);
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
    WiFi.softAPsetHostname(kDefaultHostname);
    Serial.println("wifi: connect failed, fallback to AP");
    Serial.print("wifi: AP IP=");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.print("wifi: connected IP=");
    Serial.println(WiFi.localIP());
    startMdns();
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
  setupArduinoOta();
  setupWeb();
  if (telnetServer) {
    delete telnetServer;
    telnetServer = nullptr;
  }
  telnetServer = new WiFiServer(config.telnetPort);
  telnetServer->begin();
  telnetServer->setNoDelay(true);
  Serial.print("telnet: listening on ");
  Serial.println(config.telnetPort);
}

void loop() {
  web.handleClient();
  handleTelnet();
  dnsServer.processNextRequest();
  ArduinoOTA.handle();
}