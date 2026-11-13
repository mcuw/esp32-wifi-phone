#include "provision.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_partition.h"
#include "esp_core_dump.h"
#include "esp_app_desc.h"
#include "app_config.h"
#include "net.h"
#include "storage.h"
#include "dns_server.h"
#include "json_util.h"

static const char *TAG = "prov";

/* ------------------------------------------------------------ JSON API --- */

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");   /* for `npm run dev` against a real unit */
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t fail(httpd_req_t *req, const char *status, const char *why)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, why, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_info(httpd_req_t *req)
{
    app_cfg_t c; storage_load(&c);
    uint8_t m[6]; esp_read_mac(m, ESP_MAC_WIFI_STA);
    char nm[2 * NAME_MAX_LEN + 8], buf[160];
    json_escape(c.name, nm, sizeof nm);
    snprintf(buf, sizeof buf, "{\"id\":\"%02X%02X%02X\",\"name\":\"%s\",\"fw\":\"%s\"}", m[3], m[4], m[5], nm, APP_FW_VERSION);
    return send_json(req, buf);
}

static esp_err_t api_scan(httpd_req_t *req)
{
    enum { MAXN = 16, KEEP = 10, BUFN = 1100 };
    wifi_ap_record_t *ap = calloc(MAXN, sizeof *ap);
    char *buf = malloc(BUFN);
    if (!ap || !buf) { free(ap); free(buf); return fail(req, "500 Internal Server Error", "memory"); }

    int n = net_scan(ap, MAXN), o = 0, kept = 0;
    o += snprintf(buf, BUFN, "[");
    for (int i = 0; i < n && kept < KEEP; i++) {
        const char *ssid = (const char *)ap[i].ssid;
        if (!ssid[0]) continue;
        bool dup = false;
        for (int j = 0; j < i; j++) if (!strcmp(ssid, (const char *)ap[j].ssid)) dup = true;
        if (dup) continue;
        char e[2 * 32 + 8];
        json_escape(ssid, e, sizeof e);
        if (o + 80 + strlen(e) >= BUFN) break;
        o += snprintf(buf + o, BUFN - o, "%s{\"s\":\"%s\",\"r\":%d,\"a\":%d}", kept ? "," : "", e, ap[i].rssi, ap[i].authmode);
        kept++;
    }
    snprintf(buf + o, BUFN - o, "]");
    free(ap);
    esp_err_t r = send_json(req, buf);
    free(buf);
    return r;
}

static void reboot_cb(void *arg) { esp_restart(); }

static esp_err_t api_wifi(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > 512) return fail(req, "400 Bad Request", "payload");
    char *j = malloc(req->content_len + 1);
    if (!j) return fail(req, "500 Internal Server Error", "memory");
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, j + got, req->content_len - got);
        if (r <= 0) { free(j); return ESP_FAIL; }
        got += r;
    }
    j[got] = 0;

    app_cfg_t c; storage_load(&c);
    char ssid[33] = "", pass[65] = "", name[NAME_MAX_LEN + 1] = "";
    bool ok = json_get_str(j, "ssid", ssid, sizeof ssid);
    json_get_str(j, "pass", pass, sizeof pass);
    json_get_str(j, "name", name, sizeof name);
    memset(j, 0, got);                    /* do not leave the password lying around */
    free(j);

    if (!ok || !ssid[0]) return fail(req, "400 Bad Request", "ssid");
    size_t pl = strlen(pass);
    if (pl != 0 && (pl < 8 || pl > 63)) return fail(req, "400 Bad Request", "pass length 8-63");

    strlcpy(c.ssid, ssid, sizeof c.ssid);
    strlcpy(c.pass, pass, sizeof c.pass);
    if (name[0]) strlcpy(c.name, name, sizeof c.name);
    if (storage_save(&c) != ESP_OK) return fail(req, "500 Internal Server Error", "nvs");

    ESP_LOGI(TAG, "credentials saved for SSID '%s', rebooting", c.ssid);
    esp_err_t r = send_json(req, "{\"ok\":true}");
    static esp_timer_handle_t t;
    const esp_timer_create_args_t ta = { .callback = reboot_cb, .name = "reboot" };
    if (!t) esp_timer_create(&ta, &t);
    esp_timer_start_once(t, 1500 * 1000);       /* let the response leave first */
    return r;
}

/* ------------------------------------------------------------ core dump --- */
/* Only served on the provisioning soft-AP (WPA2, random password shown on the
 * display), so fetching a dump needs the device in hand. The CPU decrypts the
 * flash transparently, so this also works with flash encryption enabled, where
 * `idf.py coredump-info` over USB only sees ciphertext. */

static esp_err_t api_coredump(httpd_req_t *req)
{
    size_t addr, size;
    if (esp_core_dump_image_check() != ESP_OK || esp_core_dump_image_get(&addr, &size) != ESP_OK)
        return send_json(req, "{\"present\":false}");

    esp_core_dump_summary_t *sm = calloc(1, sizeof *sm);
    char *buf = malloc(768);
    if (!sm || !buf) { free(sm); free(buf); return fail(req, "500 Internal Server Error", "memory"); }

    char reason[160] = "", reason_e[2 * sizeof reason + 8] = "", task_e[2 * sizeof sm->exc_task + 8] = "";
    esp_core_dump_get_panic_reason(reason, sizeof reason);
    json_escape(reason, reason_e, sizeof reason_e);

    int o = snprintf(buf, 768, "{\"present\":true,\"size\":%u,\"reason\":\"%s\"", (unsigned)size, reason_e);
    if (esp_core_dump_get_summary(sm) == ESP_OK) {
        sm->exc_task[sizeof sm->exc_task - 1] = 0;
        sm->app_elf_sha256[sizeof sm->app_elf_sha256 - 1] = 0;
        json_escape(sm->exc_task, task_e, sizeof task_e);
        const char *cur = esp_app_get_elf_sha256_str();
        o += snprintf(buf + o, 768 - o,
                      ",\"task\":\"%s\",\"pc\":\"0x%08" PRIx32 "\",\"ra\":\"0x%08" PRIx32 "\","
                      "\"sp\":\"0x%08" PRIx32 "\",\"mcause\":\"0x%08" PRIx32 "\",\"mtval\":\"0x%08" PRIx32 "\","
                      "\"elf_sha\":\"%s\",\"same_fw\":%s",
                      task_e, sm->exc_pc, sm->ex_info.ra, sm->ex_info.sp, sm->ex_info.mcause, sm->ex_info.mtval,
                      (const char *)sm->app_elf_sha256,
                      strncmp(cur, (const char *)sm->app_elf_sha256, strlen((const char *)sm->app_elf_sha256)) ? "false" : "true");
    }
    snprintf(buf + o, 768 - o, "}");
    esp_err_t r = send_json(req, buf);
    free(sm);
    free(buf);
    return r;
}

/* Raw flash image (header + ELF + checksum):
 *   esp-coredump info_corefile -t raw -c coredump.bin build/c6_phone.elf */
static esp_err_t api_coredump_bin(httpd_req_t *req)
{
    size_t addr, size;
    if (esp_core_dump_image_check() != ESP_OK || esp_core_dump_image_get(&addr, &size) != ESP_OK)
        return fail(req, "404 Not Found", "no core dump");
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!p || addr < p->address || addr + size > p->address + p->size)
        return fail(req, "500 Internal Server Error", "partition");

    char *buf = malloc(1024);
    if (!buf) return fail(req, "500 Internal Server Error", "memory");
    /* No CORS header here: the dump contains RAM, i.e. possibly the Wi-Fi password. */
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"coredump.bin\"");
    esp_err_t r = ESP_OK;
    for (size_t off = 0; off < size && r == ESP_OK; ) {
        size_t n = size - off < 1024 ? size - off : 1024;
        r = esp_partition_read(p, addr - p->address + off, buf, n);   /* decrypts if encrypted */
        if (r == ESP_OK) r = httpd_resp_send_chunk(req, buf, n);
        off += n;
    }
    memset(buf, 0, 1024);
    free(buf);
    if (r != ESP_OK) { ESP_LOGE(TAG, "coredump download: %s", esp_err_to_name(r)); return ESP_FAIL; }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t api_coredump_erase(httpd_req_t *req)
{
    esp_err_t e = esp_core_dump_image_erase();
    if (e != ESP_OK) return fail(req, "500 Internal Server Error", esp_err_to_name(e));
    ESP_LOGI(TAG, "core dump erased");
    return send_json(req, "{\"ok\":true}");
}

/* ------------------------------------------------------- static HTTP ---- */

static const char *mime(const char *p)
{
    const char *e = strrchr(p, '.');
    if (!e) return "application/octet-stream";
    if (!strcmp(e, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(e, ".js") || !strcmp(e, ".mjs")) return "text/javascript";
    if (!strcmp(e, ".css"))  return "text/css";
    if (!strcmp(e, ".json")) return "application/json";
    if (!strcmp(e, ".svg"))  return "image/svg+xml";
    if (!strcmp(e, ".png"))  return "image/png";
    if (!strcmp(e, ".ico"))  return "image/x-icon";
    if (!strcmp(e, ".woff2")) return "font/woff2";
    return "application/octet-stream";
}

/* Captive portal: the DNS server sends every name to us, so the OS probes (Android
 * /generate_204, Apple /hotspot-detect.html, Windows /connecttest.txt, Firefox
 * /canonical.html ...) arrive with a foreign Host header. Anything not addressed to
 * the portal IP is redirected there; the OS then opens the setup page. */
static bool host_is_portal(httpd_req_t *req)
{
    char h[64];
    esp_err_t e = httpd_req_get_hdr_value_str(req, "Host", h, sizeof h);
    if (e == ESP_ERR_NOT_FOUND) return true;            /* HTTP/1.0 without Host */
    if (e != ESP_OK) return false;                      /* too long -> certainly not the IP */
    char *c = strchr(h, ':');
    if (c) *c = 0;
    return strcmp(h, PORTAL_IP) == 0;
}

static esp_err_t redirect_portal(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" PORTAL_IP "/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, "Redirecting", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t static_get(httpd_req_t *req)
{
    if (!host_is_portal(req)) return redirect_portal(req);

    const char *uri = req->uri;
    const char *q = strchr(uri, '?');
    size_t ul = q ? (size_t)(q - uri) : strlen(uri);
    char path[160];
    if (ul > 120 || strstr(uri, "..")) return httpd_resp_send_404(req);
    if (ul <= 1) snprintf(path, sizeof path, "/www/index.html");
    else         snprintf(path, sizeof path, "/www%.*s", (int)ul, uri);

    FILE *f = fopen(path, "r");
    if (!f) return redirect_portal(req);                /* unknown path -> setup page */

    httpd_resp_set_type(req, mime(path));
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    char *buf = malloc(1024);
    if (!buf) { fclose(f); return httpd_resp_send_500(req); }
    size_t n;
    while ((n = fread(buf, 1, 1024, f)) > 0)
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) break;
    httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    fclose(f);
    return ESP_OK;
}

static esp_err_t start_http(void)
{
    esp_vfs_spiffs_conf_t sp = { .base_path = "/www", .partition_label = "www",
                                 .max_files = 6, .format_if_mount_failed = false };
    esp_err_t e = esp_vfs_spiffs_register(&sp);
    if (e != ESP_OK) { ESP_LOGE(TAG, "mount www: %s", esp_err_to_name(e)); return e; }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = PROV_HTTP_PORT;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    cfg.max_open_sockets = 6;
    cfg.lru_purge_enable = true;
    cfg.stack_size       = 6144;
    httpd_handle_t srv;
    e = httpd_start(&srv, &cfg);
    if (e != ESP_OK) return e;

    /* First match wins: register the API routes before the wildcard catch-all. */
    httpd_uri_t routes[] = {
        { .uri = "/api/info", .method = HTTP_GET,  .handler = api_info },
        { .uri = "/api/scan", .method = HTTP_GET,  .handler = api_scan },
        { .uri = "/api/wifi", .method = HTTP_POST, .handler = api_wifi },
        { .uri = "/api/coredump",     .method = HTTP_GET,    .handler = api_coredump },
        { .uri = "/api/coredump.bin", .method = HTTP_GET,    .handler = api_coredump_bin },
        { .uri = "/api/coredump",     .method = HTTP_DELETE, .handler = api_coredump_erase },
        { .uri = "/*",        .method = HTTP_GET,  .handler = static_get },
    };
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++) {
        e = httpd_register_uri_handler(srv, &routes[i]);
        if (e != ESP_OK) return e;
    }
    return ESP_OK;
}

esp_err_t provision_start(void)
{
    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ip);
    dns_server_start(ip.ip.addr);                       /* all names -> this phone */
    return start_http();
}
