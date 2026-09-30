#include "NightmareGateway/NightMare/Message.h"
#include "NightmareGateway/NightMare/Topic.h"
#include <algorithm>

namespace NightMare
{
    namespace
    {
        constexpr uint8_t RetainedFlag = 0x80;
        constexpr uint8_t TopicLengthMask = 0x3F;
    }

    Message Message::fromTopicAndPayload(const std::string &topic, const std::vector<uint8_t> &payload, bool persistent, Direction direction)
    {
        Message message;
        message.direction = direction;
        message.topic = topic;
        message.payload = payload;
        message.persistent = persistent;
        return message;
    }

    Message Message::fromRawData(const uint8_t *data, size_t length)
    {
        Message message;
        if (data == nullptr || length < 1)
            return message;

        const size_t topicLength = data[0] & TopicLengthMask;
        if (topicLength == 0 || length < 1 + topicLength)
            return message;

        message.direction = Direction::LOCAL_TO_REMOTE;
        message.persistent = (data[0] & RetainedFlag) != 0;
        message.topic.assign(reinterpret_cast<const char *>(data + 1), topicLength);
        message.payload.assign(data + 1 + topicLength, data + length);
        return message;
    }

    std::vector<uint8_t> Message::toRawData() const
    {
        std::vector<uint8_t> raw;
        if (!isValidPublishTopic(topic))
            return raw;

        raw.reserve(1 + topic.size() + payload.size());
        raw.push_back(static_cast<uint8_t>((persistent ? RetainedFlag : 0) | (topic.size() & TopicLengthMask)));
        raw.insert(raw.end(), topic.begin(), topic.end());
        raw.insert(raw.end(), payload.begin(), payload.end());
        return raw;
    }

    bool MessageVault::retainMessage(const std::string &topic, const std::vector<uint8_t> &payload, uint32_t payload_length, Direction direction)
    {
        if (!isValidPublishTopic(topic))
            return false;

        const size_t length = std::min<size_t>(payload_length, payload.size());
        if (length == 0)
            return retained.erase(topic) > 0;

        Message &slot = retained[topic];
        slot.direction = direction;
        slot.topic = topic;
        slot.payload.assign(payload.begin(), payload.begin() + length);
        slot.persistent = true;
        return true;
    }

    bool MessageVault::getMessagesForTopic(const std::string &topic, std::vector<Message> &output)
    {
        bool found = false;
        for (const auto &entry : retained)
        {
            if (topicMatchesPattern(entry.first, topic))
            {
                output.push_back(entry.second);
                found = true;
            }
        }
        return found;
    }
}
