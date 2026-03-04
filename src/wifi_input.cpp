/*
 * WiFi UDP input for Mac Plus emulator.
 * Receives mouse and keyboard events over UDP port 4444.
 *
 * Protocol (little-endian):
 *   Mouse:    'M' dx:int8 dy:int8 buttons:uint8  (4 bytes)
 *   Key down: 'K' scancode:uint8                  (2 bytes)
 *   Key up:   'U' scancode:uint8                  (2 bytes)
 *
 * Scancodes are Mac M0110A codes (caller does the translation).
 * The desktop app will map USB/OS keycodes → Mac scancodes.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "wifi_input.h"

extern "C" {
#include "tme/mouse.h"
#include "tme/via.h"
}

#define UDP_PORT  4444

static WiFiUDP udp;

static void wifiInputTask(void *param) {
    udp.begin(UDP_PORT);
    printf("UDP input listening on port %d\n", UDP_PORT);

    uint8_t buf[8];
    while (true) {
        int len = udp.parsePacket();
        if (len > 0) {
            int n = udp.read(buf, sizeof(buf));
            if (n >= 2) {
                switch (buf[0]) {
                case 'M': // Mouse: dx, dy, buttons
                    if (n >= 4) {
                        mouseMove((int8_t)buf[1], (int8_t)buf[2], buf[3] & 1);
                    }
                    break;
                case 'K': // Key down
                    kbdPushKey(buf[1], 0);
                    break;
                case 'U': // Key up
                    kbdPushKey(buf[1], 1);
                    break;
                }
            }
        }
        vTaskDelay(1);
    }
}

void wifiInputInit() {
    xTaskCreatePinnedToCore(wifiInputTask, "wifi_in", 4096, NULL, 3, NULL, 1);
}
