const DinplugButtonBinding *getDinplugBinding(const HvacConfig &h, uint8_t idx) {
  if (idx >= h.dinButtonCount) return nullptr;
  const uint16_t poolIdx = static_cast<uint16_t>(h.dinButtonStart) + idx;
  if (poolIdx >= dinplugBindingCount) return nullptr;
  return &dinplugBindingPool[poolIdx];
}

DinplugButtonBinding *getDinplugBinding(HvacConfig &h, uint8_t idx) {
  return const_cast<DinplugButtonBinding *>(getDinplugBinding(static_cast<const HvacConfig &>(h), idx));
}

void clearDinplugBindingPool() {
  dinplugBindingCount = 0;
  for (uint8_t i = 0; i < config.hvacCount; i++) {
    config.hvacs[i].dinButtonStart = 0;
    config.hvacs[i].dinButtonCount = 0;
  }
}

bool setHvacDinplugBindings(uint8_t hvacIndex, const DinplugButtonBinding *bindings, uint8_t count) {
  if (hvacIndex >= config.hvacCount) return false;
  if (count > kMaxDinplugBindingsTotal) return false;

  DinplugButtonBinding nextPool[kMaxDinplugBindingsTotal];
  uint8_t nextCount = 0;

  for (uint8_t i = 0; i < config.hvacCount; i++) {
    HvacConfig &h = config.hvacs[i];
    const uint8_t oldStart = h.dinButtonStart;
    const uint8_t oldCount = h.dinButtonCount;
    h.dinButtonStart = nextCount;
    if (i == hvacIndex) {
      if ((nextCount + count) > kMaxDinplugBindingsTotal) return false;
      for (uint8_t j = 0; j < count; j++) nextPool[nextCount + j] = bindings[j];
      h.dinButtonCount = count;
      nextCount += count;
      continue;
    }
    if ((nextCount + oldCount) > kMaxDinplugBindingsTotal) return false;
    for (uint8_t j = 0; j < oldCount; j++) nextPool[nextCount + j] = dinplugBindingPool[oldStart + j];
    h.dinButtonCount = oldCount;
    nextCount += oldCount;
  }

  for (uint8_t i = 0; i < nextCount; i++) dinplugBindingPool[i] = nextPool[i];
  dinplugBindingCount = nextCount;
  return true;
}

void compactDinplugBindingPool() {
  DinplugButtonBinding nextPool[kMaxDinplugBindingsTotal];
  uint8_t nextCount = 0;
  for (uint8_t i = 0; i < config.hvacCount; i++) {
    HvacConfig &h = config.hvacs[i];
    const uint8_t oldStart = h.dinButtonStart;
    const uint8_t oldCount = h.dinButtonCount;
    h.dinButtonStart = nextCount;
    for (uint8_t j = 0; j < oldCount && nextCount < kMaxDinplugBindingsTotal; j++) {
      nextPool[nextCount++] = dinplugBindingPool[oldStart + j];
    }
  }
  for (uint8_t i = 0; i < nextCount; i++) dinplugBindingPool[i] = nextPool[i];
  dinplugBindingCount = nextCount;
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

String dinButtonsJson(const HvacConfig &h) {
  DynamicJsonDocument doc(512);
  JsonArray arr = doc.to<JsonArray>();
  for (uint8_t i = 0; i < h.dinButtonCount; i++) {
    const DinplugButtonBinding *binding = getDinplugBinding(h, i);
    if (!binding) continue;
    JsonObject o = arr.add<JsonObject>();
    o["keypad_id"] = binding->keypadId;
    o["id"] = binding->buttonId;
    o["press_action"] = binding->pressAction;
    o["press_value"] = binding->pressValue;
    o["press_mode_override"] = dinplugModeOverrideToString(binding->pressModeOverride);
    o["press_light_mode"] = normalizeDinplugLightMode(binding->pressLightMode);
    o["hold_action"] = binding->holdAction;
    o["hold_value"] = binding->holdValue;
    o["hold_mode_override"] = dinplugModeOverrideToString(binding->holdModeOverride);
    o["hold_light_mode"] = normalizeDinplugLightMode(binding->holdLightMode);
    o["toggle_power_mode"] = binding->togglePowerMode;
    o["led_follow_mode"] = binding->ledFollowMode;
    o["led_follow_power"] = (binding->ledFollowMode != 0);
  }
  String out;
  serializeJson(doc, out);
  return out;
}

void handleDinplugPage() {
  sendSpiffsFallbackPage("DINplug", "/dinplug.html");
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

bool sendDinplugCommand(const String &cmd) {
  if (!dinplugClient.connected()) return false;
  size_t written = dinplugClient.print(cmd + "\r\n");
  if (written == 0) {
    dinplugClient.stop();
    dinplugConnected = false;
    dinplugWasConnected = false;
    Serial.println("dinplug: tx failed, reconnecting");
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
    const DinplugButtonBinding *b = getDinplugBinding(h, i);
    if (!b || b->ledFollowMode == 0 || b->buttonId == 0) continue;
    const uint8_t ledState = (b->ledFollowMode == 1) ? (state.power ? 1 : 0) : (state.power ? 0 : 1);
    if (b->keypadId != 0) {
      setDinplugLed(b->keypadId, b->buttonId, ledState);
      continue;
    }
    for (uint8_t k = 0; k < h.dinKeypadCount; k++) {
      if (h.dinKeypadIds[k] == 0) continue;
      setDinplugLed(h.dinKeypadIds[k], b->buttonId, ledState);
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
    dinplugWasConnected = true;
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
      const DinplugButtonBinding *bind = getDinplugBinding(h, b);
      if (!bind || bind->buttonId != buttonId) continue;
      if (bind->keypadId != 0 && bind->keypadId != keypadId) continue;
      if (applyDinplugAction(i, *bind, isHold)) {
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
  if (!dinplugClient.connected() && dinplugWasConnected) {
    dinplugWasConnected = false;
    dinplugConnected = false;
    Serial.println("dinplug: disconnected");
    if (telnetMonitorEnabled && monitorLogDinplugEnabled) addMonitorLogEntry("din: disconnected");
  }
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
    Serial.println("dinplug: rx timeout, reconnecting");
    if (telnetMonitorEnabled && monitorLogDinplugEnabled) addMonitorLogEntry("din: rx timeout, reconnecting");
    dinplugClient.stop();
    dinplugConnected = false;
    dinplugWasConnected = false;
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
