#pragma once
#include <atomic>
#include <stdint.h>

// Monotonic counters the web UI turns into rates. Bumped from whichever task
// the event happens on (ESP-NOW callback, gateway task, MQTT task), so they
// are relaxed atomics; 32-bit ones wrap, and readers treat a drop as a wrap.
struct GatewayStats
{
    // ESP-NOW radio
    std::atomic<uint32_t> espnowRxPackets{0};
    std::atomic<uint32_t> espnowTxPackets{0};
    std::atomic<uint32_t> espnowRxBytes{0};
    std::atomic<uint32_t> espnowTxBytes{0};
    std::atomic<uint32_t> espnowTxFailed{0};
    std::atomic<uint32_t> espnowRxDropped{0};
    std::atomic<uint32_t> invalidFrames{0};
    std::atomic<uint32_t> ignoredFrames{0};

    // Application messages, by direction: "local" is the ESP-NOW side, "remote" is MQTT.
    std::atomic<uint32_t> msgsFromLocal{0}; // a device published to the gateway
    std::atomic<uint32_t> msgsToLocal{0};   // the gateway delivered one to a device
    std::atomic<uint32_t> msgsFromRemote{0}; // arrived from the MQTT broker
    std::atomic<uint32_t> msgsToRemote{0};   // queued for the MQTT broker
};

inline GatewayStats g_gatewayStats;

inline void gwCount(std::atomic<uint32_t> &counter, uint32_t amount = 1)
{
    counter.fetch_add(amount, std::memory_order_relaxed);
}
