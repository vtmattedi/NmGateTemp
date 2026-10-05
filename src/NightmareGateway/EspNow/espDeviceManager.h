#pragma once
#include <stdio.h>
#include <string>
#include <vector>
#include "NightmareGateway/NightMare/Device.h"
#include "NightmareGateway/EspNow/macAddress.h"
#include "NightmareGateway/EspNow/Frame.h"

// The gateway's ESP-NOW session table: pending handshakes, established
// sessions (Devices), the cid allocator, and the rule for which frames each
// sender may send (admit()).
//
// Nothing here touches the radio. Peer changes go through PeerOps, which the
// broker points at esp_now_* and the tests at fakes.
class espDeviceManager
{
public:
    static constexpr size_t MaxDevices = 16;
    // Sessions + pending + the broadcast peer must fit ESP-NOW's 20-peer table.
    static constexpr size_t MaxPending = 3;
    static constexpr uint64_t HandshakeTimeoutMs = 5000; // CONNECT -> AUTH
    static constexpr uint64_t SecuringTimeoutMs = 5000;  // AUTH -> first encrypted PING

    struct PeerOps
    {
        // Registers mac as a plaintext peer (or turns an existing one back to plaintext).
        bool (*add)(const MacAddress &mac) = nullptr;
        // Installs the session LMK: from here on unicast to/from mac is encrypted.
        bool (*secure)(const MacAddress &mac, const uint8_t *lmk) = nullptr;
        void (*remove)(const MacAddress &mac) = nullptr;
    };

    struct PendingHandshake
    {
        bool active = false;
        MacAddress mac;
        uint64_t clientNonce = 0;
        uint64_t gatewayNonce = 0;
        uint16_t capabilities = 0;
        uint64_t startedAtMs = 0;
    };

    enum class Verdict : uint8_t
    {
        HANDSHAKE,       // CONNECT, or AUTH from a sender with a pending handshake
        SESSION,         // right MAC, right cid, allowed in the session's state
        IGNORE,          // unknown sender, a suspended session, or a frame it may not send
        INVALID_SESSION, // known MAC, wrong cid: tell it to start over
        NOT_CONNECTED,   // right cid, but the session has not finished securing
    };

    struct Admission
    {
        Verdict verdict;
        NightMare::Device *device; // set for SESSION, INVALID_SESSION, NOT_CONNECTED
    };

    explicit espDeviceManager(uint64_t sessionTimeoutMs, uint16_t firstCid = 1);
    void setPeerOps(const PeerOps &ops) { peers = ops; }

    // Which path a validated frame from `mac` takes. Never creates anything.
    Admission admit(const MacAddress &mac, const NightMare::FrameHeader &header);

    // CONNECT. Nothing here is proven yet, so an existing session for this MAC
    // is kept: it is only suspended, because the handshake needs the peer in
    // plaintext. A repeated CONNECT replaces the pending entry. Null when the
    // pending table is full of live entries or the peer could not be turned to
    // plaintext -- and then any existing session is left untouched.
    PendingHandshake *beginHandshake(const MacAddress &mac, uint64_t clientNonce, uint16_t capabilities,
                                     uint64_t gatewayNonce, uint64_t nowMs);
    PendingHandshake *pendingFor(const MacAddress &mac);
    // Failed or expired: forgets the nonces, and puts a suspended session back
    // exactly as it was, encryption included. Without one, forgets the peer.
    void abandonHandshake(const MacAddress &mac);
    // AUTH verified -- the only thing that may replace a session. Consumes the
    // pending entry (nonces wiped), drops any previous session for this MAC
    // without firing its last will, and creates an AUTHENTICATED session with
    // a fresh non-zero cid. Null (handshake abandoned) when the session table
    // is full; replacing a session never needs room, so a reconnecting device
    // always gets back in.
    NightMare::Device *completeHandshake(const MacAddress &mac, uint64_t nowMs);
    // Installs the LMK and moves to SECURING. False (session ended) if the
    // peer could not be encrypted.
    bool secureSession(NightMare::Device &device, const uint8_t *lmk);
    void endSession(const MacAddress &mac);

    NightMare::Device *sessionFor(const MacAddress &mac);
    NightMare::Device *sessionByCid(uint16_t cid);

    // Times out handshakes, sessions stuck before CONNECTED, and CONNECTED
    // sessions silent for the session timeout. onLost is called for the last
    // kind only (the ones whose last will should fire), before removal.
    void expire(uint64_t nowMs, void (*onLost)(NightMare::Device &device) = nullptr);

    // Drops every session and pending handshake without touching the radio or
    // firing last wills: for when the whole ESP-NOW stack is being torn down.
    void reset();

    uint8_t getDeviceCount() const { return (uint8_t)devices.size(); }
    NightMare::Device *deviceAt(uint8_t index);
    bool hasConnectedDeviceNamed(const std::string &name) const;
    uint8_t getSubscriberCount() const; // CONNECTED sessions with at least one subscription
    uint8_t pendingCount() const;

private:
    uint16_t allocateCid();
    void removePeer(const MacAddress &mac);
    // keepPeer: the caller is about to reuse the peer for a new session.
    void eraseSession(const MacAddress &mac, bool keepPeer);

    std::vector<NightMare::Device> devices;
    PendingHandshake pending[MaxPending];
    PeerOps peers;
    uint64_t sessionTimeoutMs;
    uint16_t nextCid;
};
