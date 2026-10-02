#include "NightmareGateway/NightMare/Device.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

namespace NightMare
{
    const char *connectionStateName(ConnectionState state)
    {
        switch (state)
        {
        case ConnectionState::HANDSHAKE: return "handshake";
        case ConnectionState::AUTHENTICATED: return "authenticated";
        case ConnectionState::SECURING: return "securing";
        case ConnectionState::CONNECTED: return "connected";
        }
        return "?";
    }

    Device::Device(const MacAddress &address, uint16_t cid, uint64_t nowMs)
        : mac(address), cid_(cid), sessionStartMs_(nowMs), lastSeenMs(nowMs) {}

    bool Device::isSubscribedTo(const std::string &topic) const
    {
        for (const std::string &filter : subscriptions)
        {
            if (!filter.empty() && topicMatchesPattern(topic, filter))
                return true;
        }
        return false;
    }

    bool Device::subscribeTo(const std::string &topic)
    {
        if (topic.empty() || topic.size() > MaxTopicLength)
            return false;

        size_t freeSlot = MaxSubscriptions;
        for (size_t i = 0; i < MaxSubscriptions; i++)
        {
            if (subscriptions[i] == topic)
                return true;
            if (subscriptions[i].empty() && freeSlot == MaxSubscriptions)
                freeSlot = i;
        }

        if (freeSlot == MaxSubscriptions)
            return false;

        subscriptions[freeSlot] = topic;
        return true;
    }

    bool Device::unsubscribeFrom(const std::string &topic)
    {
        for (std::string &filter : subscriptions)
        {
            if (filter == topic)
            {
                filter.clear();
                return true;
            }
        }
        return false;
    }

    void Device::setSessionKey(const uint8_t *key)
    {
        memcpy(sessionKey_, key, sizeof(sessionKey_));
        hasSessionKey_ = true;
    }

    void Device::forgetSessionKey()
    {
        volatile uint8_t *bytes = sessionKey_;
        for (size_t i = 0; i < sizeof(sessionKey_); i++)
            bytes[i] = 0;
        hasSessionKey_ = false;
    }

    void Device::markSeen(int8_t signal, uint64_t nowMs)
    {
        rssi = signal;
        if (!hasRssi_)
        {
            avgRssi_ = signal;
            hasRssi_ = true;
        }
        else
            avgRssi_ += ((float)signal - avgRssi_) / 8.0f;
        rxFrames_++;
        lastSeenMs = nowMs;
    }

    void Device::noteRtt(float ms)
    {
        if (!hasRtt_)
        {
            rttMs_ = ms;
            hasRtt_ = true;
        }
        else
            rttMs_ += (ms - rttMs_) / 4.0f;
    }

    std::vector<std::string> Device::subscriptionList() const
    {
        std::vector<std::string> list;
        for (const std::string &filter : subscriptions)
        {
            if (!filter.empty())
                list.push_back(filter);
        }
        return list;
    }

    size_t Device::subscriptionCount() const
    {
        size_t count = 0;
        for (const std::string &filter : subscriptions)
        {
            if (!filter.empty())
                count++;
        }
        return count;
    }

    bool Device::setLastWill(const Message &message)
    {
        if (!isValidPublishTopic(message.topic))
            return false;
        lastWill = message;
        return true;
    }

    void Device::clearLastWill()
    {
        lastWill = Message();
    }

    void Device::setAssumedName(const std::string &name){
        ESP_LOGI("[DM]", "Setting assumed name for device %s: %s", mac.toString().c_str(), name.c_str());
        assumedName_ = name;
    }
}
