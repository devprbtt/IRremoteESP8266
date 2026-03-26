#pragma once

#include <Arduino.h>

bool processCommand(JsonDocument &doc, JsonDocument &resp, int8_t sourceTelnetSlot = -1);
bool sendCustomCode(const HvacConfig &hvac, EmitterRuntime *em, const String &code, const String &encoding);
String findCustomTempCode(const HvacConfig &hvac, int tempC);
const CustomCommandCode *findCustomCommandByName(const HvacConfig &h, const String &name);
void loadCustomCommandsFromRequest(HvacConfig &h);
String customCommandsJson(const HvacConfig &h);
String customCommandNamesSummary(const HvacConfig &h);
void handleHvacTest();
void handleRawTest();
