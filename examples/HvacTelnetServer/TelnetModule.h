#pragma once

#include <Arduino.h>

void logHvacStateChange(const String &id, const HvacRuntimeState &after, int8_t sourceTelnetSlot);
void writeStateJson(JsonObject state, const String &id, const HvacRuntimeState &hvacState);
bool sendTelnetJson(WiFiClient &client, JsonDocument &doc);
void broadcastStateToTelnetClients(const String &id, const HvacRuntimeState &state, int8_t excludeSlot);
void sendAllStatesToTelnetClient(WiFiClient &client);
void handleTelnetPeriodicStateBroadcast();
void handleApiDeviceGet();
void handleApiDeviceGetAll();
void handleApiDeviceSend();
void handleApiDeviceRaw();
void respondTelnetError(WiFiClient &client, const String &message);
void handleTelnetLine(WiFiClient &client, const String &line, int8_t sourceTelnetSlot);
void handleTelnet();
