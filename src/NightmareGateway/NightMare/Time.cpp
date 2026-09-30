#include "Time.h"
#include <sys/time.h>
#include <string>
#include <cstdio>

namespace NightMare
{

namespace
{
constexpr time_t EarliestValidEpoch = 1577836800; // 2020-01-01 UTC

tm utcParts(time_t epoch)
{
    tm result{};
    gmtime_r(&epoch, &result);
    return result;
}

tm localParts(time_t epoch)
{
    tm result{};
    localtime_r(&epoch, &result);
    return result;
}

std::string strftimeString(const tm &parts, const char *format)
{
    char buffer[32] = {};
    return strftime(buffer, sizeof(buffer), format, &parts) != 0 ? std::string(buffer) : std::string();
}

std::string durationString(uint64_t seconds, bool countdown, bool negative)
{
    std::string value;
    if (negative)
        value += '-';

    if (!countdown)
    {
        if (seconds < 90)
            return value + std::to_string(static_cast<unsigned long>(seconds)) + " sec.";
        if (seconds < 90 * SecondsPerMinute)
            return value + std::to_string(static_cast<unsigned long>(seconds / SecondsPerMinute)) + " min.";
        return value + std::to_string(static_cast<unsigned long>(seconds / SecondsPerHour)) + " hours.";
    }

    const uint64_t hours = seconds / SecondsPerHour;
    const uint8_t minutes = static_cast<uint8_t>((seconds / SecondsPerMinute) % 60);
    const uint8_t remainingSeconds = static_cast<uint8_t>(seconds % 60);
    char buffer[32] = {};
    if (hours != 0)
        snprintf(buffer, sizeof(buffer), "%s%02llu:%02u", negative ? "-" : "",
                 static_cast<unsigned long long>(hours), minutes);
    else
        snprintf(buffer, sizeof(buffer), "%s%02u:%02u", negative ? "-" : "",
                 minutes, remainingSeconds);
    return std::string(buffer);
}
}

time_t now()
{
    return ::time(nullptr);
}

bool valid()
{
    return now() >= EarliestValidEpoch;
}

bool setEpoch(time_t epoch)
{
    if (epoch < EarliestValidEpoch)
        return false;
    const timeval value{epoch, 0};
    return settimeofday(&value, nullptr) == 0;
}

int second(time_t epoch) { return utcParts(epoch).tm_sec; }
int minute(time_t epoch) { return utcParts(epoch).tm_min; }
int hour(time_t epoch) { return utcParts(epoch).tm_hour; }
int day(time_t epoch) { return utcParts(epoch).tm_mday; }
int month(time_t epoch) { return utcParts(epoch).tm_mon + 1; }
int year(time_t epoch) { return utcParts(epoch).tm_year + 1900; }

}
