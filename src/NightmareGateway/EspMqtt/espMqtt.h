#pragma once
#include "NightmareGateway/NightMare/Message.h"

// Connects to the broker configured in creds.h (REMOTE_MQTT_*), subscribes to
// every topic ("#") and starts the client's own background task. Call after
// WiFi is connected.
void mqtt_init(void);

// Queues a message for the publisher task; never blocks. False if the queue
// is full or MQTT was never started. Messages still queued when the broker is
// offline are dropped, not held.
bool mqtt_publish(const NightMare::Message &message);

// True once the broker has accepted the connection.
bool mqtt_is_connected(void);

// Hook invoked whenever a message arrives on any subscribed topic. Runs in
// the esp-mqtt client task context. Implemented by the gateway.
void mqtt_onMessage(const NightMare::Message &message);
