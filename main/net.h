#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_netif_ip_addr.h"

typedef enum {
    NET_IDLE,
    NET_AP,          /* soft-AP provisioning mode */
    NET_CONNECTING,
    NET_ONLINE,
} net_state_t;

typedef void (*net_cb_t)(net_state_t st, bool itwt_active);

esp_err_t   net_init(net_cb_t cb);
esp_err_t   net_start_sta(const char *ssid, const char *pass);
esp_err_t   net_start_provisioning(char *ap_ssid, size_t ssid_n, char *ap_pass, size_t pass_n);
int         net_scan(wifi_ap_record_t *out, int max);   /* APSTA only */

/* Idle: modem sleep + iTWT (low power, ~1 s wake-up latency).
 * Active: no power save, iTWT torn down (calls, UI in use). */
void        net_power_idle(void);
void        net_power_active(void);

net_state_t net_state(void);
