// HvacTelnetServer.ino
// ESP32 example: Telnet JSON HVAC control + web configuration UI.

#include <Arduino.h>
#include <WiFi.h>
#include <ETH.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <Preferences.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#include <IRremoteESP8266.h>
#include <IRac.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>

// ---- User-tunable limits ----
static const uint8_t kMaxTelnetClients = 4;
static const uint8_t kMaxEmitters = 8;
static const uint8_t kMaxHvacs = 32;
static const uint8_t kMaxCustomTemps = 16;
static const uint8_t kMaxCustomCommands = 16;
static const uint16_t kDefaultTelnetPort = 4998;
static const uint16_t kMonitorLogCapacity = 200;
static const uint16_t kDinplugPort = 23;
static const unsigned long kDinplugReconnectIntervalMs = 5000;
static const unsigned long kDinplugKeepAliveIntervalMs = 10000;
static const unsigned long kDinplugRxTimeoutMs = 25000;
static const unsigned long kTelnetStateBroadcastIntervalMs = 30000;
static const unsigned long kHvacStatePersistDebounceMs = 1500;
static const uint8_t kMaxDinplugButtons = 8;
static const uint8_t kMaxDinplugKeypads = 8;
static const uint8_t kMaxTempSensors = 8;
static const uint16_t kMaxTelnetLineLength = 1024;
static const uint16_t kMaxMonitorEntryLength = 420;
static const uint16_t kIrRecvCaptureBufferSize = 1024;
static const uint8_t kIrRecvTimeoutMs = 50;
static const uint32_t kProntoDefaultFrequency = 38000;

static const char *kConfigPath = "/config.json";
static const char *kHvacStatePath = "/hvac_state.json";
static const char *kApSsid = "IR-HVAC-Setup";
static const char *kDefaultHostname = "ir-server";
static const char *kConfigPrefsNamespace = "hvaccfg";
static const char *kConfigPrefsKey = "json";
static const uint8_t kDinplugModeOverrideKeep = 0;
static const uint8_t kDinplugModeOverrideAuto = 1;
static const uint8_t kDinplugModeOverrideCool = 2;
static const uint8_t kDinplugModeOverrideHeat = 3;
static const uint8_t kDinplugModeOverrideDry = 4;
static const uint8_t kDinplugModeOverrideFan = 5;

struct CustomTempCode {
  int tempC;
  String code;
};

struct CustomCommandCode {
  String name;
  String encoding;  // pronto | gc | racepoint | rawhex
  String code;
};

struct DinplugButtonBinding {
  uint16_t keypadId = 0;
  uint16_t buttonId = 0;
  String pressAction = "none";
  float pressValue = 1.0f;
  uint8_t pressModeOverride = kDinplugModeOverrideKeep;
  String pressLightMode = "keep";  // keep | on | off | toggle
  String holdAction = "none";
  float holdValue = 1.0f;
  uint8_t holdModeOverride = kDinplugModeOverrideKeep;
  String holdLightMode = "keep";  // keep | on | off | toggle
  String togglePowerMode = "auto";  // Used when toggle_power turns HVAC on.
  uint8_t ledFollowMode = 0;  // 0=disabled, 1=LED on when HVAC on, 2=LED on when HVAC off
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
  CustomCommandCode customCommands[kMaxCustomCommands];
  uint8_t customCommandCount = 0;
  uint16_t dinKeypadIds[kMaxDinplugKeypads];
  uint8_t dinKeypadCount = 0;
  DinplugButtonBinding dinButtons[kMaxDinplugButtons];
  uint8_t dinButtonCount = 0;
  String currentTempSource = "setpoint";  // "setpoint" or "sensor"
  uint8_t tempSensorIndex = 0;            // Index on DS18B20 one-wire bus.
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

struct DinplugConfig {
  String gatewayHost;
  bool autoConnect = false;
};

struct TempSensorConfig {
  bool enabled = false;
  uint8_t gpio = 4;
  uint16_t readIntervalSec = 10;
  uint8_t precision = 2;  // 0-2 decimal places.
  String names[kMaxTempSensors];
};

struct EthernetConfig {
  bool enabled = false;
};

struct IrReceiverConfig {
  bool enabled = false;
  uint8_t gpio = 14;
  String mode = "auto";  // auto | pronto | rawhex
};

struct Config {
  WifiConfig wifi;
  WebConfig web;
  DinplugConfig dinplug;
  TempSensorConfig tempSensors;
  EthernetConfig eth;
  IrReceiverConfig irReceiver;
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
WiFiClient dinplugClient;
String dinplugBuffer;
bool dinplugConnected = false;
unsigned long dinplugLastAttemptMs = 0;
unsigned long dinplugLastKeepAliveMs = 0;
unsigned long dinplugLastRxMs = 0;
DNSServer dnsServer;
Preferences preferences;
bool telnetMonitorEnabled = true;
bool monitorLogTelnetEnabled = true;
bool monitorLogStateEnabled = true;
bool monitorLogDinplugEnabled = true;
bool monitorLogIrEnabled = true;
String serialConsoleBuffer;
String telnetMonitorLog[kMonitorLogCapacity];
uint16_t telnetMonitorLogStart = 0;
uint16_t telnetMonitorLogCount = 0;
OneWire *tempOneWire = nullptr;
DallasTemperature *tempBus = nullptr;
uint8_t tempSensorCount = 0;
DeviceAddress tempSensorAddresses[kMaxTempSensors];
float tempSensorReadings[kMaxTempSensors];
bool tempSensorValid[kMaxTempSensors];
unsigned long tempLastReadMs = 0;
IRrecv *irReceiver = nullptr;
decode_results irReceiverCapture;
bool ethernetUp = false;
bool irLearnActive = false;
String irLearnEncoding = "pronto";
String irLearnCode = "";
String irLearnError = "";
unsigned long irLearnStartMs = 0;
bool hvacStatesDirty = false;
unsigned long hvacStatesDirtySinceMs = 0;
unsigned long telnetLastStateBroadcastMs = 0;

// WT32-ETH01 defaults (LAN8720 PHY)
static const uint8_t kEthPhyAddr = 1;
static const int8_t kEthPowerPin = 16;
static const uint8_t kEthMdcPin = 23;
static const uint8_t kEthMdioPin = 18;

void initHvacRuntimeStates();
bool processCommand(JsonDocument &doc, JsonDocument &resp, int8_t sourceTelnetSlot = -1);
bool sendCustomCode(const HvacConfig &hvac, EmitterRuntime *em, const String &code, const String &encoding);
String findCustomTempCode(const HvacConfig &hvac, int tempC);
void handleSerialConsole();
void addMonitorLogEntry(const String &line);
void clearMonitorLog();
bool sendTelnetJson(WiFiClient &client, JsonDocument &doc);
bool monitorCategoryEnabled(const String &category);
bool saveConfigJson(const String &json);
bool loadConfigFromJson(const String &json, bool printErrors = true);
bool readStoredConfigJson(String &json);
bool importLegacyConfigFromSpiffs();
void clearPersistedData();
void handleDinplug();
void ensureDinplugConnected(bool forceNow = false);
void processDinplugLine(const String &line);
void handleDinplugButtonEvent(uint16_t keypadId, uint16_t buttonId, const String &action);
bool applyDinplugAction(uint8_t hvacIndex, const DinplugButtonBinding &binding, bool isHold);
String dinplugConnectionStatus();
void writeStateJson(JsonObject state, const String &id, const HvacRuntimeState &hvacState);
void handleDinplugPage();
void handleDinplugSave();
void handleDinplugTest();
void handleApiStatus();
void handleApiMeta();
void handleApiConfigSave();
void handleFilesystemUpdate();
void handleFilesystemUpload();
void handleFactoryReset();
bool dinActionUsesValue(const String &action);
uint8_t normalizeDinplugModeOverride(const String &modeIn);
const char *dinplugModeOverrideToString(uint8_t mode);
String normalizeDinplugLightMode(const String &modeIn);
String dinToggleModeOptionsHtml(const String &selected);
String dinKeypadsCsv(const HvacConfig &h);
void parseDinKeypadsCsv(const String &csv, HvacConfig &h);
bool hvacHasDinKeypad(const HvacConfig &h, uint16_t keypadId);
void logHvacStateChange(const String &id, const HvacRuntimeState &after, int8_t sourceTelnetSlot);
bool setDinplugLed(uint16_t keypadId, uint16_t ledId, uint8_t state);
void syncDinplugLedsForHvac(uint8_t hvacIndex);
void setupTemperatureSensors();
void readTemperatureSensors();
void handleTemperatureSensors();
bool hvacUsesSensorTemp(const HvacConfig &h);
String sensorNameForIndex(uint8_t idx);
String sensorAddressToString(const DeviceAddress addr);
uint8_t tempSensorPrecision();
float applyTempSensorPrecision(float value);
void setupIrReceiver();
void handleIrReceiver();
String normalizeIrReceiverMode(const String &modeIn);
String buildProntoFromCapture(const decode_results &capture, uint32_t frequency);
String buildRawHexFromCapture(const decode_results &capture);
String truncateForLog(const String &line, size_t maxLen = 900);
bool startEthernet();
bool networkReady();
String networkModeString();
IPAddress networkLocalIp();
String normalizeCustomEncoding(const String &encodingIn);
String customCommandsJson(const HvacConfig &h);
const CustomCommandCode *findCustomCommandByName(const HvacConfig &h, const String &name);
void loadCustomCommandsFromRequest(HvacConfig &h);
String buildGcFromCapture(const decode_results &capture, uint32_t frequency);
String buildRacepointFromCapture(const decode_results &capture, uint32_t frequency);
String buildCodeFromCaptureEncoding(const decode_results &capture, const String &encodingIn);
String customCommandNamesSummary(const HvacConfig &h);
uint8_t activeTelnetClientCount();
void markHvacStatesDirty();
void savePersistedHvacStates();
void loadPersistedHvacStates();
void handleHvacStatePersistence();
void handleTelnetPeriodicStateBroadcast();

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

uint8_t activeTelnetClientCount() {
  uint8_t count = 0;
  for (uint8_t i = 0; i < kMaxTelnetClients; i++) {
    if (telnetClients[i] && telnetClients[i].connected()) count++;
  }
  return count;
}

String sensorNameForIndex(uint8_t idx) {
  if (idx >= kMaxTempSensors) return "";
  String name = config.tempSensors.names[idx];
  name.trim();
  if (name.length()) return name;
  return "Sensor " + String(idx);
}

String sensorAddressToString(const DeviceAddress addr) {
  String out;
  for (uint8_t i = 0; i < 8; i++) {
    if (addr[i] < 0x10) out += "0";
    out += String(addr[i], HEX);
  }
  out.toUpperCase();
  return out;
}

uint8_t tempSensorPrecision() {
  return (config.tempSensors.precision <= 2) ? config.tempSensors.precision : 2;
}

float applyTempSensorPrecision(float value) {
  uint8_t p = tempSensorPrecision();
  float scale = 1.0f;
  if (p == 1) scale = 10.0f;
  else if (p == 2) scale = 100.0f;
  return roundf(value * scale) / scale;
}

void printMonitorStatus() {
  Serial.print("monitor: telnet ");
  Serial.println(telnetMonitorEnabled ? "on" : "off");
}

bool monitorCategoryEnabled(const String &categoryIn) {
  String category = categoryIn;
  category.toLowerCase();
  if (category == "telnet") return monitorLogTelnetEnabled;
  if (category == "state") return monitorLogStateEnabled;
  if (category == "dinplug") return monitorLogDinplugEnabled;
  if (category == "ir") return monitorLogIrEnabled;
  return true;
}

String dinplugConnectionStatus() {
  String gatewayHost = config.dinplug.gatewayHost;
  gatewayHost.trim();
  if (gatewayHost.length() == 0) return "not configured";
  if (dinplugClient.connected()) {
    return "connected to " + gatewayHost;
  }
  if (config.dinplug.autoConnect) return "auto-connect enabled, disconnected";
  return "configured, disconnected";
}

void addMonitorLogEntry(const String &line) {
  String entry = "[" + String(millis()) + " ms] " + truncateForLog(line, kMaxMonitorEntryLength);
  if (telnetMonitorLogCount < kMonitorLogCapacity) {
    uint16_t idx = (telnetMonitorLogStart + telnetMonitorLogCount) % kMonitorLogCapacity;
    telnetMonitorLog[idx] = entry;
    telnetMonitorLogCount++;
    return;
  }
  telnetMonitorLog[telnetMonitorLogStart] = entry;
  telnetMonitorLogStart = (telnetMonitorLogStart + 1) % kMonitorLogCapacity;
}

void clearMonitorLog() {
  telnetMonitorLogStart = 0;
  telnetMonitorLogCount = 0;
}

void handleConsoleCommand(const String &line) {
  String cmd = line;
  cmd.trim();
  cmd.toLowerCase();
  if (!cmd.length()) return;

  if (cmd == "monitor on" || cmd == "telnet monitor on") {
    telnetMonitorEnabled = true;
    printMonitorStatus();
    return;
  }
  if (cmd == "monitor off" || cmd == "telnet monitor off") {
    telnetMonitorEnabled = false;
    printMonitorStatus();
    return;
  }
  if (cmd == "monitor status" || cmd == "telnet monitor status") {
    printMonitorStatus();
    return;
  }
  if (cmd == "monitor help" || cmd == "help") {
    Serial.println("monitor commands:");
    Serial.println("  monitor on");
    Serial.println("  monitor off");
    Serial.println("  monitor status");
    return;
  }
  Serial.print("monitor: unknown command: ");
  Serial.println(line);
}

void handleSerialConsole() {
  while (Serial.available()) {
    char ch = static_cast<char>(Serial.read());
    if (ch == '\r') continue;
    if (ch == '\n') {
      if (serialConsoleBuffer.length()) {
        handleConsoleCommand(serialConsoleBuffer);
        serialConsoleBuffer = "";
      }
      continue;
    }
    serialConsoleBuffer += ch;
    if (serialConsoleBuffer.length() > 200) {
      serialConsoleBuffer = "";
      Serial.println("monitor: command too long, cleared");
    }
  }
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
  // Allow full GlobalCache strings like "sendir,1:1,1,...."
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

  // Allow either comma- or space-separated Pronto codes.
  uint16_t count = countTokensFlexible(tmp_str);
  size_t pos = 0;
  String token;

  // Optional repeat prefix e.g. "R3 ..."
  if (nextTokenFlexible(tmp_str, pos, token)) {
    if (token.length() > 1 && (token[0] == 'R' || token[0] == 'r')) {
      repeats = token.substring(1).toInt();
      count--;  // Skip repeat token from payload count.
    } else {
      // Rewind to start of first code token.
      pos = 0;
    }
  }

  if (count < kProntoMinLength) return false;
  uint16_t *code_array = newCodeArray(count);
  if (!code_array) return false;
  uint16_t filled = 0;
  // If we consumed a repeat token above, pos already points to next token.
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

bool parseStringAndSendRawHex(IRsend *irsend, const String str) {
#if SEND_RAW
  if (!irsend) return false;
  String tmp = str;
  tmp.trim();
  uint16_t count = countTokensFlexible(tmp);
  if (count == 0) return false;
  uint16_t *durations = newCodeArray(count);
  if (!durations) return false;
  uint16_t filled = 0;
  size_t pos = 0;
  String token;
  while (nextTokenFlexible(tmp, pos, token) && filled < count) {
    durations[filled++] = static_cast<uint16_t>(strtoul(token.c_str(), NULL, 16));
  }
  if (filled == 0) {
    free(durations);
    return false;
  }
  irsend->sendRaw(durations, filled, kProntoDefaultFrequency);
  free(durations);
  return true;
#else
  (void)irsend;
  (void)str;
  return false;
#endif
}

String normalizeIrReceiverMode(const String &modeIn) {
  String mode = modeIn;
  mode.toLowerCase();
  mode.trim();
  if (mode == "pronto") return "pronto";
  if (mode == "rawhex" || mode == "raw_hex" || mode == "raw") return "rawhex";
  return "auto";
}

String normalizeCustomEncoding(const String &encodingIn) {
  String encoding = encodingIn;
  encoding.toLowerCase();
  encoding.trim();
  if (encoding == "pronto") return "pronto";
  if (encoding == "gc") return "gc";
  if (encoding == "racepoint") return "racepoint";
  if (encoding == "raw" || encoding == "rawhex" || encoding == "raw_hex") return "rawhex";
  return "pronto";
}

String truncateForLog(const String &line, size_t maxLen) {
  if (line.length() <= maxLen) return line;
  return line.substring(0, maxLen) + "...(truncated)";
}

String buildRawHexFromCapture(const decode_results &capture) {
  uint16_t pulseCount = getCorrectedRawLength(&capture);
  uint16_t *raw = resultToRawArray(&capture);
  if (!raw || pulseCount == 0) {
    if (raw) delete[] raw;
    return "";
  }
  String out;
  out.reserve(static_cast<size_t>(pulseCount) * 5 + 16);
  char hex[6] = {0};
  for (uint16_t i = 0; i < pulseCount; i++) {
    snprintf(hex, sizeof(hex), "%04X", raw[i] & 0xFFFF);
    out += hex;
    if (i + 1 < pulseCount) out += ' ';
  }
  delete[] raw;
  return out;
}

String buildGcFromCapture(const decode_results &capture, uint32_t frequency) {
  if (frequency == 0) frequency = kProntoDefaultFrequency;
  uint16_t pulseCount = getCorrectedRawLength(&capture);
  uint16_t *raw = resultToRawArray(&capture);
  if (!raw || pulseCount == 0) {
    if (raw) delete[] raw;
    return "";
  }
  String out = String(frequency) + ",1,1";
  for (uint16_t i = 0; i < pulseCount; i++) {
    out += ",";
    out += String(raw[i]);
  }
  delete[] raw;
  return out;
}

String buildRacepointFromCapture(const decode_results &capture, uint32_t frequency) {
  if (frequency == 0) frequency = kProntoDefaultFrequency;
  uint16_t pulseCount = getCorrectedRawLength(&capture);
  uint16_t *raw = resultToRawArray(&capture);
  if (!raw || pulseCount == 0) {
    if (raw) delete[] raw;
    return "";
  }
  String out;
  out.reserve(static_cast<size_t>(pulseCount + 1) * 5);
  char hex[6] = {0};
  auto appendWord = [&out, &hex](uint16_t word) {
    snprintf(hex, sizeof(hex), "%04X", word & 0xFFFF);
    out += hex;
  };
  appendWord(static_cast<uint16_t>(frequency));
  for (uint16_t i = 0; i < pulseCount; i++) {
    uint32_t cycles = static_cast<uint32_t>((static_cast<double>(raw[i]) * frequency / 1000000.0) + 0.5);
    if (cycles == 0) cycles = 1;
    if (cycles > 0xFFFF) cycles = 0xFFFF;
    appendWord(static_cast<uint16_t>(cycles));
  }
  delete[] raw;
  return out;
}

String buildProntoFromCapture(const decode_results &capture, uint32_t frequency) {
  if (frequency == 0) frequency = kProntoDefaultFrequency;
  uint16_t pulseCount = getCorrectedRawLength(&capture);
  uint16_t *raw = resultToRawArray(&capture);
  if (!raw || pulseCount < 2) {
    if (raw) delete[] raw;
    return "";
  }

  if (pulseCount & 1) pulseCount--;  // Pronto sequence must have mark/space pairs.
  if (pulseCount < 2) {
    delete[] raw;
    return "";
  }

  uint16_t freqWord = static_cast<uint16_t>((1000000.0 / (frequency * 0.241246)) + 0.5);
  if (freqWord == 0) freqWord = 1;
  uint16_t burstPairs = pulseCount / 2;

  String out;
  out.reserve(static_cast<size_t>(pulseCount + 4) * 5);
  char hex[6] = {0};
  auto appendWord = [&out, &hex](uint16_t word) {
    snprintf(hex, sizeof(hex), "%04X", word & 0xFFFF);
    if (out.length()) out += ' ';
    out += hex;
  };

  appendWord(0x0000);
  appendWord(freqWord);
  appendWord(burstPairs);
  appendWord(0x0000);
  for (uint16_t i = 0; i < pulseCount; i++) {
    uint32_t cycles = static_cast<uint32_t>((static_cast<double>(raw[i]) * frequency / 1000000.0) + 0.5);
    if (cycles == 0) cycles = 1;
    if (cycles > 0xFFFF) cycles = 0xFFFF;
    appendWord(static_cast<uint16_t>(cycles));
  }

  delete[] raw;
  return out;
}

String buildCodeFromCaptureEncoding(const decode_results &capture, const String &encodingIn) {
  String encoding = normalizeCustomEncoding(encodingIn);
  if (encoding == "pronto") return buildProntoFromCapture(capture, kProntoDefaultFrequency);
  if (encoding == "gc") return buildGcFromCapture(capture, kProntoDefaultFrequency);
  if (encoding == "racepoint") return buildRacepointFromCapture(capture, kProntoDefaultFrequency);
  if (encoding == "rawhex") return buildRawHexFromCapture(capture);
  return "";
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

  JsonObject din = doc["dinplug"].to<JsonObject>();
  String gatewayHost = config.dinplug.gatewayHost;
  gatewayHost.trim();
  din["gateway_host"] = gatewayHost;
  IPAddress gatewayIp;
  din["gateway_ip"] = gatewayIp.fromString(gatewayHost) ? gatewayIp.toString() : "";
  din["auto_connect"] = config.dinplug.autoConnect;

  JsonObject ts = doc["temp_sensors"].to<JsonObject>();
  ts["enabled"] = config.tempSensors.enabled;
  ts["gpio"] = config.tempSensors.gpio;
  ts["read_interval_sec"] = config.tempSensors.readIntervalSec;
  ts["precision"] = tempSensorPrecision();
  JsonArray tsNames = ts["names"].to<JsonArray>();
  for (uint8_t i = 0; i < kMaxTempSensors; i++) tsNames.add(config.tempSensors.names[i]);

  JsonObject eth = doc["ethernet"].to<JsonObject>();
  eth["enabled"] = config.eth.enabled;

  JsonObject irrx = doc["ir_receiver"].to<JsonObject>();
  irrx["enabled"] = config.irReceiver.enabled;
  irrx["gpio"] = config.irReceiver.gpio;
  irrx["mode"] = normalizeIrReceiverMode(config.irReceiver.mode);

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
    o["current_temp_source"] = h.currentTempSource;
    o["temp_sensor_index"] = h.tempSensorIndex;
    if (h.isCustom) {
      JsonObject c = o["custom"].to<JsonObject>();
      c["encoding"] = h.customEncoding;
      c["off"] = h.customOff;
      JsonObject temps = c["temps"].to<JsonObject>();
      for (uint8_t t = 0; t < h.customTempCount; t++) {
        temps[String(h.customTemps[t].tempC)] = h.customTemps[t].code;
      }
      JsonArray cmds = c["commands"].to<JsonArray>();
      for (uint8_t cc = 0; cc < h.customCommandCount; cc++) {
        JsonObject co = cmds.add<JsonObject>();
        co["name"] = h.customCommands[cc].name;
        co["encoding"] = normalizeCustomEncoding(h.customCommands[cc].encoding);
        co["code"] = h.customCommands[cc].code;
      }
    }
    JsonObject dinplug = o["dinplug"].to<JsonObject>();
    dinplug["keypad_ids"] = dinKeypadsCsv(h);
    dinplug["keypad_id"] = (h.dinKeypadCount > 0) ? h.dinKeypadIds[0] : 0;
    JsonArray keypads = dinplug["keypads"].to<JsonArray>();
    for (uint8_t k = 0; k < h.dinKeypadCount; k++) keypads.add(h.dinKeypadIds[k]);
    JsonArray btns = dinplug["buttons"].to<JsonArray>();
    for (uint8_t b = 0; b < h.dinButtonCount; b++) {
      JsonObject bo = btns.add<JsonObject>();
      bo["keypad_id"] = h.dinButtons[b].keypadId;
      bo["id"] = h.dinButtons[b].buttonId;
      bo["press_action"] = h.dinButtons[b].pressAction;
      bo["press_value"] = h.dinButtons[b].pressValue;
      bo["press_mode_override"] = dinplugModeOverrideToString(h.dinButtons[b].pressModeOverride);
      bo["press_light_mode"] = normalizeDinplugLightMode(h.dinButtons[b].pressLightMode);
      bo["hold_action"] = h.dinButtons[b].holdAction;
      bo["hold_value"] = h.dinButtons[b].holdValue;
      bo["hold_mode_override"] = dinplugModeOverrideToString(h.dinButtons[b].holdModeOverride);
      bo["hold_light_mode"] = normalizeDinplugLightMode(h.dinButtons[b].holdLightMode);
      bo["toggle_power_mode"] = h.dinButtons[b].togglePowerMode;
      bo["led_follow_mode"] = h.dinButtons[b].ledFollowMode;
      bo["led_follow_power"] = (h.dinButtons[b].ledFollowMode != 0);
    }
  }

  String out;
  serializeJson(doc, out);
  return out;
}

bool saveConfigJson(const String &json) {
  preferences.begin(kConfigPrefsNamespace, false);
  size_t written = preferences.putBytes(kConfigPrefsKey, json.c_str(), json.length());
  preferences.end();
  if (written != json.length()) {
    Serial.println("config: failed to persist");
    return false;
  }
  Serial.println("config: saved");
  return true;
}

void saveConfig() {
  saveConfigJson(configToJsonString());
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
  config.dinplug.gatewayHost = "";
  config.dinplug.autoConnect = false;
  config.tempSensors.enabled = false;
  config.tempSensors.gpio = 4;
  config.tempSensors.readIntervalSec = 10;
  config.tempSensors.precision = 2;
  for (uint8_t i = 0; i < kMaxTempSensors; i++) config.tempSensors.names[i] = "";
  config.eth.enabled = false;
  config.irReceiver.enabled = false;
  config.irReceiver.gpio = 14;
  config.irReceiver.mode = "auto";
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
    for (uint8_t cc = 0; cc < kMaxCustomCommands; cc++) {
      h.customCommands[cc].name = "";
      h.customCommands[cc].encoding = "pronto";
      h.customCommands[cc].code = "";
    }
    h.customCommandCount = 0;
    h.dinKeypadCount = 0;
    for (uint8_t k = 0; k < kMaxDinplugKeypads; k++) h.dinKeypadIds[k] = 0;
    h.dinButtonCount = 0;
    h.currentTempSource = "setpoint";
    h.tempSensorIndex = 0;
  }
  config.hvacCount = 0;
  initHvacRuntimeStates();
}

bool loadConfigFromJson(const String &json, bool printErrors) {
  clearConfig();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    if (printErrors) {
      Serial.print("config: parse error ");
      Serial.println(err.c_str());
    }
    return false;
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

  JsonObject din = doc["dinplug"];
  if (!din.isNull()) {
    String gatewayHost = din["gateway_host"] | "";
    if (gatewayHost.length() == 0) gatewayHost = din["gateway_ip"] | "";
    gatewayHost.trim();
    config.dinplug.gatewayHost = gatewayHost;
    config.dinplug.autoConnect = din["auto_connect"] | false;
  }
  JsonObject ts = doc["temp_sensors"];
  if (!ts.isNull()) {
    config.tempSensors.enabled = ts["enabled"] | false;
    config.tempSensors.gpio = ts["gpio"] | 4;
    config.tempSensors.readIntervalSec = ts["read_interval_sec"] | 10;
    if (config.tempSensors.readIntervalSec == 0) config.tempSensors.readIntervalSec = 10;
    uint8_t precision = ts["precision"] | 2;
    if (precision > 2) precision = 2;
    config.tempSensors.precision = precision;
    JsonArray names = ts["names"];
    if (!names.isNull()) {
      uint8_t idx = 0;
      for (JsonVariant v : names) {
        if (idx >= kMaxTempSensors) break;
        String n = v.as<String>();
        n.trim();
        config.tempSensors.names[idx++] = n;
      }
    }
  }
  JsonObject eth = doc["ethernet"];
  if (!eth.isNull()) {
    config.eth.enabled = eth["enabled"] | false;
  }
  JsonObject irrx = doc["ir_receiver"];
  if (!irrx.isNull()) {
    config.irReceiver.enabled = irrx["enabled"] | false;
    config.irReceiver.gpio = irrx["gpio"] | 14;
    config.irReceiver.mode = normalizeIrReceiverMode(irrx["mode"] | "auto");
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
      h.currentTempSource = o["current_temp_source"] | "setpoint";
      h.currentTempSource.toLowerCase();
      if (h.currentTempSource != "sensor") h.currentTempSource = "setpoint";
      h.tempSensorIndex = o["temp_sensor_index"] | 0;
      if (h.tempSensorIndex >= kMaxTempSensors) h.tempSensorIndex = 0;
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
        JsonArray cmds = c["commands"];
        if (!cmds.isNull()) {
          for (JsonObject co : cmds) {
            if (h.customCommandCount >= kMaxCustomCommands) break;
            String name = co["name"] | "";
            String encoding = normalizeCustomEncoding(co["encoding"] | "pronto");
            String code = co["code"] | "";
            name.trim();
            if (!name.length() || !code.length()) continue;
            bool duplicate = false;
            for (uint8_t i = 0; i < h.customCommandCount; i++) {
              if (h.customCommands[i].name == name) { duplicate = true; break; }
            }
            if (duplicate) continue;
            CustomCommandCode &cmd = h.customCommands[h.customCommandCount++];
            cmd.name = name;
            cmd.encoding = encoding;
            cmd.code = code;
          }
        }
      }
      JsonObject dinplug = o["dinplug"];
      if (!dinplug.isNull()) {
        JsonArray keypads = dinplug["keypads"];
        if (!keypads.isNull()) {
          for (JsonVariant kv : keypads) {
            if (h.dinKeypadCount >= kMaxDinplugKeypads) break;
            uint16_t keypadId = kv | 0;
            if (keypadId == 0) continue;
            h.dinKeypadIds[h.dinKeypadCount++] = keypadId;
          }
        }
        if (h.dinKeypadCount == 0) {
          String keypadCsv = dinplug["keypad_ids"] | "";
          if (keypadCsv.length() > 0) {
            parseDinKeypadsCsv(keypadCsv, h);
          } else {
            uint16_t legacyKeypad = dinplug["keypad_id"] | 0;
            if (legacyKeypad > 0) {
              h.dinKeypadIds[0] = legacyKeypad;
              h.dinKeypadCount = 1;
            }
          }
        }
        JsonArray btns = dinplug["buttons"];
        if (!btns.isNull()) {
          for (JsonObject bo : btns) {
            if (h.dinButtonCount >= kMaxDinplugButtons) break;
            DinplugButtonBinding &b = h.dinButtons[h.dinButtonCount++];
            b.keypadId = bo["keypad_id"] | 0;
            b.buttonId = bo["id"] | 0;
            b.pressAction = bo["press_action"] | "none";
            b.pressValue = bo["press_value"] | 1.0f;
            b.pressModeOverride = normalizeDinplugModeOverride(bo["press_mode_override"] | "keep");
            b.pressLightMode = normalizeDinplugLightMode(bo["press_light_mode"] | "keep");
            b.holdAction = bo["hold_action"] | "none";
            b.holdValue = bo["hold_value"] | 1.0f;
            b.holdModeOverride = normalizeDinplugModeOverride(bo["hold_mode_override"] | "keep");
            b.holdLightMode = normalizeDinplugLightMode(bo["hold_light_mode"] | "keep");
            b.togglePowerMode = bo["toggle_power_mode"] | "auto";
            b.ledFollowMode = bo["led_follow_mode"] | 0;
            if (b.ledFollowMode > 2) b.ledFollowMode = 0;
            if (b.ledFollowMode == 0 && (bo["led_follow_power"] | false)) b.ledFollowMode = 2;
          }
        }
      }
    }
  }

  return true;
}

bool readStoredConfigJson(String &json) {
  json = "";
  preferences.begin(kConfigPrefsNamespace, true);
  size_t size = preferences.getBytesLength(kConfigPrefsKey);
  if (size == 0) {
    preferences.end();
    return false;
  }
  char *buffer = new char[size + 1];
  if (!buffer) {
    preferences.end();
    Serial.println("config: alloc failed");
    return false;
  }
  size_t read = preferences.getBytes(kConfigPrefsKey, buffer, size);
  preferences.end();
  buffer[read] = '\0';
  json = String(buffer);
  delete[] buffer;
  return read > 0;
}

bool importLegacyConfigFromSpiffs() {
  if (!SPIFFS.exists(kConfigPath)) return false;
  File f = SPIFFS.open(kConfigPath, FILE_READ);
  if (!f) {
    Serial.println("config: legacy open failed");
    return false;
  }
  String json = f.readString();
  f.close();
  if (!json.length()) {
    Serial.println("config: legacy file empty");
    return false;
  }
  if (!loadConfigFromJson(json)) return false;
  saveConfig();
  Serial.println("config: migrated from SPIFFS");
  return true;
}

void clearPersistedData() {
  preferences.begin(kConfigPrefsNamespace, false);
  preferences.clear();
  preferences.end();

  if (SPIFFS.exists(kConfigPath)) SPIFFS.remove(kConfigPath);
  if (SPIFFS.exists(kHvacStatePath)) SPIFFS.remove(kHvacStatePath);

  clearConfig();
  initHvacRuntimeStates();
  hvacStatesDirty = false;
  hvacStatesDirtySinceMs = 0;
  clearMonitorLog();
}

void loadConfig() {
  String json;
  if (readStoredConfigJson(json)) {
    if (loadConfigFromJson(json)) return;
    Serial.println("config: stored config invalid, trying legacy SPIFFS backup");
  }
  if (importLegacyConfigFromSpiffs()) return;
  clearConfig();
  Serial.println("config: not found");
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
  out.replace("\r", "");
  out.replace("\n", "");
  out.trim();
  out.toLowerCase();
  if (out == "cool" || out == "heat" || out == "dry" || out == "fan" ||
      out == "off") return out;
  return "auto";
}

String normalizeFan(const String &in) {
  String out = in;
  out.replace("\r", "");
  out.replace("\n", "");
  out.trim();
  out.toLowerCase();
  if (out == "auto") return out;
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

bool hvacUsesSensorTemp(const HvacConfig &h) {
  String source = h.currentTempSource;
  source.toLowerCase();
  return source == "sensor";
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

void logHvacStateChange(const String &id, const HvacRuntimeState &after, int8_t sourceTelnetSlot) {
  if (!telnetMonitorEnabled) return;
  if (!monitorLogStateEnabled) return;
  if (sourceTelnetSlot >= 0) return;
  JsonDocument msg;
  writeStateJson(msg.to<JsonObject>(), id, after);
  String payload;
  serializeJson(msg, payload);
  addMonitorLogEntry("TX state " + truncateForLog(payload, 300));
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
  HvacConfig *hvac = nullptr;
  if (findHvacById(id, hvac) && hvac) {
    const bool isCustom = hvac->isCustom || hvac->protocol == "CUSTOM";
    if (isCustom) {
      state["protocol"] = "CUSTOM";
      state["custom"] = true;
      JsonArray commands = state["custom_commands"].to<JsonArray>();
      for (uint8_t i = 0; i < hvac->customCommandCount; i++) {
        JsonObject c = commands.add<JsonObject>();
        c["name"] = hvac->customCommands[i].name;
        c["encoding"] = normalizeCustomEncoding(hvac->customCommands[i].encoding);
      }
    }
  }
  state["power"] = hvacState.power ? "on" : "off";
  state["mode"] = hvacState.mode;
  state["setpoint"] = hvacState.setpoint;
  state["current_temp"] = hvacState.currentTemp;
  state["fan"] = hvacState.fan;
  state["light"] = hvacState.light ? "on" : "off";
}

bool applyDinplugAction(uint8_t hvacIndex, const DinplugButtonBinding &binding, bool isHold) {
  if (hvacIndex >= config.hvacCount) return false;
  ensureHvacStateInitialized(hvacIndex);
  HvacRuntimeState &state = hvacStates[hvacIndex];
  HvacConfig &hvac = config.hvacs[hvacIndex];
  String action = isHold ? binding.holdAction : binding.pressAction;
  float value = isHold ? binding.holdValue : binding.pressValue;
  uint8_t modeOverride = isHold ? binding.holdModeOverride : binding.pressModeOverride;
  String lightMode = normalizeDinplugLightMode(isHold ? binding.holdLightMode : binding.pressLightMode);
  String toggleMode = binding.togglePowerMode;
  toggleMode.toLowerCase();
  if (toggleMode != "cool" && toggleMode != "heat" && toggleMode != "dry" &&
      toggleMode != "fan" && toggleMode != "auto") {
    toggleMode = "auto";
  }
  action.toLowerCase();
  if (action == "none" || action.length() == 0) return false;

  if (action.startsWith("custom:")) {
    String cmdName = action.substring(7);
    const CustomCommandCode *customCmd = findCustomCommandByName(hvac, cmdName);
    if (!customCmd) return false;
    JsonDocument cmd;
    cmd["cmd"] = "send";
    cmd["id"] = hvac.id;
    cmd["command_name"] = customCmd->name;
    cmd["encoding"] = normalizeCustomEncoding(customCmd->encoding);
    cmd["code"] = customCmd->code;
    JsonDocument resp;
    return processCommand(cmd, resp, -1);
  }

  JsonDocument cmd;
  cmd["cmd"] = "send";
  cmd["id"] = hvac.id;
  String baseMode = state.mode;
  if (baseMode.length() == 0 || baseMode == "off") baseMode = "auto";
  cmd["mode"] = baseMode;
  cmd["fan"] = state.fan;
  cmd["power"] = state.power ? "on" : "off";
  cmd["temp"] = state.setpoint;

  auto clampTemp = [](float t) {
    if (t < 16) return 16.0f;
    if (t > 32) return 32.0f;
    return t;
  };

  if (action == "temp_up") {
    cmd["temp"] = clampTemp(state.setpoint + value);
    cmd["power"] = "on";
  } else if (action == "temp_down") {
    cmd["temp"] = clampTemp(state.setpoint - value);
    cmd["power"] = "on";
  } else if (action == "set_temp") {
    cmd["temp"] = clampTemp(value);
    cmd["power"] = "on";
  } else if (action == "power_on") {
    cmd["power"] = "on";
  } else if (action == "power_off") {
    cmd["power"] = "off";
  } else if (action == "toggle_power") {
    bool turnOn = !state.power;
    cmd["power"] = turnOn ? "on" : "off";
    if (turnOn) cmd["mode"] = toggleMode;
  } else if (action == "mode_heat") {
    cmd["mode"] = "heat";
    cmd["power"] = "on";
  } else if (action == "mode_cool") {
    cmd["mode"] = "cool";
    cmd["power"] = "on";
  } else if (action == "mode_fan") {
    cmd["mode"] = "fan";
    cmd["power"] = "on";
  } else if (action == "mode_auto") {
    cmd["mode"] = "auto";
    cmd["power"] = "on";
  } else if (action == "mode_off") {
    cmd["power"] = "off";
  } else if (action == "light_on") {
    cmd["light"] = "true";
  } else if (action == "light_off") {
    cmd["light"] = "false";
  } else if (action == "toggle_light") {
    cmd["light"] = state.light ? "false" : "true";
  } else {
    return false;
  }

  if (lightMode == "on") {
    cmd["light"] = "true";
  } else if (lightMode == "off") {
    cmd["light"] = "false";
  } else if (lightMode == "toggle") {
    cmd["light"] = state.light ? "false" : "true";
  }
  if (modeOverride != kDinplugModeOverrideKeep) {
    cmd["mode"] = dinplugModeOverrideToString(modeOverride);
    if (String(cmd["power"] | "off") == "off") cmd["power"] = "on";
  }

  JsonDocument resp;
  bool ok = processCommand(cmd, resp, -1);
  return ok;
}

bool sendTelnetJson(WiFiClient &client, JsonDocument &doc) {
  if (!client || !client.connected()) return false;
  String payload;
  payload.reserve(512);
  serializeJson(doc, payload);
  payload += "\r\n";
  size_t written = client.write(reinterpret_cast<const uint8_t *>(payload.c_str()), payload.length());
  if (written != payload.length()) {
    client.stop();
    return false;
  }
  return true;
}

void broadcastStateToTelnetClients(const String &id, const HvacRuntimeState &state, int8_t excludeSlot) {
  JsonDocument msg;
  writeStateJson(msg.to<JsonObject>(), id, state);
  for (uint8_t i = 0; i < kMaxTelnetClients; i++) {
    if (static_cast<int8_t>(i) == excludeSlot) continue;
    WiFiClient &c = telnetClients[i];
    if (!c || !c.connected()) continue;
    if (!sendTelnetJson(c, msg)) {
      Serial.print("telnet: dropped stalled client slot ");
      Serial.println(i);
    }
  }
}

void sendAllStatesToTelnetClient(WiFiClient &client) {
  for (uint8_t i = 0; i < config.hvacCount; i++) {
    ensureHvacStateInitialized(i);
    JsonDocument msg;
    writeStateJson(msg.to<JsonObject>(), config.hvacs[i].id, hvacStates[i]);
    if (!sendTelnetJson(client, msg)) return;
  }
}

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

String dinToggleModeOptionsHtml(const String &selectedIn) {
  String selected = selectedIn;
  selected.toLowerCase();
  if (selected.length() == 0) selected = "auto";
  const char *modes[] = {"auto", "cool", "heat", "dry", "fan"};
  const size_t count = sizeof(modes) / sizeof(modes[0]);
  String out;
  for (size_t i = 0; i < count; i++) {
    String mode = modes[i];
    out += "<option value='" + mode + "'";
    if (selected == mode) out += " selected";
    out += ">" + mode + "</option>";
  }
  return out;
}

bool dinActionUsesValue(const String &actionIn) {
  String action = actionIn;
  action.toLowerCase();
  return action == "temp_up" || action == "temp_down" || action == "set_temp";
}

uint8_t normalizeDinplugModeOverride(const String &modeIn) {
  String mode = modeIn;
  mode.trim();
  mode.toLowerCase();
  if (mode == "auto") return kDinplugModeOverrideAuto;
  if (mode == "cool") return kDinplugModeOverrideCool;
  if (mode == "heat") return kDinplugModeOverrideHeat;
  if (mode == "dry") return kDinplugModeOverrideDry;
  if (mode == "fan") return kDinplugModeOverrideFan;
  return kDinplugModeOverrideKeep;
}

const char *dinplugModeOverrideToString(uint8_t mode) {
  switch (mode) {
    case kDinplugModeOverrideAuto: return "auto";
    case kDinplugModeOverrideCool: return "cool";
    case kDinplugModeOverrideHeat: return "heat";
    case kDinplugModeOverrideDry: return "dry";
    case kDinplugModeOverrideFan: return "fan";
    case kDinplugModeOverrideKeep:
    default:
      return "keep";
  }
}

String normalizeDinplugLightMode(const String &modeIn) {
  String mode = modeIn;
  mode.toLowerCase();
  mode.trim();
  if (mode == "on") return "on";
  if (mode == "off") return "off";
  if (mode == "toggle") return "toggle";
  return "keep";
}

String dinKeypadsCsv(const HvacConfig &h) {
  String out;
  for (uint8_t i = 0; i < h.dinKeypadCount; i++) {
    if (h.dinKeypadIds[i] == 0) continue;
    if (out.length()) out += ",";
    out += String(h.dinKeypadIds[i]);
  }
  return out;
}

void parseDinKeypadsCsv(const String &csvIn, HvacConfig &h) {
  h.dinKeypadCount = 0;
  for (uint8_t i = 0; i < kMaxDinplugKeypads; i++) h.dinKeypadIds[i] = 0;
  String csv = csvIn;
  csv.trim();
  if (csv.length() == 0) return;
  int start = 0;
  while (start <= csv.length()) {
    int comma = csv.indexOf(',', start);
    if (comma < 0) comma = csv.length();
    String token = csv.substring(start, comma);
    token.trim();
    if (token.length() > 0) {
      int parsed = token.toInt();
      if (parsed > 0 && parsed <= 65535) {
        uint16_t keypadId = static_cast<uint16_t>(parsed);
        bool exists = false;
        for (uint8_t i = 0; i < h.dinKeypadCount; i++) {
          if (h.dinKeypadIds[i] == keypadId) { exists = true; break; }
        }
        if (!exists && h.dinKeypadCount < kMaxDinplugKeypads) {
          h.dinKeypadIds[h.dinKeypadCount++] = keypadId;
        }
      }
    }
    if (comma >= csv.length()) break;
    start = comma + 1;
  }
}

bool hvacHasDinKeypad(const HvacConfig &h, uint16_t keypadId) {
  if (keypadId == 0) return false;
  for (uint8_t i = 0; i < h.dinKeypadCount; i++) {
    if (h.dinKeypadIds[i] == keypadId) return true;
  }
  return false;
}

void markHvacStatesDirty() {
  hvacStatesDirty = true;
  hvacStatesDirtySinceMs = millis();
}

void savePersistedHvacStates() {
  JsonDocument doc;
  JsonArray hv = doc["hvacs"].to<JsonArray>();
  for (uint8_t i = 0; i < config.hvacCount; i++) {
    ensureHvacStateInitialized(i);
    JsonObject o = hv.add<JsonObject>();
    o["id"] = config.hvacs[i].id;
    o["protocol"] = config.hvacs[i].protocol;
    o["emitter"] = config.hvacs[i].emitterIndex;
    o["model"] = config.hvacs[i].model;
    o["custom"] = (config.hvacs[i].isCustom || config.hvacs[i].protocol == "CUSTOM");
    o["power"] = hvacStates[i].power;
    o["mode"] = hvacStates[i].mode;
    o["setpoint"] = hvacStates[i].setpoint;
    o["current_temp"] = hvacStates[i].currentTemp;
    o["fan"] = hvacStates[i].fan;
    o["light"] = hvacStates[i].light;
  }

  File f = SPIFFS.open(kHvacStatePath, FILE_WRITE);
  if (!f) {
    Serial.println("state: failed to open for write");
    return;
  }
  serializeJson(doc, f);
  f.close();
  hvacStatesDirty = false;
}

void loadPersistedHvacStates() {
  hvacStatesDirty = false;
  if (!SPIFFS.exists(kHvacStatePath)) return;
  File f = SPIFFS.open(kHvacStatePath, FILE_READ);
  if (!f) {
    Serial.println("state: failed to open");
    return;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.print("state: parse error ");
    Serial.println(err.c_str());
    return;
  }
  JsonArray hv = doc["hvacs"];
  if (hv.isNull()) return;

  uint8_t restored = 0;
  for (JsonObject o : hv) {
    String id = o["id"] | "";
    if (!id.length()) continue;
    int8_t idx = findHvacIndexById(id);
    if (idx < 0) continue;
    const HvacConfig &h = config.hvacs[idx];
    const bool storedCustom = o["custom"] | false;
    const bool configCustom = (h.isCustom || h.protocol == "CUSTOM");
    if (storedCustom != configCustom) continue;
    String storedProtocol = o["protocol"] | "";
    if (storedProtocol != h.protocol) continue;
    int storedEmitter = o["emitter"] | -1;
    if (storedEmitter != h.emitterIndex) continue;
    int storedModel = o["model"] | -1;
    if (storedModel != h.model) continue;

    ensureHvacStateInitialized(idx);
    HvacRuntimeState restoredState = hvacStates[idx];
    restoredState.power = o["power"] | false;
    restoredState.mode = normalizeMode(o["mode"] | (restoredState.power ? "auto" : "off"));
    if (!restoredState.power) restoredState.mode = "off";
    restoredState.setpoint = o["setpoint"] | 24.0f;
    if (restoredState.setpoint < 16.0f) restoredState.setpoint = 16.0f;
    if (restoredState.setpoint > 32.0f) restoredState.setpoint = 32.0f;
    restoredState.currentTemp = o["current_temp"] | restoredState.setpoint;
    restoredState.fan = normalizeFan(o["fan"] | "auto");
    restoredState.light = o["light"] | false;
    hvacStates[idx] = restoredState;
    restored++;
  }
  if (restored > 0) {
    Serial.print("state: restored ");
    Serial.print(restored);
    Serial.println(" hvac state(s)");
  }
}

void handleHvacStatePersistence() {
  if (!hvacStatesDirty) return;
  unsigned long now = millis();
  if ((now - hvacStatesDirtySinceMs) < kHvacStatePersistDebounceMs) return;
  savePersistedHvacStates();
}

void handleTelnetPeriodicStateBroadcast() {
  if (!telnetServer || config.hvacCount == 0) return;
  if (activeTelnetClientCount() == 0) return;
  unsigned long now = millis();
  if ((now - telnetLastStateBroadcastMs) < kTelnetStateBroadcastIntervalMs) return;
  telnetLastStateBroadcastMs = now;
  for (uint8_t i = 0; i < kMaxTelnetClients; i++) {
    WiFiClient &c = telnetClients[i];
    if (!c || !c.connected()) continue;
    sendAllStatesToTelnetClient(c);
  }
  if (telnetMonitorEnabled && monitorLogTelnetEnabled) {
    addMonitorLogEntry("TX periodic_state_broadcast clients=" + String(activeTelnetClientCount()));
  }
}

String dinButtonsJson(const HvacConfig &h) {
  DynamicJsonDocument doc(512);
  JsonArray arr = doc.to<JsonArray>();
  for (uint8_t i = 0; i < h.dinButtonCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["keypad_id"] = h.dinButtons[i].keypadId;
    o["id"] = h.dinButtons[i].buttonId;
    o["press_action"] = h.dinButtons[i].pressAction;
    o["press_value"] = h.dinButtons[i].pressValue;
    o["press_mode_override"] = dinplugModeOverrideToString(h.dinButtons[i].pressModeOverride);
    o["press_light_mode"] = normalizeDinplugLightMode(h.dinButtons[i].pressLightMode);
    o["hold_action"] = h.dinButtons[i].holdAction;
    o["hold_value"] = h.dinButtons[i].holdValue;
    o["hold_mode_override"] = dinplugModeOverrideToString(h.dinButtons[i].holdModeOverride);
    o["hold_light_mode"] = normalizeDinplugLightMode(h.dinButtons[i].holdLightMode);
    o["toggle_power_mode"] = h.dinButtons[i].togglePowerMode;
    o["led_follow_mode"] = h.dinButtons[i].ledFollowMode;
    o["led_follow_power"] = (h.dinButtons[i].ledFollowMode != 0);
  }
  String out;
  serializeJson(doc, out);
  return out;
}

String customCommandsJson(const HvacConfig &h) {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (uint8_t i = 0; i < h.customCommandCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["name"] = h.customCommands[i].name;
    o["encoding"] = normalizeCustomEncoding(h.customCommands[i].encoding);
    o["code"] = h.customCommands[i].code;
  }
  String out;
  serializeJson(doc, out);
  return out;
}

String customCommandNamesSummary(const HvacConfig &h) {
  if (h.customCommandCount == 0) return "-";
  String out;
  for (uint8_t i = 0; i < h.customCommandCount; i++) {
    if (out.length()) out += ", ";
    out += h.customCommands[i].name;
    if (out.length() > 120) {
      out += "...";
      break;
    }
  }
  if (!out.length()) out = "-";
  return out;
}

const CustomCommandCode *findCustomCommandByName(const HvacConfig &h, const String &nameIn) {
  String name = nameIn;
  name.trim();
  if (!name.length()) return nullptr;
  for (uint8_t i = 0; i < h.customCommandCount; i++) {
    if (h.customCommands[i].name == name) return &h.customCommands[i];
  }
  return nullptr;
}

void loadCustomCommandsFromRequest(HvacConfig &h) {
  h.customCommandCount = 0;
  for (uint8_t i = 0; i < kMaxCustomCommands; i++) {
    String base = "cc" + String(i) + "_";
    String name = web.arg(base + "name");
    String encoding = normalizeCustomEncoding(web.arg(base + "encoding"));
    String code = web.arg(base + "code");
    name.trim();
    code.trim();
    if (!name.length() || !code.length()) continue;
    bool duplicate = false;
    for (uint8_t x = 0; x < h.customCommandCount; x++) {
      if (h.customCommands[x].name == name) { duplicate = true; break; }
    }
    if (duplicate) continue;
    if (h.customCommandCount >= kMaxCustomCommands) break;
    CustomCommandCode &cmd = h.customCommands[h.customCommandCount++];
    cmd.name = name;
    cmd.encoding = encoding;
    cmd.code = code;
  }
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
  html += "<nav><a href='/'>Home</a><a href='/config'>Config</a><a href='/emitters'>Emitters</a><a href='/hvacs'>HVACs</a><a href='/hvacs/test'>Test HVAC</a><a href='/dinplug'>DINplug</a><a href='/system#monitor'>Monitor</a><a href='/system#firmware'>Firmware</a><a href='/system#backup'>Upload</a><a href='/config/download'>Download</a></nav>";
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

// ---- Web Handlers ----

void handleHome() {
  if (!checkAuth()) { requestAuth(); return; }
  String html = pageHeader("IR HVAC Telnet");
  html += "<div class='card'><h2>IR HVAC Telnet Server</h2>";
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
  html += "<p>HVACs: <strong>" + String(config.hvacCount) + "</strong></p></div>";
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
  html += "<h3>DS18B20 Temperature Sensors</h3>";
  html += "<div class='row'><input type='checkbox' id='tsEnabled' name='ts_enabled'" +
          String(config.tempSensors.enabled ? " checked" : "") + "><label for='tsEnabled'>Enable DS18B20 bus</label></div>";
  html += "<label>OneWire GPIO</label>";
  html += "<input name='ts_gpio' type='number' min='0' max='39' value='" + String(config.tempSensors.gpio) + "'>";
  html += "<label>Read interval (seconds)</label>";
  html += "<input name='ts_interval' type='number' min='1' max='3600' value='" + String(config.tempSensors.readIntervalSec) + "'>";
  html += "<label>Temperature decimals (0-2)</label>";
  html += "<select name='ts_precision'>";
  html += "<option value='0'" + String(tempSensorPrecision() == 0 ? " selected" : "") + ">0</option>";
  html += "<option value='1'" + String(tempSensorPrecision() == 1 ? " selected" : "") + ">1</option>";
  html += "<option value='2'" + String(tempSensorPrecision() == 2 ? " selected" : "") + ">2</option>";
  html += "</select>";
  html += "<p>Detected sensors at runtime: <strong>" + String(tempSensorCount) + "</strong></p>";
  html += "<label>Sensor Names</label>";
  if (tempSensorCount == 0) {
    html += "<p>No sensors detected yet. Names can be set once sensors are detected.</p>";
  } else {
    html += "<table><tr><th>Index</th><th>Name</th><th>Address</th><th>Current Temp (C)</th></tr>";
    for (uint8_t i = 0; i < tempSensorCount; i++) {
      html += "<tr><td>" + String(i) + "</td><td><input name='ts_name_" + String(i) + "' value='" +
              htmlEscape(config.tempSensors.names[i]) + "' placeholder='Sensor " + String(i) + "'></td><td>" +
              htmlEscape(sensorAddressToString(tempSensorAddresses[i])) + "</td><td>";
      if (tempSensorValid[i]) html += String(tempSensorReadings[i], static_cast<unsigned int>(tempSensorPrecision()));
      else html += "N/A";
      html += "</td></tr>";
    }
    html += "</table>";
  }
  html += "<h3>Ethernet (WT32 LAN8720)</h3>";
  html += "<div class='row'><input type='checkbox' id='ethEnabled' name='eth_enabled'" +
          String(config.eth.enabled ? " checked" : "") + "><label for='ethEnabled'>Enable Ethernet</label></div>";
  html += "<p>When enabled, firmware tries Ethernet first, then falls back to WiFi/AP if link fails.</p>";
  html += "<h3>IR Receiver Log Input</h3>";
  html += "<div class='row'><input type='checkbox' id='irrxEnabled' name='irrx_enabled'" +
          String(config.irReceiver.enabled ? " checked" : "") + "><label for='irrxEnabled'>Enable IR receiver</label></div>";
  html += "<label>IR receiver GPIO</label>";
  html += "<input name='irrx_gpio' type='number' min='0' max='39' value='" + String(config.irReceiver.gpio) + "'>";
  String irMode = normalizeIrReceiverMode(config.irReceiver.mode);
  html += "<label>IR log mode</label>";
  html += "<select name='irrx_mode'>";
  html += "<option value='auto'" + String(irMode == "auto" ? " selected" : "") + ">auto (decode protocol/code)</option>";
  html += "<option value='pronto'" + String(irMode == "pronto" ? " selected" : "") + ">pronto</option>";
  html += "<option value='rawhex'" + String(irMode == "rawhex" ? " selected" : "") + ">raw hex</option>";
  html += "</select>";
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

void handleDinplugPage() {
  if (!checkAuth()) { requestAuth(); return; }
  String html = pageHeader("DINplug");
  html += "<div class='card'><h2>DINplug Gateway</h2>";
  String testResult = web.arg("test");
  if (testResult == "ok") {
    html += "<p><strong>Test result:</strong> Connected.</p>";
  } else if (testResult == "fail") {
    html += "<p><strong>Test result:</strong> Connection failed.</p>";
  } else if (testResult == "missing") {
    html += "<p><strong>Test result:</strong> Set a gateway first.</p>";
  }
  html += "<p>Status: <strong id='dinStatus'>" + htmlEscape(dinplugConnectionStatus()) + "</strong></p>";
  html += "<form method='POST' action='/dinplug/save'>";
  html += "<label>Gateway (IP or hostname)</label>";
  html += "<input name='gateway_host' placeholder='dinplug.example.com or 192.168.1.50' value='" + htmlEscape(config.dinplug.gatewayHost) + "'>";
  html += "<div class='row'><input type='checkbox' id='dinAuto' name='auto_connect'" +
          String(config.dinplug.autoConnect ? " checked" : "") +
          "><label for='dinAuto'>Connect on boot</label></div>";
  html += "<button type='submit'>Save</button>";
  html += "</form>";
  html += "<form method='POST' action='/dinplug/test'>";
  html += "<button type='submit' class='secondary'>Test connection</button>";
  html += "</form>";
  html += "<p>On save the device will attempt to connect immediately when a gateway is set.</p>";
  html += "</div>";
  html += pageFooter();
  web.send(200, "text/html", html);
}

void handleDinplugSave() {
  if (!checkAuth()) { requestAuth(); return; }
  String gatewayHost = web.arg("gateway_host");
  if (gatewayHost.length() == 0) gatewayHost = web.arg("gateway_ip");
  gatewayHost.trim();
  config.dinplug.gatewayHost = gatewayHost;
  config.dinplug.autoConnect = web.hasArg("auto_connect");
  saveConfig();
  if (config.dinplug.gatewayHost.length() == 0) {
    dinplugClient.stop();
    dinplugConnected = false;
  }
  ensureDinplugConnected(true);
  web.sendHeader("Location", "/dinplug");
  web.send(302, "text/plain", "");
}

void handleDinplugTest() {
  if (!checkAuth()) { requestAuth(); return; }
  String gatewayHost = config.dinplug.gatewayHost;
  gatewayHost.trim();
  if (gatewayHost.length() == 0) {
    web.sendHeader("Location", "/dinplug?test=missing");
    web.send(302, "text/plain", "");
    return;
  }
  ensureDinplugConnected(true);
  web.sendHeader("Location", dinplugClient.connected() ? "/dinplug?test=ok" : "/dinplug?test=fail");
  web.send(302, "text/plain", "");
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
  web.sendHeader("Location", "/hvacs?tab=emitters");
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
  web.sendHeader("Location", "/hvacs?tab=emitters");
  web.send(302, "text/plain", "");
}

void handleHvacsPage() {
  if (!checkAuth()) { requestAuth(); return; }
  String html = pageHeader("HVACs");
  html += "<div class='card'><h2>HVACs</h2>";
  html += "<table><tr><th>#</th><th>ID</th><th>Protocol</th><th>Emitter</th><th>Custom Commands</th><th>Action</th></tr>";
  for (uint8_t i = 0; i < config.hvacCount; i++) {
    const HvacConfig &h = config.hvacs[i];
    html += "<tr><td>" + String(i) + "</td><td>" + htmlEscape(h.id) + "</td><td>" + htmlEscape(h.protocol) + "</td>";
    html += "<td>" + String(h.emitterIndex) + "</td>";
    html += "<td>" + htmlEscape(customCommandNamesSummary(h)) + "</td>";
    html += "<td><a href='/hvacs/delete?index=" + String(i) + "'>Delete</a></td></tr>";
  }
  html += "</table>";

  if (config.emitterCount == 0) {
    html += "<p><strong>Add at least one emitter before registering HVACs.</strong></p>";
  } else {
    html += "<h3>Add HVAC</h3>";
    html += "<form method='POST' action='/hvacs/add'>";
    html += "<label>Protocol</label><select name='protocol' id='addProtocol'>" + protocolOptionsHtml("") + "</select>";
    html += "<label>Emitter</label><select name='emitter'>" + emitterOptionsHtml(0) + "</select>";
    html += "<label>Model (optional)</label><input name='model' value='-1'>";
    html += "<div id='addCustomFields' style='display:none;'>";
    html += "<h4>Custom Commands (add one or more before saving HVAC)</h4>";
    html += "<table><tr><th>#</th><th>Name</th><th>Encoding</th><th>Code</th><th>Learn</th><th>Delete</th></tr>";
    for (uint8_t cc = 0; cc < kMaxCustomCommands; cc++) {
      String display = (cc == 0) ? "" : " style='display:none;'";
      html += "<tr data-add-cc-row='" + String(cc) + "'" + display + ">";
      html += "<td>" + String(cc + 1) + "</td>";
      html += "<td><input name='cc" + String(cc) + "_name' placeholder='command name'></td>";
      html += "<td><select name='cc" + String(cc) + "_encoding'><option value='pronto'>pronto</option><option value='gc'>gc</option><option value='racepoint'>racepoint</option><option value='rawhex'>rawhex</option></select></td>";
      html += "<td><textarea name='cc" + String(cc) + "_code' rows='2' placeholder='code'></textarea></td>";
      html += "<td><button type='button' class='secondary add-learn-btn'>Learn</button></td>";
      html += "<td><button type='button' class='secondary add-cc-clear-btn'>Delete</button></td>";
      html += "</tr>";
    }
    html += "</table>";
    html += "<button type='button' class='secondary' id='addCmdRowBtn'>Add Command</button>";
    html += "</div>";
    html += "<button type='submit'>Add</button></form>";

    bool hasCustomSource = false;
    for (uint8_t i = 0; i < config.hvacCount; i++) {
      if (config.hvacs[i].protocol == "CUSTOM" || config.hvacs[i].isCustom) {
        hasCustomSource = true;
        break;
      }
    }
    if (hasCustomSource && config.hvacCount < kMaxHvacs) {
      html += "<h3>Clone Custom HVAC</h3>";
      html += "<form method='POST' action='/hvacs/clone'>";
      html += "<label>Source Custom HVAC</label><select name='source_index'>";
      for (uint8_t i = 0; i < config.hvacCount; i++) {
        const HvacConfig &src = config.hvacs[i];
        if (src.protocol != "CUSTOM" && !src.isCustom) continue;
        html += "<option value='" + String(i) + "'>" + htmlEscape(src.id) + " (emitter " +
                String(src.emitterIndex) + ", commands " + String(src.customCommandCount) + ")</option>";
      }
      html += "</select>";
      html += "<label>Target Emitter</label><select name='emitter'>" + emitterOptionsHtml(0) + "</select>";
      html += "<button type='submit' class='secondary'>Clone</button></form>";
    }
  }
  if (config.hvacCount > 0 && config.emitterCount > 0) {
    html += "<h3>Edit HVAC</h3>";
    html += "<form method='POST' action='/hvacs/update' id='editHvacForm'>";
    html += "<label>HVAC</label><select name='index' id='editHvacIndex'>";
    for (uint8_t i = 0; i < config.hvacCount; i++) {
      const HvacConfig &h = config.hvacs[i];
      html += "<option value='" + String(i) + "' data-protocol='" + htmlEscape(h.protocol) +
              "' data-emitter='" + String(h.emitterIndex) + "' data-model='" + String(h.model) +
              "' data-ctsrc='" + htmlEscape(h.currentTempSource) + "' data-tsidx='" + String(h.tempSensorIndex) +
              "' data-keypads='" + htmlEscape(dinKeypadsCsv(h)) + "' data-buttons='" + htmlEscape(dinButtonsJson(h)) +
              "' data-customcmds='" + htmlEscape(customCommandsJson(h)) + "'>";
      html += htmlEscape(h.id) + " (" + htmlEscape(h.protocol) + ")</option>";
    }
    html += "</select>";
    html += "<label>Protocol</label><select name='protocol' id='editHvacProtocol'>" + protocolOptionsHtml("") + "</select>";
    html += "<label>Emitter</label><select name='emitter' id='editHvacEmitter'>" + emitterOptionsHtml(0) + "</select>";
    html += "<label>Model (optional)</label><input name='model' id='editHvacModel' value='-1'>";
    html += "<label>Current Temp Source</label><select name='current_temp_source' id='editCurrentTempSource'>";
    html += "<option value='setpoint'>setpoint</option>";
    html += "<option value='sensor'>sensor</option>";
    html += "</select>";
    html += "<label>DS18B20 Sensor Index</label><input name='temp_sensor_index' id='editTempSensorIndex' type='number' min='0' max='" + String(kMaxTempSensors - 1) + "' value='0'>";
    html += "<label>DINplug Keypad IDs (comma-separated)</label><input name='din_keypad_ids' id='dinKeypads' value=''>";
    html += "<h4>Keypad Button Actions</h4>";
    html += "<table class='din-actions'><tr><th>#</th><th>Keypad ID</th><th>Button ID</th><th>Press Action</th><th>Press Value</th><th>Hold Action</th><th>Hold Value</th><th>Toggle Power Mode</th><th>LED follows HVAC power</th></tr>";
    for (uint8_t b = 0; b < kMaxDinplugButtons; b++) {
      html += "<tr data-btn-row='" + String(b) + "'>";
      html += "<td>" + String(b + 1) + "</td>";
      html += "<td><input class='din-id' name='btn" + String(b) + "_keypad_id' type='number' min='0' value='0'></td>";
      html += "<td><input class='din-id' name='btn" + String(b) + "_id' type='number' min='0' value='0'></td>";
      html += "<td><select class='din-action' name='btn" + String(b) + "_press_action'>" + dinActionOptionsHtml("none") + "</select></td>";
      html += "<td><input class='din-val' name='btn" + String(b) + "_press_value' type='number' step='0.5' value='1'></td>";
      html += "<td><select class='din-action' name='btn" + String(b) + "_hold_action'>" + dinActionOptionsHtml("none") + "</select></td>";
      html += "<td><input class='din-val' name='btn" + String(b) + "_hold_value' type='number' step='0.5' value='1'></td>";
      html += "<td><select class='din-action' name='btn" + String(b) + "_toggle_power_mode'>" + dinToggleModeOptionsHtml("auto") + "</select></td>";
      html += "<td><select class='din-action' name='btn" + String(b) + "_led_follow_mode'>";
      html += "<option value='0'>disabled</option>";
      html += "<option value='1'>LED on when HVAC on</option>";
      html += "<option value='2'>LED on when HVAC off</option>";
      html += "</select></td>";
      html += "</tr>";
    }
    html += "</table>";
    html += "<div id='editCustomCommandsWrap' style='display:none;'>";
    html += "<h4>Custom Commands</h4>";
    html += "<p>Used by DIN actions as <code>custom:&lt;name&gt;</code>.</p>";
    html += "<div id='customCmdSummary' class='card' style='margin:8px 0;'></div>";
    html += "<table><tr><th>#</th><th>Name</th><th>Encoding</th><th>Code</th><th>Learn</th><th>Delete</th></tr>";
    for (uint8_t cc = 0; cc < kMaxCustomCommands; cc++) {
      html += "<tr data-cc-row='" + String(cc) + "'>";
      html += "<td>" + String(cc + 1) + "</td>";
      html += "<td><input name='cc" + String(cc) + "_name' placeholder='command name'></td>";
      html += "<td><select name='cc" + String(cc) + "_encoding'><option value='pronto'>pronto</option><option value='gc'>gc</option><option value='racepoint'>racepoint</option><option value='rawhex'>rawhex</option></select></td>";
      html += "<td><textarea name='cc" + String(cc) + "_code' rows='2' placeholder='code'></textarea></td>";
      html += "<td><button type='button' class='secondary learn-btn' data-cc='" + String(cc) + "'>Learn</button></td>";
      html += "<td><button type='button' class='secondary cc-clear-btn' data-cc='" + String(cc) + "'>Delete</button></td>";
      html += "</tr>";
    }
    html += "</table></div>";
    html += "<button type='submit' class='secondary'>Update</button></form>";
    html += "<script>";
    html += "const addProtocol=document.getElementById('addProtocol');";
    html += "const addCustomFields=document.getElementById('addCustomFields');";
    html += "const addCmdRowBtn=document.getElementById('addCmdRowBtn');";
    html += "const addCcRows=[...document.querySelectorAll('[data-add-cc-row]')];";
    html += "if(addProtocol&&addCustomFields){const upd=()=>{addCustomFields.style.display=(addProtocol.value==='CUSTOM')?'block':'none';};addProtocol.addEventListener('change',upd);upd();}";
    html += "const hvacSel=document.getElementById('editHvacIndex');";
    html += "const protoSel=document.getElementById('editHvacProtocol');";
    html += "const emSel=document.getElementById('editHvacEmitter');";
    html += "const modelInput=document.getElementById('editHvacModel');";
    html += "const ctSrc=document.getElementById('editCurrentTempSource');";
    html += "const tsIdx=document.getElementById('editTempSensorIndex');";
    html += "const keypadsInput=document.getElementById('dinKeypads');";
    html += "const editCustomWrap=document.getElementById('editCustomCommandsWrap');";
    html += "const customCmdSummary=document.getElementById('customCmdSummary');";
    html += "const btnRows=[...document.querySelectorAll('[data-btn-row]')];";
    html += "const ccRows=[...document.querySelectorAll('[data-cc-row]')];";
    html += "const regularActions=['none','temp_up','temp_down','set_temp','power_on','power_off','toggle_power','mode_heat','mode_cool','mode_fan','mode_auto','mode_off','light_on','light_off','toggle_light'];";
    html += "const needsValue=(a)=>['temp_up','temp_down','set_temp'].includes((a||'').toLowerCase());";
    html += "const setSelectOptions=(sel,opts,selected)=>{if(!sel)return;sel.innerHTML='';opts.forEach(v=>{const o=document.createElement('option');o.value=v;o.textContent=v;o.selected=(v===selected);sel.appendChild(o);});if(!opts.includes(selected))sel.value=opts[0]||'';};";
    html += "const syncValueEnabled=(act,val,isCustom)=>{if(!act||!val)return;const on=!isCustom&&needsValue(act.value);val.disabled=!on;if(!on&&(!val.value||val.value==='0'))val.value='1';};";
    html += "const customActionList=(cmds)=>{const out=['none'];(cmds||[]).forEach(c=>{if(c&&c.name)out.push('custom:'+c.name);});return out;};";
    html += "const fillCustomRows=(cmds)=>{ccRows.forEach((row,idx)=>{const c=(cmds&&cmds[idx])||{};const n=row.querySelector(`input[name='cc${idx}_name']`);const e=row.querySelector(`select[name='cc${idx}_encoding']`);const code=row.querySelector(`textarea[name='cc${idx}_code']`);if(n)n.value=c.name||'';if(e)e.value=(c.encoding||'pronto');if(code)code.value=c.code||'';});if(customCmdSummary){const used=(cmds||[]).filter(c=>c&&c.name&&c.code);if(!used.length){customCmdSummary.innerHTML='<strong>Current saved commands:</strong> none';}else{customCmdSummary.innerHTML='<strong>Current saved commands:</strong><br>'+used.map(c=>`${c.name} (${c.encoding||'pronto'})`).join('<br>');}}};";
    html += "const applyButtonRows=(parsed,isCustom,customCmds)=>{const customActions=customActionList(customCmds);btnRows.forEach((row,idx)=>{const data=parsed[idx]||{};";
    html += "const kInput=row.querySelector(`input[name='btn${idx}_keypad_id']`);";
    html += "const idInput=row.querySelector(`input[name='btn${idx}_id']`);";
    html += "const pAct=row.querySelector(`select[name='btn${idx}_press_action']`);";
    html += "const pVal=row.querySelector(`input[name='btn${idx}_press_value']`);";
    html += "const hAct=row.querySelector(`select[name='btn${idx}_hold_action']`);";
    html += "const hVal=row.querySelector(`input[name='btn${idx}_hold_value']`);";
    html += "const tMode=row.querySelector(`select[name='btn${idx}_toggle_power_mode']`);";
    html += "const ledMode=row.querySelector(`select[name='btn${idx}_led_follow_mode']`);";
    html += "if(kInput)kInput.value=data.keypad_id||0;if(idInput)idInput.value=data.id||0;";
    html += "const pSel=(data.press_action||'none');const hSel=(data.hold_action||'none');";
    html += "setSelectOptions(pAct,isCustom?customActions:regularActions,pSel);";
    html += "setSelectOptions(hAct,isCustom?customActions:regularActions,hSel);";
    html += "if(pVal)pVal.value=data.press_value!=null?data.press_value:1;if(hVal)hVal.value=data.hold_value!=null?data.hold_value:1;";
    html += "if(tMode){const tm=(data.toggle_power_mode||'auto').toLowerCase();tMode.value=(['auto','cool','heat','dry','fan'].includes(tm)?tm:'auto');}";
    html += "if(ledMode){const m=(data.led_follow_mode!=null?String(data.led_follow_mode):(data.led_follow_power?'2':'0'));ledMode.value=(m==='1'||m==='2')?m:'0';}";
    html += "syncValueEnabled(pAct,pVal,isCustom);syncValueEnabled(hAct,hVal,isCustom);";
    html += "if(pAct&&pVal){pAct.onchange=()=>syncValueEnabled(pAct,pVal,isCustom);}if(hAct&&hVal){hAct.onchange=()=>syncValueEnabled(hAct,hVal,isCustom);}";
    html += "if(tMode)tMode.disabled=isCustom;if(ledMode)ledMode.disabled=isCustom;";
    html += "});};";
    html += "const sync=()=>{if(!hvacSel)return;const opt=hvacSel.selectedOptions[0];if(!opt)return;";
    html += "const p=opt.dataset.protocol||'';const isCustom=(p==='CUSTOM');";
    html += "const e=opt.dataset.emitter||'0';const m=opt.dataset.model||'-1';const cts=opt.dataset.ctsrc||'setpoint';const tsi=opt.dataset.tsidx||'0';";
    html += "const ks=opt.dataset.keypads||'';const btnJson=opt.dataset.buttons||'[]';const ccJson=opt.dataset.customcmds||'[]';";
    html += "if(protoSel){for(const o of protoSel.options){o.selected=(o.value===p);} }";
    html += "if(emSel){for(const o of emSel.options){o.selected=(o.value===e);} }";
    html += "if(ctSrc){for(const o of ctSrc.options){o.selected=(o.value===cts);} ctSrc.disabled=isCustom;}";
    html += "if(tsIdx){tsIdx.value=tsi;tsIdx.disabled=isCustom;}if(modelInput){modelInput.value=m;modelInput.disabled=isCustom;}";
    html += "if(keypadsInput){keypadsInput.value=ks;}if(editCustomWrap){editCustomWrap.style.display=isCustom?'block':'none';}";
    html += "let parsed=[];try{parsed=JSON.parse(btnJson);}catch(err){parsed=[];}let customCmds=[];try{customCmds=JSON.parse(ccJson);}catch(err){customCmds=[];}";
    html += "fillCustomRows(customCmds);applyButtonRows(parsed,isCustom,customCmds);};";
    html += "const sleep=(ms)=>new Promise(r=>setTimeout(r,ms));";
    html += "const startLearn=async(enc)=>{const body=new URLSearchParams({encoding:enc||'pronto'});const r=await fetch('/api/ir/learn/start',{method:'POST',body});return r.json();};";
    html += "const cancelLearn=async()=>{try{await fetch('/api/ir/learn/cancel',{method:'POST'});}catch(e){}};";
    html += "const pollLearn=async(stopState)=>{const deadline=Date.now()+10000;while(Date.now()<deadline){if(stopState.cancelled)return {error:'cancelled'};const left=Math.max(0,Math.ceil((deadline-Date.now())/1000));if(stopState.setLeft)stopState.setLeft(left);const r=await fetch('/api/ir/learn/poll');const d=await r.json();if(d&&d.ready&&d.code)return d;if(d&&d.error)return d;await sleep(250);}await cancelLearn();return {error:'timeout'};};";
    html += "const showLearnOverlay=(msg,onCancel)=>{const ov=document.createElement('div');ov.style.cssText='position:fixed;inset:0;background:rgba(0,0,0,.65);z-index:9999;display:flex;align-items:center;justify-content:center;padding:20px;';";
    html += "const box=document.createElement('div');box.style.cssText='max-width:520px;width:100%;background:#111827;color:#e2e8f0;border:1px solid #334155;border-radius:12px;padding:16px;font-family:Segoe UI,Tahoma,Arial,sans-serif;';";
    html += "const title=document.createElement('h3');title.style.margin='0 0 8px 0';title.textContent='Learning IR Code';";
    html += "const p=document.createElement('p');p.style.margin='0';p.textContent=msg;";
    html += "const t=document.createElement('p');t.style.margin='10px 0 0 0';t.textContent='Timeout in 10s';";
    html += "const row=document.createElement('div');row.style.cssText='margin-top:12px;display:flex;justify-content:flex-end;';";
    html += "const btn=document.createElement('button');btn.type='button';btn.textContent='Cancel';btn.style.cssText='background:#38bdf8;border:0;color:#0b1220;font-weight:700;padding:8px 14px;border-radius:8px;cursor:pointer;';";
    html += "btn.onclick=()=>{if(onCancel)onCancel();};row.appendChild(btn);box.appendChild(title);box.appendChild(p);box.appendChild(t);box.appendChild(row);ov.appendChild(box);document.body.appendChild(ov);";
    html += "return {close:()=>{if(ov&&ov.parentNode)ov.parentNode.removeChild(ov);},setLeft:(n)=>{t.textContent='Timeout in '+n+'s';}};};";
    html += "const clearCcRow=(row,hide)=>{if(!row)return;const n=row.querySelector('input[name$=\"_name\"]');const e=row.querySelector('select[name$=\"_encoding\"]');const c=row.querySelector('textarea[name$=\"_code\"]');if(n)n.value='';if(e)e.value='pronto';if(c)c.value='';if(hide)row.style.display='none';};";
    html += "const learnIntoFields=async(encSel,codeBox)=>{if(!encSel||!codeBox)return;while(true){const s=await startLearn(encSel.value);if(!s.ok){alert('Learn unavailable. Enable IR receiver first.');return;}const stopState={cancelled:false,setLeft:null};const ov=showLearnOverlay('Ready to learn IR code. Point the remote and press a button.',async()=>{stopState.cancelled=true;await cancelLearn();ov.close();});stopState.setLeft=ov.setLeft;const learned=await pollLearn(stopState);ov.close();if(!learned||learned.error){if(learned&&learned.error==='cancelled')return;alert('Learn failed: '+(learned&&learned.error?learned.error:'timeout')+'. Try again.');return;}const preview=String(learned.code||'');const ok=confirm('Learned code:\\n'+preview.substring(0,1000)+(preview.length>1000?'...':'')+'\\n\\nUse this code?');if(ok){codeBox.value=learned.code||'';return;}}};";
    html += "document.querySelectorAll('.learn-btn').forEach(btn=>{btn.addEventListener('click',()=>{const row=btn.closest('tr');if(!row)return;learnIntoFields(row.querySelector('select[name$=\"_encoding\"]'),row.querySelector('textarea[name$=\"_code\"]'));});});";
    html += "document.querySelectorAll('.cc-clear-btn').forEach(btn=>{btn.addEventListener('click',()=>{const row=btn.closest('tr');clearCcRow(row,false);});});";
    html += "document.querySelectorAll('.add-learn-btn').forEach(btn=>{btn.addEventListener('click',()=>{const row=btn.closest('tr');if(!row)return;row.style.display='';learnIntoFields(row.querySelector('select[name$=\"_encoding\"]'),row.querySelector('textarea[name$=\"_code\"]'));});});";
    html += "document.querySelectorAll('.add-cc-clear-btn').forEach(btn=>{btn.addEventListener('click',()=>{const row=btn.closest('tr');if(!row)return;const idx=Number(row.dataset.addCcRow||'0');clearCcRow(row,idx>0);});});";
    html += "if(addCmdRowBtn){addCmdRowBtn.addEventListener('click',()=>{const next=addCcRows.find(r=>r.style.display==='none');if(next){next.style.display='';}else{alert('Maximum commands reached.');}});}";
    html += "if(addProtocol&&addCustomFields){addProtocol.addEventListener('change',()=>{if(addProtocol.value!=='CUSTOM'){addCcRows.forEach((r,i)=>clearCcRow(r,i>0));}});}";
    html += "if(hvacSel){hvacSel.addEventListener('change',sync);sync();}";
    html += "</script>";
  } else if (config.emitterCount > 0) {
    html += "<script>";
    html += "const addProtocol=document.getElementById('addProtocol');";
    html += "const addCustomFields=document.getElementById('addCustomFields');";
    html += "const addCmdRowBtn=document.getElementById('addCmdRowBtn');";
    html += "const addCcRows=[...document.querySelectorAll('[data-add-cc-row]')];";
    html += "if(addProtocol&&addCustomFields){const upd=()=>{addCustomFields.style.display=(addProtocol.value==='CUSTOM')?'block':'none';};addProtocol.addEventListener('change',upd);upd();}";
    html += "const clearCcRow=(row,hide)=>{if(!row)return;const n=row.querySelector('input[name$=\"_name\"]');const e=row.querySelector('select[name$=\"_encoding\"]');const c=row.querySelector('textarea[name$=\"_code\"]');if(n)n.value='';if(e)e.value='pronto';if(c)c.value='';if(hide)row.style.display='none';};";
    html += "document.querySelectorAll('.add-cc-clear-btn').forEach(btn=>{btn.addEventListener('click',()=>{const row=btn.closest('tr');if(!row)return;const idx=Number(row.dataset.addCcRow||'0');clearCcRow(row,idx>0);});});";
    html += "if(addCmdRowBtn){addCmdRowBtn.addEventListener('click',()=>{const next=addCcRows.find(r=>r.style.display==='none');if(next){next.style.display='';}else{alert('Maximum commands reached.');}});}";
    html += "if(addProtocol&&addCustomFields){addProtocol.addEventListener('change',()=>{if(addProtocol.value!=='CUSTOM'){addCcRows.forEach((r,i)=>clearCcRow(r,i>0));}});}";
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
  if (!h.protocol.length()) h.protocol = "CUSTOM";
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
  web.sendHeader("Location", "/hvacs");
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
  dst.dinButtonCount = 0;

  initHvacRuntimeStates();
  saveConfig();
  Serial.print("web: hvac cloned from index ");
  Serial.print(srcIdx);
  Serial.print(" to id=");
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
  h.dinButtonCount = 0;
  for (uint8_t i = 0; i < kMaxDinplugButtons; i++) {
    String base = "btn" + String(i) + "_";
    uint16_t keypadId = web.arg(base + "keypad_id").toInt();
    uint16_t btnId = web.arg(base + "id").toInt();
    if (btnId == 0) continue;
    DinplugButtonBinding &b = h.dinButtons[h.dinButtonCount++];
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
    if (h.dinButtonCount >= kMaxDinplugButtons) break;
  }
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
      html += "<option value='" + htmlEscape(config.hvacs[i].id) + "' data-light='" + lightValue +
              "' data-protocol='" + htmlEscape(config.hvacs[i].protocol) +
              "' data-customcmds='" + htmlEscape(customCommandsJson(config.hvacs[i])) + "'>" +
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
    html += "</div>";
    html += "<div id='customTestWrap' style='display:none;margin-top:10px;'><label>Custom Command</label><select name='command_name' id='customCmdSelect'><option value=''>-- select --</option></select></div>";
    html += "<button type='submit'>Send Test</button></form>";
  }
  html += "<h4>Generated JSON</h4><pre id='jsonPreview'>{}</pre>";
  html += "<h4>Response</h4><pre id='jsonResponse'>-</pre></div>";
  html += "<div class='card'><h3>Send Raw Code</h3>";
  html += "<form id='rawForm'>";
  html += "<div class='grid'>";
  html += "<div><label>Emitter</label><select name='emitter'>" + emitterOptionsHtml(0) + "</select></div>";
  html += "<div><label>Encoding</label><select name='encoding'><option value='pronto'>pronto</option><option value='gc'>gc</option><option value='racepoint'>racepoint</option><option value='rawhex'>rawhex</option></select></div>";
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
  html += "const customWrap=form?document.getElementById('customTestWrap'):null;";
  html += "const customCmdSelect=form?document.getElementById('customCmdSelect'):null;";
  html += "const powerSel=form?form.querySelector(\"select[name='power']\"):null;";
  html += "const modeSel=form?form.querySelector(\"select[name='mode']\"):null;";
  html += "const tempInput=form?form.querySelector(\"input[name='temp']\"):null;";
  html += "const fanSel=form?form.querySelector(\"select[name='fan']\"):null;";
  html += "const swingVSel=form?form.querySelector(\"select[name='swingv']\"):null;";
  html += "const swingHSel=form?form.querySelector(\"select[name='swingh']\"):null;";
  html += "const syncLight=()=>{if(!hvacSelect||!lightSelect)return;const opt=hvacSelect.selectedOptions[0];";
  html += "if(!opt)return;const val=opt.dataset.light;if(val==='true'||val==='false'){lightSelect.value=val;}else{lightSelect.value='';}};";
  html += "const setEnabled=(el,on)=>{if(el)el.disabled=!on;};";
  html += "const syncCustomMode=()=>{if(!hvacSelect||!customWrap||!customCmdSelect)return false;const opt=hvacSelect.selectedOptions[0];if(!opt)return false;";
  html += "const protocol=(opt.dataset.protocol||'').toUpperCase();const isCustom=(protocol==='CUSTOM');customWrap.style.display=isCustom?'block':'none';";
  html += "if(isCustom){let cmds=[];try{cmds=JSON.parse(opt.dataset.customcmds||'[]');}catch(e){cmds=[];}customCmdSelect.innerHTML='<option value=\"\">-- select --</option>';cmds.forEach(c=>{if(!c||!c.name)return;const o=document.createElement('option');o.value=c.name;o.textContent=`${c.name} (${c.encoding||'pronto'})`;customCmdSelect.appendChild(o);});}";
  html += "setEnabled(powerSel,!isCustom);setEnabled(modeSel,!isCustom);setEnabled(tempInput,!isCustom);setEnabled(fanSel,!isCustom);setEnabled(swingVSel,!isCustom);setEnabled(swingHSel,!isCustom);setEnabled(lightSelect,!isCustom);";
  html += "return isCustom;};";
  html += "const update=()=>{if(!form)return;const data={cmd:'send'};const fd=new FormData(form);";
  html += "for(const [k,v] of fd.entries()){if(v==='')continue;data[k]=v;}preview.textContent=JSON.stringify(data,null,2);};";
  html += "if(hvacSelect){hvacSelect.addEventListener('change',()=>{syncLight();syncCustomMode();update();});}";
  html += "if(customCmdSelect){customCmdSelect.addEventListener('change',update);}";
  html += "if(form){form.addEventListener('input',update);syncLight();syncCustomMode();update();";
  html += "form.addEventListener('submit',async(e)=>{e.preventDefault();response.textContent='Sending...';";
  html += "if(customWrap&&customWrap.style.display!=='none'&&customCmdSelect&&customCmdSelect.value===''){response.textContent='Select a custom command first.';return;}";
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
  String json = configToJsonString();
  web.sendHeader("Content-Disposition", "attachment; filename=\"config.json\"");
  web.send(200, "application/json", json);
}

String configUploadBuffer;
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
  if (!loadConfigFromJson(configUploadBuffer)) {
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

void handleFirmwarePage() {
  if (!checkAuth()) { requestAuth(); return; }
  String html = pageHeader("Firmware Update");
  html += "<div class='card'><h2>OTA Firmware Update</h2>";
  html += "<p>Upload a compiled ESP32 firmware binary (.bin) to flash over network.</p>";
  html += "<form id='fwForm' method='POST' action='/firmware/update' enctype='multipart/form-data'>";
  html += "<input id='fwFile' type='file' name='firmware' accept='.bin,application/octet-stream' required>";
  html += "<button type='submit' id='fwBtn'>Upload & Flash</button></form>";
  html += "<div style='margin-top:12px;'><label>Upload Progress</label><progress id='fwProgress' value='0' max='100' style='width:100%;height:18px;'></progress>";
  html += "<div id='fwStatus'>Idle</div></div>";
  html += "<p>Alternative OTA endpoint: ArduinoOTA on hostname <code>" +
          htmlEscape(config.hostname.length() ? config.hostname : kDefaultHostname) +
          ".local</code>.</p>";
  html += "<script>";
  html += "const fwForm=document.getElementById('fwForm');const fwFile=document.getElementById('fwFile');";
  html += "const fwProgress=document.getElementById('fwProgress');const fwStatus=document.getElementById('fwStatus');const fwBtn=document.getElementById('fwBtn');";
  html += "if(fwForm){fwForm.addEventListener('submit',(e)=>{e.preventDefault();if(!fwFile||!fwFile.files||!fwFile.files.length){fwStatus.textContent='Select a firmware file first.';return;}";
  html += "const fd=new FormData();fd.append('firmware',fwFile.files[0]);const xhr=new XMLHttpRequest();xhr.open('POST','/firmware/update',true);";
  html += "fwBtn.disabled=true;fwProgress.value=0;fwStatus.textContent='Uploading...';";
  html += "xhr.upload.onprogress=(ev)=>{if(!ev.lengthComputable)return;const p=Math.round((ev.loaded/ev.total)*100);fwProgress.value=p;fwStatus.textContent='Uploading '+p+'%';};";
  html += "xhr.onerror=()=>{fwBtn.disabled=false;fwStatus.textContent='Upload failed.';};";
  html += "xhr.onload=()=>{fwBtn.disabled=false;if(xhr.status>=200&&xhr.status<300){fwProgress.value=100;fwStatus.textContent='Upload complete. Processing update...';document.open();document.write(xhr.responseText);document.close();}else{fwStatus.textContent='Update failed ('+xhr.status+').';}};";
  html += "xhr.send(fd);});}";
  html += "</script>";
  html += "</div>";
  html += pageFooter();
  web.send(200, "text/html", html);
}

void handleFirmwareUpdate() {
  if (!checkAuth()) { requestAuth(); return; }
  bool ok = !Update.hasError();
  String html = "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>Firmware Update</title>";
  html += "<style>body{font-family:Arial,sans-serif;padding:24px;max-width:720px;margin:auto;}</style></head><body><h3>";
  html += ok ? "Firmware updated. Rebooting..." : "Firmware update failed.";
  html += "</h3>";
  if (ok) {
    String mdnsUrl = "http://" + htmlEscape(config.hostname.length() ? config.hostname : kDefaultHostname) + ".local/";
    html += "<p>Trying to reconnect automatically.</p>";
    html += "<script>";
    html += "const targets=[];if(location&&location.origin)targets.push(location.origin+'/');";
    html += "targets.push('" + mdnsUrl + "');targets.push('http://192.168.4.1/');";
    html += "let i=0;const tryOpen=()=>{location.href=targets[i%targets.length];i++;};";
    html += "setTimeout(()=>{tryOpen();setInterval(tryOpen,3000);},2000);";
    html += "</script>";
  }
  html += "</body></html>";
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

void handleFilesystemUpdate() {
  if (!checkAuth()) { requestAuth(); return; }
  bool ok = !Update.hasError();
  String html = "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>Filesystem Update</title>";
  html += "<style>body{font-family:Arial,sans-serif;padding:24px;max-width:720px;margin:auto;}</style></head><body><h3>";
  html += ok ? "Filesystem updated. Rebooting..." : "Filesystem update failed.";
  html += "</h3></body></html>";
  web.send(ok ? 200 : 500, "text/html", html);
  if (ok) {
    delay(500);
    ESP.restart();
  }
}

void handleFilesystemUpload() {
  if (!checkAuth()) { requestAuth(); return; }
  HTTPUpload &up = web.upload();
  if (up.status == UPLOAD_FILE_START) {
    Serial.printf("fs-web: start %s\n", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS)) {
      Update.printError(Serial);
    }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) {
      Update.printError(Serial);
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("fs-web: success %u bytes\n", up.totalSize);
    } else {
      Update.printError(Serial);
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    Serial.println("fs-web: aborted");
  }
}

void handleFactoryReset() {
  if (!checkAuth()) { requestAuth(); return; }
  Serial.println("system: factory reset requested");
  clearPersistedData();

  String html = "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>Factory Reset</title>";
  html += "<style>body{font-family:Arial,sans-serif;padding:24px;max-width:720px;margin:auto;}a{word-break:break-all;}code{background:#f1f5f9;padding:2px 6px;border-radius:6px;}</style></head><body>";
  html += "<h2>Factory reset complete. Rebooting...</h2>";
  html += "<p>All saved settings and persisted HVAC state were erased.</p>";
  html += "<p>After reboot the device should start in setup AP mode at <a href='http://192.168.4.1/'>http://192.168.4.1/</a>.</p>";
  html += "<script>setTimeout(()=>{location.href='http://192.168.4.1/';},2500);</script></body></html>";
  web.send(200, "text/html", html);
  delay(500);
  ESP.restart();
}

void handleApiConfig() {
  if (!checkAuth()) { requestAuth(); return; }
  String json = configToJsonString();
  web.send(200, "application/json", json);
}

void handleApiStatus() {
  if (!checkAuth()) { requestAuth(); return; }
  JsonDocument doc;
  doc["network_mode"] = networkModeString();
  doc["ip"] = networkLocalIp().toString();
  doc["hostname"] = config.hostname.length() ? config.hostname : kDefaultHostname;
  doc["telnet_port"] = config.telnetPort;
  doc["dinplug_status"] = dinplugConnectionStatus();
  doc["emitter_count"] = config.emitterCount;
  doc["hvac_count"] = config.hvacCount;
  doc["ir_receiver_enabled"] = config.irReceiver.enabled;
  doc["ir_receiver_gpio"] = config.irReceiver.gpio;
  doc["telnet_clients_active"] = activeTelnetClientCount();
  JsonArray telnetClientsJson = doc["telnet_clients"].to<JsonArray>();
  for (uint8_t i = 0; i < kMaxTelnetClients; i++) {
    WiFiClient &c = telnetClients[i];
    if (!c || !c.connected()) continue;
    JsonObject tc = telnetClientsJson.add<JsonObject>();
    tc["slot"] = i;
    tc["ip"] = c.remoteIP().toString();
    tc["port"] = c.remotePort();
  }
  doc["temp_sensors_enabled"] = config.tempSensors.enabled;
  doc["temp_sensor_count"] = tempSensorCount;
  doc["temp_sensor_precision"] = tempSensorPrecision();
  JsonArray sensors = doc["temp_sensors"].to<JsonArray>();
  for (uint8_t i = 0; i < tempSensorCount; i++) {
    JsonObject s = sensors.add<JsonObject>();
    s["index"] = i;
    s["name"] = sensorNameForIndex(i);
    s["address"] = sensorAddressToString(tempSensorAddresses[i]);
    s["valid"] = tempSensorValid[i];
    if (tempSensorValid[i]) s["current_temp"] = tempSensorReadings[i];
  }
  doc["ethernet_enabled"] = config.eth.enabled;
  String out;
  serializeJson(doc, out);
  web.send(200, "application/json", out);
}

void handleApiMeta() {
  if (!checkAuth()) { requestAuth(); return; }
  JsonDocument doc;
  JsonArray protocols = doc["protocols"].to<JsonArray>();
  protocols.add("CUSTOM");
  for (uint16_t i = 0; i <= kLastDecodeType; i++) {
    decode_type_t proto = static_cast<decode_type_t>(i);
    if (!IRac::isProtocolSupported(proto)) continue;
    protocols.add(typeToString(proto));
  }

  JsonArray gpioOptions = doc["gpio_options"].to<JsonArray>();
  const uint8_t gpios[] = {2,4,5,12,13,14,15,16,17,18,19,21,22,23,25,26,27,32,33};
  for (uint8_t gpio : gpios) gpioOptions.add(gpio);

  JsonArray dinActions = doc["din_actions"].to<JsonArray>();
  const char *actions[] = {"none","temp_up","temp_down","set_temp","power_on","power_off","toggle_power","mode_heat","mode_cool","mode_fan","mode_auto","mode_off","light_on","light_off","toggle_light"};
  for (const char *action : actions) dinActions.add(action);

  JsonArray toggleModes = doc["toggle_modes"].to<JsonArray>();
  const char *modes[] = {"auto","cool","heat","dry","fan"};
  for (const char *mode : modes) toggleModes.add(mode);

  JsonArray modeOverrides = doc["mode_overrides"].to<JsonArray>();
  const char *modeOverrideList[] = {"keep","auto","cool","heat","dry","fan"};
  for (const char *mode : modeOverrideList) modeOverrides.add(mode);

  JsonArray lightModes = doc["light_modes"].to<JsonArray>();
  const char *lightModesList[] = {"keep","on","off","toggle"};
  for (const char *mode : lightModesList) lightModes.add(mode);

  doc["max_custom_commands"] = kMaxCustomCommands;
  doc["max_dinplug_buttons"] = kMaxDinplugButtons;
  doc["max_temp_sensors"] = kMaxTempSensors;
  doc["max_emitters"] = kMaxEmitters;
  doc["max_hvacs"] = kMaxHvacs;

  String out;
  serializeJson(doc, out);
  web.send(200, "application/json", out);
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
    cmds.add("push_all");
    cmds.add("raw");
    cmds.add("help");
    JsonArray examples = help["examples"].to<JsonArray>();
    examples.add("{\"cmd\":\"list\"}");
    examples.add("{\"cmd\":\"send\",\"id\":\"1\",\"power\":\"on\",\"mode\":\"cool\",\"temp\":24,\"fan\":\"auto\"}");
    examples.add("{\"cmd\":\"get\",\"id\":\"1\"}");
    examples.add("{\"cmd\":\"get_all\"}");
    examples.add("{\"cmd\":\"push_all\"}");
    examples.add("{\"cmd\":\"raw\",\"emitter\":0,\"encoding\":\"pronto\",\"code\":\"0000 006D 0000 ...\"}");
    examples.add("{\"cmd\":\"raw\",\"emitter\":0,\"encoding\":\"gc\",\"code\":\"sendir,1:1,1,38000,1,1,172,172,...\"}");
    examples.add("{\"cmd\":\"raw\",\"emitter\":0,\"encoding\":\"racepoint\",\"code\":\"0000000000009470...\"}");
    examples.add("{\"cmd\":\"raw\",\"emitter\":0,\"encoding\":\"rawhex\",\"code\":\"0156 00AC 0015 ...\"}");
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
      const bool isCustom = config.hvacs[i].isCustom || config.hvacs[i].protocol == "CUSTOM";
      o["id"] = config.hvacs[i].id;
      o["protocol"] = isCustom ? "CUSTOM" : config.hvacs[i].protocol;
      o["emitter"] = config.hvacs[i].emitterIndex;
      o["model"] = config.hvacs[i].model;
      o["custom"] = isCustom;
      if (isCustom) {
        JsonObject c = o["custom_data"].to<JsonObject>();
        c["encoding"] = config.hvacs[i].customEncoding;
        c["off"] = config.hvacs[i].customOff;
        JsonArray commands = c["commands"].to<JsonArray>();
        for (uint8_t cc = 0; cc < config.hvacs[i].customCommandCount; cc++) {
          JsonObject cmdObj = commands.add<JsonObject>();
          cmdObj["name"] = config.hvacs[i].customCommands[cc].name;
          cmdObj["encoding"] = normalizeCustomEncoding(config.hvacs[i].customCommands[cc].encoding);
          cmdObj["code"] = config.hvacs[i].customCommands[cc].code;
        }
      }
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

  if (cmd == "push_all") {
    if (sourceTelnetSlot < 0 || sourceTelnetSlot >= static_cast<int8_t>(kMaxTelnetClients) ||
        !telnetClients[sourceTelnetSlot] || !telnetClients[sourceTelnetSlot].connected()) {
      resp["ok"] = false;
      resp["error"] = "no_telnet_source";
      return false;
    }
    sendAllStatesToTelnetClient(telnetClients[sourceTelnetSlot]);
    resp["ok"] = true;
    resp["sent"] = config.hvacCount;
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
  if (hvac->isCustom || hvac->protocol == "CUSTOM") {
    String encoding = normalizeCustomEncoding(doc["encoding"] | hvac->customEncoding);
    bool power = IRac::strToBool((const char*)(doc["power"] | "on"));
    String command = doc["command"] | "";
    String commandName = doc["command_name"] | "";
    if (command == "off") power = false;
    String code = doc["code"] | "";
    if (!commandName.length() && command.startsWith("custom:")) commandName = command.substring(7);
    commandName.trim();

    if (commandName.length()) {
      const CustomCommandCode *customCmd = findCustomCommandByName(*hvac, commandName);
      if (!customCmd) { resp["ok"] = false; resp["error"] = "unknown_custom_command"; return false; }
      encoding = normalizeCustomEncoding(customCmd->encoding);
      code = customCmd->code;
      power = true;
    }

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
    if (hvacUsesSensorTemp(*hvac) && hvac->tempSensorIndex < tempSensorCount &&
        tempSensorValid[hvac->tempSensorIndex]) {
      nextState.currentTemp = tempSensorReadings[hvac->tempSensorIndex];
    } else {
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
    resp["protocol"] = "CUSTOM";
    resp["custom"] = true;
    resp["encoding"] = encoding;
    if (commandName.length()) resp["command_name"] = commandName;
    if (hvacStateChanged(previous, hvacStates[hvacIndex])) {
      markHvacStatesDirty();
      syncDinplugLedsForHvac(hvacIndex);
      logHvacStateChange(id, hvacStates[hvacIndex], sourceTelnetSlot);
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
  if (hvacUsesSensorTemp(*hvac) && hvac->tempSensorIndex < tempSensorCount &&
      tempSensorValid[hvac->tempSensorIndex]) {
    nextState.currentTemp = tempSensorReadings[hvac->tempSensorIndex];
  } else {
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
    markHvacStatesDirty();
    syncDinplugLedsForHvac(hvacIndex);
    logHvacStateChange(id, hvacStates[hvacIndex], sourceTelnetSlot);
    broadcastStateToTelnetClients(id, hvacStates[hvacIndex], sourceTelnetSlot);
  }
  return ok;
}

void handleHvacTest() {
  if (!checkAuth()) { requestAuth(); return; }
  String id = web.arg("id");
  HvacConfig *hvac = nullptr;
  bool isCustom = (findHvacById(id, hvac) && hvac && (hvac->isCustom || hvac->protocol == "CUSTOM"));
  JsonDocument cmd;
  cmd["cmd"] = "send";
  cmd["id"] = id;
  if (!isCustom) {
    if (web.arg("power").length()) cmd["power"] = web.arg("power");
    if (web.arg("mode").length()) cmd["mode"] = web.arg("mode");
    if (web.arg("temp").length()) cmd["temp"] = web.arg("temp").toFloat();
    if (web.arg("fan").length()) cmd["fan"] = web.arg("fan");
    if (web.arg("swingv").length()) cmd["swingv"] = web.arg("swingv");
    if (web.arg("swingh").length()) cmd["swingh"] = web.arg("swingh");
    if (web.arg("light").length()) cmd["light"] = web.arg("light");
  }
  if (web.arg("command_name").length()) cmd["command_name"] = web.arg("command_name");
  if (isCustom && !web.arg("command_name").length()) {
    JsonDocument err;
    err["ok"] = false;
    err["error"] = "missing_custom_command";
    String out;
    serializeJson(err, out);
    web.send(200, "application/json", out);
    return;
  }

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

void handleMonitorPage() {
  if (!checkAuth()) { requestAuth(); return; }
  String html = pageHeader("Telnet Monitor");
  html += "<div class='card'><h2>Telnet Monitor</h2>";
  html += "<p>Status: <strong id='monState'>-</strong></p>";
  html += "<div class='row'><button type='button' id='toggleBtn'>Toggle Monitor</button>";
  html += "<button type='button' id='copyLastIrBtn'>Copy Last IR Code</button>";
  html += "<button type='button' id='copyAllIrBtn'>Copy All IR Codes</button>";
  html += "<button type='button' class='secondary' id='clearBtn'>Clear Log</button></div>";
  html += "<p id='copyStatus'></p>";
  html += "<p>Showing latest " + String(kMonitorLogCapacity) + " lines (RX/TX, DINplug, ir-rx).</p>";
  html += "<pre id='logBox'>Loading...</pre></div>";
  html += "<script>";
  html += "const monState=document.getElementById('monState');";
  html += "const logBox=document.getElementById('logBox');";
  html += "const toggleBtn=document.getElementById('toggleBtn');";
  html += "const copyLastIrBtn=document.getElementById('copyLastIrBtn');";
  html += "const copyAllIrBtn=document.getElementById('copyAllIrBtn');";
  html += "const clearBtn=document.getElementById('clearBtn');";
  html += "const copyStatus=document.getElementById('copyStatus');";
  html += "let enabled=false;";
  html += "let lines=[];";
  html += "const setStatus=(msg)=>{copyStatus.textContent=msg;};";
  html += "const copyText=async(text)=>{";
  html += "if(!text||!text.length){setStatus('No IR code available to copy.');return false;}";
  html += "try{if(navigator.clipboard&&navigator.clipboard.writeText){await navigator.clipboard.writeText(text);setStatus('Copied to clipboard.');return true;}}catch(e){}";
  html += "const ta=document.createElement('textarea');ta.value=text;document.body.appendChild(ta);ta.select();";
  html += "let ok=false;try{ok=document.execCommand('copy');}catch(e){}document.body.removeChild(ta);";
  html += "setStatus(ok?'Copied to clipboard.':'Clipboard copy failed.');return ok;};";
  html += "const extractIrCodes=(arr)=>arr.filter(l=>l.includes('ir-rx ')).map(l=>l.substring(l.indexOf('ir-rx ')+6));";
  html += "const load=async()=>{";
  html += "try{const r=await fetch('/api/monitor');const d=await r.json();";
  html += "enabled=!!d.enabled;monState.textContent=enabled?'on':'off';";
  html += "lines=(d.lines||[]);";
  html += "logBox.textContent=lines.join('\\n');";
  html += "logBox.scrollTop=logBox.scrollHeight;";
  html += "}catch(e){logBox.textContent='Monitor API error';}};";
  html += "toggleBtn.addEventListener('click',async()=>{";
  html += "const body=new URLSearchParams({enabled:enabled?'0':'1'});";
  html += "await fetch('/monitor/toggle',{method:'POST',body});await load();});";
  html += "copyLastIrBtn.addEventListener('click',async()=>{";
  html += "const codes=extractIrCodes(lines);if(!codes.length){setStatus('No captured IR codes in current log window.');return;}";
  html += "await copyText(codes[codes.length-1]);});";
  html += "copyAllIrBtn.addEventListener('click',async()=>{";
  html += "const codes=extractIrCodes(lines);if(!codes.length){setStatus('No captured IR codes in current log window.');return;}";
  html += "await copyText(codes.join('\\n'));});";
  html += "clearBtn.addEventListener('click',async()=>{";
  html += "await fetch('/monitor/clear',{method:'POST'});setStatus('');await load();});";
  html += "load();setInterval(load,1000);";
  html += "</script>";
  html += pageFooter();
  web.send(200, "text/html", html);
}

void handleApiMonitor() {
  if (!checkAuth()) { requestAuth(); return; }
  JsonDocument doc;
  doc["enabled"] = telnetMonitorEnabled;
  JsonObject filters = doc["filters"].to<JsonObject>();
  filters["telnet"] = monitorLogTelnetEnabled;
  filters["state"] = monitorLogStateEnabled;
  filters["dinplug"] = monitorLogDinplugEnabled;
  filters["ir"] = monitorLogIrEnabled;
  JsonArray lines = doc["lines"].to<JsonArray>();
  for (uint16_t i = 0; i < telnetMonitorLogCount; i++) {
    uint16_t idx = (telnetMonitorLogStart + i) % kMonitorLogCapacity;
    lines.add(telnetMonitorLog[idx]);
  }
  String out;
  serializeJson(doc, out);
  web.send(200, "application/json", out);
}

void handleMonitorClear() {
  if (!checkAuth()) { requestAuth(); return; }
  clearMonitorLog();
  JsonDocument doc;
  doc["ok"] = true;
  String out;
  serializeJson(doc, out);
  web.send(200, "application/json", out);
}

void handleMonitorToggle() {
  if (!checkAuth()) { requestAuth(); return; }
  String enabled = web.arg("enabled");
  enabled.toLowerCase();
  telnetMonitorEnabled = (enabled == "1" || enabled == "true" || enabled == "on");
  if (web.hasArg("telnet")) {
    String value = web.arg("telnet");
    value.toLowerCase();
    monitorLogTelnetEnabled = (value == "1" || value == "true" || value == "on");
  }
  if (web.hasArg("state")) {
    String value = web.arg("state");
    value.toLowerCase();
    monitorLogStateEnabled = (value == "1" || value == "true" || value == "on");
  }
  if (web.hasArg("dinplug")) {
    String value = web.arg("dinplug");
    value.toLowerCase();
    monitorLogDinplugEnabled = (value == "1" || value == "true" || value == "on");
  }
  if (web.hasArg("ir")) {
    String value = web.arg("ir");
    value.toLowerCase();
    monitorLogIrEnabled = (value == "1" || value == "true" || value == "on");
  }
  printMonitorStatus();
  JsonDocument doc;
  doc["ok"] = true;
  doc["enabled"] = telnetMonitorEnabled;
  JsonObject filters = doc["filters"].to<JsonObject>();
  filters["telnet"] = monitorLogTelnetEnabled;
  filters["state"] = monitorLogStateEnabled;
  filters["dinplug"] = monitorLogDinplugEnabled;
  filters["ir"] = monitorLogIrEnabled;
  String out;
  serializeJson(doc, out);
  web.send(200, "application/json", out);
}

void handleIrLearnStart() {
  if (!checkAuth()) { requestAuth(); return; }
  JsonDocument doc;
  if (!irReceiver) {
    doc["ok"] = false;
    doc["error"] = "ir_receiver_disabled";
  } else {
    irLearnEncoding = normalizeCustomEncoding(web.arg("encoding"));
    irLearnCode = "";
    irLearnError = "";
    irLearnActive = true;
    irLearnStartMs = millis();
    doc["ok"] = true;
    doc["active"] = true;
    doc["encoding"] = irLearnEncoding;
    if (telnetMonitorEnabled && monitorLogIrEnabled) addMonitorLogEntry("ir-learn start encoding=" + irLearnEncoding);
  }
  String out;
  serializeJson(doc, out);
  web.send(200, "application/json", out);
}

void handleIrLearnPoll() {
  if (!checkAuth()) { requestAuth(); return; }
  if (irLearnActive && (millis() - irLearnStartMs) > 10000UL) {
    irLearnActive = false;
    irLearnError = "timeout";
  }
  JsonDocument doc;
  doc["ok"] = true;
  doc["active"] = irLearnActive;
  doc["ready"] = (!irLearnActive && irLearnCode.length() > 0);
  doc["encoding"] = normalizeCustomEncoding(irLearnEncoding);
  doc["elapsed_ms"] = millis() - irLearnStartMs;
  if (!irLearnActive && irLearnError.length()) doc["error"] = irLearnError;
  if (!irLearnActive && irLearnCode.length() > 0) {
    doc["code"] = irLearnCode;
  }
  String out;
  serializeJson(doc, out);
  web.send(200, "application/json", out);
}

void handleIrLearnCancel() {
  if (!checkAuth()) { requestAuth(); return; }
  if (irLearnActive) {
    irLearnActive = false;
    irLearnError = "cancelled";
  }
  JsonDocument doc;
  doc["ok"] = true;
  doc["active"] = false;
  doc["error"] = irLearnError;
  String out;
  serializeJson(doc, out);
  web.send(200, "application/json", out);
}

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
  web.on("/hvacs", HTTP_GET, []() {
    if (!checkAuth()) { requestAuth(); return; }
    if (!streamSpiffsFile("/hvacs.html", "text/html; charset=utf-8")) handleHvacsPage();
  });
  web.on("/hvacs/add", HTTP_POST, handleHvacsAdd);
  web.on("/hvacs/clone", HTTP_POST, handleHvacsClone);
  web.on("/hvacs/test", HTTP_GET, []() {
    if (!checkAuth()) { requestAuth(); return; }
    if (!streamSpiffsFile("/hvacs_test.html", "text/html; charset=utf-8")) handleHvacTestPage();
  });
  web.on("/hvacs/test", HTTP_POST, handleHvacTest);
  web.on("/hvacs/update", HTTP_POST, handleHvacsUpdate);
  web.on("/hvacs/delete", HTTP_GET, handleHvacsDelete);
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

  // Captive portal & connectivity check endpoints.
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
  web.on("/api/status", HTTP_GET, handleApiStatus);
  web.on("/api/meta", HTTP_GET, handleApiMeta);
  web.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
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

// ---- DINplug handling ----

bool sendDinplugCommand(const String &cmd) {
  if (!dinplugClient.connected()) return false;
  size_t written = dinplugClient.print(cmd + "\r\n");
  if (written == 0) {
    dinplugClient.stop();
    dinplugConnected = false;
    if (telnetMonitorEnabled && monitorLogDinplugEnabled) addMonitorLogEntry("din: tx failed, reconnecting");
    return false;
  }
  if (telnetMonitorEnabled && monitorLogDinplugEnabled) addMonitorLogEntry("din-tx " + cmd);
  return true;
}

bool setDinplugLed(uint16_t keypadId, uint16_t ledId, uint8_t state) {
  if (keypadId == 0 || ledId == 0) return false;
  if (state > 3) return false;
  return sendDinplugCommand("LED " + String(keypadId) + " " + String(ledId) + " " + String(state));
}

void syncDinplugLedsForHvac(uint8_t hvacIndex) {
  if (hvacIndex >= config.hvacCount) return;
  const HvacConfig &h = config.hvacs[hvacIndex];
  const HvacRuntimeState &state = hvacStates[hvacIndex];
  for (uint8_t i = 0; i < h.dinButtonCount; i++) {
    const DinplugButtonBinding &b = h.dinButtons[i];
    if (b.ledFollowMode == 0 || b.buttonId == 0) continue;
    const uint8_t ledState = (b.ledFollowMode == 1) ? (state.power ? 1 : 0) : (state.power ? 0 : 1);
    if (b.keypadId != 0) {
      setDinplugLed(b.keypadId, b.buttonId, ledState);
      continue;
    }
    for (uint8_t k = 0; k < h.dinKeypadCount; k++) {
      if (h.dinKeypadIds[k] == 0) continue;
      setDinplugLed(h.dinKeypadIds[k], b.buttonId, ledState);
    }
  }
}

void ensureDinplugConnected(bool forceNow) {
  String gatewayHost = config.dinplug.gatewayHost;
  gatewayHost.trim();
  if (gatewayHost.length() == 0) return;
  if (dinplugClient.connected()) {
    dinplugConnected = true;
    return;
  }
  unsigned long now = millis();
  if (!forceNow && (now - dinplugLastAttemptMs) < kDinplugReconnectIntervalMs) return;
  dinplugLastAttemptMs = now;
  dinplugClient.stop();
  dinplugConnected = false;
  if (!networkReady()) return;
  Serial.print("dinplug: connecting to ");
  Serial.println(gatewayHost);
  if (dinplugClient.connect(gatewayHost.c_str(), kDinplugPort)) {
    dinplugConnected = true;
    dinplugBuffer = "";
    dinplugLastKeepAliveMs = millis();
    dinplugLastRxMs = millis();
    Serial.println("dinplug: connected");
    if (telnetMonitorEnabled && monitorLogDinplugEnabled) addMonitorLogEntry("din: connected");
    sendDinplugCommand("REFRESH");
  } else {
    Serial.println("dinplug: connect failed");
  }
}

void handleDinplugButtonEvent(uint16_t keypadId, uint16_t buttonId, const String &action) {
  bool isHold = (action == "HOLD");
  for (uint8_t i = 0; i < config.hvacCount; i++) {
    const HvacConfig &h = config.hvacs[i];
    if (!hvacHasDinKeypad(h, keypadId)) continue;
    for (uint8_t b = 0; b < h.dinButtonCount; b++) {
      const DinplugButtonBinding &bind = h.dinButtons[b];
      if (bind.buttonId != buttonId) continue;
      if (bind.keypadId != 0 && bind.keypadId != keypadId) continue;
      if (applyDinplugAction(i, bind, isHold)) {
        Serial.print("dinplug: applied action to hvac ");
        Serial.println(h.id);
      }
    }
  }
}

void processDinplugLine(const String &line) {
  String trimmed = line;
  trimmed.trim();
  dinplugLastRxMs = millis();
  if (telnetMonitorEnabled && monitorLogDinplugEnabled) addMonitorLogEntry("din-rx " + trimmed);
  if (!trimmed.startsWith("R:BTN ")) return;
  int firstSpace = trimmed.indexOf(' ');
  int secondSpace = trimmed.indexOf(' ', firstSpace + 1);
  int thirdSpace = trimmed.indexOf(' ', secondSpace + 1);
  if (firstSpace < 0 || secondSpace < 0 || thirdSpace < 0) return;
  String action = trimmed.substring(firstSpace + 1, secondSpace);
  action.toUpperCase();
  uint16_t keypadId = static_cast<uint16_t>(trimmed.substring(secondSpace + 1, thirdSpace).toInt());
  uint16_t buttonId = static_cast<uint16_t>(trimmed.substring(thirdSpace + 1).toInt());
  if (action == "PRESS" || action == "HOLD") {
    handleDinplugButtonEvent(keypadId, buttonId, action);
  }
}

void handleDinplug() {
  if (config.dinplug.gatewayHost.length() == 0) return;
  if (config.dinplug.autoConnect) ensureDinplugConnected(false);
  if (!dinplugClient.connected()) return;
  unsigned long now = millis();
  if ((now - dinplugLastKeepAliveMs) > kDinplugKeepAliveIntervalMs) {
    if (!sendDinplugCommand("STA")) {
      ensureDinplugConnected(true);
      return;
    }
    dinplugLastKeepAliveMs = now;
  }
  if (dinplugLastRxMs > 0 && (now - dinplugLastRxMs) > kDinplugRxTimeoutMs) {
    if (telnetMonitorEnabled && monitorLogDinplugEnabled) addMonitorLogEntry("din: rx timeout, reconnecting");
    dinplugClient.stop();
    dinplugConnected = false;
    ensureDinplugConnected(true);
    return;
  }
  while (dinplugClient.available()) {
    char ch = static_cast<char>(dinplugClient.read());
    dinplugLastRxMs = millis();
    if (ch == '\r') continue;
    if (ch == '\n') {
      if (dinplugBuffer.length()) processDinplugLine(dinplugBuffer);
      dinplugBuffer = "";
    } else {
      dinplugBuffer += ch;
      if (dinplugBuffer.length() > 512) dinplugBuffer = "";
    }
  }
}

// ---- Telnet handling ----

void respondTelnetError(WiFiClient &client, const String &message) {
  JsonDocument doc;
  doc["ok"] = false;
  doc["error"] = message;
  sendTelnetJson(client, doc);
}

bool sendCustomCode(const HvacConfig &hvac, EmitterRuntime *em, const String &code, const String &encoding) {
  if (!em || !em->raw) return false;
  String enc = normalizeCustomEncoding(encoding);
  if (enc == "pronto") {
    return parseStringAndSendPronto(em->raw, code, 0);
  }
  if (enc == "gc") {
    return parseStringAndSendGC(em->raw, code);
  }
  if (enc == "racepoint") {
    return parseStringAndSendRacepoint(em->raw, code);
  }
  if (enc == "rawhex") {
    return parseStringAndSendRawHex(em->raw, code);
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
  if (telnetMonitorEnabled && monitorLogTelnetEnabled) {
    String rxMsg = "RX slot=" + String(sourceTelnetSlot) + " from " +
                   client.remoteIP().toString() + ":" + String(client.remotePort()) +
                   " line=" + truncateForLog(line, 240);
    addMonitorLogEntry(rxMsg);
    Serial.println(truncateForLog("telnet-" + rxMsg, 280));
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) {
    if (telnetMonitorEnabled && monitorLogTelnetEnabled) {
      addMonitorLogEntry("RX parse=invalid_json");
      Serial.println("telnet-rx parse=invalid_json");
    }
    respondTelnetError(client, "invalid_json");
    return;
  }
  JsonDocument resp;
  String cmd = doc["cmd"] | "send";
  processCommand(doc, resp, sourceTelnetSlot);
  if (!sendTelnetJson(client, resp)) {
    Serial.print("telnet: failed to reply slot ");
    Serial.println(sourceTelnetSlot);
    return;
  }
  if (telnetMonitorEnabled && monitorLogTelnetEnabled) {
    String respStr;
    serializeJson(resp, respStr);
    String txMsg = "TX slot=" + String(sourceTelnetSlot) + " cmd=" + cmd + " line=" +
                   truncateForLog(respStr, 240);
    addMonitorLogEntry(txMsg);
    Serial.println(truncateForLog("telnet-" + txMsg, 280));
  } else {
    Serial.print("telnet: ");
    Serial.println(cmd);
  }
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
        if (telnetBuffers[i].length() > kMaxTelnetLineLength) {
          telnetBuffers[i] = "";
          respondTelnetError(c, "line_too_long");
          break;
        }
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

bool startEthernet() {
  ethernetUp = false;
  if (!config.eth.enabled) return false;
  const char *host = (config.hostname.length() ? config.hostname.c_str() : kDefaultHostname);
  ETH.setHostname(host);
  Serial.println("eth: starting LAN8720 (WT32 defaults)");
  bool started = ETH.begin(kEthPhyAddr, kEthPowerPin, kEthMdcPin, kEthMdioPin,
                           ETH_PHY_LAN8720, ETH_CLOCK_GPIO0_IN);
  if (!started) {
    Serial.println("eth: begin failed");
    return false;
  }

  if (!config.wifi.dhcp) {
    const bool staticComplete = config.wifi.ip != IPAddress() &&
                                config.wifi.gateway != IPAddress() &&
                                config.wifi.subnet != IPAddress();
    if (staticComplete) {
      bool ok = ETH.config(config.wifi.ip, config.wifi.gateway, config.wifi.subnet, config.wifi.dns);
      Serial.println(ok ? "eth: static IP configured" : "eth: static IP config failed");
    } else {
      Serial.println("eth: static IP requested but incomplete values; falling back to DHCP");
      ETH.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
    }
  } else {
    ETH.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
  }

  unsigned long start = millis();
  while (millis() - start < 12000) {
    if (ETH.linkUp() && ETH.localIP() != IPAddress()) {
      ethernetUp = true;
      break;
    }
    delay(100);
  }
  if (!ethernetUp) {
    Serial.println("eth: no link/IP");
    return false;
  }
  Serial.print("eth: connected IP=");
  Serial.println(ETH.localIP());
  return true;
}

bool networkReady() {
  if (ethernetUp && ETH.linkUp() && ETH.localIP() != IPAddress()) return true;
  if (WiFi.status() == WL_CONNECTED) return true;
  return false;
}

IPAddress networkLocalIp() {
  if (ethernetUp && ETH.linkUp() && ETH.localIP() != IPAddress()) return ETH.localIP();
  if (WiFi.status() == WL_CONNECTED) return WiFi.localIP();
  if (WiFi.getMode() == WIFI_MODE_AP || WiFi.getMode() == WIFI_MODE_APSTA) return WiFi.softAPIP();
  return IPAddress();
}

String networkModeString() {
  if (ethernetUp && ETH.linkUp() && ETH.localIP() != IPAddress()) return "ETH";
  if (WiFi.status() == WL_CONNECTED) return "WiFi STA";
  if (WiFi.getMode() == WIFI_MODE_AP || WiFi.getMode() == WIFI_MODE_APSTA) return "WiFi AP";
  return "offline";
}

void startWifi() {
  if (startEthernet()) {
    dnsServer.stop();
    startMdns();
    return;
  }

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

void setupTemperatureSensors() {
  tempSensorCount = 0;
  tempLastReadMs = 0;
  for (uint8_t i = 0; i < kMaxTempSensors; i++) {
    tempSensorReadings[i] = 0;
    tempSensorValid[i] = false;
  }
  if (tempBus) {
    delete tempBus;
    tempBus = nullptr;
  }
  if (tempOneWire) {
    delete tempOneWire;
    tempOneWire = nullptr;
  }
  if (!config.tempSensors.enabled) {
    Serial.println("temp: DS18B20 disabled");
    return;
  }

  tempOneWire = new OneWire(config.tempSensors.gpio);
  tempBus = new DallasTemperature(tempOneWire);
  tempBus->begin();
  uint8_t found = static_cast<uint8_t>(tempBus->getDeviceCount());
  for (uint8_t i = 0; i < found && tempSensorCount < kMaxTempSensors; i++) {
    if (!tempBus->getAddress(tempSensorAddresses[tempSensorCount], i)) continue;
    tempBus->setResolution(tempSensorAddresses[tempSensorCount], 12);
    tempSensorCount++;
  }
  Serial.print("temp: DS18B20 bus on GPIO ");
  Serial.print(config.tempSensors.gpio);
  Serial.print(" sensors=");
  Serial.println(tempSensorCount);
  readTemperatureSensors();
  for (uint8_t i = 0; i < config.hvacCount; i++) {
    const HvacConfig &h = config.hvacs[i];
    if (!hvacUsesSensorTemp(h)) continue;
    if (h.tempSensorIndex >= tempSensorCount) continue;
    if (!tempSensorValid[h.tempSensorIndex]) continue;
    ensureHvacStateInitialized(i);
    hvacStates[i].currentTemp = tempSensorReadings[h.tempSensorIndex];
  }
}

void readTemperatureSensors() {
  if (!config.tempSensors.enabled || !tempBus) return;
  tempBus->requestTemperatures();
  for (uint8_t i = 0; i < tempSensorCount; i++) {
    float t = tempBus->getTempC(tempSensorAddresses[i]);
    bool valid = (t != DEVICE_DISCONNECTED_C) && (t > -100.0f) && (t < 150.0f);
    tempSensorValid[i] = valid;
    if (valid) tempSensorReadings[i] = applyTempSensorPrecision(t);
  }
}

void handleTemperatureSensors() {
  if (!config.tempSensors.enabled || !tempBus) return;
  unsigned long intervalMs = static_cast<unsigned long>(config.tempSensors.readIntervalSec) * 1000UL;
  if (intervalMs < 1000UL) intervalMs = 1000UL;
  unsigned long now = millis();
  if ((now - tempLastReadMs) < intervalMs) return;
  tempLastReadMs = now;
  readTemperatureSensors();
  for (uint8_t i = 0; i < config.hvacCount; i++) {
    const HvacConfig &h = config.hvacs[i];
    if (!hvacUsesSensorTemp(h)) continue;
    if (h.tempSensorIndex >= tempSensorCount) continue;
    if (!tempSensorValid[h.tempSensorIndex]) continue;
    ensureHvacStateInitialized(i);
    HvacRuntimeState previous = hvacStates[i];
    hvacStates[i].currentTemp = tempSensorReadings[h.tempSensorIndex];
    if (hvacStateChanged(previous, hvacStates[i])) {
      logHvacStateChange(h.id, hvacStates[i], -1);
      broadcastStateToTelnetClients(h.id, hvacStates[i], -1);
    }
  }
}

void setupIrReceiver() {
  if (irReceiver) {
    irReceiver->disableIRIn();
    delete irReceiver;
    irReceiver = nullptr;
  }
  config.irReceiver.mode = normalizeIrReceiverMode(config.irReceiver.mode);
  if (!config.irReceiver.enabled) {
    Serial.println("ir-rx: disabled");
    return;
  }
  irReceiver = new IRrecv(config.irReceiver.gpio, kIrRecvCaptureBufferSize, kIrRecvTimeoutMs, true);
  irReceiver->enableIRIn();
  Serial.print("ir-rx: enabled gpio=");
  Serial.print(config.irReceiver.gpio);
  Serial.print(" mode=");
  Serial.println(config.irReceiver.mode);
}

void handleIrReceiver() {
  if (!irReceiver) return;
  if (!irReceiver->decode(&irReceiverCapture)) return;

  String mode = normalizeIrReceiverMode(config.irReceiver.mode);
  String line;
  String modeCode = "";
  if (mode == "pronto") {
    modeCode = buildProntoFromCapture(irReceiverCapture, kProntoDefaultFrequency);
    line = "ir-rx pronto gpio=" + String(config.irReceiver.gpio) + " freq=" +
           String(kProntoDefaultFrequency) + " code=" + modeCode;
  } else if (mode == "rawhex") {
    modeCode = buildRawHexFromCapture(irReceiverCapture);
    line = "ir-rx rawhex gpio=" + String(config.irReceiver.gpio) + " code=" + modeCode;
  } else {
    String basic = resultToHumanReadableBasic(&irReceiverCapture);
    basic.replace('\n', ' ');
    line = "ir-rx auto gpio=" + String(config.irReceiver.gpio) + " " + basic;
  }

  if (irLearnActive) {
    String learned = buildCodeFromCaptureEncoding(irReceiverCapture, irLearnEncoding);
    String targetEnc = normalizeCustomEncoding(irLearnEncoding);
    if (!learned.length() && targetEnc == "pronto" && mode == "pronto") learned = modeCode;
    if (!learned.length() && targetEnc == "rawhex" && mode == "rawhex") learned = modeCode;
    if (learned.length()) {
      irLearnCode = learned;
      irLearnActive = false;
      irLearnError = "";
      if (telnetMonitorEnabled && monitorLogIrEnabled) addMonitorLogEntry("ir-learn captured encoding=" + targetEnc);
    }
  }

  if (irReceiverCapture.overflow) line += " overflow=1";
  line = truncateForLog(line);
  Serial.println(line);
  if (telnetMonitorEnabled && monitorLogIrEnabled) addMonitorLogEntry(line);

  irReceiver->resume();
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
  loadPersistedHvacStates();
  setupTemperatureSensors();
  setupIrReceiver();
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
  if (config.dinplug.autoConnect && config.dinplug.gatewayHost.length() > 0) {
    ensureDinplugConnected(true);
  }
  printMonitorStatus();
  Serial.println("monitor: use 'monitor on|off|status' via serial terminal");
}

void loop() {
  handleSerialConsole();
  web.handleClient();
  handleTelnet();
  handleTelnetPeriodicStateBroadcast();
  handleDinplug();
  handleTemperatureSensors();
  handleHvacStatePersistence();
  handleIrReceiver();
  dnsServer.processNextRequest();
  ArduinoOTA.handle();
}
