#include "ui.h"
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "board.h"
#include "app_config.h"

/* Built-in Montserrat has no umlauts: all device-UI strings are ASCII. */
#if LV_FONT_MONTSERRAT_28
#define FONT_BIG (&lv_font_montserrat_28)
#else
#define FONT_BIG LV_FONT_DEFAULT
#endif
#if LV_FONT_MONTSERRAT_20
#define FONT_MID (&lv_font_montserrat_20)
#else
#define FONT_MID LV_FONT_DEFAULT
#endif

#define C_BG     lv_color_hex(0x000000)     /* AMOLED: true black */
#define C_TEXT   lv_color_hex(0xF2F4F5)
#define C_DIM    lv_color_hex(0x8A949B)
#define C_ROW    lv_color_hex(0x15191C)
#define C_OK     lv_color_hex(0x2EAD62)
#define C_BAD    lv_color_hex(0xD6443A)

static ui_handlers_t H;
static char s_my_name[NAME_MAX_LEN + 1];

static lv_obj_t *scr_contacts, *scr_call, *scr_prov, *scr_set;
static lv_obj_t *c_name, *c_net, *c_list, *c_empty;
static lv_obj_t *k_name, *k_status, *k_accept, *k_hang;
static lv_obj_t *p_ssid, *p_pass;
static lv_timer_t *call_timer, *idle_timer;
static peer_t s_peers[PEER_MAX];
static int s_npeers;
static int s_call_secs;
static bool s_idle, s_in_call, s_prov_mode;

/* ---------- helpers ----------------------------------------------------- */
static lv_obj_t *screen(void)
{
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s, C_BG, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s, C_TEXT, 0);
    lv_obj_set_style_border_width(s, 0, 0);
    lv_obj_set_style_pad_all(s, 16, 0);
    lv_obj_set_flex_flow(s, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s, 12, 0);
    return s;
}

static lv_obj_t *label(lv_obj_t *p, const char *t, const lv_font_t *f, lv_color_t c)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

static lv_obj_t *button(lv_obj_t *p, const char *txt, lv_color_t bg, int w, int h, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(p);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, FONT_MID, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

/* ---------- contacts ---------------------------------------------------- */
static void on_peer_click(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= 0 && i < s_npeers && H.dial) H.dial(&s_peers[i]);
}
static void on_settings(lv_event_t *e) { lv_screen_load(scr_set); }
static void on_back(lv_event_t *e)     { lv_screen_load(scr_contacts); }
static void on_wifi(lv_event_t *e)     { if (H.wifi_setup) H.wifi_setup(); }
static void on_vol(lv_event_t *e)      { if (H.volume) H.volume(lv_slider_get_value(lv_event_get_target_obj(e))); }

static void build_contacts(void)
{
    scr_contacts = screen();
    c_name = label(scr_contacts, s_my_name, FONT_BIG, C_TEXT);
    c_net  = label(scr_contacts, "WLAN: ...", LV_FONT_DEFAULT, C_DIM);

    c_list = lv_list_create(scr_contacts);
    lv_obj_set_size(c_list, LV_PCT(100), 300);
    lv_obj_set_style_bg_color(c_list, C_BG, 0);
    lv_obj_set_style_border_width(c_list, 0, 0);
    lv_obj_set_flex_grow(c_list, 1);
    c_empty = label(scr_contacts, "Suche Telefone im WLAN ...", LV_FONT_DEFAULT, C_DIM);

    lv_obj_t *s = lv_button_create(scr_contacts);
    lv_obj_set_style_bg_color(s, C_ROW, 0);
    lv_obj_t *sl = lv_label_create(s);
    lv_label_set_text(sl, LV_SYMBOL_SETTINGS " Einstellungen");
    lv_obj_add_event_cb(s, on_settings, LV_EVENT_CLICKED, NULL);
}

static void build_settings(void)
{
    scr_set = screen();
    label(scr_set, "Einstellungen", FONT_BIG, C_TEXT);
    label(scr_set, "Lautstaerke", LV_FONT_DEFAULT, C_DIM);
    lv_obj_t *sl = lv_slider_create(scr_set);
    lv_obj_set_width(sl, LV_PCT(80));
    lv_slider_set_value(sl, 70, LV_ANIM_OFF);
    lv_obj_add_event_cb(sl, on_vol, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *w = lv_button_create(scr_set);
    lv_obj_t *wl = lv_label_create(w);
    lv_label_set_text(wl, LV_SYMBOL_WIFI " WLAN aendern");
    lv_obj_add_event_cb(w, on_wifi, LV_EVENT_CLICKED, NULL);
    lv_obj_t *b = lv_button_create(scr_set);
    lv_obj_t *bl = lv_label_create(b);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Zurueck");
    lv_obj_add_event_cb(b, on_back, LV_EVENT_CLICKED, NULL);
}

/* ---------- call screen --------------------------------------------------- */
static void on_accept(lv_event_t *e) { if (H.accept) H.accept(); }
static void on_hang(lv_event_t *e)   { if (H.hangup) H.hangup(); }

static void call_tick(lv_timer_t *t)
{
    s_call_secs++;
    lv_label_set_text_fmt(k_status, "%02d:%02d", s_call_secs / 60, s_call_secs % 60);
}

static void build_call(void)
{
    scr_call = screen();
    lv_obj_set_flex_align(scr_call, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    k_name   = label(scr_call, "", FONT_BIG, C_TEXT);
    k_status = label(scr_call, "", FONT_MID, C_DIM);
    lv_obj_t *row = lv_obj_create(scr_call);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 130);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    k_accept = button(row, LV_SYMBOL_CALL,  C_OK,  110, 110, on_accept);
    k_hang   = button(row, LV_SYMBOL_CLOSE, C_BAD, 110, 110, on_hang);
}

/* ---------- provisioning screen ------------------------------------------ */
static void build_prov(void)
{
    scr_prov = screen();
    label(scr_prov, "WLAN einrichten", FONT_BIG, C_TEXT);
    label(scr_prov, "1. Mit diesem WLAN verbinden:", LV_FONT_DEFAULT, C_DIM);
    p_ssid = label(scr_prov, "", FONT_MID, C_TEXT);
    label(scr_prov, "Passwort:", LV_FONT_DEFAULT, C_DIM);
    p_pass = label(scr_prov, "", FONT_BIG, C_TEXT);
    label(scr_prov, "2. Die Einrichtungsseite oeffnet sich von selbst. Sonst im Browser:", LV_FONT_DEFAULT, C_DIM);
    label(scr_prov, "http://192.168.4.1", FONT_MID, C_TEXT);
}

/* ---------- idle detection ------------------------------------------------ */
static void idle_tick(lv_timer_t *t)
{
    if (s_in_call || s_prov_mode) return;
    bool idle = lv_display_get_inactive_time(NULL) > IDLE_MS;
    if (idle != s_idle) { s_idle = idle; if (H.idle) H.idle(idle); }
}

/* ---------- public API (thread safe) -------------------------------------- */
void ui_init(const ui_handlers_t *h, const char *my_name)
{
    H = *h;
    strlcpy(s_my_name, my_name, sizeof s_my_name);
    board_lock(0);
    build_contacts(); build_settings(); build_call(); build_prov();
    idle_timer = lv_timer_create(idle_tick, 1000, NULL);
    board_unlock();
}

void ui_show_provisioning(const char *ssid, const char *pass)
{
    board_lock(0);
    s_prov_mode = true;
    lv_label_set_text(p_ssid, ssid);
    lv_label_set_text(p_pass, pass);
    lv_screen_load(scr_prov);
    board_unlock();
}

void ui_show_contacts(void)
{
    board_lock(0);
    lv_screen_load(scr_contacts);
    board_unlock();
}

void ui_set_peers(const peer_t *list, int n)
{
    board_lock(0);
    s_npeers = n > PEER_MAX ? PEER_MAX : n;
    memcpy(s_peers, list, s_npeers * sizeof(peer_t));
    lv_obj_clean(c_list);
    for (int i = 0; i < s_npeers; i++) {
        lv_obj_t *b = lv_list_add_button(c_list, LV_SYMBOL_CALL, s_peers[i].name);
        lv_obj_set_style_bg_color(b, C_ROW, 0);
        lv_obj_set_style_text_color(b, C_TEXT, 0);
        lv_obj_set_height(b, 72);
        lv_obj_add_event_cb(b, on_peer_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    if (s_npeers) lv_obj_add_flag(c_empty, LV_OBJ_FLAG_HIDDEN);
    else          lv_obj_remove_flag(c_empty, LV_OBJ_FLAG_HIDDEN);
    board_unlock();
}

void ui_set_net(net_state_t st, bool itwt)
{
    board_lock(0);
    const char *t = st == NET_ONLINE ? (itwt ? "WLAN: verbunden, Sparmodus iTWT" : "WLAN: verbunden")
                  : st == NET_CONNECTING ? "WLAN: verbinde ..." : "WLAN: aus";
    if (c_net) lv_label_set_text(c_net, t);
    board_unlock();
}

void ui_set_call(call_state_t st, const char *peer, const char *note)
{
    board_lock(0);
    s_in_call = (st != CALL_IDLE);
    if (call_timer) { lv_timer_delete(call_timer); call_timer = NULL; }

    if (st == CALL_IDLE) {
        lv_screen_load(scr_contacts);        /* 'note' ("Aufgelegt", ...) is shown on the next list refresh */
        if (note) lv_label_set_text(c_net, note);
    } else {
        lv_display_trigger_activity(NULL);   /* wake the screen */
        lv_label_set_text(k_name, peer ? peer : "");
        bool inc = st == CALL_INCOMING;
        if (inc) lv_obj_remove_flag(k_accept, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(k_accept, LV_OBJ_FLAG_HIDDEN);
        if (st == CALL_ACTIVE) {
            s_call_secs = 0;
            lv_label_set_text(k_status, "00:00");
            call_timer = lv_timer_create(call_tick, 1000, NULL);
        } else {
            lv_label_set_text(k_status, inc ? "Eingehender Anruf" : (note ? note : "Verbinde"));
        }
        lv_screen_load(scr_call);
    }
    board_unlock();
}
