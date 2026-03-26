#pragma once

#include <Arduino.h>

struct TrendSample {
  unsigned long uptimeMs = 0;
  uint32_t freeHeap = 0;
  uint32_t minFreeHeap = 0;
  uint32_t maxAllocHeap = 0;
  int32_t wifiRssi = 0;
  uint8_t telnetClients = 0;
};

void printMonitorStatus();
void handleSerialConsole();
void addMonitorLogEntry(const String &line);
void clearMonitorLog();
bool monitorCategoryEnabled(const String &category);
void markDiagnosticsDirty();
void savePersistedDiagnostics();
void handleDiagnosticsPersistence();
void handleApiDiagnostics();
void sampleRuntimeTrends(bool force = false);
