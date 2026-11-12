#pragma once
#include <stdint.h>

/* IMA ADPCM, 4 bit/sample. Encoder state before the block travels in the
 * packet header, so every packet decodes on its own (loss tolerant). */
typedef struct { int pred; int idx; } adpcm_state_t;

void adpcm_encode(adpcm_state_t *s, const int16_t *in, int n, uint8_t *out);  /* n even, out: n/2 bytes */
void adpcm_decode(adpcm_state_t *s, const uint8_t *in, int n, int16_t *out);
