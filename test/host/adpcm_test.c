#include <stdio.h>
#include <math.h>
#include "adpcm.h"

#define N 320

int main(void)
{
    double sig = 0, err = 0, t = 0;
    adpcm_state_t enc = { 0, 0 };
    for (int b = 0; b < 50; b++) {
        int16_t in[N], out[N];
        uint8_t pk[N / 2];
        for (int i = 0; i < N; i++, t++)
            in[i] = (int16_t)(8000 * sin(2 * M_PI * 440 * t / 16000) + 3000 * sin(2 * M_PI * 1700 * t / 16000));
        adpcm_state_t hdr = enc;                 /* state before the block goes into the packet header */
        adpcm_encode(&enc, in, N, pk);
        adpcm_state_t dec = hdr;                 /* the decoder resumes from the header alone */
        adpcm_decode(&dec, pk, N, out);
        if (dec.pred != enc.pred || dec.idx != enc.idx) { printf("FAIL: state mismatch in block %d\n", b); return 1; }
        if (b >= 2 && b % 5 == 3) continue;      /* lost packet: never seen, next header resyncs */
        if (b >= 2) for (int i = 0; i < N; i++) { double e = in[i] - out[i]; sig += (double)in[i] * in[i]; err += e * e; }
    }
    double snr = 10 * log10(sig / err);
    printf("adpcm: SNR %.1f dB with 20%% packet loss\n", snr);
    if (snr < 25.0) { puts("FAIL: SNR below 25 dB"); return 1; }
    return 0;
}
