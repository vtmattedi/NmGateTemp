#include "timeSync.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"

static const char *TAG = "timeSync";

// Fixed GMT-3 offset (Brasilia time, no DST observed since 2019).
#define NM_TIMEZONE "<-03>3"

esp_err_t ntp_sync_start(uint32_t timeout_ms)
{
    setenv("TZ", NM_TIMEZONE, 1);
    tzset();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.start = true;
    config.server_from_dhcp = false;

    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to init SNTP: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Waiting for NTP time sync (%s)...", NM_TIMEZONE);
    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "NTP sync failed/timed out: %s", esp_err_to_name(err));
        return err;
    }

    time_t now = time(NULL);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    ESP_LOGI(TAG, "Time synced: %s", buf);

    return ESP_OK;
}
