#pragma once
#define APP_FW_VERSION      "0.1.0"
#define NVS_NS              "phone"
#define NAME_MAX_LEN        24
#define PEER_MAX            16

/* Call signalling / media (plain UDP, LAN only) */
#define SIG_PORT            5070
#define MEDIA_PORT          5072
#define SVC_TYPE            "_c6phone"
#define SVC_PROTO           "_udp"

/* Audio: 16 kHz mono, 20 ms frames, IMA-ADPCM (64 kbit/s) */
#define AUDIO_RATE          16000
#define AUDIO_FRAME         320

/* Speaker -> own mic coupling, measured 2026-11-15 on the 2.06" board
 * (1 kHz, vol 70, mic gain 30 dB): mic amplitude = 1.7 x played amplitude.
 * Two phones in a call form a loop of 1.7^2 ~ 3 -> howling. 18 dB gain puts
 * one device at ~0.43 and the loop at ~0.18 even without echo ducking. */
#define MIC_GAIN_DB         18.0f
/* Half-duplex echo suppression in call.c: while the far end plays above
 * ECHO_FAR_ACTIVE (mean |x|), the mic is attenuated by ECHO_DUCK_Q8/256
 * (16/256 = -24 dB), held for ECHO_HANG_FRAMES after the far end stops. */
#define ECHO_FAR_ACTIVE     250
#define ECHO_DUCK_Q8        16
#define ECHO_HANG_FRAMES    8         /* x 20 ms */

/* Provisioning (soft-AP only) */
#define PORTAL_IP           "192.168.4.1"   /* default soft-AP address */
#define PROV_HTTP_PORT      80

/* iTWT: interval = MANT * 2^EXPN us  ->  977 * 1024 us ~= 1.0 s */
#define ITWT_INTERVAL_MANT  977
#define ITWT_INTERVAL_EXPN  10
#define ITWT_MIN_WAKE       128       /* x 256 us = 32.8 ms */

#define IDLE_MS             20000     /* screen off + iTWT after this */
