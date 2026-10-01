#include <stdio.h>
#include <string.h>
#include "LedController/ledController.h"
#include "EspWifi/espWifi.h"
#include "TimeSync/timeSync.h"
#include "NightmareGateway/NightmareGateway.h"
#include "NightmareGateway/EspNow/espBroker.h"
#include "NightmareGateway/EspMqtt/espMqtt.h"
#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "creds.h"
#include "driver/temperature_sensor.h"
#include "esp_system.h"

#ifndef NM_ESPNOW_PSK
#error "Define NM_ESPNOW_PSK (the ESP-NOW network key, same on every device) in include/creds.h"
#endif

void loop();

// The application owns where the gateway's configuration comes from; today
// that is creds.h. The gateway copies what it needs.
static NightMareGatewayConfig gatewayConfig()
{
    static const uint8_t psk[] = NM_ESPNOW_PSK;
    NightMareGatewayConfig config;
    config.espnowConfig.psk = psk;
    config.espnowConfig.pskLength = sizeof(psk) - 1; // not the terminator
    return config;
}

void initSequence()
{
    // Initialize the LED controller
    ws2812_init();
    // Set initial color and brightness
    ws2812_set_color(COLOR_WHITE);
    ws2812_set_brightness(20);
    vTaskDelay(pdMS_TO_TICKS(1000)); // Wait for 1 second
    ws2812_set_color(COLOR_RED);
    vTaskDelay(pdMS_TO_TICKS(1000)); // Wait for 1 second
    ws2812_set_color(COLOR_GREEN);
    vTaskDelay(pdMS_TO_TICKS(1000)); // Wait for 1 second
    ws2812_set_color(COLOR_BLUE);
    vTaskDelay(pdMS_TO_TICKS(1000)); // Wait for 1 second
    ws2812_set_color(COLOR_BLACK);
}

static const gpio_num_t BLINK_GPIO = GPIO_NUM_8;
static const gpio_num_t BLINK_GPIO2 = GPIO_NUM_15;

#define SERIAL_LINE_MAX 64
static char s_serialLine[SERIAL_LINE_MAX];
static size_t s_serialLen = 0;

static void printDeviceList(void)
{
    const uint64_t nowMs = (uint64_t)(esp_timer_get_time() / 1000);
    const uint8_t count = espBroker_deviceCount();

    ESP_LOGI("Serial", "%u session(s):", count);
    for (uint8_t i = 0; i < count; i++)
    {
        const NightMare::Device *device = espBroker_deviceAt(i);
        if (device == nullptr)
            continue;

        ESP_LOGI("Serial", "  %s cid=%u state=%s subs=%u lastSeen=%llus lastWill=%s",
                 device->address().toString().c_str(), (unsigned)device->cid(),
                 NightMare::connectionStateName(device->state()), (unsigned)device->subscriptionCount(),
                 (unsigned long long)((nowMs - device->lastSeenAtMs()) / 1000),
                 device->hasLastWill() ? "yes" : "no");
    }
}

static void printStatus(void)
{
    const uint32_t sinceBeacon = espBroker_secondsSinceLastBeacon();
    char beaconAge[16];
    if (sinceBeacon == UINT32_MAX)
        snprintf(beaconAge, sizeof(beaconAge), "n/a");
    else
        snprintf(beaconAge, sizeof(beaconAge), "%lus ago", (unsigned long)sinceBeacon);

    ESP_LOGI("Serial", "wifi=%s mqtt=%s subscribers=%u beacon=%s (last %s)",
             wifi_is_connected() ? "up" : "down", mqtt_is_connected() ? "up" : "down",
             (unsigned)espBroker_subscriberCount(), espBroker_beaconActive() ? "active" : "inactive", beaconAge);
}

// --- Serial commands ---------------------------------------------------------
// One line = "<command> [args]". To add one: write a handler taking the
// (already trimmed, possibly empty) argument string and add a row below.

bool chipTempRead(float &temperatureC);
static bool s_tempLogEnabled = true; // the once-a-second chip temperature log in app_main

static void cmdHelp(const char *args);

static void cmdStatus(const char *) { printStatus(); }
static void cmdDevices(const char *) { printDeviceList(); }

static void cmdHeap(const char *)
{
    ESP_LOGI("Serial", "heap free=%lu min=%lu bytes", (unsigned long)esp_get_free_heap_size(),
             (unsigned long)esp_get_minimum_free_heap_size());
}

static void cmdTemp(const char *args)
{
    if (strcmp(args, "on") == 0 || strcmp(args, "off") == 0)
    {
        s_tempLogEnabled = strcmp(args, "on") == 0;
        ESP_LOGI("Serial", "temperature log %s", s_tempLogEnabled ? "on" : "off");
        return;
    }
    float tempC;
    if (chipTempRead(tempC))
        ESP_LOGI("Serial", "chip temperature %.1f C", tempC);
    else
        ESP_LOGW("Serial", "chip temperature unavailable");
}

static void cmdReboot(const char *)
{
    ESP_LOGI("Serial", "Rebooting...");
    esp_restart();
}

struct SerialCommand
{
    const char *name;
    const char *help;
    void (*run)(const char *args);
};

static const SerialCommand s_commands[] = {
    {"help", "list commands", cmdHelp},
    {"status", "wifi, mqtt, subscribers, beacon", cmdStatus},
    {"devices", "ESP-NOW sessions", cmdDevices},
    {"heap", "free / minimum free heap", cmdHeap},
    {"temp", "chip temperature now; 'temp on|off' toggles the periodic log", cmdTemp},
    {"reboot", "restart the gateway", cmdReboot},
};

static void cmdHelp(const char *)
{
    for (const SerialCommand &command : s_commands)
        ESP_LOGI("Serial", "  %-8s %s", command.name, command.help);
}

// `line` is trimmed and non-empty; it is split in place into name + args.
static void handleSerialLine(char *line)
{
    char *args = line;
    while (*args != '\0' && *args != ' ' && *args != '\t')
        args++;
    if (*args != '\0')
    {
        *args++ = '\0';
        while (*args == ' ' || *args == '\t')
            args++;
    }

    for (const SerialCommand &command : s_commands)
    {
        if (strcmp(line, command.name) == 0)
        {
            command.run(args);
            return;
        }
    }
    ESP_LOGW("Serial", "Unknown command: %s (try 'help')", line);
}

// Non-blocking: uart_read_bytes with a 0 tick timeout returns immediately with
// however many bytes (0 or more) are already in the driver's RX buffer,
// instead of the Arduino Serial API this used to (wrongly) assume was
// available in an ESP-IDF project.
static void pumpSerial(void)
{
    uint8_t c;
    while (uart_read_bytes(UART_NUM_0, &c, 1, 0) == 1)
    {
        if (c == '\r')
            continue;

        if (c == '\n')
        {
            s_serialLine[s_serialLen] = '\0';

            // Trim leading/trailing whitespace in place.
            char *start = s_serialLine;
            while (*start == ' ' || *start == '\t')
                start++;
            char *end = start + strlen(start);
            while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
                *--end = '\0';

            if (start[0] != '\0')
            {
                ESP_LOGI("Serial", "Received: %s", start);
                handleSerialLine(start);
            }
            s_serialLen = 0;
            continue;
        }

        // Overflow just gets silently dropped until the next newline, rather
        // than overrunning the buffer or wrapping onto itself.
        if (s_serialLen < SERIAL_LINE_MAX - 1)
            s_serialLine[s_serialLen++] = (char)c;
    }
}

static const char *TEMPTAG = "chip_temp";

static temperature_sensor_handle_t s_tempSensor = nullptr;

bool chipTempInit()
{
    temperature_sensor_config_t config =
        TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 90);

    esp_err_t err =
        temperature_sensor_install(&config, &s_tempSensor);

    if (err != ESP_OK)
    {
        ESP_LOGE(TEMPTAG, "temperature_sensor_install failed: %s",
                 esp_err_to_name(err));
        return false;
    }

    err = temperature_sensor_enable(s_tempSensor);

    if (err != ESP_OK)
    {
        ESP_LOGE(TEMPTAG, "temperature_sensor_enable failed: %s",
                 esp_err_to_name(err));

        temperature_sensor_uninstall(s_tempSensor);
        s_tempSensor = nullptr;
        return false;
    }

    return true;
}

bool chipTempRead(float &temperatureC)
{
    if (s_tempSensor == nullptr)
        return false;

    esp_err_t err =
        temperature_sensor_get_celsius(
            s_tempSensor,
            &temperatureC);

    if (err != ESP_OK)
    {
        ESP_LOGW(TEMPTAG, "temperature read failed: %s",
                 esp_err_to_name(err));
        return false;
    }

    return true;
}

void chipTempDeinit()
{
    if (s_tempSensor == nullptr)
        return;

    temperature_sensor_disable(s_tempSensor);
    temperature_sensor_uninstall(s_tempSensor);
    s_tempSensor = nullptr;
}

uint64_t millis()
{
    return static_cast<uint64_t>(esp_timer_get_time() / 1000);
}

extern "C" void app_main(void)
{
    printf("Hello world\n");
    // Installing the driver (rather than relying on the boot-time console
    // setup, which only covers output) is what makes uart_read_bytes work;
    // stdout keeps working over the same UART either way.
    uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);

    ws2812_init();
    if (start_nightmare_gateway(gatewayConfig()) != pdPASS)
        ESP_LOGE("main", "Gateway did not start: check its configuration");
    wifi_init_sta();
    ntp_sync_start();
    bool tempOk = chipTempInit();
    if (!tempOk)
        ESP_LOGE(TEMPTAG, "Failed to initialize chip temperature sensor");

    int lastTempLog = 0;
    while (1)
    {
        loop(); // big ol' arduino loop. just better for reasoning about the code.
        // A real sleep, not taskYIELD(): this task never blocks on its own
        // (pumpSerial's read is non-blocking), so yield alone just hands
        // control straight back with nothing else at this priority to run,
        // starving IDLE0 and tripping its watchdog. 10ms is still well under
        // human typing speed.

        if (millis() - lastTempLog >= 1000) // Log every 1000 ticks (1 second)
        {
            lastTempLog = millis();
            float tempC;
            if (!s_tempLogEnabled)
                ; // silenced with 'temp off'
            else if (tempOk && chipTempRead(tempC))
                ESP_LOGI(TEMPTAG, "Chip temperature: %.1f C", tempC);
            else if (tempOk)
                ESP_LOGW(TEMPTAG, "Failed to read chip temperature");
        }
        vTaskDelay(100); // very small delay
    }
}

void loop()
{
    pumpSerial();
    
}
