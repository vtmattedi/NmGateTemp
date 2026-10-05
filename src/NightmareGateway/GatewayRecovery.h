#pragma once

#include <string>
#include <vector>
#include "NightmareGateway/NightMare/Message.h"

// Backend-side retained recovery policy. The live ESP-NOW registry remains
// owned by espDeviceManager; this only caches other gateways' published
// projections so their LWT can reproduce client LastWills.
class GatewayRecovery
{
public:
    // True when message belongs to the gateway status/snapshot contract.
    // `wills` receives work only once for a current-generation offline event.
    bool consume(const NightMare::Message &message,
                 std::vector<NightMare::Message> &wills);

private:
    struct Gateway
    {
        std::string id;
        std::string generation;
        std::string snapshotGeneration;
        std::string offlineGeneration;
        std::vector<NightMare::Message> wills;
        bool recoveryApplied = false;
    };
    std::vector<Gateway> gateways_;
    Gateway &gateway(const std::string &id);
};
