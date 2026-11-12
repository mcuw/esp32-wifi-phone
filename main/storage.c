#include "storage.h"
#include <stdio.h>
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_mac.h"
#include "esp_log.h"

static const char *TAG = "storage";

esp_err_t storage_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    return e;
}

static void default_name(char *out, size_t n)
{
    uint8_t m[6];
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    snprintf(out, n, "Telefon-%02X%02X", m[4], m[5]);
}

bool storage_load(app_cfg_t *c)
{
    memset(c, 0, sizeof *c);
    default_name(c->name, sizeof c->name);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;

    size_t n = sizeof c->ssid;
    if (nvs_get_str(h, "ssid", c->ssid, &n) != ESP_OK) c->ssid[0] = 0;
    n = sizeof c->pass;
    if (nvs_get_str(h, "pass", c->pass, &n) != ESP_OK) c->pass[0] = 0;

    char nm[NAME_MAX_LEN + 1];
    n = sizeof nm;
    if (nvs_get_str(h, "name", nm, &n) == ESP_OK && nm[0]) strlcpy(c->name, nm, sizeof c->name);

    nvs_close(h);
    return c->ssid[0] != 0;
}

esp_err_t storage_save(const app_cfg_t *c)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_str(h, "ssid", c->ssid);
    if (e == ESP_OK) e = nvs_set_str(h, "pass", c->pass);
    if (e == ESP_OK) e = nvs_set_str(h, "name", c->name);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(e));
    return e;
}

void storage_set_prov_flag(bool on)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "prov", on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

bool storage_take_prov_flag(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_get_u8(h, "prov", &v);
    if (v) { nvs_set_u8(h, "prov", 0); nvs_commit(h); }
    nvs_close(h);
    return v != 0;
}
