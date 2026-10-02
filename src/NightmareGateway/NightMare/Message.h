#pragma once
#include <stdint.h>
#include <map>
#include <mutex>
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
        // Consumes a reassembly buffer without copying its potentially large payload.
        static Message fromRawData(std::vector<uint8_t> &&raw);
        // Encodes into the layout described above. Empty if the topic can't be encoded.
        std::vector<uint8_t> toRawData() const;
    };
    // A retained message plus the bookkeeping the web UI shows for it.
    struct RetainedEntry
    {
        Message message;
        uint64_t updatedAtMs = 0; // caller's clock (uptime) when it was last replaced
        uint32_t revisions = 0;   // how many times this topic has been retained
    };
    // Written by the gateway task, read by the web server task: every public
    // member takes the lock.
    class MessageVault
    {
    public:
        // Retain a message for a specific topic. If a message already exists for that topic, it will be replaced.
        // If the payload is empty, the retained message for that topic will be deleted.
        // nowMs only feeds RetainedEntry::updatedAtMs.
        bool retainMessage(const std::string &topic, const std::vector<uint8_t> &payload, uint32_t payload_length, Direction direction = Direction::LOCAL_TO_REMOTE, uint64_t nowMs = 0);
        // Retrieve the retained message for a specific topic. Returns true if a message was found, false otherwise.
        // The topic is treated as a filter, so a subscriber gets every retained message below it.
        bool getMessagesForTopic(const std::string &topic, std::vector<Message> &output);
        // Copies every retained message, in topic order, for telemetry.
        size_t snapshot(std::vector<RetainedEntry> &output);
        size_t size();

    private:
        std::mutex lock_;
        std::map<std::string, RetainedEntry> retained;
    };
}
