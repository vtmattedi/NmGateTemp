#include "espBroker.h"
#include "espDeviceManager.h"
#include "Auth.h"
#include "Reassembly.h"
#include "NightmareGateway/NightMare/Topic.h"
#include "NightmareGateway/GatewayStats.h"
#include "System/events.h"
#include "NightmareGateway/GatewayState.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>
#include <algorithm>
#include <atomic>
#include <utility>
#include <vector>

static const char *TAG = "espBroker";

// A device (re)connecting sends every subscription plus its last will
// back-to-back, and each SUBSCRIBE costs an ACK and a retained replay to
// handle, so the queue has to absorb a whole burst. ~8 KB at 32.
#define RX_QUEUE_DEPTH 32
#define SEND_TIMEOUT_MS 50
#define BEACON_INTERVAL_MS 5000
// Told to every client in CONNACK. A session silent for SESSION_TIMEOUT_MS is
// gone: its last will fires and its cid is released.
#define HEARTBEAT_MS 15000
#define SESSION_TIMEOUT_MS (4 * HEARTBEAT_MS)

using NightMare::ConnectionState;
using NightMare::Device;
using NightMare::ErrorCode;
using NightMare::Frame;
using NightMare::FrameCheck;
using NightMare::FrameType;
namespace Auth = NightMare::EspNowAuth;

static const uint8_t BROADCAST_MAC[MacAddress::Length] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
// Copied in by espBroker_init(); the caller's buffer may go away after that.
static uint8_t s_psk[Auth::MaxPskLength];
static size_t s_pskLength = 0;

struct RxPacket
{
    uint8_t mac[MacAddress::Length];
    int8_t rssi;
    uint8_t length;
    // V1 only: a packet over 250 bytes is dropped in the receive callback.
    uint8_t data[NightMare::MaxPacketSizeV1];
};

static QueueHandle_t s_rxQueue = NULL;
static SemaphoreHandle_t s_sendDone = NULL;
static espDeviceManager s_devices(SESSION_TIMEOUT_MS);
static ReassemblyTable s_reassembly;
static uint16_t s_nextMessageId = 1;
static std::atomic<uint64_t> s_lastBeaconMs{0};
static std::atomic<bool> s_beaconActive{false};
static std::atomic<uint8_t> s_deviceCount{0};
static std::atomic<uint8_t> s_subscriberCount{0};
static MacAddress s_ownMac;
// The task that runs espBroker_process(), woken on every received packet so
// the queue drains now rather than at its next scheduled tick.
static TaskHandle_t s_processTask = NULL;
// Counted where they happen, reported once per beacon interval: logging each
// one is itself a flood when an old-protocol or rogue sender is chattering.
static std::atomic<uint32_t> s_rxDropped{0};
static uint32_t s_invalidFrames = 0;
static uint32_t s_ignoredFrames = 0;

// What the web server may read: the device table belongs to the gateway task,
// so it publishes a copy under this lock instead of letting others walk it.
static SemaphoreHandle_t s_snapshotLock = NULL;
static std::vector<EspBrokerDeviceInfo> s_snapshot;
static uint64_t s_snapshotAtMs = 0;
#define SNAPSHOT_INTERVAL_MS 500
// A session that has missed two heartbeats is on its way to being timed out.
#define DISCONNECT_CANDIDATE_MS (2 * HEARTBEAT_MS)

static uint64_t nowMs(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static uint16_t newMessageId(void)
{
    const uint16_t id = s_nextMessageId++;
    if (s_nextMessageId == 0)
        s_nextMessageId = 1;
    return id;
}

// --- Peers -------------------------------------------------------------------

static esp_now_peer_info_t peerInfo(const MacAddress &mac)
{
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac.bytes, sizeof(peer.peer_addr));
    peer.channel = 0; // follow whatever channel the station is on
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    return peer;
}

static bool peerAdd(const MacAddress &mac)
{
    esp_now_peer_info_t peer = peerInfo(mac);
    // A peer left over from an earlier session is turned back to plaintext.
    // Asking first avoids esp_now_add_peer() logging "Peer exists" every time.
    const esp_err_t err = esp_now_is_peer_exist(mac.bytes) ? esp_now_mod_peer(&peer) : esp_now_add_peer(&peer);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "Peer %s: %s", mac.toString().c_str(), esp_err_to_name(err));
    return err == ESP_OK;
}

static bool peerSecure(const MacAddress &mac, const uint8_t *lmk)
{
    esp_now_peer_info_t peer = peerInfo(mac);
    peer.encrypt = true;
    memcpy(peer.lmk, lmk, ESP_NOW_KEY_LEN);
    const esp_err_t err = esp_now_mod_peer(&peer);
    Auth::wipe(peer.lmk, sizeof(peer.lmk));
    if (err != ESP_OK)
        ESP_LOGE(TAG, "Encrypting peer %s: %s", mac.toString().c_str(), esp_err_to_name(err));
    return err == ESP_OK;
}

static void peerRemove(const MacAddress &mac)
{
    esp_now_del_peer(mac.bytes);
}

// --- Sending -----------------------------------------------------------------

// The radio's verdict on the frame in flight, stamped as the callback fires so
// sendRawFrame() can time the link-layer ACK without counting task wake-up.
static std::atomic<uint64_t> s_sendDoneUs{0};
static std::atomic<bool> s_sendOk{false};

static void send_cb(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    (void)info;
    s_sendOk.store(status == ESP_NOW_SEND_SUCCESS, std::memory_order_relaxed);
    s_sendDoneUs.store((uint64_t)esp_timer_get_time(), std::memory_order_relaxed);
    if (status != ESP_NOW_SEND_SUCCESS)
        gwCount(g_gatewayStats.espnowTxFailed);
    xSemaphoreGive(s_sendDone);
}

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (info == NULL || data == NULL || len <= 0 || len > (int)NightMare::MaxPacketSizeV1)
        return;

    gwCount(g_gatewayStats.espnowRxPackets);
    gwCount(g_gatewayStats.espnowRxBytes, (uint32_t)len);

    RxPacket packet;
    memcpy(packet.mac, info->src_addr, sizeof(packet.mac));
    packet.rssi = info->rx_ctrl != NULL ? (int8_t)info->rx_ctrl->rssi : 0;
    packet.length = (uint8_t)len;
    memcpy(packet.data, data, len);

    if (xQueueSend(s_rxQueue, &packet, 0) != pdTRUE)
    {
        s_rxDropped.fetch_add(1, std::memory_order_relaxed);
        gwCount(g_gatewayStats.espnowRxDropped);
        return;
    }
    if (s_processTask != NULL)
        xTaskNotifyGive(s_processTask);
}

// Every esp_now_send() in this file funnels through here: ESP-NOW only accepts
// one transmission in flight at a time, so every caller has to wait for the
// radio. Whether it goes out encrypted is the peer's setting, not the frame's.
static bool sendRawFrame(const uint8_t *mac, const Frame &frame)
{
    if (frame.header.length > NightMare::MaxFrameDataSize)
        return false;

    // Clear any stale completion left behind by a previous timeout.
    xSemaphoreTake(s_sendDone, 0);

    const uint64_t startUs = (uint64_t)esp_timer_get_time();
    const esp_err_t err = esp_now_send(mac, (const uint8_t *)&frame, NightMare::frameSize(frame));
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Send %s failed: %s", NightMare::frameTypeName(frame.header.type), esp_err_to_name(err));
        gwCount(g_gatewayStats.espnowTxFailed);
        return false;
    }
    gwCount(g_gatewayStats.espnowTxPackets);
    gwCount(g_gatewayStats.espnowTxBytes, (uint32_t)NightMare::frameSize(frame));

    const bool done = xSemaphoreTake(s_sendDone, pdMS_TO_TICKS(SEND_TIMEOUT_MS)) == pdTRUE;

    // A unicast that was ACKed doubles as a ping: how long the round trip took.
    if (done && s_sendOk.load(std::memory_order_relaxed) && memcmp(mac, BROADCAST_MAC, MacAddress::Length) != 0)
    {
        Device *device = s_devices.sessionFor(MacAddress(mac));
        const uint64_t doneUs = s_sendDoneUs.load(std::memory_order_relaxed);
        if (device != NULL && doneUs >= startUs)
            device->noteRtt((float)(doneUs - startUs) / 1000.0f);
    }
    return done;
}

static bool sendTo(const MacAddress &mac, FrameType type, uint16_t messageId, uint16_t cid,
                   const void *data = nullptr, size_t length = 0)
{
    Frame frame{};
    return NightMare::makeFrame(frame, type, messageId, cid, data, length) && sendRawFrame(mac.bytes, frame);
}

static bool sendError(const MacAddress &mac, uint16_t cid, uint16_t messageId, ErrorCode code)
{
    const uint8_t payload = (uint8_t)code;
    return sendTo(mac, FrameType::ERROR, messageId, cid, &payload, sizeof(payload));
}

bool espBroker_sendFrame(const Device *device, const Frame &frame)
{
    if (device == NULL || !device->isConnected())
        return false;
    return sendRawFrame(device->address().bytes, frame);
}

static void sendBeacon()
{
    const Frame frame = NightMare::beaconFrame(GatewayState::id().c_str());
    sendRawFrame(BROADCAST_MAC, frame);
}

bool espBroker_sendMessage(const Device *device, const NightMare::Message &message)
{
    if (device == NULL || !device->isConnected())
        return false;

    const std::vector<uint8_t> raw = message.toRawData();
    if (raw.empty())
        return false;

    const size_t dataSize = NightMare::MaxFrameDataSize;
    const size_t totalFrames = (raw.size() + dataSize - 1) / dataSize;
    if (totalFrames == 0 || totalFrames > UINT8_MAX) // the header's fragment fields
        return false;

    const uint16_t messageId = newMessageId();
    for (size_t i = 0; i < totalFrames; i++)
    {
        const size_t offset = i * dataSize;
        const size_t chunk = std::min(dataSize, raw.size() - offset);

        Frame frame{};
        if (!NightMare::makeFrame(frame, FrameType::MESSAGE, messageId, device->cid(), raw.data() + offset, chunk,
                                  (uint8_t)i, (uint8_t)totalFrames) ||
            !espBroker_sendFrame(device, frame))
            return false;
    }
    gwCount(g_gatewayStats.msgsToLocal);
    return true;
}

bool espBroker_broadcastMessage(const NightMare::Message &message, const Device *except)
{
    bool sentAny = false;
    for (uint8_t i = 0; i < s_devices.getDeviceCount(); i++)
    {
        Device *device = s_devices.deviceAt(i);
        if (device == NULL || device == except || !device->isConnected())
            continue;
        if (!device->isSubscribedTo(message.topic))
            continue;
        if (espBroker_sendMessage(device, message))
            sentAny = true;
    }
    return sentAny;
}

// --- Handshake ---------------------------------------------------------------

static Auth::HandshakeContext handshakeContext(const espDeviceManager::PendingHandshake &pending)
{
    Auth::HandshakeContext context{};
    memcpy(context.clientMac, pending.mac.bytes, Auth::MacSize);
    memcpy(context.gatewayMac, s_ownMac.bytes, Auth::MacSize);
    context.clientNonce = pending.clientNonce;
    context.gatewayNonce = pending.gatewayNonce;
    return context;
}

static void handleConnect(const MacAddress &mac, const Frame &frame)
{
    NightMare::ConnectPayload connect;
    memcpy(&connect, frame.data, sizeof(connect));

    const uint64_t gatewayNonce = Auth::freshNonce();
    espDeviceManager::PendingHandshake *pending =
        s_devices.beginHandshake(mac, connect.clientNonce, connect.capabilities, gatewayNonce, nowMs());
    if (pending == NULL)
        return;

    // The messageId echo is how the client pairs this with its CONNECT.
    const NightMare::ChallengePayload challenge{gatewayNonce};
    if (!sendTo(mac, FrameType::CHALLENGE, frame.header.messageId, 0, &challenge, sizeof(challenge)))
    {
        s_devices.abandonHandshake(mac);
        return;
    }
    ESP_LOGI(TAG, "CONNECT from %s, challenged", mac.toString().c_str());
}

static void handleAuth(const MacAddress &mac, const Frame &frame)
{
    espDeviceManager::PendingHandshake *pending = s_devices.pendingFor(mac);
    if (pending == NULL)
        return;

    NightMare::AuthPayload auth;
    memcpy(&auth, frame.data, sizeof(auth));
    Auth::HandshakeContext context = handshakeContext(*pending);

    uint8_t expected[Auth::ProofSize];
    const bool proofOk = Auth::authProof(s_psk, s_pskLength, context, expected) &&
                         Auth::constantTimeEqual(expected, auth.proof, Auth::ProofSize);
    Auth::wipe(expected, sizeof(expected));
    if (!proofOk)
    {
        ESP_LOGI(EVENT_TAG, "client auth failed: %s (wrong network key?)", mac.toString().c_str());
        sendError(mac, 0, frame.header.messageId, ErrorCode::AUTH_FAILED);
        s_devices.abandonHandshake(mac);
        Auth::wipe(&context, sizeof(context));
        return;
    }

    uint8_t lmk[Auth::LmkSize];
    const bool lmkOk = Auth::sessionLmk(s_psk, s_pskLength, context, lmk);
    Auth::wipe(&context, sizeof(context));
    Device *device = lmkOk ? s_devices.completeHandshake(mac, nowMs()) : NULL;
    if (device == NULL)
    {
        Auth::wipe(lmk, sizeof(lmk));
        s_devices.abandonHandshake(mac);
        return;
    }

    // CONNACK is the last plaintext frame, so it goes out before the peer is
    // encrypted. The client's encrypted PING that follows finishes the session.
    const NightMare::ConnAckPayload ack{0, HEARTBEAT_MS};
    const bool sent = sendTo(mac, FrameType::CONNACK, frame.header.messageId, device->cid(), &ack, sizeof(ack));
    const uint16_t cid = device->cid();
    const bool secured = sent && s_devices.secureSession(*device, lmk);
    Auth::wipe(lmk, sizeof(lmk));
    if (!sent)
    {
        s_devices.endSession(mac);
        return;
    }
    if (secured)
        ESP_LOGI(TAG, "%s authenticated as session %u, securing", mac.toString().c_str(), cid);
}

// --- Session traffic ---------------------------------------------------------

static void handleMessageFrame(Device *device, const Frame &frame)
{
    std::vector<uint8_t> whole;
    switch (s_reassembly.feed(device->address(), frame.header, frame.data, nowMs(), whole))
    {
    case ReassemblyTable::Result::COMPLETE:
    {
        NightMare::Message message = NightMare::Message::fromRawData(std::move(whole));
        if (!message.topic.empty())
            espBroker_onMessage(device, std::move(message));
        break;
    }
    case ReassemblyTable::Result::DROPPED:
        ESP_LOGW(TAG, "Session %u: fragment %u/%u of message %u dropped", device->cid(),
                 frame.header.frameIndex + 1, frame.header.totalFrames, frame.header.messageId);
        break;
    case ReassemblyTable::Result::INCOMPLETE:
        break;
    }
}

static void reply(const Device *device, FrameType type, uint16_t messageId)
{
    sendTo(device->address(), type, messageId, device->cid());
}

// Only reached through admit(): right MAC, right cid, and CONNECTED -- or
// SECURING for the PING that completes it.
static void handleSessionFrame(Device *device, const Frame &frame)
{
    switch ((FrameType)frame.header.type)
    {
    case FrameType::PING:
        if (device->state() == ConnectionState::SECURING)
        {
            // It arrived decrypted with the session LMK: both ends hold the same key.
            device->setState(ConnectionState::CONNECTED);
            ESP_LOGI(EVENT_TAG, "client connected: %s cid=%u", device->label().c_str(), (unsigned)device->cid());
        }
        reply(device, FrameType::PONG, frame.header.messageId);
        break;

    case FrameType::DISCONNECT:
    {
        // A clean goodbye: no last will, like an MQTT DISCONNECT.
        if (device->state() == ConnectionState::CONNECTED)
            ESP_LOGI(EVENT_TAG, "client disconnected: %s cid=%u (said goodbye)", device->label().c_str(),
                     (unsigned)device->cid());
        const MacAddress mac = device->address();
        s_devices.endSession(mac);
        s_reassembly.drop(mac);
        break;
    }

    case FrameType::SUBSCRIBE:
    {
        const std::string filter(reinterpret_cast<const char *>(frame.data), frame.header.length);
        const bool ok = device->subscribeTo(filter);
        if (ok)
        {
            ESP_LOGI(TAG, "%s subscribed request #%u cid=%u filter='%s' length=%u subscriptions=%u/%u",
                     device->label().c_str(), (unsigned)frame.header.messageId, (unsigned)device->cid(),
                     filter.c_str(), (unsigned)filter.size(), (unsigned)device->subscriptionCount(),
                     (unsigned)device->subscriptionCapacity());
            reply(device, FrameType::ACK, frame.header.messageId);
        }
        else
        {
            const char *reason = filter.empty() ? "empty filter"
                                 : filter.size() > NightMare::MaxTopicLength ? "filter too long"
                                                                           : "subscription table full";
            ESP_LOGW(TAG,
                     "%s rejected SUBSCRIBE request #%u cid=%u filter='%s' length=%u reason=%s "
                     "subscriptions=%u/%u",
                     device->label().c_str(), (unsigned)frame.header.messageId, (unsigned)device->cid(),
                     filter.c_str(), (unsigned)filter.size(), reason, (unsigned)device->subscriptionCount(),
                     (unsigned)device->subscriptionCapacity());
            sendError(device->address(), device->cid(), frame.header.messageId, ErrorCode::REJECTED);
        }
        if (ok)
            espBroker_onSubscribe(device, filter);
        break;
    }

    case FrameType::UNSUBSCRIBE:
    {
        const std::string filter(reinterpret_cast<const char *>(frame.data), frame.header.length);
        const bool ok = device->unsubscribeFrom(filter);
        ESP_LOGI(TAG, "%s unsubscribes from '%s'", device->address().toString().c_str(), filter.c_str());
        if (ok)
            reply(device, FrameType::ACK, frame.header.messageId);
        else
            sendError(device->address(), device->cid(), frame.header.messageId, ErrorCode::REJECTED);
        break;
    }

    case FrameType::MESSAGE:
        handleMessageFrame(device, frame);
        break;

    case FrameType::LAST_WILL:
    {
        // Same [retained|topicLen][topic][payload] encoding as MESSAGE, just
        // stored rather than routed: it only goes out once this device is
        // presumed gone, see publishLastWill().
        const NightMare::Message will = NightMare::Message::fromRawData(frame.data, frame.header.length);
        const bool ok = device->setLastWill(will);
        ESP_LOGI(TAG, "%s sets last will on '%s'%s", device->address().toString().c_str(),
                 will.topic.c_str(), ok ? "" : " (rejected)");
        // A "<name>/status" will names the device before it has published any
        // status itself. A real status message still overrides this later.
        if (ok && device->assumedName().empty() && topicMatchesPattern(will.topic, "+/status"))
            device->setAssumedName(will.topic.substr(0, will.topic.size() - strlen("/status")));
        if (ok)
            reply(device, FrameType::ACK, frame.header.messageId);
        else
            sendError(device->address(), device->cid(), frame.header.messageId, ErrorCode::REJECTED);
        break;
    }

    default:
        // PONG, ACK, ERROR from a client: nothing on the gateway waits for them.
        break;
    }
}

static void handlePacket(const RxPacket &packet)
{
    const MacAddress mac(packet.mac);
    Frame frame{};
    const FrameCheck check = NightMare::decodeFrame(frame, packet.data, packet.length);
    if (check != FrameCheck::OK)
    {
        s_invalidFrames++;
        gwCount(g_gatewayStats.invalidFrames);
        // Only a sender with a live session is worth answering; anyone else
        // could be an old-protocol device, noise, or an unproven handshake.
        const Device *device = s_devices.sessionFor(mac);
        if (device != NULL && !device->isSuspended())
        {
            ESP_LOGW(TAG, "Invalid frame from session %u: %s", device->cid(), NightMare::frameCheckName(check));
            const bool version = check == FrameCheck::UNSUPPORTED_FRAMING || check == FrameCheck::UNSUPPORTED_PROTOCOL;
            sendError(mac, device->cid(), 0, version ? ErrorCode::UNSUPPORTED_VERSION : ErrorCode::INVALID_FRAME);
        }
        return;
    }

    const espDeviceManager::Admission admission = s_devices.admit(mac, frame.header);
    switch (admission.verdict)
    {
    case espDeviceManager::Verdict::HANDSHAKE:
        if ((FrameType)frame.header.type == FrameType::CONNECT)
            handleConnect(mac, frame);
        else
            handleAuth(mac, frame);
        break;

    case espDeviceManager::Verdict::SESSION:
        admission.device->markSeen(packet.rssi, nowMs());
        handleSessionFrame(admission.device, frame);
        break;

    case espDeviceManager::Verdict::INVALID_SESSION:
        // This MAC holds a session here, but under another cid: the client is
        // out of step and must start a new handshake. cid 0 -- it has no valid
        // one. (A MAC with no session at all, e.g. after this gateway rebooted,
        // is IGNOREd instead: no peer is added just to answer it.)
        ESP_LOGW(TAG, "%s from %s with stale cid %u (session is %u)", NightMare::frameTypeName(frame.header.type),
                 mac.toString().c_str(), frame.header.cid, admission.device->cid());
        sendError(mac, 0, frame.header.messageId, ErrorCode::INVALID_SESSION);
        break;

    case espDeviceManager::Verdict::NOT_CONNECTED:
        sendError(mac, admission.device->cid(), frame.header.messageId, ErrorCode::NOT_CONNECTED);
        break;

    case espDeviceManager::Verdict::IGNORE:
        s_ignoredFrames++;
        gwCount(g_gatewayStats.ignoredFrames);
        break;
    }
}

bool espBroker_init(const uint8_t *psk, size_t pskLength)
{
    if (psk == NULL || pskLength < Auth::MinPskLength || pskLength > Auth::MaxPskLength)
    {
        ESP_LOGE(TAG, "ESP-NOW network key must be %u..%u bytes, got %u",
                 (unsigned)Auth::MinPskLength, (unsigned)Auth::MaxPskLength, (unsigned)pskLength);
        return false;
    }
    memmove(s_psk, psk, pskLength); // psk may be s_psk itself, from espBroker_resume()
    s_pskLength = pskLength;

    // espBroker_init and espBroker_process run on the same (gateway) task.
    s_processTask = xTaskGetCurrentTaskHandle();
    // Kept across espBroker_shutdown()/resume(), so only made the first time.
    if (s_snapshotLock == NULL)
        s_snapshotLock = xSemaphoreCreateMutex();
    if (s_rxQueue == NULL)
        s_rxQueue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(RxPacket));
    if (s_sendDone == NULL)
        s_sendDone = xSemaphoreCreateBinary();
    if (s_rxQueue == NULL || s_sendDone == NULL || s_snapshotLock == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate ESP-NOW resources");
        return false;
    }

    espDeviceManager::PeerOps ops;
    ops.add = peerAdd;
    ops.secure = peerSecure;
    ops.remove = peerRemove;
    s_devices.setPeerOps(ops);

    // Modem sleep parks the radio between beacons and drops ESP-NOW traffic.
    esp_wifi_set_ps(WIFI_PS_NONE);

    const esp_err_t err = esp_now_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_now_init failed: %s", esp_err_to_name(err));
        return false;
    }

    // The network's own PMK, not Espressif's default; every node derives the
    // same one from the PSK. Set before any encrypted peer exists.
    uint8_t pmk[Auth::PmkSize];
    const bool pmkOk = Auth::networkPmk(s_psk, s_pskLength, pmk) && esp_now_set_pmk(pmk) == ESP_OK;
    Auth::wipe(pmk, sizeof(pmk));
    if (!pmkOk)
    {
        ESP_LOGE(TAG, "Could not set the ESP-NOW PMK");
        esp_now_deinit();
        return false;
    }

    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));
    ESP_ERROR_CHECK(esp_now_register_send_cb(send_cb));

    esp_now_peer_info_t broadcastPeer = {};
    memcpy(broadcastPeer.peer_addr, BROADCAST_MAC, sizeof(broadcastPeer.peer_addr));
    broadcastPeer.ifidx = WIFI_IF_STA;
    broadcastPeer.encrypt = false;
    esp_now_add_peer(&broadcastPeer);

    esp_wifi_get_mac(WIFI_IF_STA, s_ownMac.bytes);
    ESP_LOGI(TAG, "ESP-NOW up on %s (protocol %u, heartbeat %u ms)", s_ownMac.toString().c_str(),
             (unsigned)NightMare::NM_PROTOCOL_VERSION, (unsigned)HEARTBEAT_MS);

    // Beaconing starts here and runs unconditionally from espBroker_process()
    // from now on -- not gated on any peer being known or subscribed.
    s_beaconActive.store(true, std::memory_order_release);
    s_lastBeaconMs.store(nowMs(), std::memory_order_release);
    return true;
}

// MQTT LWT semantics: fires once, only once the device is presumed gone --
// never on a clean DISCONNECT or a reconnect.
static void publishLastWill(Device &device)
{
    ESP_LOGI(TAG, "%s went stale, publishing its last will on '%s'",
             device.address().toString().c_str(), device.lastWillMessage().topic.c_str());
    espBroker_onMessage(&device, device.lastWillMessage());
}

static void refreshSnapshot(void)
{
    const uint64_t now = nowMs();
    if (s_snapshotLock == NULL || now - s_snapshotAtMs < SNAPSHOT_INTERVAL_MS)
        return;

    std::vector<EspBrokerDeviceInfo> fresh;
    fresh.reserve(s_devices.getDeviceCount());
    for (uint8_t i = 0; i < s_devices.getDeviceCount(); i++)
    {
        const Device *device = s_devices.deviceAt(i);
        if (device == NULL)
            continue;

        EspBrokerDeviceInfo info;
        info.mac = device->address().toString();
        info.name = device->assumedName();
        info.cid = device->cid();
        info.state = device->state();
        info.suspended = device->isSuspended();
        info.subscriptions = device->subscriptionList();
        info.hasLastWill = device->hasLastWill();
        info.lastWillTopic = device->lastWillMessage().topic;
        info.lastWillPayload = device->lastWillMessage().payload;
        info.lastWillRetained = device->lastWillMessage().persistent;
        info.lastWillPayloadSize = device->lastWillMessage().payload.size();
        info.hasRssi = device->hasRssi();
        info.rssi = device->lastRssi();
        info.avgRssi = device->averageRssi();
        info.rxFrames = device->rxFrames();
        info.hasRtt = device->hasRtt();
        info.rttMs = device->rttMs();
        info.sessionStartMs = device->sessionStartMs();
        info.lastSeenMs = device->lastSeenAtMs();
        const uint64_t silentMs = now > info.lastSeenMs ? now - info.lastSeenMs : 0;
        info.disconnectCandidate = info.suspended || silentMs > DISCONNECT_CANDIDATE_MS;
        fresh.push_back(std::move(info));
    }

    xSemaphoreTake(s_snapshotLock, portMAX_DELAY);
    s_snapshot.swap(fresh);
    s_snapshotAtMs = now;
    xSemaphoreGive(s_snapshotLock);
}

void espBroker_process(void)
{
    if (s_rxQueue == NULL)
        return;

    RxPacket packet;
    while (xQueueReceive(s_rxQueue, &packet, 0) == pdTRUE)
        handlePacket(packet);

    const uint32_t dropped = s_rxDropped.exchange(0, std::memory_order_relaxed);
    if (dropped != 0)
        ESP_LOGW(TAG, "RX queue full, dropped %lu packet(s)", (unsigned long)dropped);

    s_devices.expire(nowMs(), publishLastWill);
    refreshSnapshot();

    // Lets a device (or a scanner) find this gateway passively.
    const uint64_t now = nowMs();
    if (now - s_lastBeaconMs.load(std::memory_order_relaxed) >= BEACON_INTERVAL_MS)
    {
        sendBeacon();
        s_lastBeaconMs.store(now, std::memory_order_release);
        if (s_invalidFrames != 0 || s_ignoredFrames != 0)
        {
            ESP_LOGW(TAG, "Last %u s: %lu invalid frame(s), %lu from senders without a session",
                     BEACON_INTERVAL_MS / 1000, (unsigned long)s_invalidFrames, (unsigned long)s_ignoredFrames);
            s_invalidFrames = 0;
            s_ignoredFrames = 0;
        }
    }

    // The gateway task owns s_devices. Other tasks consume these snapshots
    // instead of racing vector mutation while serving status requests.
    s_deviceCount.store(s_devices.getDeviceCount(), std::memory_order_release);
    s_subscriberCount.store(s_devices.getSubscriberCount(), std::memory_order_release);
}

uint8_t espBroker_subscriberCount(void)
{
    return s_subscriberCount.load(std::memory_order_acquire);
}

bool espBroker_beaconActive(void)
{
    return s_beaconActive.load(std::memory_order_acquire);
}

uint32_t espBroker_secondsSinceLastBeacon(void)
{
    if (!s_beaconActive.load(std::memory_order_acquire))
        return UINT32_MAX;
    return (uint32_t)((nowMs() - s_lastBeaconMs.load(std::memory_order_acquire)) / 1000);
}

uint8_t espBroker_deviceCount(void)
{
    return s_deviceCount.load(std::memory_order_acquire);
}

void espBroker_shutdown(void)
{
    if (!s_beaconActive.exchange(false, std::memory_order_acq_rel))
        return; // never came up, or already down

    for (uint8_t i = 0; i < s_devices.getDeviceCount(); i++)
    {
        const Device *device = s_devices.deviceAt(i);
        if (device != NULL && device->state() == ConnectionState::CONNECTED)
            ESP_LOGI(EVENT_TAG, "client disconnected: %s cid=%u (gateway stopped)", device->label().c_str(),
                     (unsigned)device->cid());
    }

    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_now_deinit(); // drops every peer, so no per-session peer cleanup is needed

    xQueueReset(s_rxQueue);
    for (uint8_t i = 0; i < s_devices.getDeviceCount(); i++)
    {
        const Device *device = s_devices.deviceAt(i);
        if (device != NULL)
            s_reassembly.drop(device->address());
    }
    s_devices.reset();

    s_deviceCount.store(0, std::memory_order_release);
    s_subscriberCount.store(0, std::memory_order_release);
    xSemaphoreTake(s_snapshotLock, portMAX_DELAY);
    s_snapshot.clear();
    xSemaphoreGive(s_snapshotLock);
    ESP_LOGI(TAG, "ESP-NOW stopped, all sessions dropped");
}

bool espBroker_resume(void)
{
    return espBroker_init(s_psk, s_pskLength);
}

size_t espBroker_snapshotDevices(std::vector<EspBrokerDeviceInfo> &out)
{
    if (s_snapshotLock == NULL)
        return 0;
    xSemaphoreTake(s_snapshotLock, portMAX_DELAY);
    out = s_snapshot;
    xSemaphoreGive(s_snapshotLock);
    return out.size();
}

uint32_t espBroker_heartbeatMs(void)
{
    return HEARTBEAT_MS;
}

uint32_t espBroker_sessionTimeoutMs(void)
{
    return SESSION_TIMEOUT_MS;
}

const Device *espBroker_deviceAt(uint8_t index)
{
    return s_devices.deviceAt(index);
}

bool espBroker_connectedDeviceNamed(const std::string &name)
{
    return s_devices.hasConnectedDeviceNamed(name);
}
