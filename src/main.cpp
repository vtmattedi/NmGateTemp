#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <vector>
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
#include "esp_system.h"
#include "System/chipTemperature.h"

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

// Long enough for "wifi connect <32 char ssid> <63 char password>" with quotes.
#define SERIAL_LINE_MAX 192
static char s_serialLine[SERIAL_LINE_MAX];
static size_t s_serialLen = 0;

// Reads the broker's published snapshot (refreshed twice a second) rather than
// walking its device table, which belongs to the gateway task.
static void printDeviceList(void)
{
    const uint64_t nowMs = (uint64_t)(esp_timer_get_time() / 1000);
    std::vector<EspBrokerDeviceInfo> devices;
    espBroker_snapshotDevices(devices);

    ESP_LOGI("Serial", "%u session(s):", (unsigned)devices.size());
    for (const EspBrokerDeviceInfo &device : devices)
    {
        // The name a device reported on "<name>/status", else its MAC.
        const char *who = device.name.empty() ? device.mac.c_str() : device.name.c_str();
        char rtt[16] = "n/a";
        if (device.hasRtt)
            snprintf(rtt, sizeof(rtt), "%.1fms", device.rttMs);
        char signal[16] = "n/a";
        if (device.hasRssi)
            snprintf(signal, sizeof(signal), "%.0fdBm", device.avgRssi);

        ESP_LOGI("Serial", "  %-18s cid=%u state=%s subs=%u rssi=%s rtt=%s lastSeen=%llus lastWill=%s%s", who,
                 (unsigned)device.cid, NightMare::connectionStateName(device.state), (unsigned)device.subscriptions.size(),
                 signal, rtt, (unsigned long long)((nowMs - device.lastSeenMs) / 1000),
                 device.hasLastWill ? "yes" : "no", device.disconnectCandidate ? " [disconnect candidate]" : "");
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

// Splits `args` in place into whitespace separated words. A word may be
// "double quoted" to hold spaces (\" and \\ are escapes inside quotes), and ""
// is an empty word. Returns how many words were found, at most `max`.
static int splitArgs(char *args, char **words, int max)
{
    int count = 0;
    char *p = args;
    while (*p != '\0' && count < max)
    {
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            break;

        if (*p == '"')
        {
            char *write = ++p;
            words[count++] = write;
            while (*p != '\0' && *p != '"')
            {
                if (*p == '\\' && (p[1] == '"' || p[1] == '\\'))
                    p++;
                *write++ = *p++;
            }
            if (*p == '"')
                p++;
            *write = '\0';
        }
        else
        {
            words[count++] = p;
            while (*p != '\0' && *p != ' ' && *p != '\t')
                p++;
            if (*p != '\0')
                *p++ = '\0';
        }
    }
    return count;
}

static void cmdGateway(const char *args)
{
    if (strcmp(args, "off") == 0)
    {
        nightmare_gateway_enable(false);
        ESP_LOGI("Serial", "Gateway turning off: ESP-NOW and MQTT stop, sessions are dropped");
    }
    else if (strcmp(args, "on") == 0)
    {
        nightmare_gateway_enable(true);
        ESP_LOGI("Serial", "Gateway turning on: devices have to reconnect");
    }
    else if (args[0] != '\0')
    {
        ESP_LOGW("Serial", "usage: gateway [on|off]");
        return;
    }
    else
        ESP_LOGI("Serial", "gateway %s (%s)", nightmare_gateway_enabled() ? "on" : "off",
                 nightmare_gateway_state_name(nightmare_gateway_state()));
}

static const char *authModeName(wifi_auth_mode_t mode)
{
    switch (mode)
    {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ent";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    case WIFI_AUTH_OWE: return "OWE";
    default: return "other";
    }
}

// [=======   ] for -55 dBm: full at -50 and better, empty at -100.
static void signalBar(int rssi, char *out, size_t size)
{
    const int width = 10;
    int quality = (rssi + 100) * 2;
    quality = quality < 0 ? 0 : quality > 100 ? 100 : quality;
    const int filled = (quality * width + 50) / 100;
    snprintf(out, size, "[");
    for (int i = 0; i < width && strlen(out) + 2 < size; i++)
        strcat(out, i < filled ? "=" : " ");
    strcat(out, "]");
}

static void wifiScan(void)
{
    printf("Scanning (ESP-NOW pauses briefly)...\n");
    std::vector<wifi_ap_record_t> networks;
    const esp_err_t err = wifi_scan(networks);
    if (err != ESP_OK)
    {
        ESP_LOGW("Serial", "Scan failed: %s", esp_err_to_name(err));
        return;
    }
    std::sort(networks.begin(), networks.end(),
              [](const wifi_ap_record_t &a, const wifi_ap_record_t &b) { return a.rssi > b.rssi; });

    wifi_ap_record_t current = {};
    const bool haveCurrent = wifi_is_connected() && esp_wifi_sta_get_ap_info(&current) == ESP_OK;

    printf("\n %2s  %-12s  %8s  %3s  %-9s  %s\n", "#", "Signal", "dBm", "Ch", "Security", "SSID");
    printf(" --  ------------  --------  ---  ---------  ------------------------\n");
    int index = 1;
    for (const wifi_ap_record_t &ap : networks)
    {
        char bar[16];
        signalBar(ap.rssi, bar, sizeof(bar));
        const bool joined = haveCurrent && memcmp(ap.bssid, current.bssid, sizeof(ap.bssid)) == 0;
        const char *ssid = ap.ssid[0] != '\0' ? (const char *)ap.ssid : "<hidden>";
        printf(" %2d  %s  %4d dBm  %3u  %-9s  %s%s\n", index++, bar, ap.rssi, (unsigned)ap.primary,
               authModeName(ap.authmode), ssid, joined ? "   <- connected" : "");
    }
    printf("\n%u network(s) found\n", (unsigned)networks.size());
}

static void cmdWifi(const char *args)
{
    char line[SERIAL_LINE_MAX];
    strncpy(line, args, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    char *words[4];
    const int count = splitArgs(line, words, 4);

    if (count >= 1 && strcmp(words[0], "scan") == 0)
    {
        wifiScan();
        return;
    }
    if (count >= 2 && strcmp(words[0], "connect") == 0)
    {
        if (count > 3)
        {
            ESP_LOGW("Serial", "Too many arguments: put an ssid or password with spaces in \"quotes\"");
            return;
        }
        const char *password = count == 3 ? words[2] : "";
        const esp_err_t err = wifi_connect(words[1], password);
        if (err == ESP_ERR_INVALID_ARG)
            ESP_LOGW("Serial", "Invalid ssid (1-32 chars) or password (empty for open, else 8-63 chars)");
        else if (err != ESP_OK)
            ESP_LOGW("Serial", "Could not start connecting: %s", esp_err_to_name(err));
        else
            ESP_LOGI("Serial", "Connecting to \"%s\"%s - the log shows the result", words[1],
                     password[0] == '\0' ? " (open network)" : "");
        return;
    }
    ESP_LOGW("Serial", "usage: wifi scan | wifi connect <ssid> [password]   (quote values with spaces)");
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
    {"devices", "ESP-NOW sessions (name, signal, rtt)", cmdDevices},
    {"gateway", "on | off | (status)", cmdGateway},
    {"wifi", "scan | connect <ssid> [password]", cmdWifi},
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
                // Name only: the arguments can hold a Wi-Fi password.
                size_t nameLength = 0;
                while (start[nameLength] != '\0' && start[nameLength] != ' ' && start[nameLength] != '\t')
                    nameLength++;
                ESP_LOGI("Serial", "Received: %.*s%s", (int)nameLength, start, start[nameLength] != '\0' ? " ..." : "");
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
    bool tempOk = chipTempInit();
    if (!tempOk)
        ESP_LOGE(TEMPTAG, "Failed to initialize chip temperature sensor");
    wifi_init_sta();
    ntp_sync_start();

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
