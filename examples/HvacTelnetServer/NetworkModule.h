#pragma once

#include <Arduino.h>
#include <IPAddress.h>

void setupArduinoOta();
void startMdns();
bool startEthernet();
bool networkReady();
IPAddress networkLocalIp();
IPAddress currentGatewayIp();
IPAddress currentSubnetMask();
IPAddress currentDnsIp();
String networkModeString();
void startWifi();
void handleNetworkRecovery();
void handleFirmwarePage();
void handleFirmwareUpdate();
void handleFirmwareUpload();
void handleFilesystemUpdate();
void handleFilesystemUpload();
void handleFactoryReset();
void handleApiStatus();
void handleApiMeta();
void handleWifiScan();
