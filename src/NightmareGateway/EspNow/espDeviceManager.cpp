#include "espDeviceManager.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "espDevices";

#define MAX_DEVICES 16
#define DEVICE_TIMEOUT_S 300

static uint64_t uptimeSeconds(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000000);
}

espDeviceManager::espDeviceManager()
{
    // Reserving up front keeps the Device pointers handed out below stable:
    // the table never grows past MAX_DEVICES, so it never reallocates.
    devices.reserve(MAX_DEVICES);
}

NightMare::Device *espDeviceManager::getDeviceByMac(MacAddress mac)
{
    for (NightMare::Device &device : devices)
    {
        if (device.address() == mac)
            return &device;
    }
    return nullptr;
}

uint8_t espDeviceManager::getDeviceCount()
{
    return (uint8_t)devices.size();
}

void espDeviceManager::handleDeviceConnection(MacAddress mac)
{
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac.bytes, sizeof(peer.peer_addr));
    peer.channel = 0; // follow whatever channel the station is on
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;

    const esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST)
        ESP_LOGE(TAG, "Failed to add peer %s: %s", mac.toString().c_str(), esp_err_to_name(err));
}

void espDeviceManager::handleDeviceDisconnection(MacAddress mac)
{
    esp_now_del_peer(mac.bytes);

    for (size_t i = 0; i < devices.size(); i++)
    {
        if (devices[i].address() == mac)
        {
            devices.erase(devices.begin() + i);
            return;
        }
    }
}

NightMare::Device *espDeviceManager::addDevice(MacAddress mac)
{
    if (devices.size() >= MAX_DEVICES)
    {
        ESP_LOGW(TAG, "Device table full, ignoring %s", mac.toString().c_str());
        return nullptr;
    }

    handleDeviceConnection(mac);
    devices.emplace_back(mac);
    ESP_LOGI(TAG, "Registered device %s", mac.toString().c_str());
    return &devices.back();
}

NightMare::Device *espDeviceManager::handleReceivedFrame(const MacAddress &mac, int8_t rssi)
{
    NightMare::Device *device = getDeviceByMac(mac);
    if (device == nullptr)
        device = addDevice(mac);
    if (device == nullptr)
        return nullptr;

    device->markSeen((uint8_t)rssi, uptimeSeconds());
    return device;
}

NightMare::Device *espDeviceManager::deviceAt(uint8_t index)
{
    if (index >= devices.size())
        return nullptr;
    return &devices[index];
}

uint8_t espDeviceManager::getSubscriberCount()
{
    uint8_t count = 0;
    for (const NightMare::Device &device : devices)
    {
        if (device.subscriptionCount() > 0)
            count++;
    }
    return count;
}

void espDeviceManager::pruneStaleDevices(void (*onDeviceLost)(NightMare::Device &device))
{
    const uint64_t nowSeconds = uptimeSeconds();
    for (size_t i = devices.size(); i > 0; i--)
    {
        NightMare::Device &device = devices[i - 1];
        if (nowSeconds - device.lastSeenAt() <= DEVICE_TIMEOUT_S)
            continue;

        ESP_LOGI(TAG, "Dropping stale device %s", device.address().toString().c_str());
        if (onDeviceLost != nullptr && device.hasLastWill())
            onDeviceLost(device);
        handleDeviceDisconnection(device.address());
    }
}
