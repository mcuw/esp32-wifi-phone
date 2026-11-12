#include "dns_server.h"
#include <string.h>

size_t dns_build_reply(const uint8_t *q, size_t qlen, uint8_t *r, size_t rmax, uint32_t ip_be)
{
    if (qlen < 12 || rmax < qlen + 16) return 0;
    uint16_t flags = (uint16_t)((q[2] << 8) | q[3]);
    if (flags & 0x8000) return 0;                       /* already a response */
    if (((flags >> 11) & 0x0F) != 0) return 0;          /* standard queries only */
    if (((q[4] << 8) | q[5]) != 1) return 0;            /* exactly one question */

    size_t i = 12;
    while (i < qlen && q[i]) {
        if (q[i] & 0xC0) return 0;                      /* no compression in a question */
        i += (size_t)q[i] + 1;
    }
    if (i + 5 > qlen) return 0;                         /* root label + type + class */
    size_t qend = i + 5;
    uint16_t qtype = (uint16_t)((q[i + 1] << 8) | q[i + 2]);

    memcpy(r, q, qend);                                 /* drops EDNS/additional records */
    uint16_t rf = 0x8000 | 0x0400 | (flags & 0x0100) | 0x0080;   /* QR, AA, copy RD, RA */
    r[2] = rf >> 8; r[3] = rf & 0xFF;
    r[6] = r[7] = r[8] = r[9] = r[10] = r[11] = 0;      /* an/ns/ar = 0 */

    if (qtype != 1 && qtype != 255) return qend;        /* A or ANY get an answer */
    r[7] = 1;
    uint8_t *a = r + qend;
    a[0] = 0xC0; a[1] = 0x0C;                           /* name: pointer to question */
    a[2] = 0; a[3] = 1;                                 /* TYPE A */
    a[4] = 0; a[5] = 1;                                 /* CLASS IN */
    a[6] = a[7] = a[8] = 0; a[9] = 60;                  /* TTL 60 s */
    a[10] = 0; a[11] = 4;                               /* RDLENGTH */
    memcpy(a + 12, &ip_be, 4);
    return qend + 16;
}

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_log.h"

static const char *TAG = "dns";

static void dns_task(void *arg)
{
    uint32_t ip = (uint32_t)(uintptr_t)arg;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) < 0) {
        ESP_LOGE(TAG, "bind :53 failed");
        vTaskDelete(NULL);
    }
    uint8_t rx[300], tx[320];
    for (;;) {
        struct sockaddr_in from; socklen_t fl = sizeof from;
        int n = recvfrom(s, rx, sizeof rx, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) continue;
        size_t m = dns_build_reply(rx, (size_t)n, tx, sizeof tx, ip);
        if (m) sendto(s, tx, m, 0, (struct sockaddr *)&from, fl);
    }
}

esp_err_t dns_server_start(uint32_t ip_be)
{
    return xTaskCreate(dns_task, "dns", 3072, (void *)(uintptr_t)ip_be, 4, NULL) == pdPASS ? ESP_OK : ESP_FAIL;
}
#endif
