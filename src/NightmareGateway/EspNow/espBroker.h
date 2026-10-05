#pragma once
#include <string>
#include <vector>
#include "NightmareGateway/NightMare/Message.h"
#include "NightmareGateway/EspNow/Frame.h"
#include "NightmareGateway/NightMare/Device.h"
#include "esp_now.h"

// Brings up ESP-NOW on the station interface once the Wi-Fi radio runs. `psk`
// is the network key (MinPskLength..MaxPskLength bytes, see Auth.h); it is
// copied, so the caller may wipe its buffer on return. False on a bad key or
// an ESP-NOW failure.
bool espBroker_init(const uint8_t *psk, size_t pskLength);

// Drains what the receive callback queued, ages out silent devices, and
// broadcasts a beacon every few seconds so a device or scanner can find this
// gateway without sending anything first. Must be called from the gateway
// task, the only owner of the device table.
void espBroker_process(void);

// Tears ESP-NOW down: every session and pending handshake is dropped (no last
// wills fire -- the devices just see the gateway go quiet), the radio stops
// beaconing and listening. Safe when already down. resume() brings it back with
// the key given to espBroker_init(); false if that was never called or ESP-NOW
// fails to start. Gateway task only.
void espBroker_shutdown(void);
bool espBroker_resume(void);

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

// Both only reach a CONNECTED session (false otherwise); frames carry its cid
// and go out encrypted with its LMK.
bool espBroker_sendMessage(const NightMare::Device *device, const NightMare::Message &message);
bool espBroker_sendFrame(const NightMare::Device *device, const NightMare::Frame &frame);

// Fans a message out to every CONNECTED session subscribed to its topic.
// Passing the sender in `except` skips it, which is what keeps a message from
// echoing back to whoever produced it.
bool espBroker_broadcastMessage(const NightMare::Message &message, const NightMare::Device *except = nullptr);

// CONNECTED sessions with at least one subscription.
uint8_t espBroker_subscriberCount(void);

// Read-only enumeration of every session, in any state (see
// Device::state()), for building things like gateway telemetry. Index order is
// not meaningful and may change as devices connect/disconnect.
uint8_t espBroker_deviceCount(void);
const NightMare::Device *espBroker_deviceAt(uint8_t index);
bool espBroker_connectedDeviceNamed(const std::string &name);

// Everything the web UI shows about one session, copied so it can be read from
// any task. Published by the gateway task a couple of times a second.
struct EspBrokerDeviceInfo
{
    std::string mac;
    std::string name; // learned from "<name>/status", may be empty
    uint16_t cid = 0;
    NightMare::ConnectionState state = NightMare::ConnectionState::AUTHENTICATED;
    bool suspended = false; // a re-handshake is in progress
    std::vector<std::string> subscriptions;
    bool hasLastWill = false;
    std::string lastWillTopic;
    std::vector<uint8_t> lastWillPayload;
    bool lastWillRetained = false;
    size_t lastWillPayloadSize = 0;
    bool hasRssi = false;
    int8_t rssi = 0;
    float avgRssi = 0;
    uint32_t rxFrames = 0;
    bool hasRtt = false;
    float rttMs = 0; // link-layer ACK round trip, smoothed
    uint64_t sessionStartMs = 0; // esp_timer uptime
    uint64_t lastSeenMs = 0;     // esp_timer uptime
    // Silent for two heartbeats (or mid re-handshake): likely about to time out.
    bool disconnectCandidate = false;
};

// Thread-safe copy of every session for telemetry; returns how many.
size_t espBroker_snapshotDevices(std::vector<EspBrokerDeviceInfo> &out);
uint32_t espBroker_heartbeatMs(void);
uint32_t espBroker_sessionTimeoutMs(void);

// Hooks, both called from the gateway task. Implemented by the gateway.
// The device is mutable: this hook is where the gateway learns things about the
// sender from what it publishes (e.g. its name from "<name>/status").
void espBroker_onMessage(NightMare::Device *device, NightMare::Message message);
void espBroker_onSubscribe(const NightMare::Device *device, const std::string &filter);

// Function to build a frame from a message
// prob. internal only
// int messageBuilder(NightMare::Frame &frame, const NightMare::Message &message, uint8_t *buf, size_t bufSize);
