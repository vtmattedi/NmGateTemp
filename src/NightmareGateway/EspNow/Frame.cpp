#include "Frame.h"
#include <string.h>

// Shared byte for byte by the gateway and the NightMareNetwork client: no
// logging or framework dependency in here.

namespace NightMare
{
    namespace
    {
        bool knownType(uint8_t type)
        {
            return type <= static_cast<uint8_t>(FrameType::ERROR);
        }

        // Handshake frames run before a session exists and must carry cid 0;
        // CONNACK is the one that hands the new cid out.
        enum class CidRule : uint8_t
        {
            ZERO,
            NONZERO,
            ANY,
        };

        CidRule cidRule(FrameType type)
        {
            switch (type)
            {
            case FrameType::BEACON:
            case FrameType::CONNECT:
            case FrameType::CHALLENGE:
            case FrameType::AUTH:
                return CidRule::ZERO;
            case FrameType::ERROR:
                return CidRule::ANY; // an INVALID_SESSION reply has no valid cid to carry
            default:
                return CidRule::NONZERO;
            }
        }

        // -1: variable length.
        int fixedPayloadSize(FrameType type)
        {
            switch (type)
            {
            case FrameType::CONNECT: return sizeof(ConnectPayload);
            case FrameType::CHALLENGE: return sizeof(ChallengePayload);
            case FrameType::AUTH: return sizeof(AuthPayload);
            case FrameType::CONNACK: return sizeof(ConnAckPayload);
            case FrameType::DISCONNECT:
            case FrameType::PING:
            case FrameType::PONG:
            case FrameType::ACK:
                return 0;
            case FrameType::ERROR: return 1;
            default: return -1;
            }
        }
    }

    const char *frameTypeName(uint8_t type)
    {
        switch (static_cast<FrameType>(type))
        {
        case FrameType::BEACON: return "BEACON";
        case FrameType::CONNECT: return "CONNECT";
        case FrameType::CHALLENGE: return "CHALLENGE";
        case FrameType::AUTH: return "AUTH";
        case FrameType::CONNACK: return "CONNACK";
        case FrameType::DISCONNECT: return "DISCONNECT";
        case FrameType::PING: return "PING";
        case FrameType::PONG: return "PONG";
        case FrameType::SUBSCRIBE: return "SUBSCRIBE";
        case FrameType::UNSUBSCRIBE: return "UNSUBSCRIBE";
        case FrameType::MESSAGE: return "MESSAGE";
        case FrameType::LAST_WILL: return "LAST_WILL";
        case FrameType::ACK: return "ACK";
        case FrameType::ERROR: return "ERROR";
        }
        return "?";
    }

    const char *frameCheckName(FrameCheck check)
    {
        switch (check)
        {
        case FrameCheck::OK: return "ok";
        case FrameCheck::TOO_SHORT: return "shorter than a header";
        case FrameCheck::BAD_LENGTH: return "length disagrees with the packet";
        case FrameCheck::UNSUPPORTED_FRAMING: return "unsupported ESP-NOW framing version";
        case FrameCheck::UNSUPPORTED_PROTOCOL: return "unsupported NM protocol version";
        case FrameCheck::UNKNOWN_TYPE: return "unknown frame type";
        case FrameCheck::BAD_FRAGMENT: return "bad fragment fields";
        case FrameCheck::BAD_CID: return "cid not valid for this frame type";
        case FrameCheck::BAD_PAYLOAD: return "wrong payload size";
        }
        return "?";
    }

    FrameCheck validateFrame(const uint8_t *data, size_t length)
    {
        if (data == nullptr || length < FrameHeaderSize)
            return FrameCheck::TOO_SHORT;

        FrameHeader header;
        memcpy(&header, data, FrameHeaderSize);

        // Only V1 has a runtime; V2 is advertised as a capability at most.
        if (espNowVersion(header.version) != static_cast<uint8_t>(EspNowFrameVersion::V1))
            return FrameCheck::UNSUPPORTED_FRAMING;
        if (nmProtocolVersion(header.version) != NM_PROTOCOL_VERSION)
            return FrameCheck::UNSUPPORTED_PROTOCOL;
        if (length > MaxPacketSizeV1 || header.length != length - FrameHeaderSize)
            return FrameCheck::BAD_LENGTH;
        if (!knownType(header.type))
            return FrameCheck::UNKNOWN_TYPE;

        const FrameType type = static_cast<FrameType>(header.type);
        if (header.totalFrames == 0 || header.frameIndex >= header.totalFrames)
            return FrameCheck::BAD_FRAGMENT;
        if (type != FrameType::MESSAGE && header.totalFrames != 1)
            return FrameCheck::BAD_FRAGMENT;

        const CidRule rule = cidRule(type);
        if ((rule == CidRule::ZERO && header.cid != 0) ||
            (rule == CidRule::NONZERO && header.cid == 0))
            return FrameCheck::BAD_CID;

        const int fixed = fixedPayloadSize(type);
        if (fixed >= 0 && header.length != static_cast<uint16_t>(fixed))
            return FrameCheck::BAD_PAYLOAD;
        return FrameCheck::OK;
    }

    FrameCheck decodeFrame(Frame &out, const uint8_t *data, size_t length)
    {
        const FrameCheck check = validateFrame(data, length);
        if (check != FrameCheck::OK)
            return check;
        memcpy(&out.header, data, FrameHeaderSize);
        memcpy(out.data, data + FrameHeaderSize, out.header.length);
        return FrameCheck::OK;
    }

    bool makeFrame(Frame &out, FrameType type, uint16_t messageId, uint16_t cid,
                   const void *data, size_t length, uint8_t frameIndex, uint8_t totalFrames)
    {
        if (length > MaxFrameDataSize || (data == nullptr && length != 0))
            return false;
        out.header.messageId = messageId;
        out.header.version = CurrentFrameVersion;
        out.header.type = static_cast<uint8_t>(type);
        out.header.cid = cid;
        out.header.frameIndex = frameIndex;
        out.header.totalFrames = totalFrames;
        out.header.length = static_cast<uint16_t>(length);
        if (length > 0)
            memcpy(out.data, data, length);
        return true;
    }

    Frame beaconFrame(const char *gatewayId)
    {
        static const uint8_t versions[] = {static_cast<uint8_t>(EspNowFrameVersion::V1)};
        uint8_t data[MaxFrameDataSize] = {};
        data[0] = sizeof(versions);
        memcpy(data + 1, versions, sizeof(versions));
        size_t length = 1 + sizeof(versions);
        const size_t idLength = gatewayId == nullptr ? 0 : strlen(gatewayId);
        if (idLength <= 64 && length + 1 + idLength <= sizeof(data))
        {
            data[length++] = static_cast<uint8_t>(idLength);
            if (idLength != 0)
            {
                memcpy(data + length, gatewayId, idLength);
                length += idLength;
            }
        }
        Frame frame{};
        makeFrame(frame, FrameType::BEACON, 0, 0, data, length);
        return frame;
    }

    bool beaconSupports(const Frame &beacon, EspNowFrameVersion version)
    {
        if (beacon.header.length < 1)
            return false;
        const size_t count = beacon.data[0];
        for (size_t i = 0; i < count && i + 1 < beacon.header.length; ++i)
            if (beacon.data[1 + i] == static_cast<uint8_t>(version))
                return true;
        return false;
    }

    bool beaconGatewayId(const Frame &beacon, char *out, size_t outSize)
    {
        if (out == nullptr || outSize == 0 || beacon.header.length < 2)
            return false;
        const size_t count = beacon.data[0];
        const size_t lengthOffset = 1 + count;
        if (lengthOffset >= beacon.header.length)
            return false;
        const size_t idLength = beacon.data[lengthOffset];
        if (idLength == 0 || idLength >= outSize || lengthOffset + 1 + idLength > beacon.header.length)
            return false;
        memcpy(out, beacon.data + lengthOffset + 1, idLength);
        out[idLength] = '\0';
        return true;
    }
}
