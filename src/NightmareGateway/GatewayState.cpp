#include "GatewayState.h"

#include "EspWifi/espWifi.h"
#include "NightmareGateway/EspNow/espBroker.h"
#include "NightmareGateway/EspMqtt/espMqtt.h"
#include "cJSON.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include <stdio.h>
#include <stdlib.h>

namespace GatewayState
{
namespace
{
std::string gatewayId;
std::string bootGeneration;

std::string render(cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    std::string result = text == nullptr ? std::string() : std::string(text);
    cJSON_free(text);
    cJSON_Delete(root);
    return result;
}

void addIdentity(cJSON *root, bool online)
{
    cJSON_AddNumberToObject(root, "schema", 1);
    cJSON_AddStringToObject(root, "device", gatewayId.c_str());
    cJSON_AddStringToObject(root, "id", gatewayId.c_str());
    cJSON_AddStringToObject(root, "kind", "gateway");
    cJSON_AddBoolToObject(root, "online", online);
    cJSON_AddStringToObject(root, "generation", bootGeneration.c_str());
}
}

void begin()
{
    if (!gatewayId.empty())
        return;
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char idText[32];
    snprintf(idText, sizeof(idText), "nmnw-gateway-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    gatewayId = idText;
    char generationText[17];
    snprintf(generationText, sizeof(generationText), "%08lx%08lx",
             (unsigned long)esp_random(), (unsigned long)esp_random());
    bootGeneration = generationText;
}

const std::string &id() { begin(); return gatewayId; }
const std::string &generation() { begin(); return bootGeneration; }
std::string statusTopic() { return id() + "/status"; }
std::string networkTopic() { return id() + "/gateway/network"; }
std::string clientsTopic() { return id() + "/gateway/clients"; }

std::string statusJson(bool online)
{
    cJSON *root = cJSON_CreateObject();
    addIdentity(root, online);
    return render(root);
}

std::string networkJson(bool online)
{
    cJSON *root = cJSON_CreateObject();
    addIdentity(root, online);
    cJSON *capabilities = cJSON_AddObjectToObject(root, "capabilities");
    cJSON_AddBoolToObject(capabilities, "esp_now", espBroker_beaconActive());
#if defined(REMOTE_MQTT_URL) && defined(REMOTE_MQTT_PORT)
    constexpr bool remoteBridge = true;
    constexpr bool localBridge = false;
#elif defined(LOCAL_MQTT_HOST) && defined(LOCAL_MQTT_PORT)
    constexpr bool remoteBridge = false;
    constexpr bool localBridge = true;
#else
    constexpr bool remoteBridge = false;
    constexpr bool localBridge = false;
#endif
    cJSON_AddBoolToObject(capabilities, "mqtt_bridge_remote", remoteBridge);
    cJSON_AddBoolToObject(capabilities, "mqtt_bridge_local", localBridge);

    wifi_ap_record_t ap = {};
    const bool associated = wifi_is_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    cJSON *radio = cJSON_AddObjectToObject(root, "radio");
    cJSON_AddNumberToObject(radio, "channel", associated ? ap.primary : 0);
    cJSON_AddStringToObject(radio, "ssid", associated ? reinterpret_cast<const char *>(ap.ssid) : "");
    char bssid[18] = {};
    if (associated)
        snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
                 ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5]);
    cJSON_AddStringToObject(radio, "bssid", bssid);

    cJSON *uplinks = cJSON_AddObjectToObject(root, "uplinks");
    cJSON *remote = cJSON_AddObjectToObject(uplinks, "remote_mqtt");
    cJSON *local = cJSON_AddObjectToObject(uplinks, "local_mqtt");
    cJSON_AddBoolToObject(remote, "ready", online && remoteBridge && mqtt_is_connected());
    cJSON_AddBoolToObject(local, "ready", online && localBridge && mqtt_is_connected());
    return render(root);
}

std::string clientsJson(const std::vector<EspBrokerDeviceInfo> &devices)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "schema", 1);
    cJSON_AddStringToObject(root, "id", id().c_str());
    cJSON_AddStringToObject(root, "generation", generation().c_str());
    cJSON *clients = cJSON_AddArrayToObject(root, "clients");
    for (const EspBrokerDeviceInfo &device : devices)
    {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "id", device.mac.c_str());
        cJSON_AddStringToObject(entry, "name", device.name.c_str());
        cJSON_AddBoolToObject(entry, "connected", device.state == NightMare::ConnectionState::CONNECTED && !device.suspended);
        cJSON *will = cJSON_AddObjectToObject(entry, "last_will");
        cJSON_AddStringToObject(will, "topic", device.lastWillTopic.c_str());
        cJSON_AddBoolToObject(will, "retained", device.lastWillRetained);
        cJSON *payload = cJSON_AddArrayToObject(will, "payload");
        for (uint8_t byte : device.lastWillPayload)
            cJSON_AddItemToArray(payload, cJSON_CreateNumber(byte));
        cJSON_AddItemToArray(clients, entry);
    }
    return render(root);
}
}
