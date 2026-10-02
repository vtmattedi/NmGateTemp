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

enum class NightMareGatewayState : uint8_t
{
    Stopped,
    WaitForWifiStack,
    StartEspNow,
    Running,
};

// The application owns the config source (creds.h, provisioning, ...); the
// gateway keeps what it needs from `config` and not the struct itself.
// pdFAIL when the config is unusable (e.g. espnowBroker without a valid PSK)
// or the task cannot start.
BaseType_t start_nightmare_gateway(const NightMareGatewayConfig &config);

// Thread-safe views of the retained-message vault, for the web UI.
size_t nightmare_gateway_vault_snapshot(std::vector<NightMare::RetainedEntry> &out);
size_t nightmare_gateway_vault_size();

// "gateway off": stops ESP-NOW (sessions dropped, no beacon) and MQTT. "on"
// starts them again; devices have to reconnect. Asynchronous: the gateway task
// does the work, watch nightmare_gateway_state() for Stopped / Running.
void nightmare_gateway_enable(bool enabled);
bool nightmare_gateway_enabled();

NightMareGatewayState nightmare_gateway_state();
const char *nightmare_gateway_state_name(NightMareGatewayState state);

// Publishes a gateway-originated message to everyone: the MQTT broker (when
// connected) and every connected ESP-NOW device that has at least one
// subscription filter matching the topic. Retains it in the vault if flagged
// persistent. Must be called from the gateway task (it walks the device
// table). True if it reached at least one side (a local send, or queued for MQTT).
bool publishToAll(const NightMare::Message &message);

// Answers a "<x>/in" request on "<x>/out" -- the same convention a
// NightMareNetwork device's own console uses -- reaching whoever is
// subscribed to the reply topic, locally and/or over MQTT. Exposed for any
// future gateway/... command handler to reuse, not just the debug one.
// By default the answer goes back only to the side `current` came from (an
// ESP-NOW device -> local subscribers; MQTT -> the broker); Both publishes it
// everywhere like publishToAll().
enum class ReplyTarget : uint8_t
{
    Source,
    Both,
};
void replyMessage(const NightMare::Message &current, const NightMare::Message &response,
                  ReplyTarget target = ReplyTarget::Source);

// Same delivery rule as replyMessage, but `response` keeps the topic it already
// has (no "/in" -> "/out" rewrite).
bool respondTo(const NightMare::Message &current, const NightMare::Message &response,
               ReplyTarget target = ReplyTarget::Source);
