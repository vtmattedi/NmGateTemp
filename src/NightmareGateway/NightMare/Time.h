#pragma once

#include <time.h>

namespace NightMare
{
    constexpr time_t SecondsPerMinute = 60;
    constexpr time_t SecondsPerHour = 60 * SecondsPerMinute;

    enum TimeStampFormat
    {
        DateAndTime,
        OnlyDate,
        SmallDate,
        OnlyTime,
        OnlyTimeWithSeconds,
        OnlyTimeLive,
        DowDate,
        TimeSinceStamp,
        CountdownFromTimestamp
    };

    // Unix seconds from the ESP system clock. Calendar component accessors are UTC.
    time_t now();
    bool valid();
    bool setEpoch(time_t epoch);
    int second(time_t epoch = now());
    int minute(time_t epoch = now());
    int hour(time_t epoch = now());
    int day(time_t epoch = now());
    int month(time_t epoch = now());
    int year(time_t epoch = now());
}
// Usefull Definitions
#define HOUR 60 * 60
#define MINUTE 60
#define SECOND 1
#define DAY 24 * HOUR
#define IN_MS 1000
#define MINUTE_MS 60 * MS
#define HOUR_MS 60 * MINUTE_MS
#define SECOND_MS 1000