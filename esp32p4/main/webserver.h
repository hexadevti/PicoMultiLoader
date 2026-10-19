// webserver.h - Wi-Fi (through the board's ESP32-C6) plus a browser file manager for the SD card
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define WEB_HOSTNAME "picocalc" // also reachable as http://picocalc.local/

typedef enum {
    WEB_OFF,
    WEB_STARTING,   // bringing up the ESP32-C6
    WEB_CONNECTING, // joining the network saved in Setup (F1)
    WEB_RUNNING,    // server up (as a station or as our own access point)
    WEB_FAILED,     // see error
} web_state_t;

typedef struct {
    web_state_t state;
    bool ap_mode;      // running our own access point
    char ssid[33];     // network joined, or our AP's name
    char password[65]; // our AP's password (AP mode only)
    char ip[16];
    char activity[64]; // last thing a browser did
    int progress;      // upload progress 0-100, or -1
    char error[96];
} web_status_t;

/** Start Wi-Fi and the HTTP server in the background. Watch webserver_status(). */
esp_err_t webserver_start(void);

/** Stop the server and Wi-Fi. Waits for a running request to finish. */
void webserver_stop(void);

/** Copy the current status. Returns a counter that changes whenever the status does. */
uint32_t webserver_status(web_status_t *out);

/** A counter that changes whenever a browser uploads, deletes, renames or creates something. */
uint32_t webserver_changes(void);

/** If the browser asked to flash a file, copy its VFS path and return true (once). */
bool webserver_take_flash_request(char *path, size_t len);

// ---- the Wi-Fi network to join (kept in NVS) ----

/** Read the saved network. Returns false if none is set up. */
bool web_wifi_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

/** Save the network to join; an empty ssid forgets it. Passwords are empty (open) or 8-64 chars. */
esp_err_t web_wifi_save(const char *ssid, const char *pass);

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} web_ap_t;

/** Scan for networks (blocks ~3 s; only while the web server is off). Strongest first; -1 on error. */
int web_wifi_scan(web_ap_t *out, int max);
