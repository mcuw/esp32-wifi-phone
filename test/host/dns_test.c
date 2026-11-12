#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include "dns_server.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static size_t query(uint8_t *p, const char *name, uint16_t qtype, int edns, uint16_t flags)
{
    size_t i = 0;
    uint16_t hdr[6] = { htons(0xBEEF), htons(flags), htons(1), 0, 0, htons(edns ? 1 : 0) };
    memcpy(p, hdr, 12); i = 12;
    while (*name) {                                    /* "a.b.c" -> 1a1b1c0 */
        const char *dot = strchr(name, '.');
        size_t l = dot ? (size_t)(dot - name) : strlen(name);
        p[i++] = (uint8_t)l; memcpy(p + i, name, l); i += l;
        name += l + (dot ? 1 : 0);
    }
    p[i++] = 0;
    p[i++] = qtype >> 8; p[i++] = qtype & 0xFF; p[i++] = 0; p[i++] = 1;
    if (edns) { static const uint8_t opt[] = { 0, 0, 41, 0x10, 0, 0, 0, 0, 0, 0, 0 }; memcpy(p + i, opt, sizeof opt); i += sizeof opt; }
    return i;
}

int main(void)
{
    uint8_t q[512], r[600];
    uint32_t ip = inet_addr("192.168.4.1");
    size_t n, m;

    n = query(q, "connectivitycheck.gstatic.com", 1, 1, 0x0100);
    m = dns_build_reply(q, n, r, sizeof r, ip);
    CHECK(m > n - 11);                                  /* answer added, EDNS record dropped */
    CHECK(r[0] == 0xBE && r[1] == 0xEF);                /* id echoed */
    CHECK(r[2] & 0x80);                                 /* QR */
    CHECK(r[3] % 16 == 0);                              /* rcode NOERROR */
    CHECK(r[7] == 1 && r[11] == 0);                     /* ancount 1, arcount 0 */
    CHECK(!memcmp(r + m - 4, &ip, 4));
    CHECK(r[m - 16] == 0xC0 && r[m - 15] == 0x0C);      /* name pointer */

    n = query(q, "captive.apple.com", 28 /* AAAA */, 0, 0x0100);
    m = dns_build_reply(q, n, r, sizeof r, ip);
    CHECK(m == n && r[7] == 0 && (r[3] & 0x0F) == 0);   /* empty NOERROR */

    CHECK(dns_build_reply((const uint8_t *)"\x00\x01", 2, r, sizeof r, ip) == 0);   /* too short */

    n = query(q, "a.b", 1, 0, 0x8100);                                              /* a response */
    CHECK(dns_build_reply(q, n, r, sizeof r, ip) == 0);

    n = query(q, "a.b", 1, 0, 0x0100);
    q[12] = 0xC0;                                                                    /* compressed label */
    CHECK(dns_build_reply(q, n, r, sizeof r, ip) == 0);

    n = query(q, "a.b", 1, 0, 0x0100);
    q[12] = 0x3F;                                                                    /* label runs past end */
    CHECK(dns_build_reply(q, n, r, sizeof r, ip) == 0);

    printf("dns: %s\n", failures ? "FAILED" : "ok");
    return failures != 0;
}
