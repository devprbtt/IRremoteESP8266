static bool ethernetLinkReady();

void handleFirmwarePage() {
  sendSpiffsFallbackPage("System", "/system.html");
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

void handleApiStatus() {
  if (!checkAuth()) { requestAuth(); return; }
  JsonDocument doc;
  const bool ethLinkUp = ETH.linkUp();
  const IPAddress ethLocalIp = ETH.localIP();
  const bool ethRuntimeReady = ethernetLinkReady();
  const wifi_mode_t wifiMode = WiFi.getMode();
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
  doc["uptime_ms"] = millis();
  doc["telnet_port"] = config.telnetPort;
  doc["dinplug_status"] = dinplugConnectionStatus();
  doc["emitter_count"] = config.emitterCount;
  doc["hvac_count"] = config.hvacCount;
  doc["dinplug_bindings_used"] = dinplugBindingCount;
  doc["dinplug_bindings_total"] = kMaxDinplugBindingsTotal;
  doc["ir_receiver_enabled"] = config.irReceiver.enabled;
  doc["ir_receiver_gpio"] = config.irReceiver.gpio;
  doc["heap_free"] = ESP.getFreeHeap();
  doc["heap_min_free"] = ESP.getMinFreeHeap();
  doc["heap_max_alloc"] = ESP.getMaxAllocHeap();
  doc["trend_samples_count"] = trendHistoryCount;
  doc["trend_sample_interval_sec"] = (kTrendSampleIntervalMs / 1000UL);
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
  doc["eth_begin_last_ok"] = ethernetBeginLastOk;
  doc["eth_link_up"] = ethLinkUp;
  doc["eth_local_ip"] = ethLocalIp.toString();
  doc["eth_gateway"] = ETH.gatewayIP().toString();
  doc["eth_subnet"] = ETH.subnetMask().toString();
  doc["eth_dns"] = ETH.dnsIP().toString();
  doc["eth_runtime_ready"] = ethRuntimeReady;
  doc["eth_last_begin_ms"] = ethernetLastBeginMs;
  doc["eth_last_link_up_ms"] = ethernetLastLinkUpMs;
  doc["wifi_mode_raw"] = static_cast<int>(wifiMode);
  doc["wifi_rssi"] = WiFi.isConnected() ? WiFi.RSSI() : 0;
  doc["monitor_logging_enabled"] = telnetMonitorEnabled;
  doc["time_synced"] = clockHasValidTime();
  doc["local_time"] = localTimeString();
  String out;
  serializeJson(doc, out);
  web.send(200, "application/json", out);
}

void handleApiMeta() {
  if (!checkAuth()) { requestAuth(); return; }
  JsonDocument doc;
  doc["firmware_version"] = kFirmwareVersion;
  doc["filesystem_version"] = filesystemVersion;
  doc["filesystem_version_expected"] = kFilesystemVersionExpected;
  doc["version_match"] = (filesystemVersion == String(kFilesystemVersionExpected));
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
  doc["max_dinplug_buttons"] = kMaxDinplugBindingsTotal;
  doc["max_dinplug_bindings_total"] = kMaxDinplugBindingsTotal;
  doc["dinplug_bindings_used"] = dinplugBindingCount;
  doc["dinplug_bindings_available"] = kMaxDinplugBindingsTotal - dinplugBindingCount;
  doc["monitor_logging_enabled"] = telnetMonitorEnabled;
  doc["max_temp_sensors"] = kMaxTempSensors;
  doc["max_emitters"] = kMaxEmitters;
  doc["max_hvacs"] = kMaxHvacs;

  String out;
  serializeJson(doc, out);
  web.send(200, "application/json", out);
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

static void configureWifiStation() {
  dnsServer.stop();
  dnsServerActive = false;
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(config.hostname.length() ? config.hostname.c_str() : kDefaultHostname);
  if (!config.wifi.dhcp) {
    WiFi.config(config.wifi.ip, config.wifi.gateway, config.wifi.subnet, config.wifi.dns);
  }
}

static void beginWifiStationConnection() {
  configureWifiStation();
  wifiFallbackPending = true;
  WiFi.begin(config.wifi.ssid.c_str(), config.wifi.password.c_str());
}

static void stopWifiForEthernet() {
  dnsServer.stop();
  dnsServerActive = false;
  if (WiFi.getMode() != WIFI_OFF) {
    WiFi.disconnect(false, false);
    WiFi.mode(WIFI_OFF);
  }
}

static bool ethernetLinkReady() {
  return ethernetUp && ETH.linkUp() && ETH.localIP() != IPAddress();
}

void handleNetworkEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      ETH.setHostname(config.hostname.length() ? config.hostname.c_str() : kDefaultHostname);
      Serial.println("eth: event start");
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.println("eth: event connected");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      ethernetUp = true;
      wifiFallbackPending = false;
      ethernetLastLinkUpMs = millis();
      Serial.print("eth: event got ip=");
      Serial.println(ETH.localIP());
      if (telnetMonitorEnabled && monitorLogStateEnabled) addMonitorLogEntry("eth: got ip=" + ETH.localIP().toString());
      stopWifiForEthernet();
      startMdns();
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      Serial.println("eth: event disconnected");
      if (telnetMonitorEnabled && monitorLogStateEnabled) addMonitorLogEntry("eth: disconnected");
      ethernetUp = false;
      if (config.wifi.ssid.length() > 0) {
        WiFi.disconnect(false, false);
        beginWifiStationConnection();
      } else {
        WiFi.mode(WIFI_AP);
        WiFi.softAP(kApSsid);
        WiFi.softAPsetHostname(config.hostname.length() ? config.hostname.c_str() : kDefaultHostname);
        dnsServer.start(53, "*", WiFi.softAPIP());
        dnsServerActive = true;
        startMdns();
      }
      break;
    case ARDUINO_EVENT_ETH_STOP:
      Serial.println("eth: event stop");
      ethernetUp = false;
      break;
    default:
      break;
  }
}

void setupNetworkEvents() {
  static bool registered = false;
  if (registered) return;
  WiFi.onEvent(handleNetworkEvent);
  registered = true;
}

void startMdns() {
#if ARDUINO_ARCH_ESP32
  MDNS.end();
  const char *host = (config.hostname.length() ? config.hostname.c_str() : kDefaultHostname);
  if (!MDNS.begin(host)) {
    Serial.println("mdns: start failed");
    return;
  }
  MDNS.addService("http", "tcp", 80);
  MDNS.addService("hvactelnet", "tcp", config.telnetPort);
  MDNS.addServiceTxt("hvactelnet", "tcp", "domain", "hvactelnet");
  MDNS.addServiceTxt("hvactelnet", "tcp", "board", "ir-server-telnet");
  MDNS.addServiceTxt("hvactelnet", "tcp", "hostname", host);
  Serial.print("mdns: responding for ");
  Serial.print(host);
  Serial.println(".local");
#endif
}

bool startEthernet() {
  ethernetLastBeginMs = millis();
  if (!config.eth.enabled) return false;
  if (ethernetStarted) {
    if (ethernetLinkReady()) {
      ethernetLastLinkUpMs = millis();
      return true;
    }
    return false;
  }
  ethernetUp = false;
  const char *host = (config.hostname.length() ? config.hostname.c_str() : kDefaultHostname);
  ETH.setHostname(host);
  Serial.println("eth: starting LAN8720 (WT32 defaults)");
  bool started = ETH.begin(kEthPhyAddr, kEthPowerPin, kEthMdcPin, kEthMdioPin,
                           ETH_PHY_LAN8720, ETH_CLOCK_GPIO0_IN);
  ethernetStarted = started;
  ethernetBeginLastOk = started;
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
      ethernetLastLinkUpMs = millis();
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
  if (ethernetLinkReady()) return true;
  if (WiFi.status() == WL_CONNECTED) return true;
  return false;
}

IPAddress networkLocalIp() {
  if (ethernetLinkReady()) return ETH.localIP();
  if (WiFi.status() == WL_CONNECTED) return WiFi.localIP();
  if (WiFi.getMode() == WIFI_MODE_AP || WiFi.getMode() == WIFI_MODE_APSTA) return WiFi.softAPIP();
  return IPAddress();
}

IPAddress currentGatewayIp() {
  if (ethernetLinkReady()) return ETH.gatewayIP();
  if (WiFi.status() == WL_CONNECTED) return WiFi.gatewayIP();
  return IPAddress();
}

IPAddress currentSubnetMask() {
  if (ethernetLinkReady()) return ETH.subnetMask();
  if (WiFi.status() == WL_CONNECTED) return WiFi.subnetMask();
  return IPAddress();
}

IPAddress currentDnsIp() {
  if (ethernetLinkReady()) return ETH.dnsIP();
  if (WiFi.status() == WL_CONNECTED) return WiFi.dnsIP();
  return IPAddress();
}

String networkModeString() {
  if (ethernetLinkReady()) return "ETH";
  if (WiFi.status() == WL_CONNECTED) return "WiFi STA";
  if (WiFi.getMode() == WIFI_MODE_AP || WiFi.getMode() == WIFI_MODE_APSTA) return "WiFi AP";
  return "offline";
}

void startWifi() {
  setupNetworkEvents();
  if (startEthernet()) {
    dnsServer.stop();
    dnsServerActive = false;
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
    dnsServerActive = true;
    startMdns();
    return;
  }
  beginWifiStationConnection();
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
    dnsServer.start(53, "*", WiFi.softAPIP());
    dnsServerActive = true;
  } else {
    Serial.print("wifi: connected IP=");
    Serial.println(WiFi.localIP());
    startMdns();
  }
}

void handleNetworkRecovery() {
  static wl_status_t lastWifiStatus = WL_IDLE_STATUS;
  static bool lastEthReady = false;
  static unsigned long lastWifiRecoveryAttemptMs = 0;
  static unsigned long lastEthRecoveryAttemptMs = 0;

  const unsigned long now = millis();
  const wl_status_t wifiStatus = WiFi.status();
  const bool ethReady = ethernetLinkReady();

  if (WiFi.getMode() == WIFI_STA && wifiStatus == WL_CONNECTED && lastWifiStatus != WL_CONNECTED) {
    wifiFallbackPending = false;
    Serial.print("wifi: reconnected IP=");
    Serial.println(WiFi.localIP());
    if (telnetMonitorEnabled && monitorLogStateEnabled) addMonitorLogEntry("wifi: reconnected ip=" + WiFi.localIP().toString());
    if (!ethReady) startMdns();
  } else if (WiFi.getMode() == WIFI_STA && wifiStatus != WL_CONNECTED && lastWifiStatus == WL_CONNECTED) {
    Serial.print("wifi: disconnected status=");
    Serial.println(static_cast<int>(wifiStatus));
    if (telnetMonitorEnabled && monitorLogStateEnabled) addMonitorLogEntry("wifi: disconnected status=" + String(static_cast<int>(wifiStatus)));
  }
  lastWifiStatus = wifiStatus;

  if (ethReady && !lastEthReady) {
    Serial.print("eth: active IP=");
    Serial.println(ETH.localIP());
    if (telnetMonitorEnabled && monitorLogStateEnabled) addMonitorLogEntry("eth: active ip=" + ETH.localIP().toString());
  } else if (!ethReady && lastEthReady) {
    Serial.println("eth: link lost");
    if (telnetMonitorEnabled && monitorLogStateEnabled) addMonitorLogEntry("eth: link lost");
  }
  lastEthReady = ethReady;

  if (config.eth.enabled && !ethernetStarted && !ethReady && (now - lastEthRecoveryAttemptMs) >= 15000UL) {
    lastEthRecoveryAttemptMs = now;
    if (startEthernet()) {
      stopWifiForEthernet();
      startMdns();
      lastEthReady = true;
      return;
    }
  }

  if (ethReady) return;
  if (config.wifi.ssid.length() == 0) return;
  if (WiFi.getMode() != WIFI_STA) return;
  if (wifiStatus == WL_CONNECTED) return;
  const unsigned long wifiRecoveryIntervalMs = wifiFallbackPending ? 3000UL : 15000UL;
  if ((now - lastWifiRecoveryAttemptMs) < wifiRecoveryIntervalMs) return;
  lastWifiRecoveryAttemptMs = now;

  Serial.print("wifi: attempting recovery status=");
  Serial.println(static_cast<int>(wifiStatus));
  if (telnetMonitorEnabled && monitorLogStateEnabled) addMonitorLogEntry("wifi: attempting recovery status=" + String(static_cast<int>(wifiStatus)));

  WiFi.disconnect(false, false);
  beginWifiStationConnection();
}
