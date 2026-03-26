uint8_t tempSensorPrecision() {
  return (config.tempSensors.precision <= 2) ? config.tempSensors.precision : 2;
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

float applyTempSensorPrecision(float value) {
  uint8_t p = tempSensorPrecision();
  float scale = 1.0f;
  if (p == 1) scale = 10.0f;
  else if (p == 2) scale = 100.0f;
  return roundf(value * scale) / scale;
}

bool hvacUsesSensorTemp(const HvacConfig &h) {
  String source = h.currentTempSource;
  source.toLowerCase();
  return source == "sensor";
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
