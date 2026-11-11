#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Captive-portal DNS: every A query is answered with the soft-AP address, other
 * record types (AAAA, HTTPS, ...) get an empty NOERROR so clients fall back to A. */
esp_err_t dns_server_start(uint32_t ip_be);

/* Pure function (host-testable): builds the reply for one query. Returns 0 = ignore. */
size_t dns_build_reply(const uint8_t *q, size_t qlen, uint8_t *r, size_t rmax, uint32_t ip_be);
