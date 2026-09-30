#include "NightmareGateway/NightMare/Device.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

namespace NightMare
{
    Device::Device(const MacAddress &address) : mac(address) {}

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

    void Device::markSeen(uint8_t signal, uint64_t timestamp)
    {
        rssi = signal;
        lastSeen = timestamp;
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
