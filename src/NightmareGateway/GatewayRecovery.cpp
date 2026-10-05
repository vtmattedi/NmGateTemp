#include "GatewayRecovery.h"

#include "cJSON.h"
#include <cstring>
#include <utility>

namespace
{
bool suffixId(const std::string &topic, const char *suffix, std::string &id)
{
    const size_t length = strlen(suffix);
    if (topic.size() <= length || topic.compare(topic.size() - length, length, suffix) != 0)
        return false;
    id = topic.substr(0, topic.size() - length);
    return id.find('/') == std::string::npos;
}

cJSON *parse(const NightMare::Message &message)
{
    std::string payload(message.payload.begin(), message.payload.end());
    return cJSON_ParseWithLength(payload.c_str(), payload.size());
}
}

GatewayRecovery::Gateway &GatewayRecovery::gateway(const std::string &id)
{
    for (Gateway &entry : gateways_)
        if (entry.id == id)
            return entry;
    gateways_.push_back(Gateway());
    gateways_.back().id = id;
    return gateways_.back();
}

bool GatewayRecovery::consume(const NightMare::Message &message,
                              std::vector<NightMare::Message> &wills)
{
    std::string topicId;
    const bool clientsTopic = suffixId(message.topic, "/gateway/clients", topicId);
    const bool statusTopic = !clientsTopic && suffixId(message.topic, "/status", topicId);
    if (!clientsTopic && !statusTopic)
        return false;
    cJSON *root = parse(message);
    if (root == nullptr)
        return false;
    const cJSON *idJson = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON *generationJson = cJSON_GetObjectItemCaseSensitive(root, "generation");
    if (!cJSON_IsString(idJson) || !cJSON_IsString(generationJson) || topicId != idJson->valuestring)
    {
        cJSON_Delete(root);
        return false;
    }

    Gateway &entry = gateway(topicId);
    const std::string generation = generationJson->valuestring;
    if (clientsTopic)
    {
        const cJSON *clients = cJSON_GetObjectItemCaseSensitive(root, "clients");
        if (!cJSON_IsArray(clients))
        {
            cJSON_Delete(root);
            return false;
        }
        std::vector<NightMare::Message> next;
        const cJSON *client = nullptr;
        cJSON_ArrayForEach(client, clients)
        {
            const cJSON *connected = cJSON_GetObjectItemCaseSensitive(client, "connected");
            if (!cJSON_IsTrue(connected))
                continue;
            const cJSON *will = cJSON_GetObjectItemCaseSensitive(client, "last_will");
            const cJSON *topic = cJSON_GetObjectItemCaseSensitive(will, "topic");
            const cJSON *payload = cJSON_GetObjectItemCaseSensitive(will, "payload");
            const cJSON *retained = cJSON_GetObjectItemCaseSensitive(will, "retained");
            if (!cJSON_IsString(topic) || topic->valuestring[0] == '\0' || !cJSON_IsArray(payload))
                continue;
            NightMare::Message recovered;
            recovered.topic = topic->valuestring;
            recovered.persistent = cJSON_IsTrue(retained);
            recovered.direction = NightMare::Direction::REMOTE_TO_LOCAL;
            const cJSON *byte = nullptr;
            bool valid = true;
            cJSON_ArrayForEach(byte, payload)
            {
                if (!cJSON_IsNumber(byte) || byte->valuedouble < 0 || byte->valuedouble > 255 ||
                    byte->valuedouble != byte->valueint)
                {
                    valid = false;
                    break;
                }
                recovered.payload.push_back(static_cast<uint8_t>(byte->valueint));
            }
            if (valid)
                next.push_back(std::move(recovered));
        }
        entry.snapshotGeneration = generation;
        entry.wills.swap(next);
        if (entry.offlineGeneration == generation &&
            (entry.generation.empty() || entry.generation == generation) &&
            !entry.recoveryApplied)
        {
            entry.generation = generation;
            wills = entry.wills;
            entry.recoveryApplied = true;
        }
        cJSON_Delete(root);
        return true;
    }

    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(root, "kind");
    const cJSON *online = cJSON_GetObjectItemCaseSensitive(root, "online");
    if (!cJSON_IsString(kind) || strcmp(kind->valuestring, "gateway") != 0 || !cJSON_IsBool(online))
    {
        cJSON_Delete(root);
        return false;
    }
    if (cJSON_IsTrue(online))
    {
        entry.generation = generation;
        entry.offlineGeneration.clear();
        entry.recoveryApplied = false;
    }
    else if (entry.generation.empty() || entry.generation == generation)
    {
        entry.generation = generation;
        entry.offlineGeneration = generation;
        if (entry.snapshotGeneration == generation && !entry.recoveryApplied)
        {
            wills = entry.wills;
            entry.recoveryApplied = true;
        }
    }
    cJSON_Delete(root);
    return true;
}
