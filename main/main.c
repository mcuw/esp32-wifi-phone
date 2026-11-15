#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_core_dump.h"
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

#if CONFIG_PHONE_TEST_AUTODIAL
/* Bench test hook (see Kconfig.projbuild, test/bench/): dial a peer from the
 * mDNS list - the same path as a tap on the contact - and hang up after a
 * fixed ring time. The "autodial:" lines are parsed by the bench test. */
static peer_t        s_ad_peer;
static volatile bool s_ad_have;

static void autodial_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(CONFIG_PHONE_TEST_AUTODIAL_DELAY_S * 1000));
    for (int round = 1; CONFIG_PHONE_TEST_AUTODIAL_ROUNDS == 0 || round <= CONFIG_PHONE_TEST_AUTODIAL_ROUNDS; round++) {
        if (round > 1) vTaskDelay(pdMS_TO_TICKS(CONFIG_PHONE_TEST_AUTODIAL_PAUSE_S * 1000));
        ESP_LOGW("autodial", "round %d: dial '%s' " IPSTR " mac %s", round, s_ad_peer.name,
                 IP2STR(&s_ad_peer.ip), s_ad_peer.has_mac ? "yes" : "no");
        h_dial(&s_ad_peer);
        vTaskDelay(pdMS_TO_TICKS(CONFIG_PHONE_TEST_AUTODIAL_RING_S * 1000));
        ESP_LOGW("autodial", "round %d: hangup", round);
        h_hangup();
    }
    ESP_LOGW("autodial", "done");
    vTaskDelete(NULL);
}

static void autodial_pick(const peer_t *l, int n)
{
    if (s_ad_have) return;
    for (int i = 0; i < n; i++) {
        if (CONFIG_PHONE_TEST_AUTODIAL_PEER[0] && strcmp(l[i].name, CONFIG_PHONE_TEST_AUTODIAL_PEER)) continue;
        s_ad_peer = l[i];
        s_ad_have = true;
        ESP_LOGW("autodial", "peer '%s' found, first call in %d s", l[i].name, CONFIG_PHONE_TEST_AUTODIAL_DELAY_S);
        xTaskCreate(autodial_task, "autodial", 3072, NULL, 3, NULL);
        return;
    }
}
#endif

static void on_peers(const peer_t *l, int n)
{
#if CONFIG_PHONE_TEST_AUTODIAL
    autodial_pick(l, n);
#endif
    ui_set_peers(l, n);
}

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

/* The coredump partition can still hold bytes from an older partition layout
 * (before 72f66fc, phy_init lived at its offset) - flashing never touches it.
 * Its first word then is no valid dump size, and ESP-IDF logs "Incorrect size
 * of core dump image" on every boot until the first real crash (issue #7).
 * Erase it once in that case. A valid dump, or one with a bad checksum, has a
 * plausible size and is left alone for /api/coredump. */
static void coredump_discard_foreign_data(void)
{
    size_t addr, size;
    if (esp_core_dump_image_get(&addr, &size) != ESP_ERR_INVALID_SIZE) return;
    esp_err_t e = esp_core_dump_image_erase();
    ESP_LOGW(TAG, "coredump partition held no core dump (leftover data?) - erased: %s", esp_err_to_name(e));
}

void app_main(void)
{
    ESP_ERROR_CHECK(storage_init());
    coredump_discard_foreign_data();
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
