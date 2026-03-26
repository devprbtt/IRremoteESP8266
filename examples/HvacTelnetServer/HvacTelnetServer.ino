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
#include <esp_system.h>
#include <time.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#include <IRremoteESP8266.h>
#include <IRac.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>
#include "DiagnosticsModule.h"
#include "NetworkModule.h"

// ---- User-tunable limits ----
static const uint8_t kMaxTelnetClients = 4;
static const uint8_t kMaxEmitters = 8;
static const uint8_t kMaxHvacs = 32;
static const uint8_t kMaxCustomTemps = 16;
static const uint8_t kMaxCustomCommands = 16;
static const uint16_t kDefaultTelnetPort = 4998;
static const uint16_t kMonitorLogCapacity = 40;
static const uint16_t kDinplugPort = 23;
static const unsigned long kDinplugReconnectIntervalMs = 5000;
static const unsigned long kDinplugKeepAliveIntervalMs = 10000;
static const unsigned long kDinplugRxTimeoutMs = 25000;
static const unsigned long kTelnetStateBroadcastIntervalMs = 30000;
static const unsigned long kHvacStatePersistDebounceMs = 1500;
static const uint8_t kMaxDinplugBindingsTotal = 24;
static const uint8_t kMaxDinplugKeypads = 8;
static const uint8_t kMaxTempSensors = 8;
static const uint16_t kMaxTelnetLineLength = 1024;
static const uint16_t kIrRecvCaptureBufferSize = 1024;
static const uint8_t kIrRecvTimeoutMs = 50;
static const uint32_t kProntoDefaultFrequency = 38000;
static const uint8_t kDiagnosticsLogLines = 20;
static const unsigned long kDiagnosticsPersistDebounceMs = 10000UL;
static const uint8_t kTrendHistoryCapacity = 24;
static const unsigned long kTrendSampleIntervalMs = 60000UL;
static const char *kFirmwareVersion = "0.2.0";
static const char *kFilesystemVersionExpected = "0.2.0";

static const char *kConfigPath = "/config.json";
static const char *kHvacStatePath = "/hvac_state.json";
static const char *kDiagnosticsPath = "/diagnostics.json";
static const char *kVersionPath = "/version.json";
static const char *kApSsid = "IR-Server-Setup";
static const char *kDefaultHostname = "ir-server";
static const char *kConfigPrefsNamespace = "hvaccfg";
static const char *kConfigPrefsKey = "json";
static const char *kDiagPrefsNamespace = "diag";
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
  String profileName;
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
  uint8_t dinButtonStart = 0;
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
  String timezone;
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

#include "CommandModule.h"
#include "ConfigModule.h"
#include "DinplugModule.h"
#include "IrReceiverModule.h"
#include "TemperatureModule.h"
#include "WebUiModule.h"
#include "TelnetModule.h"

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
bool dinplugWasConnected = false;
unsigned long dinplugLastAttemptMs = 0;
unsigned long dinplugLastKeepAliveMs = 0;
unsigned long dinplugLastRxMs = 0;
DNSServer dnsServer;
bool dnsServerActive = false;
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
DinplugButtonBinding dinplugBindingPool[kMaxDinplugBindingsTotal];
uint8_t dinplugBindingCount = 0;
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
bool ethernetStarted = false;
bool ethernetBeginLastOk = false;
unsigned long ethernetLastBeginMs = 0;
unsigned long ethernetLastLinkUpMs = 0;
bool wifiFallbackPending = false;
bool irLearnActive = false;
String irLearnEncoding = "pronto";
String irLearnCode = "";
String irLearnError = "";
unsigned long irLearnStartMs = 0;
bool clockConfigured = false;
String filesystemVersion = "unknown";
uint32_t bootCount = 0;
String resetReason = "unknown";
bool hvacStatesDirty = false;
unsigned long hvacStatesDirtySinceMs = 0;
unsigned long telnetLastStateBroadcastMs = 0;
bool diagnosticsDirty = false;
unsigned long diagnosticsDirtySinceMs = 0;
TrendSample trendHistory[kTrendHistoryCapacity];
uint8_t trendHistoryStart = 0;
uint8_t trendHistoryCount = 0;
unsigned long lastTrendSampleMs = 0;

// WT32-ETH01 defaults (LAN8720 PHY)
static const uint8_t kEthPhyAddr = 1;
static const int8_t kEthPowerPin = 16;
static const uint8_t kEthMdcPin = 23;
static const uint8_t kEthMdioPin = 18;

void initHvacRuntimeStates();
bool processCommand(JsonDocument &doc, JsonDocument &resp, int8_t sourceTelnetSlot);
bool sendCustomCode(const HvacConfig &hvac, EmitterRuntime *em, const String &code, const String &encoding);
String findCustomTempCode(const HvacConfig &hvac, int tempC);
bool sendTelnetJson(WiFiClient &client, JsonDocument &doc);
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
void handleApiDeviceGet();
void handleApiDeviceGetAll();
void handleApiDeviceSend();
void handleApiDeviceRaw();
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
void setupNetworkEvents();
void handleNetworkEvent(WiFiEvent_t event);
const DinplugButtonBinding *getDinplugBinding(const HvacConfig &h, uint8_t idx);
DinplugButtonBinding *getDinplugBinding(HvacConfig &h, uint8_t idx);
void clearDinplugBindingPool();
bool setHvacDinplugBindings(uint8_t hvacIndex, const DinplugButtonBinding *bindings, uint8_t count);
void compactDinplugBindingPool();
bool networkReady();
String networkModeString();
IPAddress networkLocalIp();
IPAddress currentGatewayIp();
IPAddress currentSubnetMask();
IPAddress currentDnsIp();
void loadFilesystemVersion();
String resetReasonToString(esp_reset_reason_t reason);
void loadBootDiagnostics();
String normalizeTimezoneOffset(const String &offsetIn);
void configureClock();
bool clockHasValidTime();
String localTimeString();
void handleNetworkRecovery();
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

bool checkAuth();
void requestAuth();

#include "DiagnosticsModule.ipp"

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

void loadFilesystemVersion() {
  filesystemVersion = "unknown";
  if (!SPIFFS.exists(kVersionPath)) {
    Serial.println("fs: version file missing");
    return;
  }
  File f = SPIFFS.open(kVersionPath, FILE_READ);
  if (!f) {
    Serial.println("fs: version file open failed");
    return;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.print("fs: version parse failed: ");
    Serial.println(err.c_str());
    return;
  }
  filesystemVersion = String(doc["filesystem_version"] | "unknown");
  filesystemVersion.trim();
  if (!filesystemVersion.length()) filesystemVersion = "unknown";
  Serial.print("fs: version ");
  Serial.println(filesystemVersion);
}

String resetReasonToString(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_UNKNOWN: return "unknown";
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_EXT: return "external_reset";
    case ESP_RST_SW: return "software_reset";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt_watchdog";
    case ESP_RST_TASK_WDT: return "task_watchdog";
    case ESP_RST_WDT: return "other_watchdog";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    default: return "other";
  }
}

void loadBootDiagnostics() {
  Preferences diagPrefs;
  if (!diagPrefs.begin(kDiagPrefsNamespace, false)) {
    resetReason = resetReasonToString(esp_reset_reason());
    Serial.print("boot: reset_reason=");
    Serial.println(resetReason);
    markDiagnosticsDirty();
    return;
  }
  bootCount = diagPrefs.getUInt("boot_count", 0) + 1;
  resetReason = resetReasonToString(esp_reset_reason());
  diagPrefs.putUInt("boot_count", bootCount);
  diagPrefs.putString("reset_reason", resetReason);
  diagPrefs.end();
  Serial.print("boot: count=");
  Serial.print(bootCount);
  Serial.print(" reset_reason=");
  Serial.println(resetReason);
  markDiagnosticsDirty();
}

String normalizeTimezoneOffset(const String &offsetIn) {
  String offset = offsetIn;
  offset.trim();
  if (!offset.length()) return "+00:00";
  if (offset == "Z" || offset == "UTC" || offset == "utc") return "+00:00";

  char sign = '+';
  uint16_t start = 0;
  if (offset[0] == '+' || offset[0] == '-') {
    sign = offset[0];
    start = 1;
  }

  String rest = offset.substring(start);
  rest.trim();
  int colon = rest.indexOf(':');
  int hours = 0;
  int minutes = 0;
  if (colon >= 0) {
    hours = rest.substring(0, colon).toInt();
    minutes = rest.substring(colon + 1).toInt();
  } else if (rest.length() > 2) {
    hours = rest.substring(0, rest.length() - 2).toInt();
    minutes = rest.substring(rest.length() - 2).toInt();
  } else {
    hours = rest.toInt();
  }

  if (hours > 14) hours = 14;
  if (minutes < 0) minutes = 0;
  if (minutes > 59) minutes = 59;

  char out[8];
  snprintf(out, sizeof(out), "%c%02d:%02d", sign, hours, minutes);
  return String(out);
}

bool clockHasValidTime() {
  time_t now;
  time(&now);
  return now > 1700000000;
}

String localTimeString() {
  if (!clockHasValidTime()) return "";
  time_t now;
  time(&now);
  struct tm timeinfo;
  if (!localtime_r(&now, &timeinfo)) return "";
  char out[24];
  strftime(out, sizeof(out), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(out);
}

void configureClock() {
  config.timezone = normalizeTimezoneOffset(config.timezone);

  int sign = (config.timezone[0] == '-') ? -1 : 1;
  int hours = config.timezone.substring(1, 3).toInt();
  int minutes = config.timezone.substring(4, 6).toInt();
  char posix[16];
  if (sign >= 0) {
    snprintf(posix, sizeof(posix), "UTC-%d:%02d", hours, minutes);
  } else {
    snprintf(posix, sizeof(posix), "UTC%d:%02d", hours, minutes);
  }
  configTzTime(posix, "pool.ntp.org", "time.nist.gov", "time.google.com");
  clockConfigured = true;
  Serial.print("clock: timezone ");
  Serial.print(config.timezone);
  Serial.print(" posix=");
  Serial.println(posix);
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

#include "TelnetModule.ipp"

#include "DinplugModule.ipp"

#include "ConfigModule.ipp"

#include "CommandModule.ipp"



#include "NetworkModule.ipp"

#include "IrReceiverModule.ipp"

#include "TemperatureModule.ipp"

#include "WebUiModule.ipp"

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("IR Server Telnet boot");

  if (!SPIFFS.begin(true)) {
    Serial.println("fs: SPIFFS mount failed");
  } else {
    Serial.println("fs: SPIFFS mounted");
    loadFilesystemVersion();
  }
  loadBootDiagnostics();
  loadConfig();
  loadPersistedHvacStates();
  setupTemperatureSensors();
  setupIrReceiver();
  rebuildEmitters();
  startWifi();
  configureClock();
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
  sampleRuntimeTrends(true);
  savePersistedDiagnostics();
  printMonitorStatus();
  Serial.println("monitor: use 'monitor on|off|status' via serial terminal");
}

void loop() {
  handleSerialConsole();
  web.handleClient();
  handleTelnet();
  handleTelnetPeriodicStateBroadcast();
  handleDinplug();
  handleNetworkRecovery();
  handleTemperatureSensors();
  sampleRuntimeTrends();
  handleHvacStatePersistence();
  handleDiagnosticsPersistence();
  handleIrReceiver();
  if (dnsServerActive) dnsServer.processNextRequest();
  ArduinoOTA.handle();
}
