#pragma once
#include <stdio.h>
#include <time.h>
#include "esp_err.h"

// Blocks (up to timeout_ms) until SNTP has synced the system clock.
// Requires an active network connection (call after wifi_init_sta()).
// Timezone is fixed to GMT-3 (Brasilia time, no DST).
esp_err_t ntp_sync_start(uint32_t timeout_ms = 10000);
