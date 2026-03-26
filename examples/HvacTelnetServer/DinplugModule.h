#pragma once

const DinplugButtonBinding *getDinplugBinding(const HvacConfig &h, uint8_t idx);
DinplugButtonBinding *getDinplugBinding(HvacConfig &h, uint8_t idx);
void clearDinplugBindingPool();
bool setHvacDinplugBindings(uint8_t hvacIndex, const DinplugButtonBinding *bindings, uint8_t count);
void compactDinplugBindingPool();
String dinplugConnectionStatus();
String dinToggleModeOptionsHtml(const String &selectedIn);
bool dinActionUsesValue(const String &actionIn);
uint8_t normalizeDinplugModeOverride(const String &modeIn);
const char *dinplugModeOverrideToString(uint8_t mode);
String normalizeDinplugLightMode(const String &modeIn);
String dinKeypadsCsv(const HvacConfig &h);
void parseDinKeypadsCsv(const String &csvIn, HvacConfig &h);
bool hvacHasDinKeypad(const HvacConfig &h, uint16_t keypadId);
String dinButtonsJson(const HvacConfig &h);
void handleDinplugPage();
void handleDinplugSave();
void handleDinplugTest();
bool sendDinplugCommand(const String &cmd);
bool setDinplugLed(uint16_t keypadId, uint16_t ledId, uint8_t state);
void syncDinplugLedsForHvac(uint8_t hvacIndex);
void ensureDinplugConnected(bool forceNow);
void handleDinplugButtonEvent(uint16_t keypadId, uint16_t buttonId, const String &action);
void processDinplugLine(const String &line);
void handleDinplug();
