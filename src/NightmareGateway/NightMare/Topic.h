#pragma once
#include <stdio.h>
#include <string>
#include <string_view>
#include <vector>

namespace NightMare
{
    // The ESP-NOW message encoding carries the topic length in 6 bits.
    constexpr size_t MaxTopicLength = 63;
}

/// @brief Validates if a given topic string is valid for publishing in the Nightmare Gateway system.
/// @param topic The topic string to validate
/// @return true if the topic is valid for publishing, false otherwise
bool isValidPublishTopic(std::string_view topic);


/// @brief Validates if a given topic string is valid for subscribing in the Nightmare Gateway system.
/// @param topic The topic string to validate
/// @param filter The filter to match against
/// @return true if the topic matches the filter, false otherwise
bool topicMatchesPattern(std::string_view topic, std::string_view filter);


