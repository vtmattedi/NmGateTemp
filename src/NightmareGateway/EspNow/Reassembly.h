#pragma once
#include <stdint.h>
#include <stddef.h>
#include <vector>
#include "NightmareGateway/EspNow/macAddress.h"
#include "NightmareGateway/EspNow/Frame.h"

// Puts fragmented MESSAGE frames back together. A message is identified by
// sender MAC + session cid + messageId: after a reconnect the same device can
// reuse a messageId under its new cid, and fragments from the old session must
// never be mixed into the new message.
class ReassemblyTable
{
public:
    static constexpr size_t Slots = 4;
    static constexpr uint64_t TimeoutMs = 5000;

    enum class Result : uint8_t
    {
        COMPLETE,   // `out` holds the whole message
        INCOMPLETE, // more fragments expected
        DROPPED,    // no slot, joined mid-message, or out of order
    };

    // `header` must already have passed validateFrame(). A single-frame
    // message completes at once without taking a slot.
    Result feed(const MacAddress &mac, const NightMare::FrameHeader &header, const uint8_t *data,
                uint64_t nowMs, std::vector<uint8_t> &out);

    // Forget everything in flight for a sender, e.g. when its session ends.
    void drop(const MacAddress &mac);

    size_t activeCount() const;

private:
    struct Slot
    {
        bool active = false;
        MacAddress mac;
        uint16_t cid = 0;
        uint16_t messageId = 0;
        uint8_t nextFrame = 0;
        uint8_t totalFrames = 0;
        uint64_t startedAtMs = 0;
        std::vector<uint8_t> buffer;
    };

    Slot *find(const MacAddress &mac, uint16_t cid, uint16_t messageId);
    Slot *claim(const MacAddress &mac, uint16_t cid, uint64_t nowMs);

    Slot slots[Slots];
};
