#pragma once
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/rmt_tx.h"
struct Color {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};
Color fromHex(const char* hex);
Color fromInt(int color);
Color fromHSV(float h, float s, float v);

void ws2812_init(void);
void ws2812_set_color(Color color);
void ws2812_set_brightness(uint8_t newBrightness);

#define COLOR_RED fromInt(0xFF0000)
#define COLOR_GREEN fromInt(0x00FF00)
#define COLOR_BLUE fromInt(0x0000FF)
#define COLOR_WHITE fromInt(0xFFFFFF)
#define COLOR_BLACK fromInt(0x000000)
#define COLOR_YELLOW fromInt(0xFFFF00)
#define COLOR_CYAN fromInt(0x00FFFF)
#define COLOR_MAGENTA fromInt(0xFF00FF)
#define COLOR_PINK fromInt(0xFF1493)