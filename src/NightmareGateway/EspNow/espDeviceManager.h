#pragma once
#include <stdio.h>
#include <string>
#include <vector>
#include "NightmareGateway/NightMare/Device.h"
#include "NightmareGateway/EspNow/macAddress.h"
#include "esp_now.h"

class espDeviceManager
{
    std::vector<NightMare::Device> devices;

public:
    espDeviceManager();

    NightMare::Device *getDeviceByMac(MacAddress mac);
    uint8_t getDeviceCount();
    void handleDeviceDisconnection(MacAddress mac);
    void handleDeviceConnection(MacAddress mac);
    NightMare::Device *addDevice(MacAddress mac);
    // handles known devices: add if new, sets lastSeen and rssi.
    // Takes the sender out of esp_now_recv_info_t rather than the struct itself:
    // that pointer is only valid inside the receive callback, and frames are
    // processed later on the gateway task.
    NightMare::Device *handleReceivedFrame(const MacAddress &mac, int8_t rssi);

    NightMare::Device *deviceAt(uint8_t index);
    uint8_t getSubscriberCount();

    // Drops devices nothing has been heard from in a while. For each one that
    // had a last will set, onDeviceLost is called with it still valid, before
    // the device is erased -- the caller's chance to publish it (MQTT LWT
    // semantics: the will only fires once the device is presumed gone).
    void pruneStaleDevices(void (*onDeviceLost)(NightMare::Device &device) = nullptr);
};
