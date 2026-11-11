#include "board.h"
#include <string.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "app_config.h"

/* Display/touch/LVGL come from the Waveshare BSP. The header name and the
 * bsp_display_* calls follow the usual Espressif BSP convention - check them
 * against the installed component (managed_components/waveshare__esp32_c6_...). */
#include "bsp/esp-bsp.h"

static const char *TAG = "board";

/* Pin map: docs/hardware.md of waveshareteam/ESP32-C6-Touch-AMOLED-2.06 */
#define PIN_I2S_MCLK  19
#define PIN_I2S_BCLK  20
#define PIN_I2S_DIN   21     /* ES7210 SDOUT -> MCU */
#define PIN_I2S_WS    22
#define PIN_I2S_DOUT  23     /* MCU -> ES8311 DSDIN */
#define PIN_PA_CTRL    6
#define PIN_BOOT       9     /* BOOT button (check schematic) */

#ifndef BOARD_I2C_PORT
#define BOARD_I2C_PORT I2C_NUM_0
#endif

static i2s_chan_handle_t        s_tx, s_rx;
static esp_codec_dev_handle_t   s_spk, s_mic;
static bool                     s_audio_on;
static int                      s_volume = 70;

esp_err_t board_init(void)
{
    lv_display_t *d = bsp_display_start();    /* I2C bus, QSPI AMOLED, FT3168, LVGL task */
    if (!d) return ESP_FAIL;
    bsp_display_brightness_set(100);

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BOOT, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    return ESP_OK;
}

bool board_lock(uint32_t t) { return bsp_display_lock(t); }
void board_unlock(void)     { bsp_display_unlock(); }
void board_set_brightness(int p) { bsp_display_brightness_set(p); }
bool board_boot_button_pressed(void) { return gpio_get_level(PIN_BOOT) == 0; }

esp_err_t board_audio_start(void)
{
    if (s_audio_on) return ESP_OK;

    /* The touch controller already owns the I2C bus: share it. */
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_master_get_bus_handle(BOARD_I2C_PORT, &bus));

    /* One full-duplex I2S port; ES7210 and ES8311 share MCLK/BCLK/WS. */
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&cc, &s_tx, &s_rx));
    i2s_std_config_t sc = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = PIN_I2S_MCLK, .bclk = PIN_I2S_BCLK, .ws = PIN_I2S_WS,
                      .dout = PIN_I2S_DOUT, .din = PIN_I2S_DIN },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &sc));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &sc));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));

    audio_codec_i2s_cfg_t icfg = { .port = I2S_NUM_0, .rx_handle = s_rx, .tx_handle = s_tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&icfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    /* ES8311: speaker DAC, PA enable on GPIO6 */
    audio_codec_i2c_cfg_t c8311 = { .port = BOARD_I2C_PORT, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bus };
    es8311_codec_cfg_t e8311 = {
        .ctrl_if = audio_codec_new_i2c_ctrl(&c8311), .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = PIN_PA_CTRL, .use_mclk = true,
    };
    esp_codec_dev_cfg_t out_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = es8311_codec_new(&e8311), .data_if = data_if,
    };
    s_spk = esp_codec_dev_new(&out_cfg);

    /* ES7210: two microphones (MIC1 + MIC2) */
    audio_codec_i2c_cfg_t c7210 = { .port = BOARD_I2C_PORT, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = bus };
    es7210_codec_cfg_t e7210 = {
        .ctrl_if = audio_codec_new_i2c_ctrl(&c7210),
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
    };
    esp_codec_dev_cfg_t in_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = es7210_codec_new(&e7210), .data_if = data_if,
    };
    s_mic = esp_codec_dev_new(&in_cfg);

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = AUDIO_RATE, .channel = 2, .bits_per_sample = 16,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1),
    };
    ESP_ERROR_CHECK(esp_codec_dev_open(s_spk, &fs) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(esp_codec_dev_open(s_mic, &fs) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL);
    esp_codec_dev_set_out_vol(s_spk, s_volume);
    esp_codec_dev_set_in_gain(s_mic, 30.0f);

    s_audio_on = true;
    ESP_LOGI(TAG, "audio started");
    return ESP_OK;
}

void board_audio_stop(void)
{
    if (!s_audio_on) return;
    esp_codec_dev_close(s_spk);
    esp_codec_dev_close(s_mic);
    esp_codec_dev_delete(s_spk);
    esp_codec_dev_delete(s_mic);
    i2s_channel_disable(s_tx);
    i2s_channel_disable(s_rx);
    i2s_del_channel(s_tx);
    i2s_del_channel(s_rx);
    s_audio_on = false;
}

int board_mic_read(int16_t *mono, int samples)
{
    int16_t st[AUDIO_FRAME * 2];
    if (!s_audio_on || samples > AUDIO_FRAME) return 0;
    if (esp_codec_dev_read(s_mic, st, samples * 4) != ESP_CODEC_DEV_OK) return 0;
    /* Two mics: averaging halves uncorrelated noise (~3 dB). Real beamforming
     * would need a DSP library - see README. */
    for (int i = 0; i < samples; i++) mono[i] = (int16_t)(((int)st[2 * i] + st[2 * i + 1]) / 2);
    return samples;
}

int board_spk_write(const int16_t *mono, int samples)
{
    int16_t st[AUDIO_FRAME * 2];
    if (!s_audio_on || samples > AUDIO_FRAME) return 0;
    for (int i = 0; i < samples; i++) st[2 * i] = st[2 * i + 1] = mono[i];
    return esp_codec_dev_write(s_spk, st, samples * 4) == ESP_CODEC_DEV_OK ? samples : 0;
}

void board_set_volume(int p)
{
    s_volume = p < 0 ? 0 : (p > 100 ? 100 : p);
    if (s_audio_on) esp_codec_dev_set_out_vol(s_spk, s_volume);
}
