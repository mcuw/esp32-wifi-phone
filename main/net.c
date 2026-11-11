#include "net.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "app_config.h"
#include "dhcpserver/dhcpserver.h"   /* dhcps_offer_t; adjust path if your IDF exposes it elsewhere */

static const char *TAG = "net";

static net_cb_t        s_cb;
static net_state_t     s_state = NET_IDLE;
static bool            s_want_sta;
static bool            s_twt;            /* iTWT agreement active */
static bool            s_twt_pending;
static int             s_retry;
static esp_timer_handle_t s_retry_timer;

static void set_state(net_state_t st)
{
    s_state = st;
    if (s_cb) s_cb(st, s_twt);
}


/* Captive portal, part 1 (DHCP): hand out the AP as DNS server and announce the
 * portal URL with DHCP option 114 (RFC 8910, used by Android 11+ / iOS 14+). */
static void ap_portal_setup(void)
{
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap) return;
    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(ap, &ip);
    char uri[32];
    snprintf(uri, sizeof uri, "http://" IPSTR, IP2STR(&ip.ip));

    esp_netif_dhcps_stop(ap);                       /* options can only change while stopped */
    esp_netif_dns_info_t dns = { .ip.type = ESP_IPADDR_TYPE_V4 };
    dns.ip.u_addr.ip4 = ip.ip;
    esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns);
    dhcps_offer_t offer = OFFER_DNS;
    esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof offer);
    esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, uri, strlen(uri));
    esp_netif_dhcps_start(ap);
    ESP_LOGI(TAG, "captive portal: DNS + DHCP opt 114 -> %s", uri);
}

static void retry_cb(void *arg)
{
    if (s_want_sta) esp_wifi_connect();
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case WIFI_EVENT_AP_START:
        ap_portal_setup();
        break;
    case WIFI_EVENT_STA_START:
        if (s_want_sta) esp_wifi_connect();
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        s_twt = false;
        s_twt_pending = false;
        if (s_want_sta) {
            set_state(NET_CONNECTING);
            uint64_t delay_ms = 500ULL << (s_retry < 5 ? s_retry : 5);   /* 0.5 s .. 16 s */
            s_retry++;
            esp_timer_start_once(s_retry_timer, delay_ms * 1000ULL);
        }
        break;
    case WIFI_EVENT_ITWT_SETUP: {
        wifi_event_sta_itwt_setup_t *e = (wifi_event_sta_itwt_setup_t *)data;
        s_twt_pending = false;
        if (e->status == ITWT_SETUP_SUCCESS) {
            s_twt = true;
            ESP_LOGI(TAG, "iTWT agreed (flow %d)", e->config.flow_id);
        } else {
            s_twt = false;
            ESP_LOGW(TAG, "iTWT setup failed (status %d) - AP may not support TWT; using modem sleep", e->status);
        }
        if (s_cb) s_cb(s_state, s_twt);
        break;
    }
    case WIFI_EVENT_ITWT_TEARDOWN:
        s_twt = false;
        ESP_LOGI(TAG, "iTWT torn down");
        if (s_cb) s_cb(s_state, s_twt);
        break;
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&e->ip_info.ip));
        s_retry = 0;
        set_state(NET_ONLINE);
    }
}

esp_err_t net_init(net_cb_t cb)
{
    s_cb = cb;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    /* We keep credentials in our own NVS namespace, not in the Wi-Fi driver's. */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL));

    const esp_timer_create_args_t ta = { .callback = retry_cb, .name = "wifi_retry" };
    return esp_timer_create(&ta, &s_retry_timer);
}

esp_err_t net_start_sta(const char *ssid, const char *pass)
{
    wifi_config_t sta = { 0 };
    strlcpy((char *)sta.sta.ssid, ssid, sizeof sta.sta.ssid);
    strlcpy((char *)sta.sta.password, pass, sizeof sta.sta.password);
    sta.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    sta.sta.pmf_cfg.capable = true;
    sta.sta.pmf_cfg.required = false;
    sta.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;     /* WPA2/WPA3 mixed routers */

    s_want_sta = true;
    s_retry = 0;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    /* Wi-Fi 6 (HE) is mandatory for iTWT. ESP32-C6 is 2.4 GHz only -> HE20. */
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    set_state(NET_CONNECTING);
    return esp_wifi_start();
}

esp_err_t net_start_provisioning(char *ap_ssid, size_t ssid_n, char *ap_pass, size_t pass_n)
{
    /* Random per-boot password, shown on the display. No ambiguous characters. */
    static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
    char pw[11];
    for (int i = 0; i < 10; i++) pw[i] = alphabet[esp_random() % (sizeof alphabet - 1)];
    pw[10] = 0;

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[33];
    snprintf(ssid, sizeof ssid, "C6Phone-%02X%02X", mac[4], mac[5]);

    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, ssid, sizeof ap.ap.ssid);
    ap.ap.ssid_len = strlen(ssid);
    strlcpy((char *)ap.ap.password, pw, sizeof ap.ap.password);
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.pmf_cfg.required = false;

    s_want_sta = false;
    /* APSTA so that the web UI can scan for networks. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    strlcpy(ap_ssid, ssid, ssid_n);
    strlcpy(ap_pass, pw, pass_n);
    set_state(NET_AP);
    return ESP_OK;
}

int net_scan(wifi_ap_record_t *out, int max)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return 0;
    uint16_t n = max;
    if (esp_wifi_scan_get_ap_records(&n, out) != ESP_OK) n = 0;
    return n;
}

void net_power_idle(void)
{
    if (s_state != NET_ONLINE) return;
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    wifi_phy_mode_t mode;
    if (esp_wifi_sta_get_negotiated_phymode(&mode) != ESP_OK || mode != WIFI_PHY_MODE_HE20) {
        ESP_LOGW(TAG, "AP did not negotiate HE20 (got %d): no iTWT, modem sleep only", (int)mode);
        return;
    }
    if (s_twt || s_twt_pending) return;

    /* Struct name: wifi_twt_setup_config_t in current headers (esp_wifi_he_types.h);
     * older IDF releases call it wifi_itwt_setup_config_t. */
    wifi_twt_setup_config_t c = {
        .setup_cmd          = TWT_REQUEST,
        .flow_id            = 0,
        .twt_id             = 0,
        .flow_type          = 1,                 /* 1 = unannounced */
        .trigger            = false,
        .min_wake_dura      = ITWT_MIN_WAKE,
        .wake_duration_unit = 0,                 /* 256 us units */
        .wake_invl_expn     = ITWT_INTERVAL_EXPN,
        .wake_invl_mant     = ITWT_INTERVAL_MANT,
        .timeout_time_ms    = 5000,
    };
    s_twt_pending = true;
    esp_err_t e = esp_wifi_sta_itwt_setup(&c);
    if (e != ESP_OK) {
        s_twt_pending = false;
        ESP_LOGW(TAG, "itwt_setup: %s", esp_err_to_name(e));
    }
}

void net_power_active(void)
{
    if (s_state != NET_ONLINE) return;
    if (s_twt) esp_wifi_sta_itwt_teardown(0);
    esp_wifi_set_ps(WIFI_PS_NONE);
}

net_state_t net_state(void) { return s_state; }
