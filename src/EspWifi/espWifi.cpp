#include "espWifi.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "webServer/webserver.h"
#include <atomic>

static const char *TAG = "espWifi";
static EventGroupHandle_t wifi_event_group;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_MAX_RETRY 5

static int retry_count = 0;
static std::atomic<bool> s_connected{false};
static std::atomic<bool> s_started{false};

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        s_connected.store(false, std::memory_order_release);
        web_server::stop();
        if (retry_count < WIFI_MAX_RETRY)
        {
            esp_wifi_connect();
            retry_count++;
            ESP_LOGI(TAG, "Retrying connection to AP (%d/%d)", retry_count, WIFI_MAX_RETRY);
        }
        else
        {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
            NightmareGateway::mDNS::end();
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        retry_count = 0;
        s_connected.store(true, std::memory_order_release);
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        NightmareGateway::mDNS::begin("nightmare-gateway");
        esp_err_t web_server_result = web_server::start();
        if (web_server_result != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to start web server: %s",
                     esp_err_to_name(web_server_result));
        }
    }
}

bool wifi_is_connected(void)
{
    return s_connected.load(std::memory_order_acquire);
}

bool wifi_is_started(void)
{
    return s_started.load(std::memory_order_acquire);
}

esp_err_t wifi_scan(std::vector<wifi_ap_record_t> &out)
{
    if (!wifi_is_started())
        return ESP_ERR_WIFI_NOT_STARTED;

    wifi_scan_config_t config = {};
    config.show_hidden = true;
    esp_err_t err = esp_wifi_scan_start(&config, true);
    if (err != ESP_OK)
        return err;

    uint16_t count = 0;
    err = esp_wifi_scan_get_ap_num(&count);
    if (err != ESP_OK)
        return err;
    if (count > 40)
        count = 40; // the rest is discarded by get_ap_records
    out.assign(count, wifi_ap_record_t{});
    err = esp_wifi_scan_get_ap_records(&count, out.data());
    out.resize(err == ESP_OK ? count : 0);
    return err;
}

esp_err_t wifi_connect(const char *ssid, const char *password)
{
    if (!wifi_is_started())
        return ESP_ERR_WIFI_NOT_STARTED;
    if (password == nullptr)
        password = "";
    const size_t ssidLength = ssid != nullptr ? strlen(ssid) : 0;
    const size_t passwordLength = strlen(password);
    wifi_config_t config = {};
    if (ssidLength == 0 || ssidLength > sizeof(config.sta.ssid) ||
        passwordLength > 63 || (passwordLength != 0 && passwordLength < 8))
        return ESP_ERR_INVALID_ARG;

    memcpy(config.sta.ssid, ssid, ssidLength);
    memcpy(config.sta.password, password, passwordLength);
    config.sta.threshold.authmode = passwordLength == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err != ESP_OK)
        return err;

    // Leaving the old network raises DISCONNECTED, whose handler reconnects
    // with the new config. When nothing was connected there is no such event.
    const bool wasConnected = wifi_is_connected();
    retry_count = 0;
    esp_wifi_disconnect();
    if (!wasConnected)
        esp_wifi_connect();
    return ESP_OK;
}

void wifi_init_sta(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid, DEFAULT_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, DEFAULT_PASSWORD, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_started.store(true, std::memory_order_release);

    ESP_LOGI(TAG, "Connecting to SSID: %s", DEFAULT_SSID);

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "Connected to SSID: %s", DEFAULT_SSID);
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGE(TAG, "Failed to connect to SSID: %s", DEFAULT_SSID);
    }
}
