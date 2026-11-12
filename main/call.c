#include "call.h"
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "board.h"
#include "adpcm.h"

static const char *TAG = "call";

/* ---- wire formats (little endian, both ends are ESP32-C6) -------------- */
enum { SIG_INVITE = 1, SIG_RINGING, SIG_ACCEPT, SIG_REJECT, SIG_BYE };
typedef struct __attribute__((packed)) {
    uint8_t  magic;                  /* 0xC6 */
    uint8_t  type;
    uint16_t call_id;
    char     name[NAME_MAX_LEN + 1];
} sig_t;

typedef struct __attribute__((packed)) {
    uint8_t  magic;                  /* 0xA1 */
    uint8_t  flags;
    uint16_t seq;
    int16_t  pred;                   /* ADPCM state BEFORE this block */
    uint8_t  idx;
    uint8_t  rsv;
    uint8_t  data[AUDIO_FRAME / 2];
} media_t;

/* ---- state -------------------------------------------------------------- */
static call_cb_t   s_cb;
static char        s_my_name[NAME_MAX_LEN + 1];
static SemaphoreHandle_t s_mx;
static call_state_t s_state = CALL_IDLE;
static struct sockaddr_in s_peer;
static char        s_peer_name[NAME_MAX_LEN + 1];
static uint16_t    s_call_id;
static int         s_sig_sock = -1, s_media_sock = -1;
static int         s_invite_tries;
static int64_t     s_last_invite_us;

static volatile bool s_media_run, s_ring_run;
static EventGroupHandle_t s_done;                 /* bit per media task */
static QueueHandle_t s_rxq;
static volatile int s_far_level;                  /* mean |x| of what we play */

enum { B_CAP = 1, B_PLAY = 2, B_RX = 4, B_RING = 8 };

static void notify(call_state_t st, const char *note) { if (s_cb) s_cb(st, s_peer_name, note); }

static void sig_send(uint8_t type)
{
    sig_t m = { .magic = 0xC6, .type = type, .call_id = s_call_id };
    strlcpy(m.name, s_my_name, sizeof m.name);
    sendto(s_sig_sock, &m, sizeof m, 0, (struct sockaddr *)&s_peer, sizeof s_peer);
}

/* ---- ringer (also used as ring-back tone) ------------------------------ */
static void ringer_task(void *arg)
{
    int16_t f[AUDIO_FRAME];
    uint32_t t = 0;
    while (s_ring_run) {
        for (int k = 0; k < 100 && s_ring_run; k++) {            /* 2 s cadence: 0.4 s tone, 1.6 s pause */
            bool on = k < 20;
            for (int i = 0; i < AUDIO_FRAME; i++, t++)
                f[i] = on ? (int16_t)(5000 * sinf(2.0f * (float)M_PI * 425.0f * t / AUDIO_RATE)) : 0;
            board_spk_write(f, AUDIO_FRAME);
        }
    }
    xEventGroupSetBits(s_done, B_RING);
    vTaskDelete(NULL);
}
static void ring_start(void)
{
    board_audio_start();
    s_ring_run = true;
    xEventGroupClearBits(s_done, B_RING);
    xTaskCreate(ringer_task, "ring", 3072, NULL, 4, NULL);
}
static void ring_stop(void)
{
    if (!s_ring_run) return;
    s_ring_run = false;
    xEventGroupWaitBits(s_done, B_RING, pdTRUE, pdTRUE, pdMS_TO_TICKS(500));
}

/* ---- media tasks -------------------------------------------------------- */
static void cap_task(void *arg)
{
    int16_t pcm[AUDIO_FRAME];
    media_t pkt = { .magic = 0xA1 };
    adpcm_state_t st = { 0, 0 };
    uint16_t seq = 0;
    int gain_q8 = 256;
    while (s_media_run) {
        if (board_mic_read(pcm, AUDIO_FRAME) != AUDIO_FRAME) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }

        /* Crude speakerphone echo ducking (no AEC on this MCU, see README):
         * while the far end is loud, attenuate the mic path. */
        int target = s_far_level > 1500 ? 64 : 256;
        gain_q8 += (target - gain_q8) / 4;
        for (int i = 0; i < AUDIO_FRAME; i++) pcm[i] = (int16_t)((pcm[i] * gain_q8) >> 8);

        pkt.seq = seq++;
        pkt.pred = (int16_t)st.pred;
        pkt.idx = (uint8_t)st.idx;
        adpcm_encode(&st, pcm, AUDIO_FRAME, pkt.data);
        sendto(s_media_sock, &pkt, sizeof pkt, 0, (struct sockaddr *)&s_peer, sizeof s_peer);
    }
    xEventGroupSetBits(s_done, B_CAP);
    vTaskDelete(NULL);
}

static void rx_task(void *arg)
{
    media_t p;
    int last_seq = -1;
    while (s_media_run) {
        struct sockaddr_in from; socklen_t fl = sizeof from;
        int n = recvfrom(s_media_sock, &p, sizeof p, 0, (struct sockaddr *)&from, &fl);
        if (n != sizeof p || p.magic != 0xA1) continue;
        if (from.sin_addr.s_addr != s_peer.sin_addr.s_addr) continue;     /* only our peer */
        if (last_seq >= 0 && (int16_t)(p.seq - last_seq) <= 0) continue;  /* late / duplicate */
        last_seq = p.seq;
        if (xQueueSend(s_rxq, &p, 0) != pdTRUE) {                          /* full: drop oldest */
            media_t tmp; xQueueReceive(s_rxq, &tmp, 0); xQueueSend(s_rxq, &p, 0);
        }
    }
    xEventGroupSetBits(s_done, B_RX);
    vTaskDelete(NULL);
}

static void play_task(void *arg)
{
    int16_t pcm[AUDIO_FRAME] = { 0 };
    adpcm_state_t st = { 0, 0 };
    bool primed = false;
    int gap = 0;
    media_t p;
    while (s_media_run) {
        if (!primed) {                                  /* ~40 ms jitter pre-buffer */
            if (uxQueueMessagesWaiting(s_rxq) >= 2) primed = true;
            else { int16_t z[AUDIO_FRAME] = { 0 }; board_spk_write(z, AUDIO_FRAME); s_far_level = 0; continue; }
        }
        if (xQueueReceive(s_rxq, &p, 0) == pdTRUE) {
            gap = 0;
            st.pred = p.pred; st.idx = p.idx;
            adpcm_decode(&st, p.data, AUDIO_FRAME, pcm);
        } else {                                         /* concealment: fade out last frame */
            gap++;
            for (int i = 0; i < AUDIO_FRAME; i++) pcm[i] = (int16_t)(pcm[i] / 2);
            if (gap > 6) primed = false;
        }
        int acc = 0;
        for (int i = 0; i < AUDIO_FRAME; i += 8) acc += pcm[i] < 0 ? -pcm[i] : pcm[i];
        s_far_level = acc / (AUDIO_FRAME / 8);
        board_spk_write(pcm, AUDIO_FRAME);               /* blocks -> paces the loop at 20 ms */
    }
    xEventGroupSetBits(s_done, B_PLAY);
    vTaskDelete(NULL);
}

static void media_start(void)
{
    board_audio_start();
    s_media_sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(MEDIA_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
    int one = 1; setsockopt(s_media_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    bind(s_media_sock, (struct sockaddr *)&a, sizeof a);
    struct timeval tv = { .tv_usec = 100000 };
    setsockopt(s_media_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    s_peer.sin_port = htons(MEDIA_PORT);                 /* media goes to the peer's media port */

    s_rxq = xQueueCreate(6, sizeof(media_t));
    xEventGroupClearBits(s_done, B_CAP | B_PLAY | B_RX);
    s_media_run = true;
    xTaskCreate(rx_task,   "rx",   4096, NULL, 6, NULL);
    xTaskCreate(play_task, "play", 4096, NULL, 7, NULL);
    xTaskCreate(cap_task,  "cap",  5120, NULL, 7, NULL);
}

static void media_stop(void)
{
    if (!s_media_run) return;
    s_media_run = false;
    xEventGroupWaitBits(s_done, B_CAP | B_PLAY | B_RX, pdTRUE, pdTRUE, pdMS_TO_TICKS(1000));
    close(s_media_sock); s_media_sock = -1;
    vQueueDelete(s_rxq); s_rxq = NULL;
    s_far_level = 0;
}

/* ---- state transitions (call with s_mx held) --------------------------- */
static void to_idle(const char *note, bool send_bye)
{
    if (send_bye && s_state != CALL_IDLE) {
        s_peer.sin_port = htons(SIG_PORT);
        for (int i = 0; i < 3; i++) sig_send(SIG_BYE);
    }
    ring_stop();
    media_stop();
    board_audio_stop();
    s_state = CALL_IDLE;
    notify(CALL_IDLE, note);
}

static void to_active(void)
{
    ring_stop();
    s_state = CALL_ACTIVE;
    media_start();
    notify(CALL_ACTIVE, NULL);
}

/* ---- signalling task ---------------------------------------------------- */
static void sig_task(void *arg)
{
    for (;;) {
        sig_t m;
        struct sockaddr_in from; socklen_t fl = sizeof from;
        int n = recvfrom(s_sig_sock, &m, sizeof m, 0, (struct sockaddr *)&from, &fl);

        xSemaphoreTake(s_mx, portMAX_DELAY);
        if (n == sizeof m && m.magic == 0xC6) {
            m.name[NAME_MAX_LEN] = 0;
            bool same = (m.call_id == s_call_id) && from.sin_addr.s_addr == s_peer.sin_addr.s_addr;
            switch (m.type) {
            case SIG_INVITE:
                if (s_state == CALL_IDLE) {
                    s_peer = from; s_peer.sin_port = htons(SIG_PORT);
                    s_call_id = m.call_id;
                    strlcpy(s_peer_name, m.name, sizeof s_peer_name);
                    s_state = CALL_INCOMING;
                    sig_send(SIG_RINGING);
                    ring_start();
                    notify(CALL_INCOMING, NULL);
                } else if (same && s_state == CALL_INCOMING) {
                    sig_send(SIG_RINGING);                     /* retransmitted INVITE */
                } else {
                    struct sockaddr_in keep = s_peer; uint16_t id = s_call_id;
                    s_peer = from; s_peer.sin_port = htons(SIG_PORT); s_call_id = m.call_id;
                    sig_send(SIG_REJECT);                      /* busy */
                    s_peer = keep; s_call_id = id;
                }
                break;
            case SIG_RINGING:
                if (same && s_state == CALL_OUTGOING) { s_invite_tries = 0; notify(CALL_OUTGOING, "Klingelt"); }
                break;
            case SIG_ACCEPT:
                if (same && s_state == CALL_OUTGOING) to_active();
                break;
            case SIG_REJECT:
                if (same && s_state == CALL_OUTGOING) to_idle("Abgelehnt", false);
                break;
            case SIG_BYE:
                if (same && s_state != CALL_IDLE) to_idle("Aufgelegt", false);
                break;
            }
        }
        /* INVITE retransmission: the callee may sleep in an iTWT interval (~1 s) */
        if (s_state == CALL_OUTGOING && esp_timer_get_time() - s_last_invite_us > 700000) {
            if (++s_invite_tries > 12) to_idle("Nicht erreichbar", false);
            else { sig_send(SIG_INVITE); s_last_invite_us = esp_timer_get_time(); }
        }
        xSemaphoreGive(s_mx);
    }
}

/* ---- public API --------------------------------------------------------- */
void call_init(const char *my_name, call_cb_t cb)
{
    s_cb = cb;
    strlcpy(s_my_name, my_name, sizeof s_my_name);
    s_mx = xSemaphoreCreateMutex();
    s_done = xEventGroupCreate();

    s_sig_sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(SIG_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
    bind(s_sig_sock, (struct sockaddr *)&a, sizeof a);
    struct timeval tv = { .tv_usec = 200000 };
    setsockopt(s_sig_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    xTaskCreate(sig_task, "sig", 4096, NULL, 6, NULL);
    ESP_LOGI(TAG, "signalling on UDP %d", SIG_PORT);
}

esp_err_t call_dial(const peer_t *p)
{
    esp_err_t e = ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (s_state == CALL_IDLE) {
        memset(&s_peer, 0, sizeof s_peer);
        s_peer.sin_family = AF_INET;
        s_peer.sin_port = htons(SIG_PORT);
        s_peer.sin_addr.s_addr = p->ip.addr;
        strlcpy(s_peer_name, p->name, sizeof s_peer_name);
        s_call_id = (uint16_t)esp_random();
        s_state = CALL_OUTGOING;
        s_invite_tries = 0;
        sig_send(SIG_INVITE);
        s_last_invite_us = esp_timer_get_time();
        ring_start();
        notify(CALL_OUTGOING, "Verbinde");
        e = ESP_OK;
    }
    xSemaphoreGive(s_mx);
    return e;
}

void call_accept(void)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (s_state == CALL_INCOMING) { sig_send(SIG_ACCEPT); to_active(); }
    xSemaphoreGive(s_mx);
}

void call_hangup(void)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (s_state == CALL_INCOMING) { sig_send(SIG_REJECT); to_idle("Abgelehnt", false); }
    else if (s_state != CALL_IDLE) to_idle("Aufgelegt", true);
    xSemaphoreGive(s_mx);
}

call_state_t call_state(void) { return s_state; }
