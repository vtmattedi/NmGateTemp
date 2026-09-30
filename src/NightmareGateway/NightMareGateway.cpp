#include "NightmareGateway.h"
#include "EspWifi/espWifi.h"
#include "LedController/ledController.h"
#include "NightmareGateway/EspMqtt/espMqtt.h"
#include "NightmareGateway/EspNow/espBroker.h"
#include "NightmareGateway/NightMare/Message.h"
#include "NightmareGateway/NightMare/Time.h"
#include "freertos/queue.h"
#include <strings.h>
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "gateway";

// Only has to absorb one tick's worth of arrivals, but subscribing to "#" makes
// the broker replay every retained topic at once, so the burst is large.
#define INBOUND_QUEUE_DEPTH 64
#define STATUS_BRIGHTNESS 20
#define DEBUG_TOPIC "gateway/debug/in"
#define CONTROL_REQUEST_TOPIC "Control/request"

TaskHandle_t gatewayTaskHandle = NULL;

enum class GatewayState
{
    WaitForWifiStack,
    StartEspNow,
    Running,
};

static QueueHandle_t s_inbound = NULL; // NightMare::Message* from the MQTT task
static NightMare::MessageVault s_vault;
static bool s_mqttStarted = false;

// Keeps a copy of anything flagged persistent so a device that subscribes later
// still gets the last value, the way a broker would replay retained messages.
static void retainIfPersistent(const NightMare::Message &message)
{
    if (!message.persistent)
        return;

    s_vault.retainMessage(message.topic, message.payload, (uint32_t)message.payload.size(), message.direction);
}

// A request on "<x>/in" is answered on "<x>/out" -- the same convention a
// NightMareNetwork device's own console uses (both the unaddressed console/in
// and the id-addressed console/controlled/<id>/in forms; the id just rides
// along inside <x>). Delivery is an ordinary broadcast + MQTT publish: there
// is no unicast-to-sender path, so whoever asked has to be subscribed to the
// reply topic to see the answer -- which, for a one-shot id-addressed
// request, only they normally are.
void replyMessage(const NightMare::Message &current, const NightMare::Message &response)
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

    retainIfPersistent(reply);
    espBroker_broadcastMessage(reply);
    if (mqtt_is_connected())
        mqtt_publish(reply);
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
// go out on "Control/<what>" to local subscribers only; the request is not
// forwarded, so a remote Control service does not answer it a second time.
// Returns false to let the request pass through unanswered (unknown request,
// or nothing valid to answer with).
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
        espBroker_broadcastMessage(reply);
        return true;
    }
    return false;
}

// The one place a message from either side is handled, before it is routed:
// device == nullptr means it arrived over MQTT, a non-null device means it
// arrived from that local ESP-NOW peer over ESP-NOW. Everything below
// branches on that instead of carrying a parallel origin flag, since the two
// always moved in lockstep anyway.
void onMessage(const NightMare::Device *device, const NightMare::Message &message)
{
    if (message.topic == DEBUG_TOPIC)
    {
        replyMessage(message, buildDebugTelemetry());
        return;
    }
    if (device != nullptr && message.topic == CONTROL_REQUEST_TOPIC && answerControlRequest(message))
        return;

    retainIfPersistent(message);

    // Broker stage: every local device subscribed to this topic, minus
    // whoever produced it, which is what stops a message bouncing back to
    // its own sender. A remote (MQTT) message has already reached its far
    // side by arriving here at all.
    espBroker_broadcastMessage(message, device);

    if (device != nullptr && mqtt_is_connected())
        mqtt_publish(message);
}

void espBroker_onMessage(NightMare::Device *device, const NightMare::Message &message)
{
    ESP_LOGI(TAG, "Local '%s' (%d bytes) from %s%s", message.topic.c_str(),
             (int)message.payload.size(), device->address().toString().c_str(), message.persistent ? " [retained]" : "");

    onMessage(device, message);
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
void mqtt_onMessage(const NightMare::Message &message)
{
    if (s_inbound == NULL)
        return;
    ESP_LOGI(TAG, "MQTT '%s' (%d bytes) from %s", message.topic.c_str(),
             (int)message.payload.size(), message.persistent ? " [retained]" : "");

    NightMare::Message *copy = new NightMare::Message(message);
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
        onMessage(nullptr, *message);
        delete message;
        // A reconnect replays every retained topic at once, and each one can
        // take several bounded ESP-NOW sends; don't let the burst look like a hang.
        esp_task_wdt_reset();
    }
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

    GatewayState state = GatewayState::WaitForWifiStack;

    while (1)
    {
        switch (state)
        {
        case GatewayState::WaitForWifiStack:
            // ESP-NOW only needs the radio running, so it comes up before (and
            // independently of) any association with an access point.
            if (wifi_is_started())
                state = GatewayState::StartEspNow;
            break;

        case GatewayState::StartEspNow:
            if (!espBroker_init())
                ESP_LOGE(TAG, "ESP-NOW unavailable, running MQTT only");
            state = GatewayState::Running;
            break;

        case GatewayState::Running:
            if (!s_mqttStarted && wifi_is_connected())
            {
                ESP_LOGI(TAG, "Network up, starting MQTT");
                mqtt_init();
                s_mqttStarted = true;
            }
            espBroker_process();
            drainInbound();
            break;
        }

        updateStatusLed();
        esp_task_wdt_reset();
        // Sleeps until work arrives (an ESP-NOW packet or an MQTT message
        // notifies this task), or 50 ms at most for the timed work: beacon,
        // pruning, LED.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
    }
}

BaseType_t start_nightmare_gateway(void)
{
    s_inbound = xQueueCreate(INBOUND_QUEUE_DEPTH, sizeof(NightMare::Message *));
    if (s_inbound == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate inbound queue");
        return pdFAIL;
    }

    return xTaskCreate(gateway_task, "GatewayTask", GATEWAY_TASK_STACK_SIZE, NULL, GATEWAY_TASK_PRIORITY, &gatewayTaskHandle);
}
