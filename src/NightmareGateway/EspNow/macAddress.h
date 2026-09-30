#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string>

struct MacAddress {
    static constexpr size_t Length = 6;
    uint8_t bytes[Length] = {};

    MacAddress() = default;
    explicit MacAddress(const uint8_t *raw);
    bool operator==(const MacAddress &other) const;
    MacAddress operator=(const uint8_t *other);
    std::string toString() const;
};
