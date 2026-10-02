#pragma once

#include "esp_err.h"

namespace NightmareGateway::mDNS
{
    esp_err_t begin(const char *hostname);
    void end();

    bool running();
}