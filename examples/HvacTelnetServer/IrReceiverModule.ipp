String normalizeIrReceiverMode(const String &modeIn) {
  String mode = modeIn;
  mode.toLowerCase();
  mode.trim();
  if (mode == "pronto") return "pronto";
  if (mode == "rawhex" || mode == "raw_hex" || mode == "raw") return "rawhex";
  return "auto";
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

  if (pulseCount & 1) pulseCount--;
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
  Serial.println(truncateForLog(line));
  if (telnetMonitorEnabled && monitorLogIrEnabled) addMonitorLogEntry(line);

  irReceiver->resume();
}
