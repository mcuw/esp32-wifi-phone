#pragma once
#include "peers.h"

typedef enum { CALL_IDLE, CALL_OUTGOING, CALL_INCOMING, CALL_ACTIVE } call_state_t;

/* note: short status text ("Klingelt", "Abgelehnt", ...) or NULL */
typedef void (*call_cb_t)(call_state_t st, const char *peer_name, const char *note);

void        call_init(const char *my_name, call_cb_t cb);
esp_err_t   call_dial(const peer_t *p);
void        call_accept(void);
void        call_hangup(void);       /* hang up / reject / cancel */
call_state_t call_state(void);
