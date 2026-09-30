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
void loop();

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

static void handleSerialLine(const char *line)
{
    if (strcmp(line, "reboot") == 0)
    {
        ESP_LOGI("Serial", "Rebooting...");
        esp_restart();
    }
    else if (strcmp(line, "devices") == 0)
    {
        printDeviceList();
    }
    else if (strcmp(line, "status") == 0)
    {
        printStatus();
    }
    else
    {
        ESP_LOGW("Serial", "Unknown command: %s", line);
    }
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

extern "C" void app_main(void)
{
    printf("Hello world\n");
    // Installing the driver (rather than relying on the boot-time console
    // setup, which only covers output) is what makes uart_read_bytes work;
    // stdout keeps working over the same UART either way.
    uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);

    initSequence();
    start_nightmare_gateway();
    wifi_init_sta();
    ntp_sync_start();
    gpio_reset_pin(BLINK_GPIO);
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
    gpio_reset_pin(BLINK_GPIO2);
    gpio_set_direction(BLINK_GPIO2, GPIO_MODE_OUTPUT);
    gpio_set_level(BLINK_GPIO2, 0); // Turn LED ON
    while (1)
    {
        loop(); // big ol' arduino loop. just better for reasoning about the code.
        // A real sleep, not taskYIELD(): this task never blocks on its own
        // (pumpSerial's read is non-blocking), so yield alone just hands
        // control straight back with nothing else at this priority to run,
        // starving IDLE0 and tripping its watchdog. 10ms is still well under
        // human typing speed.
        vTaskDelay(100 ); //very small delay
    }
}

void loop()
{
    pumpSerial();
}
