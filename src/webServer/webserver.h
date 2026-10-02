#pragma once

#include <cstdint>

#include "esp_err.h"

namespace web_server
{
    // Safe to call again after Wi-Fi obtains a new IP address.
    esp_err_t start(uint16_t port = 80);

    // Safe to call when the server is already stopped.
    void stop();
}
