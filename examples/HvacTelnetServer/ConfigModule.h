#pragma once

String configToJsonString();
bool saveConfigJson(const String &json);
void saveConfig();
void clearConfig();
bool loadConfigFromJson(const String &json, bool printErrors);
bool readStoredConfigJson(String &json);
bool importLegacyConfigFromSpiffs();
void clearPersistedData();
void loadConfig();
void rebuildEmitters();
bool findHvacById(const String &id, HvacConfig *&out);
EmitterRuntime *getEmitter(uint8_t idx);
void initHvacRuntimeStates();
void resetHvacRuntimeState(uint8_t idx);
int8_t findHvacIndexById(const String &id);
void markHvacStatesDirty();
void savePersistedHvacStates();
void loadPersistedHvacStates();
void handleHvacStatePersistence();
