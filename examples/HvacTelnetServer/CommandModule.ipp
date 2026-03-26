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
    cmds.add("status");
    cmds.add("help");
    JsonArray examples = help["examples"].to<JsonArray>();
    examples.add("{\"cmd\":\"list\"}");
    examples.add("{\"cmd\":\"send\",\"id\":\"1\",\"power\":\"on\",\"mode\":\"cool\",\"temp\":24,\"fan\":\"auto\"}");
    examples.add("{\"cmd\":\"get\",\"id\":\"1\"}");
    examples.add("{\"cmd\":\"get_all\"}");
    examples.add("{\"cmd\":\"push_all\"}");
    examples.add("{\"cmd\":\"status\"}");
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
      o["profile_name"] = config.hvacs[i].profileName;
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

  if (cmd == "status") {
    resp["ok"] = true;
    resp["type"] = "status";
    resp["firmware_version"] = kFirmwareVersion;
    resp["filesystem_version"] = filesystemVersion;
    resp["filesystem_version_expected"] = kFilesystemVersionExpected;
    resp["version_match"] = (filesystemVersion == String(kFilesystemVersionExpected));
    resp["boot_count"] = bootCount;
    resp["reset_reason"] = resetReason;
    resp["network_mode"] = networkModeString();
    resp["ip"] = networkLocalIp().toString();
    resp["hostname"] = config.hostname.length() ? config.hostname : kDefaultHostname;
    resp["uptime_ms"] = millis();
    resp["telnet_port"] = config.telnetPort;
    resp["telnet_clients_active"] = activeTelnetClientCount();
    resp["dinplug_status"] = dinplugConnectionStatus();
    resp["emitter_count"] = config.emitterCount;
    resp["device_count"] = config.hvacCount;
    resp["heap_free"] = ESP.getFreeHeap();
    resp["heap_min_free"] = ESP.getMinFreeHeap();
    resp["heap_max_alloc"] = ESP.getMaxAllocHeap();
    resp["trend_samples_count"] = trendHistoryCount;
    resp["trend_sample_interval_sec"] = (kTrendSampleIntervalMs / 1000UL);
    resp["wifi_rssi"] = WiFi.isConnected() ? WiFi.RSSI() : 0;
    resp["monitor_logging_enabled"] = telnetMonitorEnabled;
    resp["time_synced"] = clockHasValidTime();
    resp["local_time"] = localTimeString();
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
      if (h.customCommands[x].name == name) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;
    if (h.customCommandCount >= kMaxCustomCommands) break;
    CustomCommandCode &cmd = h.customCommands[h.customCommandCount++];
    cmd.name = name;
    cmd.encoding = encoding;
    cmd.code = code;
  }
}
