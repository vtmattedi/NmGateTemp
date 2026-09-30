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
    class Device
    {
        static constexpr size_t MaxSubscriptions = 32;

        MacAddress mac;
        uint8_t rssi = 0;
        uint64_t lastSeen = 0;                       // timestamp of the last time this device was seen
        std::string subscriptions[MaxSubscriptions]; // list of topics this device is subscribed to
        Message lastWill;                            // the last will message for this device
        bool authenticated = false;                  // whether the device has been authenticated
        std::string assumedName_ = "";                // a name for the device, if known (e.g. from its last will)
    public:
        Device() = default;
        explicit Device(const MacAddress &address);

        bool isSubscribedTo(const std::string &topic) const;
        bool subscribeTo(const std::string &topic);
        bool unsubscribeFrom(const std::string &topic);

        const MacAddress &address() const { return mac; }
        uint64_t lastSeenAt() const { return lastSeen; }
        void markSeen(uint8_t signal, uint64_t timestamp);
        size_t subscriptionCount() const;
        bool hasLastWill() const { return !lastWill.topic.empty(); }
        const Message &lastWillMessage() const { return lastWill; }
        // Rejects wildcard/empty/oversized topics, the same rule an ordinary
        // publish follows. Returns false (and leaves any existing will alone)
        // if the message doesn't qualify.
        bool setLastWill(const Message &message);
        void clearLastWill();
        bool isAuthenticated() const { return authenticated; }
        const std::string &assumedName() const { return assumedName_; }
        void setAssumedName(const std::string &name);
    };
}
