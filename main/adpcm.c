#include "adpcm.h"

static const int8_t idx_tab[16] = { -1,-1,-1,-1,2,4,6,8, -1,-1,-1,-1,2,4,6,8 };
static const int16_t step_tab[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,
    107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,
    796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,
    4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,
    16818,18500,20350,22385,24623,27086,29794,32767 };

static inline int clamp16(int v) { return v > 32767 ? 32767 : (v < -32768 ? -32768 : v); }

static void apply(adpcm_state_t *s, uint8_t code)
{
    int step = step_tab[s->idx];
    int vpdiff = step >> 3;
    if (code & 4) vpdiff += step;
    if (code & 2) vpdiff += step >> 1;
    if (code & 1) vpdiff += step >> 2;
    s->pred = clamp16((code & 8) ? s->pred - vpdiff : s->pred + vpdiff);
    s->idx += idx_tab[code];
    if (s->idx < 0) s->idx = 0;
    if (s->idx > 88) s->idx = 88;
}

static uint8_t enc1(adpcm_state_t *s, int16_t x)
{
    int step = step_tab[s->idx];
    int diff = x - s->pred;
    uint8_t code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    if (diff >= step)      { code |= 4; diff -= step; }
    if (diff >= step >> 1) { code |= 2; diff -= step >> 1; }
    if (diff >= step >> 2) { code |= 1; }
    apply(s, code);
    return code;
}

void adpcm_encode(adpcm_state_t *s, const int16_t *in, int n, uint8_t *out)
{
    for (int i = 0; i < n; i += 2) {
        uint8_t lo = enc1(s, in[i]);
        uint8_t hi = enc1(s, in[i + 1]);
        out[i / 2] = lo | (hi << 4);
    }
}

void adpcm_decode(adpcm_state_t *s, const uint8_t *in, int n, int16_t *out)
{
    for (int i = 0; i < n; i += 2) {
        apply(s, in[i / 2] & 0x0F); out[i]     = (int16_t)s->pred;
        apply(s, in[i / 2] >> 4);   out[i + 1] = (int16_t)s->pred;
    }
}
