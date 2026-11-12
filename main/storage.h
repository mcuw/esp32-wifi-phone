#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "app_config.h"

typedef struct {
    char ssid[33];
    char pass[65];
    char name[NAME_MAX_LEN + 1];
} app_cfg_t;

esp_err_t storage_init(void);
/* Fills defaults (name) always; returns true if a Wi-Fi SSID is stored. */
bool      storage_load(app_cfg_t *c);
esp_err_t storage_save(const app_cfg_t *c);
/* "Open Wi-Fi setup on next boot" flag (set from Settings screen). */
void      storage_set_prov_flag(bool on);
bool      storage_take_prov_flag(void);   /* read + clear */
