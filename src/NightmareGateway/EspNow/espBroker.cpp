#include "espBroker.h"
#include "espDeviceManager.h"
#include "NightmareGateway/NightMare/Topic.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>
#include <algorithm>
#include <atomic>
#include <vector>

static const char *TAG = "espBroker";

#define MAX_FRAMES_PER_MESSAGE 16
// A device (re)connecting sends every subscription plus its last will
// back-to-back, and each SUBSCRIBE costs an ACK and a retained replay to
// handle, so the queue has to absorb a whole burst. ~8 KB at 32.
#define RX_QUEUE_DEPTH 32
#define REASSEMBLY_SLOTS 4
#define REASSEMBLY_TIMEOUT_S 5
#define SEND_TIMEOUT_MS 50
#define BEACON_INTERVAL_S 5

static constexpr size_t FrameHeaderSize = sizeof(NightMare::FrameHeader);
static constexpr size_t FrameDataSize = sizeof(NightMare::Frame::data);
static const uint8_t BROADCAST_MAC[MacAddress::Length] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

struct RxPacket
{
    uint8_t mac[MacAddress::Length];
    int8_t rssi;
    uint8_t length;
    uint8_t data[ESP_NOW_MAX_DATA_LEN];
};

struct Reassembly
{
    bool active = false;
    MacAddress mac;
    uint16_t messageId = 0;
    uint16_t nextFrame = 0;
    uint16_t totalFrames = 0;
    uint64_t startedAt = 0;
    std::vector<uint8_t> buffer;
};

static QueueHandle_t s_rxQueue = NULL;
static SemaphoreHandle_t s_sendDone = NULL;
static espDeviceManager s_devices;
static Reassembly s_reassembly[REASSEMBLY_SLOTS];
static uint16_t s_nextMessageId = 1;
static uint64_t s_lastBeaconSeconds = 0;
static bool s_beaconActive = false;
// The task that runs espBroker_process(), woken on every received packet so
// the queue drains now rather than at its next scheduled tick.
static TaskHandle_t s_processTask = NULL;
// Counted in the receive callback, reported from espBroker_process(): logging
// per drop from the Wi-Fi task is itself a flood during exactly those bursts.
static std::atomic<uint32_t> s_rxDropped{0};

static uint64_t uptimeSeconds(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000000);
}

static void send_cb(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    (void)info;
    (void)status;
    xSemaphoreGive(s_sendDone);
}

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (info == NULL || data == NULL || len <= 0 || len > ESP_NOW_MAX_DATA_LEN)
        return;

    RxPacket packet;
    memcpy(packet.mac, info->src_addr, sizeof(packet.mac));
    packet.rssi = info->rx_ctrl != NULL ? (int8_t)info->rx_ctrl->rssi : 0;
    packet.length = (uint8_t)len;
    memcpy(packet.data, data, len);

    if (xQueueSend(s_rxQueue, &packet, 0) != pdTRUE)
    {
        s_rxDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (s_processTask != NULL)
        xTaskNotifyGive(s_processTask);
}

// Every esp_now_send() in this file funnels through here: ESP-NOW only accepts
// one transmission in flight at a time, so every caller has to wait for the
// radio, not just the ones talking to a known device.
static bool sendRawFrame(const uint8_t *mac, const NightMare::Frame &frame)
{
    if (frame.header.length > FrameDataSize)
        return false;

    // Clear any stale completion left behind by a previous timeout.
    xSemaphoreTake(s_sendDone, 0);

    const esp_err_t err = esp_now_send(mac, (const uint8_t *)&frame, FrameHeaderSize + frame.header.length);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Send failed: %s", esp_err_to_name(err));
        return false;
    }

    return xSemaphoreTake(s_sendDone, pdMS_TO_TICKS(SEND_TIMEOUT_MS)) == pdTRUE;
}

bool espBroker_sendFrame(const NightMare::Device *device, const NightMare::Frame &frame)
{
    if (device == NULL)
        return false;
    return sendRawFrame(device->address().bytes, frame);
}

static void sendBeacon()
{
    static const uint8_t supported[] = {(uint8_t)NightMare::VersionType::ESP_NOW};
    const NightMare::Frame frame = NightMare::beaconFrame(supported, sizeof(supported));
    sendRawFrame(BROADCAST_MAC, frame);
}

static bool sendControlFrame(const NightMare::Device *device, NightMare::FrameType type, uint16_t messageId)
{
    NightMare::Frame frame{};
    frame.header = NightMare::encodeFrameHeader(messageId, NightMare::VersionType::ESP_NOW, type, 0, 1, 0);
    return espBroker_sendFrame(device, frame);
}

bool espBroker_sendMessage(const NightMare::Device *device, const NightMare::Message &message)
{
    if (device == NULL)
        return false;

    const std::vector<uint8_t> raw = message.toRawData();
    if (raw.empty())
        return false;

    const size_t totalFrames = (raw.size() + FrameDataSize - 1) / FrameDataSize;
    if (totalFrames == 0 || totalFrames > MAX_FRAMES_PER_MESSAGE)
        return false;

    const uint16_t messageId = s_nextMessageId++;
    for (size_t i = 0; i < totalFrames; i++)
    {
        const size_t offset = i * FrameDataSize;
        const size_t chunk = std::min(FrameDataSize, raw.size() - offset);

        NightMare::Frame frame{};
        frame.header = NightMare::encodeFrameHeader(messageId, NightMare::VersionType::ESP_NOW,
                                                     NightMare::FrameType::MESSAGE, (uint16_t)i,
                                                     (uint16_t)totalFrames, (uint16_t)chunk);
        memcpy(frame.data, raw.data() + offset, chunk);

        if (!espBroker_sendFrame(device, frame))
            return false;
    }
    return true;
}

bool espBroker_broadcastMessage(const NightMare::Message &message, const NightMare::Device *except)
{
    bool sentAny = false;
    for (uint8_t i = 0; i < s_devices.getDeviceCount(); i++)
    {
        NightMare::Device *device = s_devices.deviceAt(i);
        if (device == NULL || device == except)
            continue;
        if (!device->isSubscribedTo(message.topic))
            continue;
        if (espBroker_sendMessage(device, message))
            sentAny = true;
    }
    return sentAny;
}

static Reassembly *reassemblySlotFor(const MacAddress &mac, uint16_t messageId)
{
    const uint64_t nowSeconds = uptimeSeconds();

    for (Reassembly &slot : s_reassembly)
    {
        if (slot.active && slot.mac == mac && slot.messageId == messageId)
            return &slot;
    }

    for (Reassembly &slot : s_reassembly)
    {
        if (!slot.active || nowSeconds - slot.startedAt > REASSEMBLY_TIMEOUT_S)
        {
            slot.active = false;
            return &slot;
        }
    }
    return NULL;
}

static void handleMessageFrame(NightMare::Device *device, const NightMare::Frame &frame)
{
    if (frame.header.totalFrames == 0 || frame.header.totalFrames > MAX_FRAMES_PER_MESSAGE)
        return;

    // Single frame messages skip the reassembly bookkeeping entirely.
    if (frame.header.totalFrames == 1)
    {
        const NightMare::Message message = NightMare::Message::fromRawData(frame.data, frame.header.length);
        if (!message.topic.empty())
            espBroker_onMessage(device, message);
        return;
    }

    Reassembly *slot = reassemblySlotFor(device->address(), frame.header.messageId);
    if (slot == NULL)
    {
        ESP_LOGW(TAG, "No reassembly slot for message %u", frame.header.messageId);
        return;
    }

    if (!slot->active)
    {
        if (frame.header.frameIndex != 0)
            return; // joined mid-message, wait for the next one to start cleanly
        slot->active = true;
        slot->mac = device->address();
        slot->messageId = frame.header.messageId;
        slot->totalFrames = frame.header.totalFrames;
        slot->nextFrame = 0;
        slot->startedAt = uptimeSeconds();
        slot->buffer.clear();
    }

    // Frames are consumed strictly in order; anything else drops the message.
    if (frame.header.frameIndex != slot->nextFrame)
    {
        ESP_LOGW(TAG, "Out of order frame %u for message %u", frame.header.frameIndex, frame.header.messageId);
        slot->active = false;
        return;
    }

    slot->buffer.insert(slot->buffer.end(), frame.data, frame.data + frame.header.length);
    slot->nextFrame++;

    if (slot->nextFrame >= slot->totalFrames)
    {
        const NightMare::Message message = NightMare::Message::fromRawData(slot->buffer.data(), slot->buffer.size());
        slot->active = false;
        slot->buffer.clear();
        if (!message.topic.empty())
            espBroker_onMessage(device, message);
    }
}

void onEspNowMessageReceived(NightMare::Device *device, const NightMare::Frame &frame)
{
    if (device == NULL)
        return;

    switch ((NightMare::FrameType)frame.header.type)
    {
    case NightMare::FrameType::SUBSCRIBE:
    {
        const std::string filter(reinterpret_cast<const char *>(frame.data), frame.header.length);
        const bool ok = device->subscribeTo(filter);
        ESP_LOGI(TAG, "%s subscribes to '%s'%s", device->address().toString().c_str(),
                 filter.c_str(), ok ? "" : " (rejected)");
        sendControlFrame(device, ok ? NightMare::FrameType::ACK : NightMare::FrameType::ERROR, frame.header.messageId);
        if (ok)
            espBroker_onSubscribe(device, filter);
        break;
    }

    case NightMare::FrameType::UNSUBSCRIBE:
    {
        const std::string filter(reinterpret_cast<const char *>(frame.data), frame.header.length);
        const bool ok = device->unsubscribeFrom(filter);
        ESP_LOGI(TAG, "%s unsubscribes from '%s'", device->address().toString().c_str(), filter.c_str());
        sendControlFrame(device, ok ? NightMare::FrameType::ACK : NightMare::FrameType::ERROR, frame.header.messageId);
        break;
    }

    case NightMare::FrameType::MESSAGE:
        handleMessageFrame(device, frame);
        break;

    case NightMare::FrameType::LAST_WILL:
    {
        // Same [retained|topicLen][topic][payload] encoding as MESSAGE, just
        // stored rather than routed: it only goes out once this device is
        // presumed gone, see publishLastWill() in espBroker_process().
        const NightMare::Message will = NightMare::Message::fromRawData(frame.data, frame.header.length);
        const bool ok = device->setLastWill(will);
        ESP_LOGI(TAG, "%s sets last will on '%s'%s", device->address().toString().c_str(),
                 will.topic.c_str(), ok ? "" : " (rejected)");
        sendControlFrame(device, ok ? NightMare::FrameType::ACK : NightMare::FrameType::ERROR, frame.header.messageId);
        break;
    }

    case NightMare::FrameType::CONTROL:
        // A keep-alive: ACK it so the sender can read a round trip off it, the
        // same way it would off a SUBSCRIBE/UNSUBSCRIBE ACK.
        sendControlFrame(device, NightMare::FrameType::ACK, frame.header.messageId);
        break;

    case NightMare::FrameType::BEACON:
        // Some other node's beacon (or an echo of our own to the broadcast
        // address). handleReceivedFrame() above already bumped this device's
        // lastSeen; nothing else to do with it.
        break;

    default:
        break;
    }
}

static void handlePacket(const RxPacket &packet)
{
    NightMare::FrameHeader header;
    if (!NightMare::decodeFrameHeader(header, packet.data, packet.length))
    {
        ESP_LOGW(TAG, "Malformed frame (%u bytes)", packet.length);
        return;
    }
    if (header.version != (uint8_t)NightMare::VersionType::ESP_NOW)
    {
        ESP_LOGW(TAG, "Unsupported frame version %u", header.version);
        return;
    }

    NightMare::Frame frame{};
    frame.header = header;
    memcpy(frame.data, packet.data + FrameHeaderSize, header.length);

    const MacAddress mac(packet.mac);
    NightMare::Device *device = s_devices.handleReceivedFrame(mac, packet.rssi);
    if (device == NULL)
        return;

    onEspNowMessageReceived(device, frame);
}

bool espBroker_init(void)
{
    // espBroker_init and espBroker_process run on the same (gateway) task.
    s_processTask = xTaskGetCurrentTaskHandle();
    s_rxQueue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(RxPacket));
    s_sendDone = xSemaphoreCreateBinary();
    if (s_rxQueue == NULL || s_sendDone == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate ESP-NOW resources");
        return false;
    }

    // Modem sleep parks the radio between beacons and drops ESP-NOW traffic.
    esp_wifi_set_ps(WIFI_PS_NONE);

    const esp_err_t err = esp_now_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_now_init failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));
    ESP_ERROR_CHECK(esp_now_register_send_cb(send_cb));

    esp_now_peer_info_t broadcastPeer = {};
    memcpy(broadcastPeer.peer_addr, BROADCAST_MAC, sizeof(broadcastPeer.peer_addr));
    broadcastPeer.ifidx = WIFI_IF_STA;
    broadcastPeer.encrypt = false;
    esp_now_add_peer(&broadcastPeer);

    uint8_t mac[MacAddress::Length] = {};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    ESP_LOGI(TAG, "ESP-NOW up on %s", MacAddress(mac).toString().c_str());

    // Beaconing starts here and runs unconditionally from espBroker_process()
    // from now on -- not gated on any peer being known or subscribed.
    s_beaconActive = true;
    s_lastBeaconSeconds = uptimeSeconds();
    return true;
}

// MQTT LWT semantics: fires once, only once the device is presumed gone --
// never on a clean UNSUBSCRIBE, since there isn't one for a whole device.
static void publishLastWill(NightMare::Device &device)
{
    ESP_LOGI(TAG, "%s went stale, publishing its last will on '%s'",
             device.address().toString().c_str(), device.lastWillMessage().topic.c_str());
    espBroker_onMessage(&device, device.lastWillMessage());
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

    s_devices.pruneStaleDevices(publishLastWill);

    // Lets a device (or a scanner) find this gateway passively, without
    // having to broadcast a SUBSCRIBE first and hope something answers.
    const uint64_t nowSeconds = uptimeSeconds();
    if (nowSeconds - s_lastBeaconSeconds >= BEACON_INTERVAL_S)
    {
        sendBeacon();
        s_lastBeaconSeconds = nowSeconds;
    }
}

uint8_t espBroker_subscriberCount(void)
{
    return s_devices.getSubscriberCount();
}

bool espBroker_beaconActive(void)
{
    return s_beaconActive;
}

uint32_t espBroker_secondsSinceLastBeacon(void)
{
    if (!s_beaconActive)
        return UINT32_MAX;
    return (uint32_t)(uptimeSeconds() - s_lastBeaconSeconds);
}

uint8_t espBroker_deviceCount(void)
{
    return s_devices.getDeviceCount();
}

const NightMare::Device *espBroker_deviceAt(uint8_t index)
{
    return s_devices.deviceAt(index);
}
