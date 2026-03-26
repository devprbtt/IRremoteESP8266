void logHvacStateChange(const String &id, const HvacRuntimeState &after, int8_t sourceTelnetSlot) {
  if (!telnetMonitorEnabled) return;
  if (!monitorLogStateEnabled) return;
  if (sourceTelnetSlot >= 0) return;
  JsonDocument msg;
  writeStateJson(msg.to<JsonObject>(), id, after);
  String payload;
  serializeJson(msg, payload);
  addMonitorLogEntry("TX state " + payload);
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

void handleApiDeviceGet() {
  if (!checkAuth()) { requestAuth(); return; }
  JsonDocument cmd;
  cmd["cmd"] = "get";
  cmd["id"] = web.arg("id");
  JsonDocument resp;
  processCommand(cmd, resp);
  String out;
  serializeJson(resp, out);
  web.send(200, "application/json", out);
}

void handleApiDeviceGetAll() {
  if (!checkAuth()) { requestAuth(); return; }
  JsonDocument cmd;
  cmd["cmd"] = "get_all";
  JsonDocument resp;
  processCommand(cmd, resp);
  String out;
  serializeJson(resp, out);
  web.send(200, "application/json", out);
}

void handleApiDeviceSend() {
  if (!checkAuth()) { requestAuth(); return; }
  JsonDocument cmd;
  cmd["cmd"] = "send";

  String body = web.arg("plain");
  if (body.length()) {
    DeserializationError err = deserializeJson(cmd, body);
    if (err) {
      String out = "{\"ok\":false,\"error\":\"invalid_json\",\"detail\":\"";
      out += err.c_str();
      out += "\"}";
      web.send(400, "application/json", out);
      return;
    }
    cmd["cmd"] = "send";
  } else {
    cmd["id"] = web.arg("id");
    if (web.arg("power").length()) cmd["power"] = web.arg("power");
    if (web.arg("mode").length()) cmd["mode"] = web.arg("mode");
    if (web.arg("temp").length()) cmd["temp"] = web.arg("temp").toFloat();
    if (web.arg("fan").length()) cmd["fan"] = web.arg("fan");
    if (web.arg("swingv").length()) cmd["swingv"] = web.arg("swingv");
    if (web.arg("swingh").length()) cmd["swingh"] = web.arg("swingh");
    if (web.arg("light").length()) cmd["light"] = web.arg("light");
    if (web.arg("quiet").length()) cmd["quiet"] = web.arg("quiet");
    if (web.arg("turbo").length()) cmd["turbo"] = web.arg("turbo");
    if (web.arg("econo").length()) cmd["econo"] = web.arg("econo");
    if (web.arg("filter").length()) cmd["filter"] = web.arg("filter");
    if (web.arg("clean").length()) cmd["clean"] = web.arg("clean");
    if (web.arg("beep").length()) cmd["beep"] = web.arg("beep");
    if (web.arg("sleep").length()) cmd["sleep"] = web.arg("sleep").toInt();
    if (web.arg("clock").length()) cmd["clock"] = web.arg("clock").toInt();
    if (web.arg("celsius").length()) cmd["celsius"] = web.arg("celsius");
    if (web.arg("model").length()) cmd["model"] = web.arg("model").toInt();
    if (web.arg("command").length()) cmd["command"] = web.arg("command");
    if (web.arg("command_name").length()) cmd["command_name"] = web.arg("command_name");
    if (web.arg("code").length()) cmd["code"] = web.arg("code");
    if (web.arg("encoding").length()) cmd["encoding"] = web.arg("encoding");
  }

  JsonDocument resp;
  processCommand(cmd, resp);
  String out;
  serializeJson(resp, out);
  web.send(200, "application/json", out);
}

void handleApiDeviceRaw() {
  if (!checkAuth()) { requestAuth(); return; }
  JsonDocument cmd;
  cmd["cmd"] = "raw";

  String body = web.arg("plain");
  if (body.length()) {
    DeserializationError err = deserializeJson(cmd, body);
    if (err) {
      String out = "{\"ok\":false,\"error\":\"invalid_json\",\"detail\":\"";
      out += err.c_str();
      out += "\"}";
      web.send(400, "application/json", out);
      return;
    }
    cmd["cmd"] = "raw";
  } else {
    cmd["emitter"] = web.arg("emitter").toInt();
    if (web.arg("encoding").length()) cmd["encoding"] = web.arg("encoding");
    cmd["code"] = web.arg("code");
  }

  JsonDocument resp;
  processCommand(cmd, resp);
  String out;
  serializeJson(resp, out);
  web.send(200, "application/json", out);
}

void respondTelnetError(WiFiClient &client, const String &message) {
  JsonDocument doc;
  doc["ok"] = false;
  doc["error"] = message;
  sendTelnetJson(client, doc);
}

void handleTelnetLine(WiFiClient &client, const String &line, int8_t sourceTelnetSlot) {
  if (telnetMonitorEnabled && monitorLogTelnetEnabled) {
    String rxMsg = "RX slot=" + String(sourceTelnetSlot) + " from " +
                   client.remoteIP().toString() + ":" + String(client.remotePort()) +
                   " line=" + line;
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
    String txMsg = "TX slot=" + String(sourceTelnetSlot) + " cmd=" + cmd + " line=" + respStr;
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
