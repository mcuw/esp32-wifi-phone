#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Display + touch + LVGL (via Waveshare BSP) and I2C. */
esp_err_t board_init(void);
bool      board_lock(uint32_t timeout_ms);     /* LVGL lock (0 = wait forever) */
void      board_unlock(void);
void      board_set_brightness(int percent);
bool      board_boot_button_pressed(void);

/* Audio: ES7210 (2 mics) in, ES8311 + PA out, 16 kHz. Idempotent. */
esp_err_t board_audio_start(void);
void      board_audio_stop(void);
/* Blocking, exactly 'samples' mono samples. Mic read returns L/R averaged. */
int       board_mic_read(int16_t *mono, int samples);
int       board_spk_write(const int16_t *mono, int samples);
void      board_set_volume(int percent);
