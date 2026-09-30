#include "macAddress.h"
#include <stdio.h>
#include <string.h>

MacAddress::MacAddress(const uint8_t *raw)
{
    if (raw != nullptr)
        memcpy(bytes, raw, Length);
}

bool MacAddress::operator==(const MacAddress &other) const
{
    return memcmp(bytes, other.bytes, Length) == 0;
}

MacAddress MacAddress::operator=(const uint8_t *other)
{
    if (other != nullptr)
        memcpy(bytes, other, Length);
    return *this;
}

std::string MacAddress::toString() const
{
    char buffer[18];
    snprintf(buffer, sizeof(buffer), "%02X:%02X:%02X:%02X:%02X:%02X",
             bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
    return std::string(buffer);
}
