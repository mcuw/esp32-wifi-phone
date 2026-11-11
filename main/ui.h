#pragma once
#include <stdbool.h>
#include "peers.h"
#include "call.h"
#include "net.h"

/* All callbacks run on the LVGL task: they must only enqueue work. */
typedef struct {
    void (*dial)(const peer_t *p);
    void (*accept)(void);
    void (*hangup)(void);
    void (*wifi_setup)(void);
    void (*volume)(int percent);
    void (*idle)(bool idle);
} ui_handlers_t;

void ui_init(const ui_handlers_t *h, const char *my_name);

void ui_show_provisioning(const char *ssid, const char *pass);
void ui_show_contacts(void);
void ui_set_peers(const peer_t *list, int n);
void ui_set_net(net_state_t st, bool itwt);
void ui_set_call(call_state_t st, const char *peer, const char *note);
