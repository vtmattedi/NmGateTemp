#include "Reassembly.h"

ReassemblyTable::Slot *ReassemblyTable::find(const MacAddress &mac, uint16_t cid, uint16_t messageId)
{
    for (Slot &slot : slots)
        if (slot.active && slot.mac == mac && slot.cid == cid && slot.messageId == messageId)
            return &slot;
    return nullptr;
}

// A free slot, a timed-out one, or one this sender left behind under an
// earlier session -- that message can never complete now.
ReassemblyTable::Slot *ReassemblyTable::claim(const MacAddress &mac, uint16_t cid, uint64_t nowMs)
{
    for (Slot &slot : slots)
    {
        if (!slot.active || nowMs - slot.startedAtMs > TimeoutMs || (slot.mac == mac && slot.cid != cid))
        {
            slot.active = false;
            slot.buffer.clear();
            return &slot;
        }
    }
    return nullptr;
}

ReassemblyTable::Result ReassemblyTable::feed(const MacAddress &mac, const NightMare::FrameHeader &header,
                                              const uint8_t *data, uint64_t nowMs, std::vector<uint8_t> &out)
{
    if (header.totalFrames == 1)
    {
        out.assign(data, data + header.length);
        return Result::COMPLETE;
    }

    Slot *slot = find(mac, header.cid, header.messageId);
    if (slot == nullptr)
    {
        if (header.frameIndex != 0)
            return Result::DROPPED; // joined mid-message, wait for the next one to start cleanly
        slot = claim(mac, header.cid, nowMs);
        if (slot == nullptr)
            return Result::DROPPED;
        slot->active = true;
        slot->mac = mac;
        slot->cid = header.cid;
        slot->messageId = header.messageId;
        slot->totalFrames = header.totalFrames;
        slot->nextFrame = 0;
        slot->startedAtMs = nowMs;
    }

    // Frames are consumed strictly in order; anything else drops the message.
    if (header.frameIndex != slot->nextFrame || header.totalFrames != slot->totalFrames)
    {
        slot->active = false;
        slot->buffer.clear();
        return Result::DROPPED;
    }

    slot->buffer.insert(slot->buffer.end(), data, data + header.length);
    if (++slot->nextFrame < slot->totalFrames)
        return Result::INCOMPLETE;

    out.swap(slot->buffer);
    slot->buffer.clear();
    slot->active = false;
    return Result::COMPLETE;
}

void ReassemblyTable::drop(const MacAddress &mac)
{
    for (Slot &slot : slots)
    {
        if (slot.active && slot.mac == mac)
        {
            slot.active = false;
            slot.buffer.clear();
        }
    }
}

size_t ReassemblyTable::activeCount() const
{
    size_t count = 0;
    for (const Slot &slot : slots)
        if (slot.active)
            count++;
    return count;
}
