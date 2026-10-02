#include <stdio.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mdns.h"

static const char *TAG = "mdns_example";

void wifi_init_and_connect(void) {
    // Standard Wi-Fi initialization code goes here...
    // Ensure esp_netif_init() has been called before initializing mDNS.
}

void initialise_mdns(void) {
    // 1. Initialize the mDNS service
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS Init failed: %d", err);
        return;
    }

    // 2. Set the global hostname (resolves to custom-esp32s3.local)
    err = mdns_hostname_set("custom-esp32s3");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set mDNS hostname: %d", err);
        return;
    }
    ESP_LOGI(TAG, "mDNS hostname set to: custom-esp32s3.local");

    // 3. Set the default instance name (friendly name shown in network scanners)
    mdns_instance_name_set("ESP32-S3 Smart Device");

    // 4. (Optional) Advertise a specific network service (e.g., HTTP Web Server)
    // Structure: mdns_service_add(instance, service_type, protocol, port, txt_records, num_txt_records)
    mdns_txt_item_t serviceTxtData[2] = {
        {"board", "esp32s3"},
        {"version", "1.0"}
    };

    err = mdns_service_add(NULL, "_http", "_tcp", 80, serviceTxtData, 2);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add mDNS service: %d", err);
    } else {
        ESP_LOGI(TAG, "mDNS HTTP service advertised on port 80");
    }
}