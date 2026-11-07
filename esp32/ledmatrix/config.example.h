#pragma once

// ---- WiFi ----
#define WIFI_SSID "SN"
#define WIFI_PASS "CHANGE_ME"

// Static IP (set USE_STATIC_IP to 0 to use DHCP)
#define USE_STATIC_IP 1
#define STATIC_IP   192, 168, 2, 38
#define STATIC_GW   192, 168, 2, 1
#define STATIC_MASK 255, 255, 255, 0
#define STATIC_DNS1 1, 1, 1, 1
#define STATIC_DNS2 8, 8, 8, 8

// ---- Your server (the machine running docker compose) ----
#define SERVER_URL  "http://192.168.2.10:8000"   // GIFs are downloaded from here
#define MQTT_HOST   "192.168.2.10"
#define MQTT_PORT   1883
#define MQTT_USER   ""                            // leave empty for anonymous
#define MQTT_PASS   ""
#define BASE_TOPIC  "ledmatrix"                   // must match BASE_TOPIC on the server

// ---- Time ----
#define TZ_INFO "CET-1CEST,M3.5.0/02,M10.5.0/03"  // Germany
#define NTP_SERVER "ptbtime1.ptb.de"

// ---- SD card (optional, these are the defaults) ----
// #define SD_CS   5
// #define SD_SCK  18
// #define SD_MISO 19
// #define SD_MOSI 23
