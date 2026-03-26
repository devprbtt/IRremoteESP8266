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

void addMonitorLogEntry(const String &line) {
  String entry;
  String clockText = localTimeString();
  if (clockText.length()) {
    entry = "[" + clockText + "] [" + String(millis()) + " ms] ";
  } else {
    entry = "[" + String(millis()) + " ms] ";
  }
  entry += line;
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

void markDiagnosticsDirty() {
  diagnosticsDirty = true;
  diagnosticsDirtySinceMs = millis();
}

void sampleRuntimeTrends(bool force) {
  unsigned long now = millis();
  if (!force && (now - lastTrendSampleMs) < kTrendSampleIntervalMs) return;
  lastTrendSampleMs = now;

  TrendSample sample;
  sample.uptimeMs = now;
  sample.freeHeap = ESP.getFreeHeap();
  sample.minFreeHeap = ESP.getMinFreeHeap();
  sample.maxAllocHeap = ESP.getMaxAllocHeap();
  sample.wifiRssi = WiFi.isConnected() ? WiFi.RSSI() : 0;
  sample.telnetClients = activeTelnetClientCount();

  if (trendHistoryCount < kTrendHistoryCapacity) {
    uint8_t idx = (trendHistoryStart + trendHistoryCount) % kTrendHistoryCapacity;
    trendHistory[idx] = sample;
    trendHistoryCount++;
  } else {
    trendHistory[trendHistoryStart] = sample;
    trendHistoryStart = (trendHistoryStart + 1) % kTrendHistoryCapacity;
  }
  markDiagnosticsDirty();
}

void savePersistedDiagnostics() {
  sampleRuntimeTrends(true);
  JsonDocument doc;
  String nowText = localTimeString();
  doc["saved_at"] = nowText;
  doc["uptime_ms"] = millis();
  doc["firmware_version"] = kFirmwareVersion;
  doc["filesystem_version"] = filesystemVersion;
  doc["filesystem_version_expected"] = kFilesystemVersionExpected;
  doc["version_match"] = (filesystemVersion == String(kFilesystemVersionExpected));
  doc["boot_count"] = bootCount;
  doc["reset_reason"] = resetReason;
  doc["network_mode"] = networkModeString();
  doc["ip"] = networkLocalIp().toString();
  doc["gateway"] = currentGatewayIp().toString();
  doc["subnet"] = currentSubnetMask().toString();
  doc["dns"] = currentDnsIp().toString();
  doc["hostname"] = config.hostname.length() ? config.hostname : kDefaultHostname;
  doc["timezone"] = normalizeTimezoneOffset(config.timezone);
  doc["wifi_rssi"] = WiFi.isConnected() ? WiFi.RSSI() : 0;
  doc["time_synced"] = clockHasValidTime();
  doc["local_time"] = nowText;
  doc["heap_free"] = ESP.getFreeHeap();
  doc["heap_min_free"] = ESP.getMinFreeHeap();
  doc["heap_max_alloc"] = ESP.getMaxAllocHeap();
  doc["telnet_port"] = config.telnetPort;
  doc["telnet_clients_active"] = activeTelnetClientCount();
  doc["dinplug_status"] = dinplugConnectionStatus();
  doc["dinplug_bindings_used"] = dinplugBindingCount;
  doc["dinplug_bindings_total"] = kMaxDinplugBindingsTotal;
  doc["hvac_count"] = config.hvacCount;
  doc["emitter_count"] = config.emitterCount;
  doc["temp_sensor_count"] = tempSensorCount;
  doc["ir_receiver_enabled"] = config.irReceiver.enabled;
  doc["ir_receiver_gpio"] = config.irReceiver.gpio;

  JsonArray telnetClientsJson = doc["telnet_clients"].to<JsonArray>();
  for (uint8_t i = 0; i < kMaxTelnetClients; i++) {
    WiFiClient &c = telnetClients[i];
    if (!c || !c.connected()) continue;
    JsonObject tc = telnetClientsJson.add<JsonObject>();
    tc["slot"] = i;
    tc["ip"] = c.remoteIP().toString();
    tc["port"] = c.remotePort();
  }

  JsonArray lines = doc["recent_lines"].to<JsonArray>();
  uint16_t limit = kDiagnosticsLogLines;
  if (limit > telnetMonitorLogCount) limit = telnetMonitorLogCount;
  uint16_t startOffset = telnetMonitorLogCount - limit;
  for (uint16_t i = startOffset; i < telnetMonitorLogCount; i++) {
    uint16_t idx = (telnetMonitorLogStart + i) % kMonitorLogCapacity;
    lines.add(telnetMonitorLog[idx]);
  }

  JsonArray trends = doc["trend_samples"].to<JsonArray>();
  for (uint8_t i = 0; i < trendHistoryCount; i++) {
    uint8_t idx = (trendHistoryStart + i) % kTrendHistoryCapacity;
    JsonObject t = trends.add<JsonObject>();
    t["uptime_ms"] = trendHistory[idx].uptimeMs;
    t["heap_free"] = trendHistory[idx].freeHeap;
    t["heap_min_free"] = trendHistory[idx].minFreeHeap;
    t["heap_max_alloc"] = trendHistory[idx].maxAllocHeap;
    t["wifi_rssi"] = trendHistory[idx].wifiRssi;
    t["telnet_clients_active"] = trendHistory[idx].telnetClients;
  }
  doc["monitor_logging_enabled"] = telnetMonitorEnabled;

  File f = SPIFFS.open(kDiagnosticsPath, FILE_WRITE);
  if (!f) {
    Serial.println("diag: failed to open for write");
    return;
  }
  serializeJson(doc, f);
  f.close();
  diagnosticsDirty = false;
}

void handleDiagnosticsPersistence() {
  if (!diagnosticsDirty) return;
  unsigned long now = millis();
  if ((now - diagnosticsDirtySinceMs) < kDiagnosticsPersistDebounceMs) return;
  savePersistedDiagnostics();
}

void handleApiMonitor() {
  if (!checkAuth()) { requestAuth(); return; }
  uint16_t limit = kMonitorLogCapacity;
  if (web.hasArg("limit")) {
    long requested = web.arg("limit").toInt();
    if (requested > 0) {
      limit = (uint16_t)requested;
      if (limit > kMonitorLogCapacity) limit = kMonitorLogCapacity;
    }
  }
  if (limit > telnetMonitorLogCount) limit = telnetMonitorLogCount;
  JsonDocument doc;
  doc["enabled"] = telnetMonitorEnabled;
  JsonObject filters = doc["filters"].to<JsonObject>();
  filters["telnet"] = monitorLogTelnetEnabled;
  filters["state"] = monitorLogStateEnabled;
  filters["dinplug"] = monitorLogDinplugEnabled;
  filters["ir"] = monitorLogIrEnabled;
  JsonArray lines = doc["lines"].to<JsonArray>();
  uint16_t startOffset = telnetMonitorLogCount - limit;
  for (uint16_t i = startOffset; i < telnetMonitorLogCount; i++) {
    uint16_t idx = (telnetMonitorLogStart + i) % kMonitorLogCapacity;
    lines.add(telnetMonitorLog[idx]);
  }
  String out;
  serializeJson(doc, out);
  web.send(200, "application/json", out);
}

void handleApiDiagnostics() {
  if (!checkAuth()) { requestAuth(); return; }
  if (!SPIFFS.exists(kDiagnosticsPath)) {
    web.send(404, "application/json", "{\"ok\":false,\"error\":\"diagnostics_unavailable\"}");
    return;
  }
  File f = SPIFFS.open(kDiagnosticsPath, FILE_READ);
  if (!f) {
    web.send(500, "application/json", "{\"ok\":false,\"error\":\"diagnostics_open_failed\"}");
    return;
  }
  if (web.hasArg("download")) {
    web.sendHeader("Content-Disposition", "attachment; filename=\"diagnostics.json\"");
  }
  web.streamFile(f, "application/json");
  f.close();
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
