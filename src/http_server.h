#pragma once

#include "app_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

// Connect to Wi-Fi and keep the station interface alive for NTP and HTTP.
bool macHttpNetworkStart(const AppSettings *cfg);
// Report whether the active network is a connected station, not setup AP mode.
bool macHttpStationConnected(void);
// Stop the active Wi-Fi mode and retry station connection from current settings.
bool macHttpReconnectWifi(const AppSettings *cfg);
// Append Wi-Fi status and nearby APs to a caller-provided text buffer.
void macHttpDescribeWifi(char *response, size_t responseSize);
// Report the best current Web UI URL for QR display.
bool macHttpGetActiveUrl(char *url, size_t urlSize);

// Publish esp32mac.local and start the SD-backed web UI on the existing Wi-Fi.
bool macHttpServerStart(const AppSettings *cfg);

#ifdef __cplusplus
}
#endif
