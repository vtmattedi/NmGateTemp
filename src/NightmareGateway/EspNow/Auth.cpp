#include "Auth.h"
#include <esp_random.h>
#include <mbedtls/md.h>
#include <string.h>

// Shared byte for byte by the gateway and the NightMareNetwork client.

namespace NightMare::EspNowAuth
{
    namespace
    {
        constexpr char AuthLabel[] = "NM-AUTH";
        constexpr char LmkLabel[] = "NM-LMK";
        constexpr size_t MaxLabelLength = 8;
        constexpr size_t Sha256Size = 32;

        void putLe64(uint8_t *out, uint64_t value)
        {
            for (size_t i = 0; i < 8; ++i)
                out[i] = static_cast<uint8_t>(value >> (8 * i));
        }

        bool derive(const char *label, const uint8_t *psk, size_t pskLength,
                    const HandshakeContext &context, uint8_t *out, size_t outLength)
        {
            memset(out, 0, outLength);
            const size_t labelLength = strlen(label);
            if (psk == nullptr || pskLength == 0 || labelLength > MaxLabelLength)
                return false;

            uint8_t message[MaxLabelLength + 2 * MacSize + 2 * 8];
            size_t used = 0;
            memcpy(message + used, label, labelLength);
            used += labelLength;
            memcpy(message + used, context.clientMac, MacSize);
            used += MacSize;
            memcpy(message + used, context.gatewayMac, MacSize);
            used += MacSize;
            putLe64(message + used, context.clientNonce);
            used += 8;
            putLe64(message + used, context.gatewayNonce);
            used += 8;

            uint8_t digest[Sha256Size];
            const mbedtls_md_info_t *sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
            const bool ok = sha256 != nullptr &&
                            mbedtls_md_hmac(sha256, psk, pskLength, message, used, digest) == 0;
            if (ok)
                memcpy(out, digest, outLength);
            wipe(digest, sizeof(digest));
            wipe(message, sizeof(message));
            return ok;
        }
    }

    bool authProof(const uint8_t *psk, size_t pskLength, const HandshakeContext &context,
                   uint8_t out[ProofSize])
    {
        return derive(AuthLabel, psk, pskLength, context, out, ProofSize);
    }

    bool sessionLmk(const uint8_t *psk, size_t pskLength, const HandshakeContext &context,
                    uint8_t out[LmkSize])
    {
        return derive(LmkLabel, psk, pskLength, context, out, LmkSize);
    }

    bool constantTimeEqual(const uint8_t *a, const uint8_t *b, size_t length)
    {
        uint8_t difference = 0;
        for (size_t i = 0; i < length; ++i)
            difference |= static_cast<uint8_t>(a[i] ^ b[i]);
        return difference == 0;
    }

    uint64_t freshNonce()
    {
        uint64_t nonce = 0;
        esp_fill_random(&nonce, sizeof(nonce));
        return nonce;
    }

    void wipe(void *data, size_t length)
    {
        volatile uint8_t *bytes = static_cast<volatile uint8_t *>(data);
        while (length-- > 0)
            *bytes++ = 0;
    }
}
