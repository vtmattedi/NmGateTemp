#include "espmDNS.h"

#include "esp_log.h"
#include "mdns.h"

namespace NightmareGateway::mDNS
{
    namespace
    {
        constexpr const char *TAG = "mDNS";
        bool initialized = false;
    }

    esp_err_t begin(const char *hostname)
    {
        if (initialized)
            return ESP_OK;

        if (!hostname || !hostname[0])
            return ESP_ERR_INVALID_ARG;

        esp_err_t err = mdns_init();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
            return err;
        }

        err = mdns_hostname_set(hostname);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "mdns_hostname_set failed: %s", esp_err_to_name(err));
            mdns_free();
            return err;
        }

        err = mdns_instance_name_set("NightMare Network Gateway");
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "mdns_instance_name_set failed: %s",
                     esp_err_to_name(err));
            mdns_free();
            return err;
        }

        initialized = true;

        ESP_LOGI(TAG, "Available at %s.local", hostname);

        return ESP_OK;
    }

    void end()
    {
        if (!initialized)
            return;

        mdns_free();
        initialized = false;

        ESP_LOGI(TAG, "Stopped");
    }

    bool running()
    {
        return initialized;
    }
}