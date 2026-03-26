#pragma once

String normalizeIrReceiverMode(const String &modeIn);
String buildRawHexFromCapture(const decode_results &capture);
String buildGcFromCapture(const decode_results &capture, uint32_t frequency);
String buildRacepointFromCapture(const decode_results &capture, uint32_t frequency);
String buildProntoFromCapture(const decode_results &capture, uint32_t frequency);
String buildCodeFromCaptureEncoding(const decode_results &capture, const String &encodingIn);
void handleIrLearnStart();
void handleIrLearnPoll();
void handleIrLearnCancel();
void setupIrReceiver();
void handleIrReceiver();
