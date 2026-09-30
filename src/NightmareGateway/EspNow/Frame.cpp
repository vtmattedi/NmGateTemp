#include "Frame.h"
#include <string.h>
#include "esp_log.h"

static const char *TAG = "Frame";

namespace NightMare
{
    FrameHeader encodeFrameHeader(uint16_t messageId, VersionType version, FrameType type, uint16_t frameIndex, uint16_t totalFrames, uint16_t length)
    {
        FrameHeader header;
        header.messageId = messageId;
        header.version = static_cast<uint8_t>(version);
        header.type = static_cast<uint8_t>(type);
        header.frameIndex = frameIndex;
        header.totalFrames = totalFrames;
        header.length = length;
        return header;
    }

    bool decodeFrameHeader(FrameHeader &out, const uint8_t *data, size_t length)
    {
        if (data == nullptr || length < sizeof(FrameHeader))
            return false;

        FrameHeader header;
        memcpy(&header, data, sizeof(FrameHeader));
        if (header.length > length - sizeof(FrameHeader))
            return false;

        out = header;
        return true;
    }

    /// @brief Builds a beacon: data is [versionCount][version bytes...], the
    /// VersionType values this node speaks. Lets a listener that only knows an
    /// older/newer protocol version recognize the frame is not for it instead
    /// of misreading a payload it does not understand.
    Frame beaconFrame(const uint8_t *supportedVersions, uint8_t versionCount)
    {
        const size_t length = (size_t)versionCount + 1; // +1 for the count prefix byte
        if (length > MaxFrameDataSize)
        {
            ESP_LOGE(TAG, "Beacon frame too large: %zu bytes, max is %zu", length, MaxFrameDataSize);
            return Frame{};
        }

        Frame frame{};
        frame.header = encodeFrameHeader(0, VersionType::ESP_NOW, FrameType::BEACON, 0, 1, (uint16_t)length);
        frame.data[0] = versionCount;
        if (versionCount > 0)
            memcpy(frame.data + 1, supportedVersions, versionCount);
        return frame;
    }

    Frame keepAliveFrame(uint16_t messageId)
    {
        Frame frame{};
        frame.header = encodeFrameHeader(messageId, VersionType::ESP_NOW, FrameType::CONTROL, 0, 1, 0);
        return frame;
    }
}
