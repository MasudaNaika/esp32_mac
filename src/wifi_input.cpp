/*
 * WiFi WebSocket input for Mac Plus emulator.
 * Serves HTML UI at http://macplus.local (port 80).
 * Receives mouse and keyboard events via WebSocket (port 81).
 *
 * Protocol (binary frames):
 *   Mouse:    'M' dx:int8 dy:int8 buttons:uint8  (4 bytes)
 *   Key down: 'K' scancode:uint8                  (2 bytes)
 *   Key up:   'U' scancode:uint8                  (2 bytes)
 */
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include "wifi_input.h"
#include "index_html.h"

extern "C" {
#include "tme/mouse.h"
#include "tme/via.h"
}

extern void wifiReset();

static WebServer *httpServer;
static WebSocketsServer *wsServer;

static void handleRoot() {
    httpServer->send(200, "text/html", INDEX_HTML);
}

static void wsEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length) {
    if (type == WStype_CONNECTED) {
        printf("WS[%u] connected\n", num);
        return;
    }
    if (type == WStype_DISCONNECTED) {
        printf("WS[%u] disconnected\n", num);
        return;
    }
    if (type != WStype_BIN || length < 1) return;

    switch (payload[0]) {
    case 'M':
        if (length >= 4)
            mouseMove((int8_t)payload[1], (int8_t)payload[2], payload[3] & 1);
        break;
    case 'K':
        if (length >= 2) kbdPushKey(payload[1], 0);
        break;
    case 'U':
        if (length >= 2) kbdPushKey(payload[1], 1);
        break;
    }
}

static void wifiInputTask(void *param) {
    httpServer = new WebServer(80);
    wsServer = new WebSocketsServer(81);

    httpServer->on("/", handleRoot);
    httpServer->on("/wifi-reset", HTTP_POST, []() {
        httpServer->send(200, "text/plain", "Resetting WiFi...");
        delay(500);
        wifiReset();
    });
    httpServer->begin();
    printf("HTTP server started on port 80\n");

    wsServer->begin();
    wsServer->onEvent(wsEvent);
    printf("WebSocket server started on port 81\n");

    while (true) {
        httpServer->handleClient();
        wsServer->loop();
        vTaskDelay(1);
    }
}

void wifiInputInit() {
    xTaskCreatePinnedToCore(wifiInputTask, "wifi_in", 8192, NULL, 3, NULL, 1);
}
