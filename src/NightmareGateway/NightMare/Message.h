#pragma once
#include <stdint.h>
#include <map>
#include <string>
#include <vector>
namespace NightMare
{
    enum class Direction : uint8_t
    {
        LOCAL_TO_REMOTE,
        REMOTE_TO_LOCAL,
    };
    struct Message
    {
        Direction direction = Direction::LOCAL_TO_REMOTE;
        std::string topic;
        std::vector<uint8_t> payload;
        bool persistent = false;
        static Message fromTopicAndPayload(const std::string &topic, const std::vector<uint8_t> &payload, bool persistent = false, Direction direction = Direction::LOCAL_TO_REMOTE);
        // NightMare Over ESP-NOW. After getting all the frames, this function will reconstruct the message from the raw data from the final bufffer.
        // [Retained - 1 bit] [Reserved - 1 bit] [Topic Length - 6 bits] [Topic - variable length] [Payload - variable length]
        // An empty topic in the result means the buffer was malformed.
        static Message fromRawData(const uint8_t *data, size_t length);
        // Encodes into the layout described above. Empty if the topic can't be encoded.
        std::vector<uint8_t> toRawData() const;
    };
    class MessageVault
    {
    public:
        // Retain a message for a specific topic. If a message already exists for that topic, it will be replaced.
        // If the payload is empty, the retained message for that topic will be deleted.
        bool retainMessage(const std::string &topic, const std::vector<uint8_t> &payload, uint32_t payload_length, Direction direction = Direction::LOCAL_TO_REMOTE);
        // Retrieve the retained message for a specific topic. Returns true if a message was found, false otherwise.
        // The topic is treated as a filter, so a subscriber gets every retained message below it.
        bool getMessagesForTopic(const std::string &topic, std::vector<Message> &output);

    private:
        std::map<std::string, Message> retained;
    };
}