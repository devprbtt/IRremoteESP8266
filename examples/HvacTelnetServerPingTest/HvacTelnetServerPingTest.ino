#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <Preferences.h>

static const char *kApSsid = "IR-Server-Test-Setup";
static const char *kDefaultHostname = "ir-server-test";
static const char *kPrefsNamespace = "pingtest";
static const char *kPrefsKey = "config";
static const char *kFirmwareVersion = "0.1.0-test";
static const char *kFilesystemVersion = "0.1.0-test";

struct WifiConfig {
  String ssid;
  String password;
  bool dhcp = true;
  String ip;
  String gateway;
  String subnet;
  String dns;
};

struct Config {
  WifiConfig wifi;
  String hostname = kDefaultHostname;
};

Config config;
WebServer web(80);
Preferences prefs;
bool apMode = false;
String statusMessage = "idle";

String configToJsonString() {
  JsonDocument doc;
  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["ssid"] = config.wifi.ssid;
  wifi["password"] = config.wifi.password;
  wifi["dhcp"] = config.wifi.dhcp;
  wifi["ip"] = config.wifi.ip;
  wifi["gateway"] = config.wifi.gateway;
  wifi["subnet"] = config.wifi.subnet;
  wifi["dns"] = config.wifi.dns;
  doc["hostname"] = config.hostname.length() ? config.hostname : kDefaultHostname;
  String out;
  serializeJson(doc, out);
  return out;
}

void saveConfig() {
  String json = configToJsonString();
  prefs.begin(kPrefsNamespace, false);
  prefs.putString(kPrefsKey, json);
  prefs.end();
}

void loadConfig() {
  config.hostname = kDefaultHostname;
  prefs.begin(kPrefsNamespace, true);
  String json = prefs.getString(kPrefsKey, "");
  prefs.end();
  if (!json.length()) return;

  JsonDocument doc;
  if (deserializeJson(doc, json)) return;
  JsonObject wifi = doc["wifi"];
  config.wifi.ssid = String((const char*)wifi["ssid"]);
  config.wifi.password = String((const char*)wifi["password"]);
  config.wifi.dhcp = wifi["dhcp"].isNull() ? true : bool(wifi["dhcp"]);
  config.wifi.ip = String((const char*)wifi["ip"]);
  config.wifi.gateway = String((const char*)wifi["gateway"]);
  config.wifi.subnet = String((const char*)wifi["subnet"]);
  config.wifi.dns = String((const char*)wifi["dns"]);
  String host = String((const char*)doc["hostname"]);
  host.trim();
  if (host.length()) config.hostname = host;
}

bool parseIp(const String &value, IPAddress &ip) {
  if (!value.length()) return false;
  return ip.fromString(value);
}

void startMdns() {
  MDNS.end();
  const char *host = config.hostname.length() ? config.hostname.c_str() : kDefaultHostname;
  if (MDNS.begin(host)) {
    MDNS.addService("http", "tcp", 80);
  }
}

void startApMode() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(kApSsid);
  WiFi.softAPsetHostname(config.hostname.length() ? config.hostname.c_str() : kDefaultHostname);
  statusMessage = "setup_ap";
  startMdns();
  Serial.print("wifi: AP mode SSID=");
  Serial.println(kApSsid);
}

void startWifi() {
  WiFi.disconnect(true, true);
  delay(100);
  String ssid = config.wifi.ssid;
  ssid.trim();
  if (!ssid.length()) {
    startApMode();
    return;
  }

  apMode = false;
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(config.hostname.length() ? config.hostname.c_str() : kDefaultHostname);
  if (!config.wifi.dhcp) {
    IPAddress ip, gw, sn, dns;
    if (parseIp(config.wifi.ip, ip) && parseIp(config.wifi.gateway, gw) && parseIp(config.wifi.subnet, sn)) {
      if (!parseIp(config.wifi.dns, dns)) dns = gw;
      WiFi.config(ip, gw, sn, dns);
    }
  }

  statusMessage = "connecting";
  WiFi.begin(config.wifi.ssid.c_str(), config.wifi.password.c_str());
  unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - started) < 15000UL) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    statusMessage = "connected";
    startMdns();
    Serial.print("wifi: connected ip=");
    Serial.println(WiFi.localIP());
    return;
  }

  statusMessage = "connect_failed_setup_ap";
  startApMode();
}

void setupArduinoOta() {
  const char *host = config.hostname.length() ? config.hostname.c_str() : kDefaultHostname;
  ArduinoOTA.setHostname(host);
  ArduinoOTA.onStart([]() {
    statusMessage = ArduinoOTA.getCommand() == U_SPIFFS ? "ota_spiffs" : "ota_firmware";
  });
  ArduinoOTA.onEnd([]() { statusMessage = "ota_done"; });
  ArduinoOTA.onError([](ota_error_t error) {
    statusMessage = "ota_error_" + String((int)error);
  });
  ArduinoOTA.begin();
}

bool streamSpiffsFile(const char *path, const char *contentType) {
  if (!SPIFFS.exists(path)) return false;
  File f = SPIFFS.open(path, FILE_READ);
  if (!f) return false;
  web.streamFile(f, contentType);
  f.close();
  return true;
}

void handleApiStatus() {
  JsonDocument doc;
  doc["firmware_version"] = kFirmwareVersion;
  doc["filesystem_version"] = kFilesystemVersion;
  doc["hostname"] = config.hostname.length() ? config.hostname : kDefaultHostname;
  doc["ap_mode"] = apMode;
  doc["status"] = statusMessage;
  doc["uptime_ms"] = millis();
  doc["heap_free"] = ESP.getFreeHeap();
  doc["heap_min_free"] = ESP.getMinFreeHeap();
  doc["heap_max_alloc"] = ESP.getMaxAllocHeap();
  if (WiFi.status() == WL_CONNECTED) {
    doc["network_mode"] = "WiFi STA";
    doc["ip"] = WiFi.localIP().toString();
    doc["gateway"] = WiFi.gatewayIP().toString();
    doc["subnet"] = WiFi.subnetMask().toString();
    doc["dns"] = WiFi.dnsIP().toString();
    doc["wifi_rssi"] = WiFi.RSSI();
  } else if (apMode) {
    doc["network_mode"] = "WiFi AP";
    doc["ip"] = WiFi.softAPIP().toString();
    doc["wifi_rssi"] = 0;
  } else {
    doc["network_mode"] = "offline";
    doc["ip"] = "0.0.0.0";
    doc["wifi_rssi"] = 0;
  }
  String out;
  serializeJson(doc, out);
  web.send(200, "application/json", out);
}

void handleApiConfig() {
  web.send(200, "application/json", configToJsonString());
}

void handleConfigSave() {
  config.wifi.ssid = web.arg("ssid");
  config.wifi.password = web.arg("password");
  config.wifi.dhcp = !web.hasArg("dhcp") || web.arg("dhcp") == "1" || web.arg("dhcp") == "on" || web.arg("dhcp") == "true";
  config.wifi.ip = web.arg("ip");
  config.wifi.gateway = web.arg("gateway");
  config.wifi.subnet = web.arg("subnet");
  config.wifi.dns = web.arg("dns");
  String hostname = web.arg("hostname");
  hostname.trim();
  config.hostname = hostname.length() ? hostname : kDefaultHostname;
  saveConfig();
  web.send(200, "text/html", "<!doctype html><html><body style='font-family:sans-serif;padding:24px'><h2>Saved</h2><p>Rebooting now...</p></body></html>");
  delay(500);
  ESP.restart();
}

void handleFirmwareUpload() {
  HTTPUpload &upload = web.upload();
  if (upload.status == UPLOAD_FILE_START) {
    statusMessage = "http_fw_upload";
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) Update.printError(Serial);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    Update.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!Update.end(true)) Update.printError(Serial);
  }
}

void handleFirmwareUpdate() {
  bool ok = !Update.hasError();
  web.send(200, "text/plain", ok ? "OK" : "FAIL");
  delay(300);
  if (ok) ESP.restart();
}

void handleFilesystemUpload() {
  HTTPUpload &upload = web.upload();
  if (upload.status == UPLOAD_FILE_START) {
    statusMessage = "http_fs_upload";
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS)) Update.printError(Serial);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    Update.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!Update.end(true)) Update.printError(Serial);
  }
}

void handleFilesystemUpdate() {
  bool ok = !Update.hasError();
  web.send(200, "text/plain", ok ? "OK" : "FAIL");
  delay(300);
  if (ok) ESP.restart();
}

void setupWeb() {
  web.on("/", HTTP_GET, []() {
    if (!streamSpiffsFile("/index.html", "text/html; charset=utf-8")) web.send(200, "text/plain", "Missing index.html");
  });
  web.on("/config", HTTP_GET, []() {
    if (!streamSpiffsFile("/config.html", "text/html; charset=utf-8")) web.send(200, "text/plain", "Missing config.html");
  });
  web.on("/system", HTTP_GET, []() {
    if (!streamSpiffsFile("/system.html", "text/html; charset=utf-8")) web.send(200, "text/plain", "Missing system.html");
  });
  web.on("/api/status", HTTP_GET, handleApiStatus);
  web.on("/api/config", HTTP_GET, handleApiConfig);
  web.on("/config/save", HTTP_POST, handleConfigSave);
  web.on("/firmware/update", HTTP_POST, handleFirmwareUpdate, handleFirmwareUpload);
  web.on("/spiffs/update", HTTP_POST, handleFilesystemUpdate, handleFilesystemUpload);
  web.on("/version.js", HTTP_GET, []() {
    if (!streamSpiffsFile("/version.js", "application/javascript; charset=utf-8")) web.send(404, "text/plain", "missing version.js");
  });
  web.on("/version.json", HTTP_GET, []() {
    if (!streamSpiffsFile("/version.json", "application/json; charset=utf-8")) web.send(404, "application/json", "{}");
  });
  web.onNotFound([]() { web.sendHeader("Location", "/", true); web.send(302, "text/plain", ""); });
  web.begin();
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("IR Server Ping Test boot");
  SPIFFS.begin(true);
  loadConfig();
  startWifi();
  setupArduinoOta();
  setupWeb();
}

void loop() {
  web.handleClient();
  ArduinoOTA.handle();
}
