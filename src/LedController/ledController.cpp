#include "ledController.h"

// Onboard addressable RGB LED (WS2812) on ESP32-S3-DevKitC-1
#define RGB_LED_GPIO 48
#define RGB_LED_COUNT 1

// WS2812 timing (in RMT ticks, 1 tick = 1/resolution)
#define RMT_RESOLUTION_HZ 10000000     // 10MHz -> 1 tick = 0.1us
static uint8_t brightness = 127;           // Default brightness percentage (0-255)
static Color currentColor = {0, 0, 0}; // Current color state
Color fromHex(const char *hex)
{
    Color color = {0, 0, 0};
    if (hex[0] == '#')
    {
        hex++;
    }
    if (strlen(hex) == 6)
    {
        sscanf(hex, "%02hhx%02hhx%02hhx", &color.r, &color.g, &color.b);
    }
    return color;
}
Color fromInt(int color)
{
    Color c = {0, 0, 0};
    c.r = (color >> 16) & 0xFF;
    c.g = (color >> 8) & 0xFF;
    c.b = color & 0xFF;
    return c;
}
Color fromHSV(float h, float s, float v)
{
    Color color = {0, 0, 0};
    if (s == 0)
    {
        color.r = color.g = color.b = v * 255;
        return color;
    }
    h /= 60;
    int i = (int)h;
    float f = h - i;
    float p = v * (1 - s);
    float q = v * (1 - s * f);
    float t = v * (1 - s * (1 - f));
    switch (i % 6)
    {
    case 0:
        color.r = v * 255;
        color.g = t * 255;
        color.b = p * 255;
        break;
    case 1:
        color.r = q * 255;
        color.g = v * 255;
        color.b = p * 255;
        break;
    case 2:
        color.r = p * 255;
        color.g = v * 255;
        color.b = t * 255;
        break;
    case 3:
        color.r = p * 255;
        color.g = q * 255;
        color.b = v * 255;
        break;
    case 4:
        color.r = t * 255;
        color.g = p * 255;
        color.b = v * 255;
        break;
    case 5:
        color.r = v * 255;
        color.g = p * 255;
        color.b = q * 255;
        break;
    }
    return color;
}

static rmt_channel_handle_t led_chan = NULL;
static rmt_encoder_handle_t led_encoder = NULL;

void ws2812_init(void)
{
    rmt_tx_channel_config_t tx_chan_config = {
        .gpio_num = (gpio_num_t)RGB_LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &led_chan));

    rmt_bytes_encoder_config_t bytes_encoder_config = {
        .bit0 = {
            .duration0 = 3, // 0.3us
            .level0 = 1,
            .duration1 = 9, // 0.9us
            .level1 = 0,
        },
        .bit1 = {
            .duration0 = 9, // 0.9us
            .level0 = 1,
            .duration1 = 3, // 0.3us
            .level1 = 0,
        },
        .flags = {.msb_first = 1},
    };
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&bytes_encoder_config, &led_encoder));

    ESP_ERROR_CHECK(rmt_enable(led_chan));
}

void writeData(Color color, uint8_t brightness)
{
    uint8_t pixel[3] = {color.g, color.r, color.b};
    for (int i = 0; i < 3; i++)
    {
        pixel[i] = (pixel[i] * brightness) / 255;
    }
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };
    ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, pixel, sizeof(pixel), &tx_config));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
}

void ws2812_set_brightness(uint8_t newBrightness)
{
    brightness = newBrightness;
    writeData(currentColor, brightness);
}

void ws2812_set_color(Color color)
{
    currentColor = color;
    writeData(color, brightness);
}