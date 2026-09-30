#pragma once
#include <stdint.h>
#include <stddef.h>

/// @brief Frame structure for ESP-NOW communication in the Nightmare Gateway system.
/// @note The header is 10 bytes. Data holds up to 240 bytes for ESP-NOW v1 and 1460 bytes for ESP-NOW v2.
/// @note A keep-alive frame is a CONTROL frame with no data: a heartbeat to keep the device's registration alive.

namespace NightMare
{
    constexpr size_t MaxFrameDataSize = 240;   // 250 (ESP-NOW v1 payload) - 10 (header)
    constexpr size_t MaxFrameDataSizeV2 = 1460; // 1,470 (ESP-NOW v2 payload) - 10 (header)

    enum class FrameType : uint8_t
    {
        BEACON = 0,
        AUTH = 1, // reserved
        SUBSCRIBE = 2,
        UNSUBSCRIBE = 3,
        MESSAGE = 4,
        CONTROL = 5,
        ACK = 6,
        ERROR = 7,
        LAST_WILL = 8,
    };

    enum class VersionType : uint8_t
    {
        ESP_NOW = 0,
        ESP_NOW_V2 = 1,
    };

    struct FrameHeader // 10 bytes
    {
        uint16_t messageId;   // 2 bytes for messageId
        uint8_t version;      // 1 byte: VersionType, which ESP-NOW payload size this frame was built for
        uint8_t type;         // 1 byte for type
        uint16_t frameIndex;  // 2 bytes for frameIndex
        uint16_t totalFrames; // 2 bytes for totalFrames
        uint16_t length;      // 2 bytes for length -> data[length]
    } __attribute__((packed));

    FrameHeader encodeFrameHeader(uint16_t messageId, VersionType version, FrameType type, uint16_t frameIndex, uint16_t totalFrames, uint16_t length);

    // Reads a FrameHeader out of a raw buffer. Fails (returns false, *out*
    // untouched) if the buffer is shorter than a header or the header claims
    // more data than the buffer actually has after it.
    bool decodeFrameHeader(FrameHeader &out, const uint8_t *data, size_t length);

    struct Frame
    {
        FrameHeader header;
        uint8_t data[MaxFrameDataSize];
    } __attribute__((packed));

    struct FrameV2
    {
        FrameHeader header;
        uint8_t data[MaxFrameDataSizeV2];
    } __attribute__((packed));

    // A gateway/device announcing itself: data is [versionCount][version...],
    // the VersionType values this node can speak. versionCount above
    // MaxFrameDataSize - 1 returns an empty (all-zero) frame.
    Frame beaconFrame(const uint8_t *supportedVersions, uint8_t versionCount);

    // A CONTROL frame with no data, purely to keep a device's registration
    // (and, for whoever sent it, RTT) alive. The receiver answers with ACK.
    Frame keepAliveFrame(uint16_t messageId);
}
