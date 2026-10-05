#pragma once
#include "esp_log.h"

// The only log lines meant to be read in normal operation: things that happen
// to the network (Wi-Fi up/down, a client connecting, disconnecting or failing
// to authenticate). main() silences everything else below warnings, so log a
// state change with ESP_LOGI(EVENT_TAG, ...) and keep chatter on its own tag.
#define EVENT_TAG "event"
