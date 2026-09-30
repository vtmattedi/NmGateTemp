#pragma once
#include <stdio.h>
#include "NightmareGateway/EspNow/Frame.h"

/// @brief ESP-NOW authentication frame handling for the Nightmare Gateway system.
/// Auth Stategy.
/// Auth Frames must be in v1 i.e. max 240 bytes.
/// Auth LifeCycle should be handled by the controller here is just the protocol.
/// Auth Frame Data: should be a JSON object with the following fields:
/// - "type": 1 byte Auth Frame Type.
/// - "Method": 1 byte Auth Method.
/// - "payload": payload to be parsed by the auth Method: must be 238 bytes max for now.

namespace NightMare
{
    enum class AuthFrameType : uint8_t
    {
        AUTH_SUCCESS,
        AUTH_FAILURE,
        AUTH_REQUEST,
        AUTH_AVAILABLE_METHODS,
    };

    enum class AuthMethod : uint8_t
    {
        NONE = 0,
        PSK = 1,
        CERTIFICATE = 2,
    };

    struct AuthResult
    {
        bool wasSuccessful = false;
        Frame responseFrame;
    };
}