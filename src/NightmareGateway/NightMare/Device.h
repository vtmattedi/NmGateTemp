#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>
#include "NightmareGateway/NightMare/Topic.h"
#include "NightmareGateway/EspNow/macAddress.h"
#include "NightmareGateway/NightMare/Message.h"

namespace NightMare
{
    // Where an ESP-NOW session stands. A peer still in CONNECT/CHALLENGE is not
    // a Device yet: it lives in espDeviceManager's pending-handshake table.
    enum class ConnectionState : uint8_t
    {
        HANDSHAKE,     // not used by a Device; named for completeness of the lifecycle
        AUTHENTICATED, // AUTH verified and a cid assigned; CONNACK going out
        SECURING,      // LMK installed, waiting for the first encrypted PING
        CONNECTED,     // encrypted PING/PONG done: broker traffic allowed
    };

    const char *connectionStateName(ConnectionState state);

    // One ESP-NOW session: the peer MAC, the gateway-assigned cid, and what the
    // broker keeps for it. Session and broker state go away together.
    class Device
    {
        static constexpr size_t MaxSubscriptions = 32;

        MacAddress mac;
        uint16_t cid_ = 0;
        ConnectionState state_ = ConnectionState::AUTHENTICATED;
        // The session's ESP-NOW key. Kept because a handshake from this MAC
        // turns the peer back to plaintext, and an unproven one has to leave
        // the session exactly as it was -- encryption included.
        uint8_t sessionKey_[16] = {};
        bool hasSessionKey_ = false;
        // A handshake is running for this MAC. The peer is plaintext meanwhile,
        // so nothing is delivered to the session and nothing is accepted from
        // it, until the handshake either proves itself or is abandoned.
        bool suspended_ = false;
        uint8_t rssi = 0;
        uint64_t lastSeenMs = 0;                     // last accepted frame (or session start)
        std::string subscriptions[MaxSubscriptions]; // list of topics this device is subscribed to
        Message lastWill;                            // the last will message for this device
        std::string assumedName_ = "";               // a name for the device, if known (e.g. from its status)
    public:
        Device() = default;
        Device(const MacAddress &address, uint16_t cid, uint64_t nowMs);

        bool isSubscribedTo(const std::string &topic) const;
        bool subscribeTo(const std::string &topic);
        bool unsubscribeFrom(const std::string &topic);

        const MacAddress &address() const { return mac; }
        uint16_t cid() const { return cid_; }
        ConnectionState state() const { return state_; }
        void setState(ConnectionState state) { state_ = state; }
        // Usable right now: secured, and not paused by a handshake.
        bool isConnected() const { return state_ == ConnectionState::CONNECTED && !suspended_; }

        static constexpr size_t SessionKeySize = sizeof(sessionKey_);
        void setSessionKey(const uint8_t *key);
        const uint8_t *sessionKey() const { return sessionKey_; }
        bool hasSessionKey() const { return hasSessionKey_; }
        void forgetSessionKey();

        bool isSuspended() const { return suspended_; }
        void suspend() { suspended_ = true; }
        void resume() { suspended_ = false; }

        uint64_t lastSeenAtMs() const { return lastSeenMs; }
        void markSeen(uint8_t signal, uint64_t nowMs);
        size_t subscriptionCount() const;
        bool hasLastWill() const { return !lastWill.topic.empty(); }
        const Message &lastWillMessage() const { return lastWill; }
        // Rejects wildcard/empty/oversized topics, the same rule an ordinary
        // publish follows. Returns false (and leaves any existing will alone)
        // if the message doesn't qualify.
        bool setLastWill(const Message &message);
        void clearLastWill();
        const std::string &assumedName() const { return assumedName_; }
        void setAssumedName(const std::string &name);
    };
}
