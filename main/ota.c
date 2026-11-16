#include "ota.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "call.h"
#include "json_util.h"
#include "version.h"

static const char *TAG = "ota";

#if CONFIG_PHONE_TEST_OTA
#define TEST_OTA 1
#else
#define TEST_OTA 0
#endif
#if CONFIG_PHONE_TEST_AUTODIAL
#define AUTODIAL 1
#else
#define AUTODIAL 0
#endif

#define EV_ONLINE   (1u << 0)
#define EV_CHECK    (1u << 1)
#define EV_INSTALL  (1u << 2)

#define MANIFEST_MAX     1024
#define URL_MAX          512
#define VER_MAX          32
#define RETRY_BUSY_US    (10LL * 60 * 1000000)     /* periodic check during a call */
#define HTTP_RX_BUF      4096   /* GitHub redirects carry ~1 KB Location headers */
#define HTTP_TX_BUF      2048

static ota_cb_t     s_cb;
static TaskHandle_t s_task;
static ota_state_t  s_state = OTA_IDLE;
static char         s_offer[VER_MAX];          /* version on offer (OTA_AVAILABLE) */
static char         s_url[URL_MAX];
static char         s_rolled_back[VER_MAX];    /* version the bootloader rolled back from */
static bool         s_pending;                 /* this boot: new firmware, not yet confirmed */
static bool         s_boot_pending;            /* s_pending as it was at boot (for the status line) */
static esp_timer_handle_t s_verify_timer;
static int64_t      s_next_check_us;
static bool         s_logged_status;           /* 0 = periodic check not armed yet */

static const char *running(void) { return esp_app_get_description()->version; }
const char *ota_version(void)    { return version_display(running()); }

static void log_heap(const char *what)
{
    ESP_LOGI(TAG, "heap before %s: free %u, largest block %u, min ever %u", what,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
}

static void set_state(ota_state_t st, const char *ver, int pct, const char *msg)
{
    s_state = st;
    if (s_cb) s_cb(st, ver, pct, msg);
}

/* ---------------------------------------------------------- rollback ---- */

static void verify_timeout(void *arg)
{
    ESP_LOGW(TAG, "firmware %s did not come up within %d s - rolling back",
             running(), CONFIG_PHONE_OTA_VERIFY_TIMEOUT_S);
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

void ota_app_ok(void)
{
    if (!s_pending) return;
#if CONFIG_PHONE_TEST_OTA_NEVER_VALID
    ESP_LOGW(TAG, "test build: firmware %s never marks itself valid", running());
#else
    esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
    if (e == ESP_OK) {
        s_pending = false;
        esp_timer_stop(s_verify_timer);
        ESP_LOGI(TAG, "firmware %s marked valid", running());
    } else {
        ESP_LOGE(TAG, "mark valid: %s", esp_err_to_name(e));
    }
#endif
}

static void rollback_init(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(run, &st);
    ESP_LOGI(TAG, "running %s from %s (state %d)", running(), run->label, (int)st);

    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    esp_app_desc_t d;
    if (bad && esp_ota_get_partition_description(bad, &d) == ESP_OK) {
        strlcpy(s_rolled_back, d.version, sizeof s_rolled_back);
        ESP_LOGW(TAG, "update to %s was rolled back (%s)", d.version, bad->label);
    }

    if (st == ESP_OTA_IMG_PENDING_VERIFY) {
        s_pending = s_boot_pending = true;
        const esp_timer_create_args_t ta = { .callback = verify_timeout, .name = "ota_verify" };
        esp_timer_create(&ta, &s_verify_timer);
        esp_timer_start_once(s_verify_timer, (uint64_t)CONFIG_PHONE_OTA_VERIFY_TIMEOUT_S * 1000000);
        ESP_LOGW(TAG, "new firmware %s: pending verify (%d s)", running(), CONFIG_PHONE_OTA_VERIFY_TIMEOUT_S);
    }
}

/* ---------------------------------------------------------- manifest ---- */

typedef struct { char *buf; int len; bool overflow; } body_t;

static esp_err_t on_http(esp_http_client_event_t *e)
{
    body_t *b = e->user_data;
    /* Bodies of redirect responses also arrive here - keep only the final one. */
    if (e->event_id != HTTP_EVENT_ON_DATA || esp_http_client_get_status_code(e->client) != 200) return ESP_OK;
    if (b->len + e->data_len >= MANIFEST_MAX) { b->overflow = true; return ESP_OK; }
    memcpy(b->buf + b->len, e->data, e->data_len);
    b->len += e->data_len;
    return ESP_OK;
}

static void http_cfg(esp_http_client_config_t *c, const char *url)
{
    *c = (esp_http_client_config_t){
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = HTTP_RX_BUF,
        .buffer_size_tx = HTTP_TX_BUF,
        .timeout_ms = 20000,
        .max_redirection_count = 5,
        .user_agent = "c6-phone-ota",
        .keep_alive_enable = true,
    };
}

/* Fetches the manifest. ESP_OK and filled ver/url, or an error. */
static esp_err_t fetch_manifest(char *ver, size_t vn, char *url, size_t un)
{
    char *buf = calloc(1, MANIFEST_MAX);
    if (!buf) return ESP_ERR_NO_MEM;
    body_t b = { .buf = buf };
    esp_http_client_config_t c;
    http_cfg(&c, CONFIG_PHONE_OTA_MANIFEST_URL);
    c.event_handler = on_http;
    c.user_data = &b;
    esp_http_client_handle_t h = esp_http_client_init(&c);
    esp_err_t e = h ? esp_http_client_perform(h) : ESP_ERR_NO_MEM;
    int status = h ? esp_http_client_get_status_code(h) : 0;
    if (h) esp_http_client_cleanup(h);

    if (e == ESP_OK && status != 200) {
        ESP_LOGW(TAG, "manifest: HTTP %d", status);
        e = ESP_ERR_NOT_FOUND;
    } else if (e == ESP_OK && b.overflow) {
        e = ESP_ERR_INVALID_SIZE;
    } else if (e == ESP_OK) {
        buf[b.len] = 0;
        if (!json_get_str(buf, "version", ver, vn) || !json_get_str(buf, "url", url, un) || !url[0])
            e = ESP_ERR_INVALID_RESPONSE;
    }
    free(buf);
    return e;
}

static void do_check(void)
{
    char ver[VER_MAX] = "";
    set_state(OTA_CHECKING, NULL, 0, NULL);    /* main.c: Wi-Fi out of power save */
    vTaskDelay(pdMS_TO_TICKS(300));
    log_heap("check");
    esp_err_t e = fetch_manifest(ver, sizeof ver, s_url, sizeof s_url);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "check failed: %s", esp_err_to_name(e));
        set_state(OTA_FAILED, NULL, 0, "Keine Verbindung zum Update-Server");
        return;
    }
    if (!version_is_update(running(), ver)) {
        ESP_LOGI(TAG, "up to date (%s, offered %s)", running(), ver);
        set_state(OTA_UP_TO_DATE, ver, 0, NULL);
        return;
    }
    if (!strcmp(ver, s_rolled_back)) {
        /* That version did not come up here before - do not offer it again. */
        ESP_LOGW(TAG, "offered %s was rolled back before - ignored", ver);
        set_state(OTA_UP_TO_DATE, ver, 0, NULL);
        return;
    }
    strlcpy(s_offer, ver, sizeof s_offer);
    ESP_LOGI(TAG, "update available: %s -> %s", running(), ver);
    set_state(OTA_AVAILABLE, ver, 0, NULL);
}

/* ----------------------------------------------------------- install ---- */

static void do_install(void)
{
    if (s_state != OTA_AVAILABLE) return;
    if (!call_block(true)) {
        ESP_LOGW(TAG, "update refused: call in progress");
        set_state(OTA_AVAILABLE, s_offer, 0, "Nicht waehrend eines Anrufs");
        return;
    }
    set_state(OTA_INSTALLING, s_offer, 0, NULL);  /* main.c: Wi-Fi out of power save, screen on */
    vTaskDelay(pdMS_TO_TICKS(300));
    log_heap("install");
    ESP_LOGI(TAG, "install %s from %s", s_offer, s_url);

    esp_http_client_config_t hc;
    http_cfg(&hc, s_url);
    esp_https_ota_config_t oc = { .http_config = &hc };
    esp_https_ota_handle_t h = NULL;
    const char *why = "Download fehlgeschlagen";
    esp_err_t e = esp_https_ota_begin(&oc, &h);

    if (e == ESP_OK) {
        esp_app_desc_t d;
        e = esp_https_ota_get_img_desc(h, &d);
        if (e == ESP_OK && strncmp(d.project_name, esp_app_get_description()->project_name, sizeof d.project_name)) {
            ESP_LOGE(TAG, "image is '%.32s', not this project", d.project_name);
            why = "Falsches Image";
            e = ESP_ERR_INVALID_VERSION;
        } else if (e == ESP_OK && strncmp(d.version, s_offer, sizeof d.version)) {
            ESP_LOGE(TAG, "image version %.32s, manifest says %s", d.version, s_offer);
            why = "Falsche Version im Image";
            e = ESP_ERR_INVALID_VERSION;
        }
    }
    int last = -1;
    while (e == ESP_OK) {
        e = esp_https_ota_perform(h);
        if (e != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
        int size = esp_https_ota_get_image_size(h), got = esp_https_ota_get_image_len_read(h);
        int pct = size > 0 ? (int)((int64_t)got * 100 / size) : 0;
        if (pct != last) {
            if (pct / 10 != last / 10) ESP_LOGI(TAG, "progress %d%%", pct);
            last = pct;
            set_state(OTA_INSTALLING, s_offer, pct, NULL);
        }
        /* On a fast LAN this loop never blocks; let IDLE run (task watchdog). */
        vTaskDelay(1);
        e = ESP_OK;
    }
    if (e == ESP_OK && !esp_https_ota_is_complete_data_received(h)) e = ESP_ERR_INVALID_SIZE;

    if (e == ESP_OK) {
        e = esp_https_ota_finish(h);                /* verifies the image, sets the boot partition */
        h = NULL;
        if (e != ESP_OK) why = "Image ungueltig";
    } else if (h) {
        esp_https_ota_abort(h);
    }

    if (e != ESP_OK) {
        ESP_LOGE(TAG, "update to %s failed: %s", s_offer, esp_err_to_name(e));
        call_block(false);
        set_state(OTA_FAILED, s_offer, 0, why);
        s_state = OTA_AVAILABLE;                    /* offer stays: tap again to retry */
        return;
    }
    ESP_LOGI(TAG, "update to %s written, rebooting", s_offer);
    set_state(OTA_REBOOTING, s_offer, 100, NULL);
    vTaskDelay(pdMS_TO_TICKS(1500));                /* let the display show it */
    esp_restart();
}

/* -------------------------------------------------------------- task ---- */

static void ota_task(void *arg)
{
    for (;;) {
        TickType_t wait = portMAX_DELAY;
        if (s_next_check_us) {
            /* pdMS_TO_TICKS overflows 32 bit for a day at 1 kHz - compute in 64 bit */
            int64_t dt = s_next_check_us - esp_timer_get_time();
            wait = dt > 0 ? (TickType_t)(dt * configTICK_RATE_HZ / 1000000) + 1 : 0;
        }
        uint32_t ev = 0;
        xTaskNotifyWait(0, UINT32_MAX, &ev, wait);

        /* Bench test with autodial: the call triggers the check (main.c). */
        if ((ev & EV_ONLINE) && !s_logged_status) {
            /* Repeated from rollback_init(): the boot lines are often lost
             * (serial reconnect), the bench test reads this one. */
            s_logged_status = true;
            ESP_LOGI(TAG, "status: running %s from %s%s%s%s", running(), esp_ota_get_running_partition()->label,
                     s_boot_pending ? ", booted pending verify" : "",
                     s_rolled_back[0] ? ", rolled back from " : "", s_rolled_back);
        }
        if ((ev & EV_ONLINE) && !s_next_check_us && !(TEST_OTA && AUTODIAL))
            s_next_check_us = esp_timer_get_time() + (int64_t)CONFIG_PHONE_OTA_FIRST_CHECK_S * 1000000;

        bool periodic = s_next_check_us && esp_timer_get_time() >= s_next_check_us;
        if (periodic && call_state() != CALL_IDLE && !TEST_OTA) {
            s_next_check_us = esp_timer_get_time() + RETRY_BUSY_US;      /* TLS needs RAM the call uses */
            periodic = false;
        } else if (periodic) {
            s_next_check_us = esp_timer_get_time() + (int64_t)CONFIG_PHONE_OTA_CHECK_INTERVAL_H * 3600 * 1000000;
        }
        if (periodic || (ev & EV_CHECK)) do_check();

        if (TEST_OTA && s_state == OTA_AVAILABLE) {
            /* Bench test: install without a tap. Refused during a call -> retry soon. */
            do_install();
            if (s_state == OTA_AVAILABLE) s_next_check_us = esp_timer_get_time() + 20LL * 1000000;
        } else if (ev & EV_INSTALL) {
            do_install();
        }
    }
}

static void notify(uint32_t ev) { if (s_task) xTaskNotify(s_task, ev, eSetBits); }

void ota_online(void)  { notify(EV_ONLINE); }
void ota_check(void)   { notify(EV_CHECK); }
void ota_install(void) { notify(EV_INSTALL); }

void ota_init(ota_cb_t cb)
{
    s_cb = cb;
    rollback_init();
    xTaskCreate(ota_task, "ota", 8192, NULL, 4, &s_task);
}
