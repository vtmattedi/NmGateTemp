#pragma once
#include "NightmareGateway/NightMare/Message.h"
#include "mqtt_client.h"

// Connects to the broker configured in creds.h (REMOTE_MQTT_*), subscribes to
// every topic ("#") and starts the client's own background task. Call after
// WiFi is connected. After mqtt_stop() it restarts the existing client.
void mqtt_init(void);

// Disconnects and stops the client, keeping it so mqtt_init() can start it
// again. Call from the gateway task, never from an MQTT event handler.
void mqtt_stop(void);

// Queues a message for the publisher task; never blocks. False if the queue
// is full or MQTT was never started. Messages still queued when the broker is
// offline are dropped, not held.
// Takes ownership so large payloads can be queued without another allocation.
bool mqtt_publish(NightMare::Message message);

// Synchronous retained publication used only during MQTT_EVENT_CONNECTED to
// establish gateway-owned truth before subscriptions admit retained replay.
bool mqtt_publish_immediate(esp_mqtt_client_handle_t client,
                            const NightMare::Message &message);

// True once the broker has accepted the connection.
bool mqtt_is_connected(void);

// Hook invoked whenever a message arrives on any subscribed topic. Runs in
// the esp-mqtt client task context. Implemented by the gateway.
void mqtt_onMessage(NightMare::Message message);

// Publishes the gateway's retained state and client snapshot. Called before
// the MQTT transport subscribes to normal topics.
bool mqtt_prepare_session(esp_mqtt_client_handle_t client);
