#include "espMqtt.h"
#include "mqtt_client.h"
#include "mqtt5_client.h"
#include "esp_log.h"
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "creds.h"
#include "brokerCreds.h"
#include "NightmareGateway/GatewayState.h"
#include <new>
#include <utility>

static const char *TAG = "espMqtt";
static esp_mqtt_client_handle_t s_client = NULL;
static std::atomic<bool> s_connected{false};

// esp_mqtt_client_publish() takes the client's API lock, which the esp-mqtt
// task holds through a stalled TLS read/write, DNS lookup or reconnect -- for
// seconds. Called from the gateway task that blocked it past the task
// watchdog, so publishes are handed to this queue and a task of their own
// does the blocking call instead.
#define OUTBOUND_QUEUE_DEPTH 32
#define PUBLISHER_STACK_SIZE 6144
#define PUBLISHER_PRIORITY 4
static QueueHandle_t s_outbound = NULL; // NightMare::Message*; NULL is the "session connected" marker
static std::atomic<uint32_t> s_dropped{0};

// MQTT 5 brokers cap unacknowledged QoS1 publishes (Receive Maximum; 10 on the
// broker in use) and esp-mqtt refuses to send past it. PUBACKs are read by the
// esp-mqtt task, so nothing may publish from its event handler in bulk: the
// acks that would free a slot can't be processed until the handler returns.
// Publishing therefore stays on the publisher task, paced to this window.
#define MAX_INFLIGHT_PUBLISHES 8
#define INFLIGHT_WAIT_MS 5000
static std::atomic<int> s_inflight{0};
static std::atomic<bool> s_sessionPending{false};
static std::atomic<uint32_t> s_session{0}; // bumped on every CONNECTED

// Reassembly of a multi-event payload. Only ever touched by the esp-mqtt task.
static NightMare::Message s_partial;
static bool s_partialActive = false;

static void subscribeToEverything(esp_mqtt_client_handle_t client)
{
    // no_local stops the broker from echoing back what this gateway published,
    // which is what would otherwise loop MQTT -> ESP-NOW -> MQTT forever.
    esp_mqtt5_subscribe_property_config_t property = {};
    property.no_local_flag = true;
    property.retain_as_published_flag = true;

    const esp_err_t err = esp_mqtt5_client_set_subscribe_property(client, &property);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set subscribe property: %s", esp_err_to_name(err));
        return;
    }

    esp_mqtt_client_subscribe(client, "#", 1);
}

// Waits for a free slot in the broker's receive window, then publishes. False
// when the connection went away, no ack freed a slot in time, or the publish
// itself was refused. Never call from the esp-mqtt task (it would wait on itself).
static bool publish_paced(esp_mqtt_client_handle_t client, const NightMare::Message &message)
{
    const TickType_t started = xTaskGetTickCount();
    while (s_inflight.load(std::memory_order_acquire) >= MAX_INFLIGHT_PUBLISHES)
    {
        if (!s_connected.load(std::memory_order_acquire) ||
            (xTaskGetTickCount() - started) >= pdMS_TO_TICKS(INFLIGHT_WAIT_MS))
            return false;
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    s_inflight.fetch_add(1); // before publishing: the ack can beat us back here
    if (esp_mqtt_client_publish(client, message.topic.c_str(),
                                reinterpret_cast<const char *>(message.payload.data()),
                                (int)message.payload.size(), 1, message.persistent) < 0)
    {
        s_inflight.fetch_sub(1);
        return false;
    }
    return true;
}

// Publishes the gateway's retained truth, and only then subscribes so the
// broker's retained replay can't overwrite it. On failure the connection is
// dropped: a half-prepared session would look up while nothing is subscribed.
static void run_session_setup(void)
{
    const uint32_t session = s_session.load();
    const bool prepared = mqtt_prepare_session(s_client);
    if (session != s_session.load() || !s_connected.load(std::memory_order_acquire))
        return; // the connection went away meanwhile; the next CONNECTED starts over

    if (prepared)
    {
        subscribeToEverything(s_client);
        return;
    }
    ESP_LOGE(TAG, "Gateway state publication failed; reconnecting");
    esp_mqtt_client_disconnect(s_client);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to broker");
        s_inflight.store(0); // esp-mqtt starts the session with an empty outbox
        s_session.fetch_add(1);
        s_connected.store(true, std::memory_order_release);
        s_sessionPending.store(true, std::memory_order_release);
        {
            NightMare::Message *marker = NULL; // wakes the publisher task if it is idle
            xQueueSend(s_outbound, &marker, 0);
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from broker");
        s_connected.store(false, std::memory_order_release);
        s_inflight.store(0);
        break;

    case MQTT_EVENT_PUBLISHED:
        if (s_inflight.load() > 0)
            s_inflight.fetch_sub(1);
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "Subscribed, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_DATA:
    {
        // A payload bigger than the client buffer arrives as several DATA
        // events and only the first one carries the topic, so reassemble here
        // instead of treating each event as a message of its own.
        if (event->current_data_offset == 0)
        {
            s_partial = NightMare::Message();
            s_partial.direction = NightMare::Direction::REMOTE_TO_LOCAL;
            s_partial.topic.assign(event->topic, event->topic_len);
            s_partial.persistent = event->retain;
            s_partial.payload.reserve(event->total_data_len);
            s_partialActive = true;
        }
        if (!s_partialActive)
            break; // joined mid-message, wait for the next one to start cleanly

        s_partial.payload.insert(s_partial.payload.end(), event->data, event->data + event->data_len);

        if ((int)s_partial.payload.size() >= event->total_data_len)
        {
            s_partialActive = false;
            mqtt_onMessage(std::move(s_partial));
            s_partial = NightMare::Message();
        }
        break;
    }

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error, type=%d", event->error_handle ? event->error_handle->error_type : -1);
        break;

    default:
        break;
    }
}

static void publisher_task(void *)
{
    uint32_t reportedDrops = 0;
    while (1)
    {
        NightMare::Message *message = NULL;
        if (xQueueReceive(s_outbound, &message, portMAX_DELAY) != pdTRUE)
            continue;

        // Before anything queued behind it, so the retained replay goes first.
        if (s_sessionPending.exchange(false, std::memory_order_acq_rel))
            run_session_setup();

        if (message == NULL)
            continue; // just the wake-up marker

        // Offline: drop rather than let esp-mqtt's outbox pile up QoS1 messages
        // for a broker that is gone. Anything persistent is in the vault anyway.
        if (!s_connected.load(std::memory_order_acquire) || !publish_paced(s_client, *message))
            s_dropped.fetch_add(1);
        delete message;

        const uint32_t dropped = s_dropped.load();
        if (dropped != reportedDrops && uxQueueMessagesWaiting(s_outbound) == 0)
        {
            ESP_LOGW(TAG, "%lu publish(es) dropped (broker offline or unresponsive)",
                     (unsigned long)(dropped - reportedDrops));
            reportedDrops = dropped;
        }
    }
}

void mqtt_stop(void)
{
    if (s_client == NULL)
        return;
    if (s_connected.load(std::memory_order_acquire))
    {
        NightMare::Message offline;
        offline.topic = GatewayState::statusTopic();
        const std::string payload = GatewayState::statusJson(false);
        offline.payload.assign(payload.begin(), payload.end());
        offline.persistent = true;
        if (!mqtt_publish_immediate(s_client, offline))
            ESP_LOGW(TAG, "Failed to publish graceful gateway offline status");
    }
    esp_mqtt_client_stop(s_client);
    s_connected.store(false, std::memory_order_release);
    s_partialActive = false; // the client task is gone, so this is ours now
    s_partial = NightMare::Message();
    ESP_LOGI(TAG, "MQTT stopped");
}

void mqtt_init(void)
{
    if (s_client != NULL) // stopped by mqtt_stop(): reconnect with the same client
    {
        ESP_LOGI(TAG, "Reconnecting");
        ESP_ERROR_CHECK(esp_mqtt_client_start(s_client));
        return;
    }

    s_outbound = xQueueCreate(OUTBOUND_QUEUE_DEPTH, sizeof(NightMare::Message *));
    if (s_outbound == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate outbound queue");
        return;
    }

    esp_mqtt_client_config_t config = {};
#if defined(REMOTE_MQTT_URL) && defined(REMOTE_MQTT_PORT)
    config.broker.address.hostname = REMOTE_MQTT_URL;
    config.broker.address.port = REMOTE_MQTT_PORT;
    config.broker.address.transport = MQTT_TRANSPORT_OVER_SSL;
#ifdef ROOT_CA
    config.broker.verification.certificate = ROOT_CA;
#endif
#ifdef MQTT_USER
    config.credentials.username = MQTT_USER;
#endif
#ifdef MQTT_PASSWD
    config.credentials.authentication.password = MQTT_PASSWD;
#endif
#elif defined(LOCAL_MQTT_HOST) && defined(LOCAL_MQTT_PORT)
    config.broker.address.hostname = LOCAL_MQTT_HOST;
    config.broker.address.port = LOCAL_MQTT_PORT;
    config.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
#ifdef GATEWAY_USER
    config.credentials.username = GATEWAY_USER;
#endif
#ifdef GATEWAY_PASSWD
    config.credentials.authentication.password = GATEWAY_PASSWD;
#endif
#else
#error "Define either REMOTE_MQTT_URL/REMOTE_MQTT_PORT or LOCAL_MQTT_HOST/LOCAL_MQTT_PORT"
#endif
    config.session.protocol_ver = MQTT_PROTOCOL_V_5; // no_local is an MQTT 5 subscription option
    config.session.keepalive = 60;
    const std::string willTopic = GatewayState::statusTopic();
    const std::string willPayload = GatewayState::statusJson(false);
    config.session.last_will.topic = willTopic.c_str();
    config.session.last_will.msg = willPayload.c_str();
    config.session.last_will.msg_len = willPayload.size();
    config.session.last_will.qos = 1;
    config.session.last_will.retain = true;
    config.credentials.client_id = GatewayState::id().c_str();

    s_client = esp_mqtt_client_init(&config);
    if (s_client == NULL)
    {
        ESP_LOGE(TAG, "Failed to init MQTT client");
        return;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, mqtt_event_handler, NULL));
    if (xTaskCreate(publisher_task, "MqttPublisher", PUBLISHER_STACK_SIZE, NULL,
                    PUBLISHER_PRIORITY, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to start publisher task");
        return;
    }
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_client));
    ESP_LOGI(TAG, "Connecting to %s:%d", config.broker.address.hostname,
             config.broker.address.port);
}

bool mqtt_publish(NightMare::Message message)
{
    if (s_client == NULL || s_outbound == NULL)
        return false;

    NightMare::Message *copy = new (std::nothrow) NightMare::Message(std::move(message));
    if (copy == nullptr)
    {
        s_dropped.fetch_add(1);
        ESP_LOGE(TAG, "Unable to allocate outbound publish envelope");
        return false;
    }
    if (xQueueSend(s_outbound, &copy, 0) != pdTRUE)
    {
        delete copy;
        s_dropped.fetch_add(1);
        return false;
    }
    return true;
}

bool mqtt_publish_immediate(esp_mqtt_client_handle_t client,
                            const NightMare::Message &message)
{
    return client != NULL && publish_paced(client, message);
}

bool mqtt_is_connected(void)
{
    return s_connected.load(std::memory_order_acquire);
}
