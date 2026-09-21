/*
 * MarkExtras: the preset blob's app-owned part (src/extras.h). Native, no
 * hardware. What is proven here:
 *
 *   - a fresh struct carries the first-boot TRACK page for all five tracks
 *   - Serialize -> Deserialize is the identity on every in-range value
 *   - a track with one out-of-range byte comes back as defaults, whole, and
 *     its neighbours are untouched
 *   - a garbage cpu_peak is refused
 *   - Deserialize counts itself (the app's only "a preset loaded" signal)
 *   - the schema tag moved off 0x01, so a v0.1.0 slot 0 fails the gate
 */
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include "extras.h"

static int g_fail = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            g_fail++;                                                        \
        }                                                                    \
    } while (0)

int main()
{
    /* first boot */
    {
        MarkExtras e;
        CHECK(e.SerializedSize() == 4u + kExtrasTracks * kNumTrackParams);
        for (uint8_t t = 0; t < kExtrasTracks; t++)
        {
            CHECK(e.trk[t][kTpShot] == 0);
            CHECK(e.trk[t][kTpRev] == 0);
            CHECK(e.trk[t][kTpFx] == 0);
            CHECK(e.trk[t][kTpFxAmt] == 50);   /* the engine's own default fxp */
            CHECK(e.trk[t][kTpPan] == 50);     /* centre */
        }
        CHECK(e.loads == 0);
    }

    /* round trip, every value at its extreme and somewhere inside */
    {
        MarkExtras a;
        a.cpu_peak = 0.4375f;
        for (uint8_t t = 0; t < kExtrasTracks; t++)
            for (uint8_t w = 0; w < kNumTrackParams; w++)
                a.trk[t][w] = (uint8_t)((t * 7u + w * 13u) % (kTrackParamMax[w] + 1u));
        a.trk[4][kTpFx]    = kTrackParamMax[kTpFx];
        a.trk[4][kTpFxAmt] = kTrackParamMax[kTpFxAmt];
        a.trk[0][kTpPan]   = 0;

        uint8_t blob[64];
        CHECK(a.SerializedSize() <= sizeof blob);
        a.Serialize(blob);

        MarkExtras b;
        CHECK(b.Deserialize(blob));
        CHECK(b.cpu_peak == a.cpu_peak);
        CHECK(std::memcmp(a.trk, b.trk, sizeof a.trk) == 0);
        CHECK(b.loads == 1);
        CHECK(b.Deserialize(blob));
        CHECK(b.loads == 2);
    }

    /* one bad byte resets that track only */
    {
        MarkExtras a;
        a.trk[1][kTpPan] = 10;
        a.trk[2][kTpPan] = 20;
        a.trk[2][kTpFx]  = 3;
        a.trk[3][kTpPan] = 30;

        uint8_t blob[64];
        a.Serialize(blob);
        blob[4u + 2u * kNumTrackParams + kTpFx] = 7;   /* tfx_t has no 7 */

        MarkExtras b;
        CHECK(b.Deserialize(blob));
        CHECK(b.trk[1][kTpPan] == 10);
        CHECK(b.trk[3][kTpPan] == 30);
        CHECK(b.trk[2][kTpFx] == kTrackParamDefault[kTpFx]);
        CHECK(b.trk[2][kTpPan] == kTrackParamDefault[kTpPan]);
    }

    /* garbage peak */
    {
        MarkExtras a;
        uint8_t    blob[64];
        a.Serialize(blob);
        const float bad[3] = {-0.5f, 7.0f, std::numeric_limits<float>::quiet_NaN()};
        for (float f : bad)
        {
            std::memcpy(blob, &f, 4u);
            MarkExtras b;
            b.cpu_peak = 0.9f;
            CHECK(b.Deserialize(blob));
            CHECK(b.cpu_peak == 0.0f);
        }
    }

    /* the layout changed, so the tag must have */
    {
        MarkExtras e;
        CHECK((e.SchemaHash() & 0xFFFFFF00u) == 0x4D524B00u);
        CHECK((e.SchemaHash() & 0xFFu) != 0x01u);
    }

    if (g_fail) { std::printf("test_extras: %d FAILED\n", g_fail); return 1; }
    std::printf("test_extras: ok\n");
    return 0;
}
