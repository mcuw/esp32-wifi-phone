#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"
#include "app_config.h"
#include "storage.h"
#include "board.h"
#include "net.h"
#include "provision.h"
#include "peers.h"
#include "call.h"
#include "ui.h"

static const char *TAG = "app";

/* ---- UI -> app command queue ------------------------------------------
 * LVGL callbacks run while holding the display lock; call_*() take the call
 * mutex, and the call task takes the display lock while holding that mutex.
 * Going through a queue breaks the lock cycle. */
typedef enum { CMD_DIAL, CMD_ACCEPT, CMD_HANGUP, CMD_WIFI_SETUP, CMD_VOLUME, CMD_IDLE } cmd_type_t;
typedef struct { cmd_type_t t; peer_t peer; int val; } cmd_t;
static QueueHandle_t s_cmdq;
static app_cfg_t s_cfg;
static bool s_services_up;

static void post(cmd_t c) { xQueueSend(s_cmdq, &c, 0); }
static void h_dial(const peer_t *p)   { post((cmd_t){ .t = CMD_DIAL, .peer = *p }); }
static void h_accept(void)            { post((cmd_t){ .t = CMD_ACCEPT }); }
static void h_hangup(void)            { post((cmd_t){ .t = CMD_HANGUP }); }
static void h_wifi(void)              { post((cmd_t){ .t = CMD_WIFI_SETUP }); }
static void h_volume(int v)           { post((cmd_t){ .t = CMD_VOLUME, .val = v }); }
static void h_idle(bool i)            { post((cmd_t){ .t = CMD_IDLE, .val = i }); }

static void cmd_task(void *arg)
{
    cmd_t c;
    for (;;) {
        xQueueReceive(s_cmdq, &c, portMAX_DELAY);
        switch (c.t) {
        case CMD_DIAL:   net_power_active(); call_dial(&c.peer); break;
        case CMD_ACCEPT: call_accept(); break;
        case CMD_HANGUP: call_hangup(); break;
        case CMD_VOLUME: board_set_volume(c.val); break;
        case CMD_WIFI_SETUP:
            storage_set_prov_flag(true);
            esp_restart();
            break;
        case CMD_IDLE:
            if (call_state() != CALL_IDLE) break;
            if (c.val) { board_set_brightness(0);   peers_set_active(false); net_power_idle(); }
            else       { board_set_brightness(100); net_power_active();       peers_set_active(true); }
            break;
        }
    }
}

/* ---- notifications -> UI ------------------------------------------------ */
static void on_call(call_state_t st, const char *peer, const char *note)
{
    if (st != CALL_IDLE) { board_set_brightness(100); net_power_active(); }
    ui_set_call(st, peer, note);
    if (st == CALL_IDLE) peers_refresh();
}

static void on_peers(const peer_t *l, int n) { ui_set_peers(l, n); }

static void on_net(net_state_t st, bool itwt)
{
    ui_set_net(st, itwt);
    if (st == NET_ONLINE && !s_services_up) {
        s_services_up = true;
        peers_init(s_cfg.name, on_peers);
        call_init(s_cfg.name, on_call);
        ESP_LOGI(TAG, "online as '%s'", s_cfg.name);
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(storage_init());
    ESP_ERROR_CHECK(board_init());

    bool have_wifi = storage_load(&s_cfg);
    bool want_prov = storage_take_prov_flag() || board_boot_button_pressed() || !have_wifi;

    s_cmdq = xQueueCreate(8, sizeof(cmd_t));
    ui_handlers_t h = { h_dial, h_accept, h_hangup, h_wifi, h_volume, h_idle };
    ui_init(&h, s_cfg.name);
    xTaskCreate(cmd_task, "cmd", 4096, NULL, 5, NULL);

    ESP_ERROR_CHECK(net_init(on_net));

    if (want_prov) {
        char ssid[33], pass[12];
        ESP_ERROR_CHECK(net_start_provisioning(ssid, sizeof ssid, pass, sizeof pass));
        ESP_ERROR_CHECK(provision_start());
        ui_show_provisioning(ssid, pass);
        ESP_LOGI(TAG, "provisioning: join '%s', open http://192.168.4.1", ssid);
    } else {
        ui_show_contacts();
        ESP_ERROR_CHECK(net_start_sta(s_cfg.ssid, s_cfg.pass));
    }
}
