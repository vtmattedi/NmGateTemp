#pragma once

#include <string>
#include <vector>
#include "NightmareGateway/EspNow/espBroker.h"

namespace GatewayState
{
void begin();
const std::string &id();
const std::string &generation();
std::string statusTopic();
std::string networkTopic();
std::string clientsTopic();
std::string statusJson(bool online);
std::string networkJson(bool online);
std::string clientsJson(const std::vector<EspBrokerDeviceInfo> &devices);
}
