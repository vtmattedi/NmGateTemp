#include "espDeviceManager.h"
#include "NightmareGateway/EspNow/Auth.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "espSessions";

using NightMare::ConnectionState;
using NightMare::Device;
using NightMare::FrameType;

static_assert(Device::SessionKeySize == NightMare::EspNowAuth::LmkSize,
              "A Device must hold exactly one session LMK");

espDeviceManager::espDeviceManager(uint64_t sessionTimeoutMs, uint16_t firstCid)
    : sessionTimeoutMs(sessionTimeoutMs), nextCid(firstCid == 0 ? 1 : firstCid)
{
    // Reserving up front keeps Device pointers stable while no session is
    // added: the table never grows past MaxDevices, so it never reallocates.
    devices.reserve(MaxDevices);
}

void espDeviceManager::removePeer(const MacAddress &mac)
{
    if (peers.remove != nullptr)
        peers.remove(mac);
}

espDeviceManager::Admission espDeviceManager::admit(const MacAddress &mac, const NightMare::FrameHeader &header)
{
    const FrameType type = static_cast<FrameType>(header.type);
    switch (type)
    {
    case FrameType::CONNECT:
        return {Verdict::HANDSHAKE, nullptr};
    case FrameType::AUTH:
        return {pendingFor(mac) != nullptr ? Verdict::HANDSHAKE : Verdict::IGNORE, nullptr};
    case FrameType::BEACON:    // another gateway, or our own echo
    case FrameType::CHALLENGE: // only a gateway sends these
    case FrameType::CONNACK:
        return {Verdict::IGNORE, nullptr};
    default:
        break;
    }

    // Everything else belongs to a session: the sender MAC picks it, the cid
    // must then match. A valid cid from another MAC finds no session here.
    Device *device = sessionFor(mac);
    if (device == nullptr)
        return {Verdict::IGNORE, nullptr};
    // Suspended: the peer is plaintext for a handshake nobody has proven yet,
    // so anything claiming to be this session is unauthenticated. Dropped
    // without a word -- an error here would tell the real device to tear down
    // a session that is about to be handed straight back to it.
    if (device->isSuspended())
        return {Verdict::IGNORE, nullptr};
    if (header.cid != device->cid())
        return {Verdict::INVALID_SESSION, device};

    switch (device->state())
    {
    case ConnectionState::CONNECTED:
        return {Verdict::SESSION, device};
    case ConnectionState::SECURING:
        // The encrypted PING is what completes it; DISCONNECT may always end it.
        if (type == FrameType::PING || type == FrameType::DISCONNECT)
            return {Verdict::SESSION, device};
        return {Verdict::NOT_CONNECTED, device};
    default:
        return {Verdict::NOT_CONNECTED, device};
    }
}

espDeviceManager::PendingHandshake *espDeviceManager::pendingFor(const MacAddress &mac)
{
    for (PendingHandshake &entry : pending)
        if (entry.active && entry.mac == mac)
            return &entry;
    return nullptr;
}

espDeviceManager::PendingHandshake *espDeviceManager::beginHandshake(const MacAddress &mac, uint64_t clientNonce,
                                                                     uint16_t capabilities, uint64_t gatewayNonce,
                                                                     uint64_t nowMs)
{
    PendingHandshake *slot = pendingFor(mac);
    if (slot == nullptr)
    {
        for (PendingHandshake &entry : pending)
        {
            if (!entry.active || nowMs - entry.startedAtMs > HandshakeTimeoutMs)
            {
                if (entry.active)
                    abandonHandshake(entry.mac);
                slot = &entry;
                break;
            }
        }
    }
    if (slot == nullptr)
    {
        ESP_LOGW(TAG, "Handshake table full, ignoring CONNECT from %s", mac.toString().c_str());
        return nullptr;
    }

    // The handshake runs in plaintext: the device asking for one cannot hold
    // the current session's key (that is the whole reason it is asking), so
    // the peer goes back to plaintext for the duration.
    if (peers.add != nullptr && !peers.add(mac))
    {
        ESP_LOGW(TAG, "Could not add %s as a peer", mac.toString().c_str());
        slot->active = false;
        return nullptr;
    }
    // Only now, with the peer actually plaintext, does the session pause --
    // and it survives: nothing but a verified AUTH may replace it.
    Device *existing = sessionFor(mac);
    if (existing != nullptr && !existing->isSuspended())
    {
        existing->suspend();
        ESP_LOGI(TAG, "Handshake from %s; session %u paused until it is proven", mac.toString().c_str(),
                 existing->cid());
    }
    slot->active = true;
    slot->mac = mac;
    slot->clientNonce = clientNonce;
    slot->gatewayNonce = gatewayNonce;
    slot->capabilities = capabilities;
    slot->startedAtMs = nowMs;
    return slot;
}

void espDeviceManager::abandonHandshake(const MacAddress &mac)
{
    PendingHandshake *entry = pendingFor(mac);
    if (entry == nullptr)
        return;
    NightMare::EspNowAuth::wipe(entry, sizeof(*entry)); // also clears `active`

    Device *existing = sessionFor(mac);
    if (existing == nullptr)
    {
        removePeer(mac);
        return;
    }
    if (!existing->isSuspended())
        return;

    // Nothing was proven, so the session goes back exactly as it was -- which
    // means putting its key back on the peer we turned to plaintext.
    const bool restored = existing->hasSessionKey() &&
                          (peers.secure == nullptr || peers.secure(mac, existing->sessionKey()));
    if (!restored)
    {
        // Encryption cannot be restored, so the session is unusable. End it
        // quietly: the device is presumably still there and will reconnect.
        ESP_LOGE(TAG, "Could not re-secure session %u (%s); ending it", existing->cid(), mac.toString().c_str());
        eraseSession(mac, false);
        return;
    }
    existing->resume();
    ESP_LOGI(TAG, "Handshake with %s went nowhere; session %u resumes", mac.toString().c_str(), existing->cid());
}

Device *espDeviceManager::completeHandshake(const MacAddress &mac, uint64_t nowMs)
{
    PendingHandshake *entry = pendingFor(mac);
    if (entry == nullptr)
        return nullptr;

    // Replacing takes the old session's place, so only a brand new one needs
    // room in the table.
    Device *previous = sessionFor(mac);
    if (previous == nullptr && devices.size() >= MaxDevices)
    {
        ESP_LOGW(TAG, "Session table full, refusing %s", mac.toString().c_str());
        abandonHandshake(mac);
        return nullptr;
    }
    // The nonces are only needed to verify AUTH and derive the LMK, both done
    // by the caller before this.
    NightMare::EspNowAuth::wipe(entry, sizeof(*entry));

    if (previous != nullptr)
    {
        // A verified AUTH: the sender holds the network key, so it is the
        // device, and this supersedes its old session. No last will -- it is
        // reconnecting, not gone. The peer stays, for the new session.
        ESP_LOGI(TAG, "%s proved itself; session %u replaced", mac.toString().c_str(), previous->cid());
        eraseSession(mac, true);
    }
    devices.emplace_back(mac, allocateCid(), nowMs);
    return &devices.back();
}

bool espDeviceManager::secureSession(Device &device, const uint8_t *lmk)
{
    if (peers.secure != nullptr && !peers.secure(device.address(), lmk))
    {
        const MacAddress mac = device.address();
        endSession(mac);
        return false;
    }
    // Kept so an unproven handshake later can put encryption back.
    device.setSessionKey(lmk);
    device.setState(ConnectionState::SECURING);
    return true;
}

void espDeviceManager::eraseSession(const MacAddress &mac, bool keepPeer)
{
    for (size_t i = 0; i < devices.size(); i++)
    {
        if (devices[i].address() == mac)
        {
            devices[i].forgetSessionKey();
            devices.erase(devices.begin() + i);
            if (!keepPeer)
                removePeer(mac);
            return;
        }
    }
}

void espDeviceManager::endSession(const MacAddress &mac)
{
    eraseSession(mac, false);
}

Device *espDeviceManager::sessionFor(const MacAddress &mac)
{
    for (Device &device : devices)
        if (device.address() == mac)
            return &device;
    return nullptr;
}

Device *espDeviceManager::sessionByCid(uint16_t cid)
{
    if (cid == 0)
        return nullptr;
    for (Device &device : devices)
        if (device.cid() == cid)
            return &device;
    return nullptr;
}

// Unique among live sessions, never 0. Wraps; a value comes back only once no
// session holds it.
uint16_t espDeviceManager::allocateCid()
{
    for (;;)
    {
        const uint16_t candidate = nextCid++;
        if (nextCid == 0)
            nextCid = 1;
        if (candidate != 0 && sessionByCid(candidate) == nullptr)
            return candidate;
    }
}

void espDeviceManager::expire(uint64_t nowMs, void (*onLost)(Device &device))
{
    for (PendingHandshake &entry : pending)
    {
        if (entry.active && nowMs - entry.startedAtMs > HandshakeTimeoutMs)
        {
            ESP_LOGI(TAG, "Handshake with %s timed out", entry.mac.toString().c_str());
            const MacAddress mac = entry.mac;
            abandonHandshake(mac);
        }
    }

    for (size_t i = devices.size(); i > 0; i--)
    {
        Device &device = devices[i - 1];
        // Paused for a handshake: nothing is accepted from it, so its silence
        // means nothing. The handshake timeout above decides its fate first.
        if (device.isSuspended())
            continue;
        const uint64_t silentMs = nowMs - device.lastSeenAtMs();
        const bool connected = device.isConnected();
        if (silentMs <= (connected ? sessionTimeoutMs : SecuringTimeoutMs))
            continue;

        ESP_LOGI(TAG, "Session %u (%s) %s", device.cid(), device.address().toString().c_str(),
                 connected ? "went silent" : "never finished securing");
        if (connected && onLost != nullptr && device.hasLastWill())
            onLost(device);
        const MacAddress mac = device.address();
        endSession(mac);
    }
}

Device *espDeviceManager::deviceAt(uint8_t index)
{
    if (index >= devices.size())
        return nullptr;
    return &devices[index];
}

uint8_t espDeviceManager::getSubscriberCount() const
{
    uint8_t count = 0;
    for (const Device &device : devices)
        if (device.isConnected() && device.subscriptionCount() > 0)
            count++;
    return count;
}

uint8_t espDeviceManager::pendingCount() const
{
    uint8_t count = 0;
    for (const PendingHandshake &entry : pending)
        if (entry.active)
            count++;
    return count;
}
