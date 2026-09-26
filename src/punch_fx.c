/* punch_fx -- see punch_fx.h. */
#include "punch_fx.h"

#include <math.h>
#include <string.h>

const char *const pfx_names[PFX_COUNT] = {
    "Stutter 1/2", "Stutter 1/4", "Stutter 1/8", "Stutter 1/16", "Buzz",
    "Reverse",     "Tape Stop",   "Half Speed",  "Low-pass",     "High-pass",
    "Crush",       "Gate",        "Echo",        "Drive",
};

#define XFADE      64u   /* frames: the seam inside a looping effect      */
#define FADE_MS    1.5f  /* switching an effect in or out                 */
#define ECHO_FB    0.45f
#define ECHO_MIX   0.6f

static const float kPi = 3.14159265358979f;

/* ---------------------------------------------------------------- helpers */

static uint32_t wrap(const pfx_t *p, int64_t i)
{
    const int64_t n = (int64_t)p->ring_frames;
    i %= n;
    if (i < 0) i += n;
    return (uint32_t)i;
}

/* The ring at press_w + off (frames, may be negative). */
static void rd(const pfx_t *p, int64_t off, float *l, float *r)
{
    const uint32_t i = wrap(p, (int64_t)p->press_w + off);
    *l = p->ring[2u * i];
    *r = p->ring[2u * i + 1u];
}

/* Linear interpolation at a fractional offset from press_w. */
static void rdf(const pfx_t *p, float off, float *l, float *r)
{
    const float   f  = floorf(off);
    const float   a  = off - f;
    float         l0, r0, l1, r1;
    const int64_t i  = (int64_t)f;
    rd(p, i, &l0, &r0);
    rd(p, i + 1, &l1, &r1);
    *l = l0 + (l1 - l0) * a;
    *r = r0 + (r1 - r0) * a;
}

static uint32_t clampu(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Loop `len` frames from the press: the first pass is the live signal (the
 * read position IS the write position), then it repeats, with a short
 * crossfade from where the audio would have gone on into the loop start. */
static void loop_fwd(const pfx_t *p, uint32_t len, float *l, float *r)
{
    const uint32_t ph = p->t % len;
    rd(p, (int64_t)ph, l, r);
    if (p->t >= len && ph < XFADE)
    {
        float cl, cr;
        rd(p, (int64_t)len + ph, &cl, &cr); /* the continuation of the last pass */
        const float a = (float)ph / (float)XFADE;
        *l = cl + (*l - cl) * a;
        *r = cr + (*r - cr) * a;
    }
}

/* Soft clip, |x| <= 3 -> odd rational tanh fit. */
static float soft(float x)
{
    if (x > 3.0f) x = 3.0f;
    if (x < -3.0f) x = -3.0f;
    return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}

/* Beat fractions, latched at the press. */
static uint32_t beat_len(const pfx_t *p, float frac)
{
    const uint32_t hi = p->ring_frames / 2u;
    return clampu((uint32_t)(p->beat * frac), 2u * XFADE, hi);
}

/* ---------------------------------------------------------------- API */

void pfx_init(pfx_t *p, float *ring, uint32_t ring_frames, float *echo, uint32_t echo_frames,
              float sample_rate)
{
    memset(p, 0, sizeof *p);
    p->ring        = ring;
    p->ring_frames = ring_frames;
    p->echo        = echo;
    p->echo_frames = echo_frames;
    p->sr          = sample_rate;
    p->beat        = sample_rate * 0.5f; /* 120 BPM */
    p->want        = -1;
    p->cur         = -1;
    p->g_step      = 1.0f / (FADE_MS * 0.001f * sample_rate);
    memset(ring, 0, sizeof(float) * 2u * ring_frames);
    if (echo) memset(echo, 0, sizeof(float) * 2u * echo_frames);
}

void pfx_set_beat(pfx_t *p, float frames_per_beat)
{
    /* 40..300 BPM */
    const float lo = p->sr * 60.0f / 300.0f, hi = p->sr * 60.0f / 40.0f;
    if (frames_per_beat < lo) frames_per_beat = lo;
    if (frames_per_beat > hi) frames_per_beat = hi;
    p->beat = frames_per_beat;
}

void pfx_hold(pfx_t *p, int fx)
{
    p->want = (fx >= 0 && fx < PFX_COUNT) ? fx : -1;
}

int pfx_active(const pfx_t *p) { return p->g > 0.0f ? p->cur : -1; }

static void start(pfx_t *p, int fx)
{
    p->cur        = fx;
    p->t          = 0;
    p->press_w    = p->w;
    p->rpos       = 0.0f;
    p->speed      = 1.0f;
    p->s1[0] = p->s1[1] = p->s2[0] = p->s2[1] = 0.0f;
    p->cutoff     = fx == PFX_HIGH_PASS ? 30.0f : 16000.0f;
    p->hold[0]    = p->hold[1] = 0.0f;
    p->hold_phase = 1.0f;
}

/* One frame of the current effect; dl/dr is the dry input. */
static void render(pfx_t *p, float dl, float dr, float *l, float *r, float *svf_k, float *svf_g)
{
    const uint32_t t = p->t;
    switch (p->cur)
    {
        case PFX_STUTTER_2:  loop_fwd(p, beat_len(p, 0.5f), l, r); return;
        case PFX_STUTTER_4:  loop_fwd(p, beat_len(p, 0.25f), l, r); return;
        case PFX_STUTTER_8:  loop_fwd(p, beat_len(p, 0.125f), l, r); return;
        case PFX_STUTTER_16: loop_fwd(p, beat_len(p, 0.0625f), l, r); return;
        case PFX_BUZZ:       loop_fwd(p, beat_len(p, 1.0f / 64.0f), l, r); return;

        case PFX_REVERSE:
        {
            uint32_t w = (uint32_t)(p->beat * 2.0f);
            w = clampu(w, 4u * XFADE, p->ring_frames * 3u / 4u);
            const uint32_t ph = t % w;
            rd(p, -1 - (int64_t)ph, l, r);
            if (t >= w && ph < XFADE)
            {
                float cl, cr;
                rd(p, -1 - (int64_t)w - (int64_t)ph, &cl, &cr);
                const float a = (float)ph / (float)XFADE;
                *l = cl + (*l - cl) * a;
                *r = cr + (*r - cr) * a;
            }
            return;
        }

        case PFX_TAPE_STOP:
        {
            if (p->speed <= 0.0f) { *l = *r = 0.0f; return; }
            rdf(p, p->rpos, l, r);
            p->rpos += p->speed;
            p->speed -= 1.0f / p->beat;
            return;
        }

        case PFX_HALF_SPEED:
        {
            const uint32_t seg = clampu((uint32_t)(p->beat * 2.0f), 4u * XFADE,
                                        p->ring_frames / 2u);
            const uint32_t k = t / seg, ph = t % seg;
            rdf(p, (float)k * (float)seg + (float)ph * 0.5f, l, r);
            if (k > 0 && ph < XFADE)
            {
                float cl, cr;
                rdf(p, (float)(k - 1u) * (float)seg + (float)(ph + seg) * 0.5f, &cl, &cr);
                const float a = (float)ph / (float)XFADE;
                *l = cl + (*l - cl) * a;
                *r = cr + (*r - cr) * a;
            }
            return;
        }

        case PFX_LOW_PASS:
        case PFX_HIGH_PASS:
        {
            /* Simper's trapezoidal SVF; the coefficient every 16 frames. */
            if ((t & 15u) == 0u)
            {
                const float span = 2.0f * p->beat;
                const float x    = t < span ? (float)t / span : 1.0f;
                p->cutoff = p->cur == PFX_LOW_PASS ? 16000.0f * powf(250.0f / 16000.0f, x)
                                                   : 30.0f * powf(3000.0f / 30.0f, x);
                *svf_g = tanf(kPi * p->cutoff / p->sr);
            }
            const float g = *svf_g, k = *svf_k;
            const float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
            const float in[2] = {dl, dr};
            float       out[2];
            for (int c = 0; c < 2; c++)
            {
                const float v3 = in[c] - p->s2[c];
                const float v1 = a1 * p->s1[c] + a2 * v3;
                const float v2 = p->s2[c] + a2 * p->s1[c] + a3 * v3;
                p->s1[c] = 2.0f * v1 - p->s1[c];
                p->s2[c] = 2.0f * v2 - p->s2[c];
                out[c] = p->cur == PFX_LOW_PASS ? v2 : in[c] - k * v1 - v2;
            }
            *l = out[0];
            *r = out[1];
            return;
        }

        case PFX_CRUSH:
        {
            p->hold_phase += 8000.0f / p->sr;
            if (p->hold_phase >= 1.0f)
            {
                p->hold_phase -= 1.0f;
                p->hold[0] = floorf(dl * 16.0f + 0.5f) * (1.0f / 16.0f);
                p->hold[1] = floorf(dr * 16.0f + 0.5f) * (1.0f / 16.0f);
            }
            *l = p->hold[0];
            *r = p->hold[1];
            return;
        }

        case PFX_GATE:
        {
            const uint32_t period = clampu((uint32_t)(p->beat * 0.25f), 2u * XFADE, 0xFFFFFFu);
            const float    on     = (t % period) < period / 2u ? 1.0f : 0.0f;
            /* one-pole smoothing, ~1 ms */
            p->hold[0] += (on - p->hold[0]) * (1.0f / (0.001f * p->sr));
            *l = dl * p->hold[0];
            *r = dr * p->hold[0];
            return;
        }

        case PFX_DRIVE:
            *l = soft(dl * 5.0f) * 0.4f;
            *r = soft(dr * 5.0f) * 0.4f;
            return;

        case PFX_ECHO:
        default:
            *l = dl;
            *r = dr;
            return;
    }
}

void pfx_process(pfx_t *p, float *left, float *right, uint32_t frames)
{
    float svf_k = 0.7f; /* resonance: 1/Q */
    float svf_g = tanf(kPi * (p->cutoff > 0.0f ? p->cutoff : 1000.0f) / p->sr);
    const uint32_t edly = p->echo ? clampu((uint32_t)(p->beat * 0.75f), 1u, p->echo_frames - 1u) : 0u;

    for (uint32_t i = 0; i < frames; i++)
    {
        const float dl = left[i], dr = right[i];
        p->ring[2u * p->w]      = dl;
        p->ring[2u * p->w + 1u] = dr;

        /* Fade out whatever plays, switch, fade in. */
        const int want = p->want;
        if (want != p->cur)
        {
            if (p->cur < 0 || p->g <= 0.0f)
            {
                p->g = 0.0f;
                if (want >= 0) start(p, want);
                else p->cur = -1;
            }
            else
                p->g -= p->g_step;
        }
        else if (p->cur >= 0 && p->g < 1.0f)
        {
            p->g += p->g_step;
            if (p->g > 1.0f) p->g = 1.0f;
        }

        float ol = dl, orr = dr;
        if (p->cur >= 0 && p->g > 0.0f)
        {
            float wl, wr;
            render(p, dl, dr, &wl, &wr, &svf_k, &svf_g);
            ol = dl + (wl - dl) * p->g;
            orr = dr + (wr - dr) * p->g;
        }
        if (p->cur >= 0) p->t++;

        /* The echo line: fed while ECHO is held, rings on after. */
        if (p->echo)
        {
            const float feed = p->cur == PFX_ECHO ? p->g : 0.0f;
            if (feed > 0.0f) p->echo_fb_gain = 1.0f;
            if (p->echo_fb_gain > 0.0f)
            {
                const uint32_t ri = (p->ew + p->echo_frames - edly) % p->echo_frames;
                const float    el = p->echo[2u * ri], er = p->echo[2u * ri + 1u];
                /* ping-pong: each side feeds the other */
                p->echo[2u * p->ew]      = dl * feed + er * ECHO_FB;
                p->echo[2u * p->ew + 1u] = dr * feed + el * ECHO_FB;
                ol += el * ECHO_MIX;
                orr += er * ECHO_MIX;
                /* after release, run ~14 repeats (0.45^14 ~ -97 dB), then rest */
                if (feed <= 0.0f)
                {
                    p->echo_fb_gain -= 1.0f / (14.0f * (float)edly);
                    /* What stays in the line is ~-97 dB: never cleared here
                     * (a 1 s memset would overrun the audio block). */
                    if (p->echo_fb_gain <= 0.0f) p->echo_fb_gain = 0.0f;
                }
            }
            if (++p->ew >= p->echo_frames) p->ew = 0;
        }

        left[i]  = ol;
        right[i] = orr;
        if (++p->w >= p->ring_frames) p->w = 0;
    }
}
