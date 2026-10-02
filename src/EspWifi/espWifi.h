#pragma once
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "espmDNS.h"
#include "creds.h"
#include <vector>
void wifi_init_sta(void);

// Blocking scan (about 2 s); the access points found, in no particular order.
// ESP-NOW and the station link hop channels while it runs.
esp_err_t wifi_scan(std::vector<wifi_ap_record_t> &out);

// Switches the station to another network and reconnects. Returns once the
// attempt is under way, not when it succeeds: watch wifi_is_connected(). An
// empty or null password means an open network. Not saved: the next boot uses
// the credentials from creds.h again.
esp_err_t wifi_connect(const char *ssid, const char *password);
bool wifi_is_connected(void);
// True once the WiFi driver is running, whether or not an AP was joined.
// ESP-NOW only needs this much, not an association.
bool wifi_is_started(void);
