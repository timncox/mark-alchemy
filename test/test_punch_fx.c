/* punch_fx native test: a drum loop through every effect in turn.
 *   cc -std=c99 -O1 -g -fsanitize=address,undefined -I../src test_punch_fx.c ../src/punch_fx.c -lm
 *   ./a.out [out.wav]   -> checks, and a WAV to listen to */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "punch_fx.h"

#define SR    48000
#define BLOCK 128
#define RING  (2 * SR)
#define ECHO  (SR)

static float ring[2 * RING], echo[2 * ECHO];

/* 120 BPM: kick on beats, hat on 8ths, a saw bass note per bar. */
static void source(long n, float *l, float *r)
{
    const long beat = SR / 2, e8 = beat / 2;
    const long tb = n % beat, t8 = n % e8;
    float k = 0.f, h = 0.f;
    if (tb < 6000) k = sinf(2.f * 3.14159f * (50.f + 120.f * expf(-tb / 800.f)) * tb / SR) * expf(-tb / 3000.f);
    if (t8 < 1500) h = ((float)(rand() % 65536) / 32768.f - 1.f) * expf(-t8 / 300.f) * 0.3f;
    const float f = (n / (4 * beat)) % 2 ? 55.f : 73.4f;
    const float b = (fmodf(n * f / SR, 1.f) * 2.f - 1.f) * 0.15f;
    *l = 0.6f * k + h + b;
    *r = 0.6f * k + 0.7f * h + b;
}

static void put16(FILE *f, int v) { fputc(v & 255, f); fputc(v >> 8 & 255, f); }
static void put32(FILE *f, long v) { put16(f, v & 0xFFFF); put16(f, v >> 16 & 0xFFFF); }

int main(int argc, char **argv)
{
    pfx_t p;
    pfx_init(&p, ring, RING, echo, ECHO, SR);
    pfx_set_beat(&p, SR / 2.f);
    /* each effect: 2 beats dry, 4 beats held */
    const long seg = 6 * (SR / 2), total = seg * PFX_COUNT + 4 * SR;
    float *out = malloc(sizeof(float) * 2 * total);
    float L[BLOCK], R[BLOCK], dl[BLOCK], dr[BLOCK];
    int fails = 0;
    float peak = 0.f;
    for (long n = 0; n < total; n += BLOCK)
    {
        const long s = n / seg, in_seg = n % seg;
        int fx = (s < PFX_COUNT && in_seg >= 2 * (SR / 2)) ? (int)s : -1;
        pfx_hold(&p, fx);
        for (int i = 0; i < BLOCK; i++) { source(n + i, &L[i], &R[i]); dl[i] = L[i]; dr[i] = R[i]; }
        pfx_process(&p, L, R, BLOCK);
        for (int i = 0; i < BLOCK; i++)
        {
            if (!isfinite(L[i]) || !isfinite(R[i])) { if (fails++ < 5) printf("NaN at %ld\n", n + i); }
            if (fabsf(L[i]) > peak) peak = fabsf(L[i]);
            /* well after a release, with the echo tail done, output == input */
            if (fx < 0 && in_seg > SR / 2 && s != PFX_ECHO + 1 && s < PFX_COUNT && s > 0
                && (L[i] != dl[i] || R[i] != dr[i]))
                if (fails++ < 5) printf("not dry at %ld (seg %ld): %f vs %f\n", n + i, s, L[i], dl[i]);
            out[2 * (n + i)] = L[i];
            out[2 * (n + i) + 1] = R[i];
        }
    }
    printf("peak %.3f, %s\n", peak, fails ? "FAIL" : "ok");
    if (peak > 4.f) { printf("FAIL: peak runaway\n"); fails++; }
    const char *path = argc > 1 ? argv[1] : "punch_fx.wav";
    FILE *f = fopen(path, "wb");
    long bytes = total * 2 * 2;
    fwrite("RIFF", 1, 4, f); put32(f, 36 + bytes); fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16); put16(f, 1); put16(f, 2); put32(f, SR); put32(f, SR * 4); put16(f, 4); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, bytes);
    for (long i = 0; i < 2 * total; i++) { float v = out[i] * 0.8f; if (v > 1) v = 1; if (v < -1) v = -1; put16(f, (int)lrintf(v * 32767)); }
    fclose(f);
    printf("wrote %s: %d effects, 2 beats dry + 4 beats held each\n", path, PFX_COUNT);
    for (int i = 0; i < PFX_COUNT; i++) printf("  %5.1fs %s\n", (double)(i * seg + SR) / SR, pfx_names[i]);
    free(out);
    return fails ? 1 : 0;
}
