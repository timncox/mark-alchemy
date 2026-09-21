/*
 * What no pot position can express, carried in the preset blob so it
 * survives a power cycle:
 *
 *   cpu_peak  The worst-case audio-block load seen during the PREVIOUS
 *             session, replayed on a ring at boot. The live alarm on B2 can
 *             only be read by someone watching it, and you are not, with
 *             five loops running and both hands on the pots.
 *
 *   trk       The TRACK page, for all five tracks. The Pager stores one value
 *             per (page, pot), and the TRACK page shows whichever track is
 *             selected, so the Pager can only ever hold one track's worth.
 *             These are the other four, and the record the page is reloaded
 *             from when the selection moves (mark_alchemy.cpp, sync_tracks).
 *
 * The free-run tempo needs no slot of its own here: it is the TEMPO pot on
 * the Setup page, which the Pager already stores.
 *
 * Every Alchemy firmware in this family (Smack, Mark, Belt) has its own tag
 * here, so a slot 0 written by one of them fails the schema gate in the next
 * and lands in that firmware's first-boot defaults rather than being read as
 * its own settings.
 */
#pragma once

#include <cstdint>
#include <cstring>
#include "alchemy/surface/serializable.h"

/* The TRACK page's parameters, in pot order (P1..P5; P6 is TRACK select). */
enum : uint8_t
{
    kTpShot = 0,   /* 0 loop, 1 one-shot                                  */
    kTpRev,        /* 0 forward, 1 reverse                                */
    kTpFx,         /* 0 off, 1..6 = the engine's tfx_t code, switched on  */
    kTpFxAmt,      /* 0..100, the engine's fxp                            */
    kTpPan,        /* 0..100, 50 = centre                                 */
    kNumTrackParams
};

static constexpr uint8_t kExtrasTracks = 5u;   /* == MARK_TRACKS, asserted in mark_alchemy.cpp */

static constexpr uint8_t kTrackParamMax[kNumTrackParams]     = {1u, 1u, 6u, 100u, 100u};
static constexpr uint8_t kTrackParamDefault[kNumTrackParams] = {0u, 0u, 0u, 50u, 50u};

struct MarkExtras : public alchemy::Serializable
{
    float   cpu_peak = 0.0f;   /* 0..1; 0 == no data yet */
    uint8_t trk[kExtrasTracks][kNumTrackParams];

    /* Not serialized: counts Deserialize() calls. The SDK has no "a preset
     * was loaded" hook, and a load replaces trk[] and the Pager's TRACK page
     * behind the app's back; the app watches this to push the new values to
     * the engine and reload the page. */
    uint32_t loads = 0;

    MarkExtras() { ResetTracks(); }

    void ResetTracks()
    {
        for (uint8_t t = 0; t < kExtrasTracks; t++)
            std::memcpy(trk[t], kTrackParamDefault, kNumTrackParams);
    }

    size_t SerializedSize() const override { return 4u + sizeof trk; }

    void Serialize(uint8_t* out) const override
    {
        std::memcpy(out, &cpu_peak, 4u);
        std::memcpy(out + 4u, trk, sizeof trk);
    }

    bool Deserialize(const uint8_t* in) override
    {
        float p;
        std::memcpy(&p, in, 4u);
        /* Refuse values a stale or foreign blob could carry; the schema hash
         * catches layout changes, this catches garbage inside a valid one. */
        if (!(p >= 0.0f && p <= 1.0f)) p = 0.0f;
        cpu_peak = p;

        /* A track with any value out of range goes back to its defaults
         * whole, rather than playing half of somebody else's settings. */
        std::memcpy(trk, in + 4u, sizeof trk);
        for (uint8_t t = 0; t < kExtrasTracks; t++)
            for (uint8_t w = 0; w < kNumTrackParams; w++)
                if (trk[t][w] > kTrackParamMax[w])
                {
                    std::memcpy(trk[t], kTrackParamDefault, kNumTrackParams);
                    break;
                }
        loads++;
        return true;
    }

    /* 'MRK' + layout version. Bump the low byte when the layout changes.
     * 0x02: the TRACK page's per-track values. */
    uint32_t SchemaHash() const override { return 0x4D524B00u | 0x02u; }
};
