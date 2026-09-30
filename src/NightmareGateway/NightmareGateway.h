#pragma once
#define GATEWAY_TASK_STACK_SIZE 8192
#define GATEWAY_TASK_PRIORITY 5
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
//watchdog timer
#include "esp_task_wdt.h"
#include "NightmareGateway/NightMare/Message.h"

struct NightMareMqttBrokerConfig 
{
    uint8_t maxClients; //not used do not touch
};

struct NightMareEspNowConfig
{
    // The ESP-NOW network key, shared with every device (16..64 bytes).
    // start_nightmare_gateway() copies it: the caller's buffer may be wiped
    // or freed once that returns.
    const uint8_t *psk = nullptr;
    size_t pskLength = 0;
};

struct NightMareMqttConfig
{
    const char *brokerAddress = nullptr;
    uint16_t brokerPort = 1883;
    const char *username = nullptr;
    const char *password = nullptr;
};

struct NightMareGatewayConfig
{
    bool espnowBroker = true;
    bool mqttClient = true;
    bool mqttBroker = true; // not implemented yet do not implent.
    NightMareEspNowConfig espnowConfig;
};

// The application owns the config source (creds.h, provisioning, ...); the
// gateway keeps what it needs from `config` and not the struct itself.
// pdFAIL when the config is unusable (e.g. espnowBroker without a valid PSK)
// or the task cannot start.
BaseType_t start_nightmare_gateway(const NightMareGatewayConfig &config);

// Answers a "<x>/in" request on "<x>/out" -- the same convention a
// NightMareNetwork device's own console uses -- reaching whoever is
// subscribed to the reply topic, locally and/or over MQTT. Exposed for any
// future gateway/... command handler to reuse, not just the debug one.
void replyMessage(const NightMare::Message &current, const NightMare::Message &response);
