#pragma once
#define GATEWAY_TASK_STACK_SIZE 8192
#define GATEWAY_TASK_PRIORITY 5
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
//watchdog timer
#include "esp_task_wdt.h"
#include "NightmareGateway/NightMare/Message.h"

BaseType_t start_nightmare_gateway(void);

// Answers a "<x>/in" request on "<x>/out" -- the same convention a
// NightMareNetwork device's own console uses -- reaching whoever is
// subscribed to the reply topic, locally and/or over MQTT. Exposed for any
// future gateway/... command handler to reuse, not just the debug one.
void replyMessage(const NightMare::Message &current, const NightMare::Message &response);
