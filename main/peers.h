#pragma once
#include <stdbool.h>
#include "esp_netif_ip_addr.h"
#include "app_config.h"

typedef struct {
    char            name[NAME_MAX_LEN + 1];
    esp_ip4_addr_t  ip;
} peer_t;

typedef void (*peers_cb_t)(const peer_t *list, int n);

/* Advertises this phone via mDNS (_c6phone._udp) and browses for others. */
void peers_init(const char *my_name, peers_cb_t cb);
/* Browse only while the screen is on: multicast is not reliably delivered
 * while the station sleeps in an iTWT agreement. */
void peers_set_active(bool on);
void peers_refresh(void);
