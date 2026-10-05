#include "NightmareGateway.h"
#include "EspWifi/espWifi.h"
#include "LedController/ledController.h"
#include "NightmareGateway/EspMqtt/espMqtt.h"
#include "NightmareGateway/EspNow/espBroker.h"
#include "NightmareGateway/NightMare/Message.h"
#include "NightmareGateway/NightMare/Time.h"
#include "NightmareGateway/GatewayStats.h"
#include "NightmareGateway/EspNow/Auth.h"
#include "freertos/queue.h"
#include <strings.h>
#include "esp_timer.h"
#include "esp_log.h"
#include <atomic>
#include <new>
#include <utility>

static const char *TAG = "gateway";

// Only has to absorb one tick's worth of arrivals, but subscribing to "#" makes
// the broker replay every retained topic at once, so the burst is large.
#define INBOUND_QUEUE_DEPTH 64
#define STATUS_BRIGHTNESS 20
#define DEBUG_TOPIC "gateway/debug/in"
#define CONTROL_REQUEST_TOPIC "Control/request"

TaskHandle_t gatewayTaskHandle = NULL;

static QueueHandle_t s_inbound = NULL; // NightMare::Message* from the MQTT task
static NightMare::MessageVault s_vault;
static bool s_mqttStarted = false;
static std::atomic<NightMareGatewayState> s_gatewayState{NightMareGatewayState::Stopped};
static std::atomic<bool> s_enabled{true}; // cleared by "gateway off", see nightmare_gateway_enable()

// What start_nightmare_gateway() kept from its config. The PSK copy only lives
// until the gateway task hands it to espBroker_init(), which keeps its own.
static bool s_espnowEnabled = true;
static bool s_mqttEnabled = true;
static uint8_t s_pendingPsk[NightMare::EspNowAuth::MaxPskLength];
static size_t s_pendingPskLength = 0;

// Keeps a copy of anything flagged persistent so a device that subscribes later
// still gets the last value, the way a broker would replay retained messages.
static void retainIfPersistent(const NightMare::Message &message)
{
    if (!message.persistent)
        return;

    s_vault.retainMessage(message.topic, message.payload, (uint32_t)message.payload.size(), message.direction,
                          (uint64_t)(esp_timer_get_time() / 1000));
}

// Where a message came in: the ESP-NOW side (a device published it) or the
// MQTT broker. onMessage() stamps this into Message::direction on arrival.
enum class Origin : uint8_t
{
    Local,
    Remote,
};

static Origin originOf(const NightMare::Message &message)
{
    return message.direction == NightMare::Direction::REMOTE_TO_LOCAL ? Origin::Remote : Origin::Local;
}

static bool sendLocal(const NightMare::Message &message)
{
    // Every connected local device with a filter matching the topic.
    return espBroker_broadcastMessage(message);
}

static bool sendRemote(const NightMare::Message &message)
{
    if (!mqtt_is_connected() || !mqtt_publish(message))
        return false;
    gwCount(g_gatewayStats.msgsToRemote);
    return true;
}

bool publishToAll(const NightMare::Message &message)
{
    retainIfPersistent(message);
    const bool local = sendLocal(message);
    const bool remote = sendRemote(message);
    return local || remote;
}

// Same side the request came from, so a question asked on one side is not
// answered on the other. Delivery is still topic based: there is no
// unicast-to-sender path, so the asker has to be subscribed to the reply topic.
bool respondTo(const NightMare::Message &current, const NightMare::Message &response, ReplyTarget target)
{
    if (target == ReplyTarget::Both)
        return publishToAll(response);

    retainIfPersistent(response);
    return originOf(current) == Origin::Remote ? sendRemote(response) : sendLocal(response);
}

// A request on "<x>/in" is answered on "<x>/out" -- the same convention a
// NightMareNetwork device's own console uses (both the unaddressed console/in
// and the id-addressed console/controlled/<id>/in forms; the id just rides
// along inside <x>). Goes back to the side the request came from (see respondTo),
// unless `target` says Both.
void replyMessage(const NightMare::Message &current, const NightMare::Message &response, ReplyTarget target)
{
    static const std::string suffix = "/in";
    if (current.topic.size() <= suffix.size() ||
        current.topic.compare(current.topic.size() - suffix.size(), suffix.size(), suffix) != 0)
    {
        ESP_LOGW(TAG, "Can't reply to '%s', it doesn't end in /in", current.topic.c_str());
        return;
    }

    NightMare::Message reply = response;
    reply.topic = current.topic.substr(0, current.topic.size() - suffix.size()) + "/out";
    reply.direction = NightMare::Direction::LOCAL_TO_REMOTE;

    respondTo(current, reply, target);
}

// {"wifi":bool,"mqtt":bool,"subscribers":N,"devices":[{"mac","cid","state","subs","lastSeenS","lastWill"},...]}
static NightMare::Message buildDebugTelemetry()
{
    std::string json = "{\"wifi\":";
    json += wifi_is_connected() ? "true" : "false";
    json += ",\"mqtt\":";
    json += mqtt_is_connected() ? "true" : "false";
    json += ",\"subscribers\":" + std::to_string(espBroker_subscriberCount());
    json += ",\"devices\":[";

    const uint64_t nowMs = (uint64_t)(esp_timer_get_time() / 1000);
    const uint8_t deviceCount = espBroker_deviceCount();
    for (uint8_t i = 0; i < deviceCount; i++)
    {
        const NightMare::Device *device = espBroker_deviceAt(i);
        if (device == nullptr)
            continue;
        if (i > 0)
            json += ",";

        char row[160];
        snprintf(row, sizeof(row), "{\"mac\":\"%s\",\"cid\":%u,\"state\":\"%s\",\"subs\":%u,\"lastSeenS\":%llu,\"lastWill\":%s}",
                 device->address().toString().c_str(), (unsigned)device->cid(),
                 NightMare::connectionStateName(device->state()), (unsigned)device->subscriptionCount(),
                 (unsigned long long)((nowMs - device->lastSeenAtMs()) / 1000),
                 device->hasLastWill() ? "true" : "false");
        json += row;
    }
    json += "]}";

    NightMare::Message telemetry;
    telemetry.payload.assign(json.begin(), json.end());
    return telemetry;
}

// "Control/request" from a local device, payload naming what it wants. Answers
// go out on "Control/<what>" back to the asking side only; the request is then
// consumed, so a remote Control service does not answer it a second time.
// Returns false to let the request pass through (unknown request, or nothing
// valid to answer with) and be bridged to MQTT.
//   time -> Control/time {"timestamp": <unix seconds, UTC>, "offset": 0}
//           NightMareNetwork's syncTime() requires both fields; it only uses
//           the timestamp.
static bool answerControlRequest(const NightMare::Message &request)
{
    std::string what(request.payload.begin(), request.payload.end());
    while (!what.empty() && isspace((unsigned char)what.back()))
        what.pop_back();

    if (strcasecmp(what.c_str(), "time") == 0)
    {
        if (!NightMare::valid())
        {
            ESP_LOGW(TAG, "Time requested but the clock is not synced yet");
            return false;
        }
        char json[48];
        snprintf(json, sizeof(json), "{\"timestamp\":%lld,\"offset\":0}", (long long)NightMare::now());

        NightMare::Message reply;
        reply.topic = "Control/time";
        reply.payload.assign(json, json + strlen(json));
        reply.direction = NightMare::Direction::LOCAL_TO_REMOTE;
        respondTo(request, reply);
        return true;
    }
    return false;
}

// Messages the gateway can answer itself. A handler returns true when it
// resolved the message (it replied, or deliberately swallowed it): the message
// is then consumed and goes no further. False means "not mine / cannot answer",
// and the message escalates: routed to local subscribers and bridged to MQTT
// as usual. To resolve another request locally, add a row.
struct LocalHandler
{
    const char *filter;
    bool localOnly; // only for messages that came from an ESP-NOW device
    bool (*handle)(const NightMare::Message &message);
};

static bool answerDebugRequest(const NightMare::Message &request)
{
    replyMessage(request, buildDebugTelemetry());
    return true;
}

static const LocalHandler s_localHandlers[] = {
    {DEBUG_TOPIC, false, answerDebugRequest},
    {CONTROL_REQUEST_TOPIC, true, answerControlRequest},
};

static bool resolveLocally(const NightMare::Message &message)
{
    for (const LocalHandler &handler : s_localHandlers)
    {
        if (handler.localOnly && originOf(message) != Origin::Local)
            continue;
        if (topicMatchesPattern(message.topic, handler.filter) && handler.handle(message))
            return true;
    }
    return false;
}

// The one place a message from either side is handled, before it is routed:
// device == nullptr means it arrived over MQTT, a non-null device means it
// arrived from that local ESP-NOW peer over ESP-NOW. Order: try to resolve it
// locally, and only if the gateway cannot, route and bridge it.
void onMessage(const NightMare::Device *device, NightMare::Message message)
{
    gwCount(device != nullptr ? g_gatewayStats.msgsFromLocal : g_gatewayStats.msgsFromRemote);
    message.direction = device != nullptr ? NightMare::Direction::LOCAL_TO_REMOTE : NightMare::Direction::REMOTE_TO_LOCAL;

    if (resolveLocally(message))
        return;

    retainIfPersistent(message);

    // Broker stage: every local device subscribed to this topic, minus
    // whoever produced it, which is what stops a message bouncing back to
    // its own sender. A remote (MQTT) message has already reached its far
    // side by arriving here at all.
    espBroker_broadcastMessage(message, device);

    if (device != nullptr)
        sendRemote(message);
}

void espBroker_onMessage(NightMare::Device *device, NightMare::Message message)
{
    ESP_LOGI(TAG, "Local '%s' (%d bytes) from %s%s", message.topic.c_str(),
             (int)message.payload.size(), device->address().toString().c_str(), message.persistent ? " [retained]" : "");

    if (topicMatchesPattern(message.topic, "+/status"))
    {
        std::string name = message.topic.substr(0, message.topic.size() - 7);
        ESP_LOGI(TAG, "Device %s reports status '%s'", name.c_str(), std::string(message.payload.begin(), message.payload.end()).c_str());
        // Do something with the status update, e.g., update a display or trigger an action.
        if (device)
        {
            device->setAssumedName(name);
        }
    }
    onMessage(device, std::move(message));
}

// A device just subscribed, so replay whatever we have retained below its filter.
void espBroker_onSubscribe(const NightMare::Device *device, const std::string &filter)
{
    std::vector<NightMare::Message> retained;
    if (!s_vault.getMessagesForTopic(filter, retained))
        return;

    for (const NightMare::Message &message : retained)
        espBroker_sendMessage(device, message);
}

// Runs in the esp-mqtt task, so hand the message to the gateway task instead of
// touching the device table from here.
void mqtt_onMessage(NightMare::Message message)
{
    if (s_inbound == NULL)
        return;
    ESP_LOGI(TAG, "MQTT '%s' (%d bytes) from %s", message.topic.c_str(),
             (int)message.payload.size(), message.persistent ? " [retained]" : "");

    NightMare::Message *copy = new (std::nothrow) NightMare::Message(std::move(message));
    if (copy == nullptr)
    {
        ESP_LOGW(TAG, "Unable to allocate inbound message envelope");
        return;
    }
    if (xQueueSend(s_inbound, &copy, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "Inbound queue full, dropping '%s'", message.topic.c_str());
        delete copy;
        return;
    }
    if (gatewayTaskHandle != NULL)
        xTaskNotifyGive(gatewayTaskHandle); // drain now, not at the next tick
}

static void drainInbound(void)
{
    NightMare::Message *message = NULL;
    while (xQueueReceive(s_inbound, &message, 0) == pdTRUE)
    {
        onMessage(nullptr, std::move(*message));
        delete message;
        // A reconnect replays every retained topic at once, and each one can
        // take several bounded ESP-NOW sends; don't let the burst look like a hang.
        esp_task_wdt_reset();
    }
}

static void drainInboundDiscarding(void)
{
    NightMare::Message *message = NULL;
    while (xQueueReceive(s_inbound, &message, 0) == pdTRUE)
        delete message;
}

// Everything the gateway runs is stopped, so the radio and the broker see
// nothing from it. The vault is kept: it is the gateway's memory, not a session.
static void stopServices(void)
{
    ESP_LOGW(TAG, "Gateway stopping");
    espBroker_shutdown();
    if (s_mqttStarted)
    {
        mqtt_stop();
        s_mqttStarted = false;
    }
    drainInboundDiscarding();
}

static void updateStatusLed(void)
{
    const bool remoteUp = mqtt_is_connected();
    const bool localUp = espBroker_subscriberCount() > 0;

    Color color = COLOR_RED;
    if (remoteUp && localUp)
        color = COLOR_GREEN;
    else if (remoteUp)
        color = COLOR_BLUE;
    else if (localUp)
        color = COLOR_PINK;

    static bool initialised = false;
    static Color current = {};
    if (initialised && color.r == current.r && color.g == current.g && color.b == current.b)
        return;

    current = color;
    initialised = true;
    ws2812_set_color(color);
}

void gateway_task(void *pvParameters)
{
    esp_task_wdt_add(NULL);
    ws2812_set_brightness(STATUS_BRIGHTNESS);

    NightMareGatewayState state = NightMareGatewayState::WaitForWifiStack;
    s_gatewayState.store(state, std::memory_order_release);

    while (1)
    {
        const bool wanted = s_enabled.load(std::memory_order_acquire);
        if (!wanted && state != NightMareGatewayState::Stopped)
        {
            stopServices();
            state = NightMareGatewayState::Stopped;
        }
        else if (wanted && state == NightMareGatewayState::Stopped)
        {
            ESP_LOGI(TAG, "Gateway starting");
            state = NightMareGatewayState::WaitForWifiStack;
        }

        switch (state)
        {
        case NightMareGatewayState::WaitForWifiStack:
            // ESP-NOW only needs the radio running, so it comes up before (and
            // independently of) any association with an access point.
            if (wifi_is_started())
                state = s_espnowEnabled ? NightMareGatewayState::StartEspNow : NightMareGatewayState::Running;
            break;

        case NightMareGatewayState::StartEspNow:
            {
                // The first time the key comes from the config; after a
                // "gateway off"/"on" the broker still holds its own copy.
                const bool firstStart = s_pendingPskLength != 0;
                const bool up = firstStart ? espBroker_init(s_pendingPsk, s_pendingPskLength) : espBroker_resume();
                if (!up)
                    ESP_LOGE(TAG, "ESP-NOW unavailable%s", s_mqttEnabled ? ", running MQTT only" : "");
                NightMare::EspNowAuth::wipe(s_pendingPsk, sizeof(s_pendingPsk));
                s_pendingPskLength = 0;
            }
            state = NightMareGatewayState::Running;
            break;

        case NightMareGatewayState::Running:
            if (s_mqttEnabled && !s_mqttStarted && wifi_is_connected())
            {
                ESP_LOGI(TAG, "Network up, starting MQTT");
                mqtt_init();
                s_mqttStarted = true;
            }
            espBroker_process();
            drainInbound();
            break;

        case NightMareGatewayState::Stopped:
            break;
        }

        s_gatewayState.store(state, std::memory_order_release);

        updateStatusLed();
        esp_task_wdt_reset();
        // Sleeps until work arrives (an ESP-NOW packet or an MQTT message
        // notifies this task), or 50 ms at most for the timed work: beacon,
        // pruning, LED.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
    }
}

size_t nightmare_gateway_vault_snapshot(std::vector<NightMare::RetainedEntry> &out, uint32_t *version)
{
    return s_vault.snapshot(out, version);
}

uint32_t nightmare_gateway_vault_version()
{
    return s_vault.version();
}

size_t nightmare_gateway_vault_size()
{
    return s_vault.size();
}

void nightmare_gateway_enable(bool enabled)
{
    s_enabled.store(enabled, std::memory_order_release);
    if (gatewayTaskHandle != NULL)
        xTaskNotifyGive(gatewayTaskHandle); // act now, not at the next 50 ms tick
}

bool nightmare_gateway_enabled()
{
    return s_enabled.load(std::memory_order_acquire);
}

NightMareGatewayState nightmare_gateway_state()
{
    return s_gatewayState.load(std::memory_order_acquire);
}

const char *nightmare_gateway_state_name(NightMareGatewayState state)
{
    switch (state)
    {
    case NightMareGatewayState::Stopped:
        return "stopped";
    case NightMareGatewayState::WaitForWifiStack:
        return "waiting_for_wifi_stack";
    case NightMareGatewayState::StartEspNow:
        return "starting_esp_now";
    case NightMareGatewayState::Running:
        return "running";
    }
    return "unknown";
}

BaseType_t start_nightmare_gateway(const NightMareGatewayConfig &config)
{
    // mqttBroker is not implemented; the flag is ignored.
    s_espnowEnabled = config.espnowBroker;
    s_mqttEnabled = config.mqttClient;
    if (s_espnowEnabled)
    {
        const NightMareEspNowConfig &espnow = config.espnowConfig;
        if (espnow.psk == NULL || espnow.pskLength < NightMare::EspNowAuth::MinPskLength ||
            espnow.pskLength > NightMare::EspNowAuth::MaxPskLength)
        {
            ESP_LOGE(TAG, "ESP-NOW needs a %u..%u byte network key, got %u",
                     (unsigned)NightMare::EspNowAuth::MinPskLength,
                     (unsigned)NightMare::EspNowAuth::MaxPskLength, (unsigned)espnow.pskLength);
            return pdFAIL;
        }
        memcpy(s_pendingPsk, espnow.psk, espnow.pskLength);
        s_pendingPskLength = espnow.pskLength;
    }

    s_inbound = xQueueCreate(INBOUND_QUEUE_DEPTH, sizeof(NightMare::Message *));
    if (s_inbound == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate inbound queue");
        return pdFAIL;
    }

    return xTaskCreate(gateway_task, "GatewayTask", GATEWAY_TASK_STACK_SIZE, NULL, GATEWAY_TASK_PRIORITY, &gatewayTaskHandle);
}
