#pragma once
#include <stdint.h>
#include <stddef.h>

// NightMare ESP-NOW wire contract, pre-v1. This file is shared byte for byte by
// the gateway and the NightMareNetwork client; the full contract (handshake,
// sessions, encryption) is in docs/modules/espnow-protocol.md.
//
// Every multi-byte field is little-endian (both ends are ESP32s and the structs
// are packed, so the wire is simply the in-memory layout).

namespace NightMare
{
    // Low nibble of FrameHeader::version. 0 is the current pre-v1 contract and
    // stays 0 until an explicit v1 freeze.
    constexpr uint8_t NM_PROTOCOL_VERSION = 0;

    // High nibble of FrameHeader::version: the ESP-NOW payload limit the frame
    // was built for. Not the NM protocol revision.
    enum class EspNowFrameVersion : uint8_t
    {
        V1 = 0, // 250-byte ESP-NOW payload
        V2 = 1, // 1470-byte ESP-NOW payload -- capability only, not sent or accepted yet
    };

    constexpr uint8_t makeVersion(uint8_t espNowVersion, uint8_t nmVersion)
    {
        return static_cast<uint8_t>(((espNowVersion & 0x0F) << 4) | (nmVersion & 0x0F));
    }

    constexpr uint8_t espNowVersion(uint8_t version)
    {
        return static_cast<uint8_t>((version >> 4) & 0x0F);
    }

    constexpr uint8_t nmProtocolVersion(uint8_t version)
    {
        return static_cast<uint8_t>(version & 0x0F);
    }

    // What this build puts on the wire.
    constexpr uint8_t CurrentFrameVersion =
        makeVersion(static_cast<uint8_t>(EspNowFrameVersion::V1), NM_PROTOCOL_VERSION);

    enum class FrameType : uint8_t
    {
        BEACON = 0,

        CONNECT = 1,
        CHALLENGE = 2,
        AUTH = 3,
        CONNACK = 4,
        DISCONNECT = 5,

        PING = 6,
        PONG = 7,

        SUBSCRIBE = 8,
        UNSUBSCRIBE = 9,
        MESSAGE = 10,
        LAST_WILL = 11,

        ACK = 12,
        ERROR = 13,
    };

    struct FrameHeader // 10 bytes
    {
        uint16_t messageId;  // request/response correlation; fragments of one MESSAGE share it
        uint8_t version;     // high nibble EspNowFrameVersion, low nibble NM protocol version
        uint8_t type;        // FrameType
        uint16_t cid;        // 0 = no session (handshake); otherwise the gateway-assigned session
        uint8_t frameIndex;  // 0..totalFrames-1
        uint8_t totalFrames; // 1 for everything but a fragmented MESSAGE
        uint16_t length;     // bytes of data after the header
    } __attribute__((packed));
    static_assert(sizeof(FrameHeader) == 10, "FrameHeader must stay 10 bytes");

    constexpr size_t FrameHeaderSize = sizeof(FrameHeader);
    constexpr size_t MaxPacketSizeV1 = 250;
    constexpr size_t MaxPacketSizeV2 = 1470;
    constexpr size_t MaxFrameDataSize = MaxPacketSizeV1 - FrameHeaderSize;   // 240
    constexpr size_t MaxFrameDataSizeV2 = MaxPacketSizeV2 - FrameHeaderSize; // 1460
    // What either side reassembles. The header could say 255.
    constexpr uint8_t MaxFramesPerMessage = 16;

    struct Frame
    {
        FrameHeader header;
        uint8_t data[MaxFrameDataSize];
    } __attribute__((packed));

    // --- Handshake payloads -------------------------------------------------

    // Capability bits, CONNECT and CONNACK. None is defined for use yet: V2
    // framing is reserved here and never advertised until it has a runtime.
    enum Capability : uint16_t
    {
        CAPABILITY_FRAME_V2 = 1u << 0,
    };

    struct ConnectPayload // client -> gateway, cid 0
    {
        uint64_t clientNonce;
        uint16_t capabilities;
    } __attribute__((packed));

    struct ChallengePayload // gateway -> client, cid 0, messageId echoes the CONNECT
    {
        uint64_t gatewayNonce;
    } __attribute__((packed));

    constexpr size_t AuthProofSize = 16;
    struct AuthPayload // client -> gateway, cid 0
    {
        uint8_t proof[AuthProofSize];
    } __attribute__((packed));

    struct ConnAckPayload // gateway -> client, header.cid = the new session's cid
    {
        uint16_t capabilities;
        uint32_t heartbeatMs;
    } __attribute__((packed));

    // ERROR payload: one byte.
    enum class ErrorCode : uint8_t
    {
        AUTH_FAILED = 1,
        UNSUPPORTED_VERSION = 2,
        INVALID_SESSION = 3, // cid unknown/stale for this sender: start a new handshake
        INVALID_FRAME = 4,
        NOT_CONNECTED = 5,   // session exists but has not finished securing
        REJECTED = 6,        // a well-formed request refused (e.g. subscription table full)
    };

    // --- Validation ---------------------------------------------------------

    enum class FrameCheck : uint8_t
    {
        OK,
        TOO_SHORT,            // fewer bytes than a header
        BAD_LENGTH,           // header.length disagrees with the bytes received
        UNSUPPORTED_FRAMING,  // ESP-NOW framing version this build does not run
        UNSUPPORTED_PROTOCOL, // NM protocol version other than NM_PROTOCOL_VERSION
        UNKNOWN_TYPE,
        BAD_FRAGMENT,         // totalFrames 0, frameIndex out of range, or fragments on a non-MESSAGE
        TOO_MANY_FRAGMENTS,   // over MaxFramesPerMessage
        BAD_CID,              // cid 0 where a session is required, or the reverse
        BAD_PAYLOAD,          // wrong payload size for a fixed-size type
    };

    const char *frameTypeName(uint8_t type);
    const char *frameCheckName(FrameCheck check);

    // Every received packet goes through this before anything reads its payload.
    FrameCheck validateFrame(const uint8_t *data, size_t length);

    // validateFrame(), then copies the header and data out. `out` is untouched
    // unless the result is OK.
    FrameCheck decodeFrame(Frame &out, const uint8_t *data, size_t length);

    // Header for this build's version. Fails (false) if length does not fit a
    // V1 frame.
    bool makeFrame(Frame &out, FrameType type, uint16_t messageId, uint16_t cid,
                   const void *data = nullptr, size_t length = 0,
                   uint8_t frameIndex = 0, uint8_t totalFrames = 1);

    // Bytes to hand to esp_now_send() for a frame built above.
    inline size_t frameSize(const Frame &frame) { return FrameHeaderSize + frame.header.length; }

    // Broadcast, plaintext, cid 0. Data: [count][EspNowFrameVersion...], the
    // framing versions this gateway runs. No secret material.
    Frame beaconFrame();
    // Whether a beacon's data lists `version`.
    bool beaconSupports(const Frame &beacon, EspNowFrameVersion version);
}
