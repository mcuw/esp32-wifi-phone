#include "peers.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mdns.h"

static const char *TAG = "peers";

static peers_cb_t  s_cb;
static volatile bool s_active = true;
static TaskHandle_t  s_task;
static char        s_host[24];

static void query_once(void)
{
    mdns_result_t *res = NULL;
    if (mdns_query_ptr(SVC_TYPE, SVC_PROTO, 2500, PEER_MAX, &res) != ESP_OK) return;

    peer_t list[PEER_MAX];
    int n = 0;
    for (mdns_result_t *r = res; r && n < PEER_MAX; r = r->next) {
        if (!r->instance_name) continue;
        if (r->hostname && !strcmp(r->hostname, s_host)) continue;          /* myself */
        esp_ip4_addr_t ip = { 0 };
        for (mdns_ip_addr_t *a = r->addr; a; a = a->next)
            if (a->addr.type == ESP_IPADDR_TYPE_V4) { ip = a->addr.u_addr.ip4; break; }
        if (!ip.addr) continue;
        bool dup = false;
        for (int i = 0; i < n; i++) if (!strcmp(list[i].name, r->instance_name)) dup = true;
        if (dup) continue;
        strlcpy(list[n].name, r->instance_name, sizeof list[n].name);
        list[n].ip = ip;
        n++;
    }
    mdns_query_results_free(res);
    if (s_cb) s_cb(list, n);
}

static void peers_task(void *arg)
{
    for (;;) {
        if (s_active) query_once();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));      /* or woken by peers_refresh() */
    }
}

void peers_init(const char *my_name, peers_cb_t cb)
{
    s_cb = cb;
    uint8_t m[6];
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    snprintf(s_host, sizeof s_host, "c6phone-%02x%02x", m[4], m[5]);

    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(s_host);
    mdns_instance_name_set(my_name);
    ESP_ERROR_CHECK(mdns_service_add(my_name, SVC_TYPE, SVC_PROTO, SIG_PORT, NULL, 0));
    ESP_LOGI(TAG, "advertising '%s' as %s.local", my_name, s_host);
    xTaskCreate(peers_task, "peers", 4096, NULL, 3, &s_task);
}

void peers_set_active(bool on) { s_active = on; if (on) peers_refresh(); }
void peers_refresh(void)       { if (s_task) xTaskNotifyGive(s_task); }
