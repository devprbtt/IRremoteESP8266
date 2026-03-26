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
  doc["timezone"] = normalizeTimezoneOffset(config.timezone);
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
    o["profile_name"] = h.profileName;
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
      const DinplugButtonBinding *binding = getDinplugBinding(h, b);
      if (!binding) continue;
      JsonObject bo = btns.add<JsonObject>();
      bo["keypad_id"] = binding->keypadId;
      bo["id"] = binding->buttonId;
      bo["press_action"] = binding->pressAction;
      bo["press_value"] = binding->pressValue;
      bo["press_mode_override"] = dinplugModeOverrideToString(binding->pressModeOverride);
      bo["press_light_mode"] = normalizeDinplugLightMode(binding->pressLightMode);
      bo["hold_action"] = binding->holdAction;
      bo["hold_value"] = binding->holdValue;
      bo["hold_mode_override"] = dinplugModeOverrideToString(binding->holdModeOverride);
      bo["hold_light_mode"] = normalizeDinplugLightMode(binding->holdLightMode);
      bo["toggle_power_mode"] = binding->togglePowerMode;
      bo["led_follow_mode"] = binding->ledFollowMode;
      bo["led_follow_power"] = (binding->ledFollowMode != 0);
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
  dinplugBindingCount = 0;
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
  config.timezone = "+00:00";
  config.telnetPort = kDefaultTelnetPort;
  for (uint8_t i = 0; i < kMaxEmitters; i++) {
    config.emitterGpios[i] = 0;
  }
  config.emitterCount = 0;
  for (uint8_t i = 0; i < kMaxHvacs; i++) {
    HvacConfig &h = config.hvacs[i];
    h.id = "";
    h.protocol = "";
    h.profileName = "";
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
    h.dinButtonStart = 0;
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
  config.timezone = normalizeTimezoneOffset(doc["timezone"] | "+00:00");

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
      h.profileName = o["profile_name"] | "";
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
          h.dinButtonStart = dinplugBindingCount;
          for (JsonObject bo : btns) {
            if (dinplugBindingCount >= kMaxDinplugBindingsTotal) break;
            DinplugButtonBinding &b = dinplugBindingPool[dinplugBindingCount++];
            h.dinButtonCount++;
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
  if (SPIFFS.exists(kDiagnosticsPath)) SPIFFS.remove(kDiagnosticsPath);

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
