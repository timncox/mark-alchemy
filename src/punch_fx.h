/*
 * punch_fx -- hold-to-punch performance effects on a stereo output, for the
 * firmwares whose engine has none of its own (Mark, Belt). Smack punches
 * through its own engine instead.
 *
 * One effect at a time, held: pfx_hold(fx) punches it in, pfx_hold(-1)
 * lets go. The dry signal is always recorded into a ring (a couple of
 * seconds), so the stutters, reverse, tape stop and half speed have
 * something to play; they are tempo-synced to the beat length given with
 * pfx_set_beat. Changes fade over ~1.5 ms, so nothing clicks. The echo
 * keeps ringing out after release (a dub throw).
 *
 * HAL-free C99: one allocation-free state struct and a caller-owned ring
 * (put it in SDRAM on the Lab). Not thread-safe: call pfx_hold and
 * pfx_set_beat from the control thread and pfx_process from the audio
 * callback -- both only write single words the other side reads.
 *
 * Shared by copy with belt-alchemy.
 */
#ifndef PUNCH_FX_H
#define PUNCH_FX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    PFX_STUTTER_2 = 0, /* repeat the first 1/2 beat from the press      */
    PFX_STUTTER_4,     /* 1/4 beat                                      */
    PFX_STUTTER_8,     /* 1/8 beat                                      */
    PFX_STUTTER_16,    /* 1/16 beat                                     */
    PFX_BUZZ,          /* 1/64 beat: a frozen buzz                      */
    PFX_REVERSE,       /* the last two beats, backwards, looping        */
    PFX_TAPE_STOP,     /* slows to a stop over one beat, then silence   */
    PFX_HALF_SPEED,    /* an octave down, re-anchored every two beats   */
    PFX_LOW_PASS,      /* low-pass sweeping down over two beats         */
    PFX_HIGH_PASS,     /* high-pass sweeping up over two beats          */
    PFX_CRUSH,         /* sample-rate and bit reduction                 */
    PFX_GATE,          /* 1/16 chop                                     */
    PFX_ECHO,          /* dotted-1/8 echo that rings on after release   */
    PFX_DRIVE,         /* waveshaper distortion                         */
    PFX_COUNT
} pfx_fx_t;

/* Short names for a display, index == pfx_fx_t. */
extern const char *const pfx_names[PFX_COUNT];

typedef struct
{
    /* the ring: interleaved stereo, caller-owned */
    float   *ring;
    uint32_t ring_frames;
    uint32_t w;              /* write position (frames, wraps)          */
    /* the echo line: interleaved stereo, caller-owned */
    float   *echo;
    uint32_t echo_frames;
    uint32_t ew;
    float    echo_fb_gain;   /* 0 when idle and silent                  */
    float    sr;
    float    beat;           /* frames per beat                         */

    volatile int want;       /* what the control side asked for, -1 off */
    int      cur;            /* what is playing, -1 = dry               */
    float    g;              /* wet gain, 0..1                          */
    float    g_step;

    uint32_t t;              /* frames since the current effect started */
    uint32_t press_w;        /* ring position at the press              */
    float    rpos;           /* fractional read position (tape/half)    */
    float    speed;
    /* filter state (per channel) */
    float    s1[2], s2[2];
    float    cutoff;         /* current, Hz                             */
    /* crush */
    float    hold[2];
    float    hold_phase;
} pfx_t;

/* ring: 2 * ring_frames floats; echo: 2 * echo_frames floats. Both are
 * cleared here. Two seconds of ring and one of echo cover 60 BPM. */
void pfx_init(pfx_t *p, float *ring, uint32_t ring_frames, float *echo, uint32_t echo_frames,
              float sample_rate);
void pfx_set_beat(pfx_t *p, float frames_per_beat);
void pfx_hold(pfx_t *p, int fx); /* pfx_fx_t, or -1 to let go */
int  pfx_active(const pfx_t *p); /* the effect sounding, -1 = none */

/* In place, non-interleaved stereo. */
void pfx_process(pfx_t *p, float *left, float *right, uint32_t frames);

#ifdef __cplusplus
}
#endif

#endif
