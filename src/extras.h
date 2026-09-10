/*
 * The one value no control on the panel can express, carried in the preset
 * blob so it survives a power cycle:
 *
 *   cpu_peak  The worst-case audio-block load seen during the PREVIOUS
 *             session, replayed on a ring at boot. The live alarm on B2 can
 *             only be read by someone watching it, and you are not, with
 *             five loops running and both hands on the pots.
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

struct MarkExtras : public alchemy::Serializable
{
    float cpu_peak = 0.0f;   /* 0..1; 0 == no data yet */

    size_t SerializedSize() const override { return 4u; }

    void Serialize(uint8_t* out) const override
    {
        std::memcpy(out, &cpu_peak, 4u);
    }

    bool Deserialize(const uint8_t* in) override
    {
        float p;
        std::memcpy(&p, in, 4u);
        /* Refuse values a stale or foreign blob could carry; the schema hash
         * catches layout changes, this catches garbage inside a valid one. */
        if (!(p >= 0.0f && p <= 1.0f)) p = 0.0f;
        cpu_peak = p;
        return true;
    }

    /* 'MRK' + layout version. Bump the low byte when the layout changes. */
    uint32_t SchemaHash() const override { return 0x4D524B00u | 0x01u; }
};
