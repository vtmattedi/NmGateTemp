#pragma once
#include <stdint.h>
#include <stddef.h>

// NightMare ESP-NOW session authentication. Shared byte for byte by the gateway
// and the NightMareNetwork client.
//
// Both values are HMAC-SHA256 keyed with the network PSK, truncated to 16 bytes,
// over
//     label | clientMac(6) | gatewayMac(6) | clientNonce(8, LE) | gatewayNonce(8, LE)
// with label "NM-AUTH" for the proof sent in AUTH and "NM-LMK" for the ESP-NOW
// peer key. The labels keep the two apart: the proof is on the wire, the LMK
// never is. Each MAC is the one ESP-NOW reports as the sender, never a value a
// frame claims.

namespace NightMare::EspNowAuth
{
    constexpr size_t MacSize = 6;
    constexpr size_t ProofSize = 16;
    constexpr size_t LmkSize = 16;
    // A shorter PSK is refused by both ends at compile time.
    constexpr size_t MinPskLength = 16;

    struct HandshakeContext
    {
        uint8_t clientMac[MacSize];
        uint8_t gatewayMac[MacSize];
        uint64_t clientNonce;
        uint64_t gatewayNonce;
    };

    // False only if the HMAC could not be computed; `out` is then zeroed.
    bool authProof(const uint8_t *psk, size_t pskLength, const HandshakeContext &context,
                   uint8_t out[ProofSize]);
    bool sessionLmk(const uint8_t *psk, size_t pskLength, const HandshakeContext &context,
                    uint8_t out[LmkSize]);

    // Timing does not depend on where the inputs differ.
    bool constantTimeEqual(const uint8_t *a, const uint8_t *b, size_t length);

    // From the hardware RNG. Fresh per connection attempt, never reused.
    uint64_t freshNonce();

    // Zeroes key material in a way the compiler cannot drop.
    void wipe(void *data, size_t length);
}
