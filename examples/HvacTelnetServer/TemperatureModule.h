#pragma once

uint8_t tempSensorPrecision();
String sensorNameForIndex(uint8_t idx);
String sensorAddressToString(const DeviceAddress addr);
float applyTempSensorPrecision(float value);
bool hvacUsesSensorTemp(const HvacConfig &h);
void setupTemperatureSensors();
void readTemperatureSensors();
void handleTemperatureSensors();
