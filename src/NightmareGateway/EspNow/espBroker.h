#pragma once
#include <string>
#include "NightmareGateway/NightMare/Message.h"
#include "NightmareGateway/EspNow/Frame.h"
#include "NightmareGateway/NightMare/Device.h"
#include "esp_now.h"

// Brings up ESP-NOW on the station interface. Call after wifi connect to force both onto the same channel.
bool espBroker_init(void);

// Drains what the receive callback queued, ages out silent devices, and
// broadcasts a beacon every few seconds so a device or scanner can find this
// gateway without sending anything first. Must be called from the gateway
// task, the only owner of the device table.
void espBroker_process(void);

// The beacon is unconditional and unaddressed: it runs from a successful
// espBroker_init() onward regardless of whether anything is listening, and
// any device on the right channel can use it to find this gateway -- it is
// not specific to (or gated on) the scanner tool.
bool espBroker_beaconActive(void);

// Seconds since the last beacon broadcast was sent. Should stay within a
// couple of BEACON_INTERVAL_S; anything climbing well past that means the
// gateway task has stalled or the radio has a problem, not that no one is
// listening -- the beacon doesn't care whether anyone is.
uint32_t espBroker_secondsSinceLastBeacon(void);

void onEspNowMessageReceived(NightMare::Device *device, const NightMare::Frame &frame);
bool espBroker_sendMessage(const NightMare::Device *device, const NightMare::Message &message);
bool espBroker_sendFrame(const NightMare::Device *device, const NightMare::Frame &frame);

// Fans a message out to every device subscribed to its topic. Passing the
// sender in `except` skips it, which is what keeps a message from echoing back
// to whoever produced it.
bool espBroker_broadcastMessage(const NightMare::Message &message, const NightMare::Device *except = nullptr);

// Devices currently registered with at least one subscription.
uint8_t espBroker_subscriberCount(void);

// Read-only enumeration of every currently registered device, for building
// things like gateway telemetry. Index order is not meaningful and may change
// as devices connect/disconnect.
uint8_t espBroker_deviceCount(void);
const NightMare::Device *espBroker_deviceAt(uint8_t index);

// Hooks, both called from the gateway task. Implemented by the gateway.
// The device is mutable: this hook is where the gateway learns things about the
// sender from what it publishes (e.g. its name from "<name>/status").
void espBroker_onMessage(NightMare::Device *device, const NightMare::Message &message);
void espBroker_onSubscribe(const NightMare::Device *device, const std::string &filter);

// Function to build a frame from a message
// prob. internal only
// int messageBuilder(NightMare::Frame &frame, const NightMare::Message &message, uint8_t *buf, size_t bufSize);
