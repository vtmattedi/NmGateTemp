#pragma once
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "creds.h"
void wifi_init_sta(void);
bool wifi_is_connected(void);
// True once the WiFi driver is running, whether or not an AP was joined.
// ESP-NOW only needs this much, not an association.
bool wifi_is_started(void);
