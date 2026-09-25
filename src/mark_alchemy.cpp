/*
 * Mark on the Hermetic Modular Alchemy Lab V2 -- the RC-505-style five-track
 * live looper as Alchemy SDK firmware.
 *
 * The DSP is mark_core, vendored from schwung-mark (see vendor/mark_core.h
 * for the two edits: 48 kHz, and a build-time track capacity). This file is
 * the host shim: six pots on two pages, three buttons, six CV jacks and 102
 * LEDs to the engine's string parameter API, and the clock jack turned into
 * the 24 ppqn MIDI clock the engine already speaks (clock_adapter.c, from
 * the Smack ports, unchanged).
 *
 * Shared by copy with smack-alchemy and belt-alchemy: the shape of this
 * file, the picker, the allocator, the autosave. See DESIGN.md.
 *
 *   PLAY   LEVEL 1-5 (each ring is that track's RC-505 display) · TRACK select
 *   TRACK  the selected track's ONE-SHOT · REVERSE · FX · FX AMOUNT · PAN,
 *          and TRACK select again on the same pot                  (tap B3)
 *   SETUP  QUANTIZE · REC-> · DUB · PLAY MODE · TEMPO · MASTER   (hold B3)
 *   B1     the selected track's REC/PLAY button: tap = record -> play ->
 *          overdub, hold = undo/redo, hold 2 s = clear
 *   B2     tap = stop the track, hold = ALL start/stop
 *   J3     clock; J4 record trigger; J5 stop trigger; J6-J8 CV
 *   Settings (B2+B3 2 s): dry input, clock ratio; page 1 = the SD picker
 *
 * The Move's sessions are not on this panel; the engine still has them.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "daisy_seed.h"
#include "hid/usb.h"
#include "util/CpuLoadMeter.h"

#include "alchemy/hw/alchemy_lab.h"
#include "alchemy/hw/v2_calibration.h"
#include "alchemy/led/anims/catch_pip.h"
#include "alchemy/host_link/cdc_transport.h"
#include "alchemy/host_link/host.h"
#include "alchemy/storage/fs_extension.h"
#include "alchemy/storage/sd_card.h"
#include "alchemy/surface/control_loop.h"
#include "alchemy/surface/cv_matrix.h"
#include "alchemy/surface/jack.h"
#include "alchemy/surface/manual.h"
#include "alchemy/surface/page.h"
#include "alchemy/surface/pager.h"
#include "alchemy/surface/presets.h"
#include "alchemy/surface/settings.h"
#include "alchemy/surface/virtual_button.h"
#include "alchemy/surface/virtual_knob.h"

#include "clock_adapter.h"
#include "extras.h"
#include "picker.h"
#include "launchpad.h"
#include "ff.h"
#include "versio_alloc.h"

/* versio_alloc.h first, then the engine inside extern "C" -- see the note in
 * versio_alloc.h about include order and linkage. */
extern "C" {
#include "vendor/mark_core.h"
}

#ifndef MARK_VERSION
#define MARK_VERSION "0.0.0"
#endif
#ifndef MARK_GIT_HASH
#define MARK_GIT_HASH "dev"
#endif

using namespace alchemy;
using daisy::System;

/* ---- geometry ----------------------------------------------------------- */

/* The engine's block (MARK_BLOCK_FRAMES); mark_process clamps to it. */
static constexpr uint32_t kBlockSize = 128u;
static_assert(kBlockSize == 128u, "mark_core's MARK_BLOCK_FRAMES is 128");

/* CV indices: J3 clock, J4 record trigger, J5 stop trigger. */
static constexpr uint8_t kClockJack = 0u;
static constexpr uint8_t kRecJack   = 1u;
static constexpr uint8_t kStopJack  = 2u;

enum : uint8_t { kPagePlay = 0, kPageSetup = 1, kPageTrack = 2, kNumAppPages = 3 };
enum : uint8_t { kSettingsMain = 0, kSettingsFirmware = 1 };

/* ---- hardware + engine -------------------------------------------------- */

static AlchemyLab          hw;
static daisy::CpuLoadMeter cpu;

/* Sized in versio_alloc.h so this and test/test_alloc.c cannot drift. Five
 * tracks and the undo buffer at MARK_MAX_SECONDS each; the Seed2 DFM has
 * 64 MB of SDRAM. */
static uint8_t DSY_SDRAM_BSS g_pool[VERSIO_POOL_BYTES];

static mark_t*         M = nullptr;
static clock_adapter_t CLK;
static host_api_v1_t   HOST;

/* Engine readbacks published by the control loop for the LEDs. Plain
 * aligned words: a torn read is impossible, a one-frame-stale value is
 * invisible. */
static int   G_TSTATE[MARK_TRACKS];   /* mk_tstate_t per track */
static int   G_TPEND[MARK_TRACKS];    /* 1 rec pending, 2 play pending */
static int   G_TPOS[MARK_TRACKS];     /* 0..127 through the loop */
static int   G_UNDO = 0;              /* 0 none, 1 undo, 2 redo available */
static int   g_track = 0;             /* the track B1/B2/J4/J5 act on */

/* Trigger jacks, set in the callback, consumed at the 1 ms poll. */
static volatile bool G_TRIG_REC  = false;
static volatile bool G_TRIG_STOP = false;

/* ---- colours (the RC-505's track colours) --------------------------------- */

static constexpr LedPanel::Rgb kColRec    = {0xFF, 0x00, 0x00};
static constexpr LedPanel::Rgb kColPlay   = {0x00, 0xFF, 0x00};
static constexpr LedPanel::Rgb kColDub    = {0xFF, 0xC0, 0x00};
static constexpr LedPanel::Rgb kColStop   = {0xC0, 0xC0, 0xC0};
static constexpr LedPanel::Rgb kColEmpty  = {0x20, 0x20, 0x28};
static constexpr LedPanel::Rgb kColTrack  = {0x40, 0xC0, 0xFF};
static constexpr LedPanel::Rgb kColQuant  = {0x40, 0xFF, 0xC0};
static constexpr LedPanel::Rgb kColRecAct = {0xFF, 0x80, 0x40};
static constexpr LedPanel::Rgb kColDubMd  = {0xFF, 0xC0, 0x40};
static constexpr LedPanel::Rgb kColPlayMd = {0x80, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kColTempo  = {0xFF, 0xC0, 0x40};
static constexpr LedPanel::Rgb kColMaster = {0xFF, 0x40, 0xA0};
static constexpr LedPanel::Rgb kColDim    = {0x18, 0x18, 0x18};
static constexpr LedPanel::Rgb kColSetup  = {0x30, 0x10, 0x40};
static constexpr LedPanel::Rgb kColShot   = {0xFF, 0xFF, 0x60};
static constexpr LedPanel::Rgb kColRev    = {0xFF, 0x60, 0x60};
static constexpr LedPanel::Rgb kColFx     = {0xA0, 0x80, 0xFF};
static constexpr LedPanel::Rgb kColFxAmt  = {0x80, 0x60, 0xFF};
static constexpr LedPanel::Rgb kColPanR   = {0x40, 0xE0, 0xFF};
static constexpr LedPanel::Rgb kColPanL   = {0xFF, 0xA0, 0x40};
static constexpr LedPanel::Rgb kColTrkPg  = {0x50, 0x28, 0x08};   /* B3 on the TRACK page */

static constexpr LedPanel::Rgb kIdle   = {0x00, 0x00, 0x30};
static constexpr LedPanel::Rgb kAmber  = {0xFF, 0x80, 0x00};
static constexpr LedPanel::Rgb kRed    = {0xFF, 0x00, 0x00};
static constexpr LedPanel::Rgb kGreen  = {0x00, 0xFF, 0x00};
static constexpr LedPanel::Rgb kWhite  = {0xFF, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kGrey   = {0x60, 0x60, 0x60};
static constexpr LedPanel::Rgb kBlue   = {0x00, 0x50, 0xFF};

/* ---- labels (descriptor metadata; static storage, borrowed by pointer) -- */

static const char* const kTrackLabels[5]  = {"1", "2", "3", "4", "5"};
static const char* const kQuantLabels[5]  = {"Off", "Bar", "Beat", "8th", "16th"};
static const char* const kRecActLabels[2] = {"Play", "Overdub"};
static const char* const kDubLabels[2]    = {"Overdub", "Replace"};
static const char* const kPlayLabels[2]   = {"Multi", "Single"};
static const char* const kDryLabels[2]    = {"Through", "Muted"};
static const char* const kRatioLabels[3]  = {"/2", "=1", "x2"};
static const char* const kUsbLabels[2]    = {"Mac", "Launchpad"};
static const char* const kShotLabels[2]   = {"Loop", "One-shot"};
static const char* const kRevLabels[2]    = {"Forward", "Reverse"};
static const char* const kFxLabels[7]     = {"Off", "Low-pass", "High-pass", "Crush",
                                             "Delay", "Phaser", "Ring mod"};

/* ---- rings ---------------------------------------------------------------- */

/*
 * Each LEVEL ring is its track's RC-505 display: the level as an arc in the
 * track's state colour (red recording, green playing, yellow overdubbing,
 * white stopped, near-dark empty), blinking while an action waits for the
 * bar line, with a white playhead pip once around per loop pass. A custom
 * ring rather than the SDK's Level style because the colour has to follow
 * the state. The pot's catch pip is lost on these five; the PLAY page
 * adopts the physical pots at boot so it rarely matters.
 */
extern Pager pager;   /* defined with the surfaces below; read by TrackRing */

static void TrackRing(LedPanel& panel, uint8_t pot, const ArcGeometry& geo,
                      float norm, uint32_t t_ms, void* ctx)
{
    const int  ti   = (int)(uintptr_t)ctx;
    const int  st   = G_TSTATE[ti];
    const bool pend = G_TPEND[ti] != 0;

    LedPanel::Rgb c;
    switch (st)
    {
        case MK_REC:  c = kColRec;   break;
        case MK_PLAY: c = kColPlay;  break;
        case MK_DUB:  c = kColDub;   break;
        case MK_STOP: c = kColStop;  break;
        default:      c = kColEmpty; break;
    }
    if (pend && ((t_ms / 150u) & 1u)) c = kColDim;

    if (norm < 0.0f) norm = 0.0f;
    if (norm > 1.0f) norm = 1.0f;
    int n = (int)lroundf(norm * (float)geo.arc_leds);
    if (n < 1) n = 1;   /* a level of zero still shows the state */
    for (int i = 0; i < n; i++)
        panel.SetRingByHour(pot, fmodf(geo.start_hour + geo.step_hours * (float)i, 12.0f),
                            panel.ScaleGlobal(c));

    if (st == MK_PLAY || st == MK_DUB)
    {
        const float p    = (float)G_TPOS[ti] / 127.0f;
        const float hour = geo.start_hour + geo.step_hours * p * (float)(geo.arc_leds - 1);
        panel.SetRingByHour(pot, fmodf(hour, 12.0f), panel.ScaleGlobal(kWhite));
    }

    /* A custom ring gets no catch pip from the SDK, and these five need it
     * most: after a picker switch the pot points wherever the last firmware
     * left it, and the level only moves once the pot has swept past the
     * saved one. Amber, at the saved level, until it catches -- amber so it
     * cannot be taken for the white playhead. */
    DrawCatchPip(panel, pot, pager.State(kPagePlay, pot), geo, kAmber);
}

/*
 * TRACK select on the TRACK page. The five LEVEL rings are out of sight
 * there, so this one ring stands in for them: five dots across the arc (the
 * SDK's Distributed selector geometry), each in its track's state colour,
 * the selected one at full brightness and the rest at a quarter. It reads
 * g_track rather than the pot, so it follows J6 as well.
 */
static void TrackSelectRing(LedPanel& panel, uint8_t pot, const ArcGeometry& geo,
                            float /*norm*/, uint32_t t_ms, void* /*ctx*/)
{
    for (int i = 0; i < MARK_TRACKS; i++)
    {
        LedPanel::Rgb c;
        switch (G_TSTATE[i])
        {
            case MK_REC:  c = kColRec;   break;
            case MK_PLAY: c = kColPlay;  break;
            case MK_DUB:  c = kColDub;   break;
            case MK_STOP: c = kColStop;  break;
            default:      c = kColEmpty; break;
        }
        if (G_TPEND[i] && ((t_ms / 150u) & 1u)) c = kColDim;
        if (i == g_track)
        {
            /* An empty track's near-dark would hide the selection. */
            if (G_TSTATE[i] != MK_REC && G_TSTATE[i] != MK_PLAY
                && G_TSTATE[i] != MK_DUB && G_TSTATE[i] != MK_STOP)
                c = kColTrack;
        }
        else
        {
            c = {(uint8_t)(c.r / 4u), (uint8_t)(c.g / 4u), (uint8_t)(c.b / 4u)};
        }
        const int led = (int)lroundf((float)i * (float)(geo.arc_leds - 1)
                                     / (float)(MARK_TRACKS - 1));
        panel.SetRingByHour(pot, fmodf(geo.start_hour + geo.step_hours * (float)led, 12.0f),
                            panel.ScaleGlobal(c));
    }
}

/* ---- pages --------------------------------------------------------------- */

/* PLAY. Panel positions, front view:
 *      [P1] B1 [P2]        tracks 1 and 2
 *      [P3] B2 [P4]        tracks 3 and 4
 *      [P5] B3 [P6]        track 5, TRACK select                          */

static VirtualKnob lvl1 = VirtualKnob(kPotTopLeft, "Track 1")
    .Linear(0.f, 100.f).Ident("t1_level").Unit("%")
    .Ring(Custom(TrackRing, (void*)(uintptr_t)0));

static VirtualKnob lvl2 = VirtualKnob(kPotTopRight, "Track 2")
    .Linear(0.f, 100.f).Ident("t2_level").Unit("%")
    .Ring(Custom(TrackRing, (void*)(uintptr_t)1));

static VirtualKnob lvl3 = VirtualKnob(kPotMiddleLeft, "Track 3")
    .Linear(0.f, 100.f).Ident("t3_level").Unit("%")
    .Ring(Custom(TrackRing, (void*)(uintptr_t)2));

static VirtualKnob lvl4 = VirtualKnob(kPotMiddleRight, "Track 4")
    .Linear(0.f, 100.f).Ident("t4_level").Unit("%")
    .Ring(Custom(TrackRing, (void*)(uintptr_t)3));

static VirtualKnob lvl5 = VirtualKnob(kPotBottomLeft, "Track 5")
    .Linear(0.f, 100.f).Ident("t5_level").Unit("%")
    .Ring(Custom(TrackRing, (void*)(uintptr_t)4));

static VirtualKnob track = VirtualKnob(kPotBottomRight, "Track")
    .Selector(5).Labels(kTrackLabels).Ident("track")
    .Ring(SelectorRing(kColTrack, kColDim, 5));

/* SETUP: hold B3. The RC's system settings. */

static VirtualKnob quant = VirtualKnob(kPotTopLeft, "Quantize")
    .Selector(5).Labels(kQuantLabels).Ident("quantize")
    .Ring(SelectorRing(kColQuant, kColDim, 5));

static VirtualKnob recact = VirtualKnob(kPotTopRight, "Rec ->")
    .Selector(2).Labels(kRecActLabels).Ident("rec_action")
    .Ring(SelectorRing(kColRecAct, kColDim, 2));

static VirtualKnob dubmode = VirtualKnob(kPotMiddleLeft, "Dub")
    .Selector(2).Labels(kDubLabels).Ident("dub_mode")
    .Ring(SelectorRing(kColDubMd, kColDim, 2));

static VirtualKnob playmode = VirtualKnob(kPotMiddleRight, "Play")
    .Selector(2).Labels(kPlayLabels).Ident("play_mode")
    .Ring(SelectorRing(kColPlayMd, kColDim, 2));

static VirtualKnob tempo = VirtualKnob(kPotBottomLeft, "Tempo")
    .Linear(50.f, 200.f).Ident("tempo").Unit("BPM")
    .Ring(Level(kColTempo));

static VirtualKnob master = VirtualKnob(kPotBottomRight, "Master")
    .Linear(0.f, 200.f).Ident("master").Unit("%")
    .Ring(Level(kColMaster));

/* TRACK: tap B3. What the engine keeps per track and the PLAY page has no
 * room for, shown for the selected track. The Pager holds one value per
 * (page, pot), so these five are reloaded from the extras whenever the
 * selection moves (sync_tracks / load_track_page below). P6 is TRACK select
 * again, on the pot it has on the PLAY page, so picking a track never costs
 * a page change; the two stored values are kept equal in OnFrame. */

static VirtualKnob trk_shot = VirtualKnob(kPotTopLeft, "One-shot")
    .Selector(2).Labels(kShotLabels).Ident("trk_shot")
    .Ring(SelectorRing(kColShot, kColDim, 2));

static VirtualKnob trk_rev = VirtualKnob(kPotTopRight, "Reverse")
    .Selector(2).Labels(kRevLabels).Ident("trk_rev")
    .Ring(SelectorRing(kColRev, kColDim, 2));

static VirtualKnob trk_fx = VirtualKnob(kPotMiddleLeft, "FX")
    .Selector(7).Labels(kFxLabels).Ident("trk_fx")
    .Ring(SelectorRing(kColFx, kColDim, 7));

static VirtualKnob trk_fxamt = VirtualKnob(kPotMiddleRight, "FX Amount")
    .Linear(0.f, 100.f).Ident("trk_fxp").Unit("%")
    .Ring(Level(kColFxAmt));

static VirtualKnob trk_pan = VirtualKnob(kPotBottomLeft, "Pan")
    .Linear(-50.f, 50.f).Ident("trk_pan")
    .Ring(Bipolar(kColPanR, kColPanL, kWhite));

static VirtualKnob trk_sel = VirtualKnob(kPotBottomRight, "Track")
    .Selector(5).Labels(kTrackLabels).Ident("trk_track")
    .Ring(Custom(TrackSelectRing));

static Page play_page = Page(kPagePlay).Name("Play").Color("#40ff80")
    .Knobs(lvl1, lvl2, lvl3, lvl4, lvl5, track);

static Page setup_page = Page(kPageSetup).Name("Setup").Color("#c060ff")
    .Knobs(quant, recact, dubmode, playmode, tempo, master);

static Page track_page = Page(kPageTrack).Name("Track").Color("#ff9030")
    .Knobs(trk_shot, trk_rev, trk_fx, trk_fxamt, trk_pan, trk_sel);

/* ---- descriptor metadata -------------------------------------------------- */

static Jack jk_in_l   ("J1",  "In L",       JackSig::AudioIn);
static Jack jk_in_r   ("J2",  "In R",       JackSig::AudioIn);
static Jack jk_clk    ("J3",  "Clock",      JackSig::Trig);
static Jack jk_rec    ("J4",  "Rec",        JackSig::Trig);
static Jack jk_stop   ("J5",  "Stop",       JackSig::Trig);
static Jack jk_cv_trk ("J6",  "CV Track",   JackSig::CvBi);
static Jack jk_cv_l1  ("J7",  "CV Level 1", JackSig::CvBi);
static Jack jk_cv_l2  ("J8",  "CV Level 2", JackSig::CvBi);
static Jack jk_out_l  ("J9",  "Out L",      JackSig::AudioOut);
static Jack jk_out_r  ("J10", "Out R",      JackSig::AudioOut);

static VirtualButton bt_track = VirtualButton("b1", "Rec / Play")
    .Action("Tap", "Record, then play, then overdub, on the selected track")
    .Action("Hold", "Undo the last recording or overdub; hold again to redo")
    .Action("Hold 2 s", "Clear the selected track");

static VirtualButton bt_stop = VirtualButton("b2", "Stop")
    .Action("Tap", "Stop the selected track")
    .Action("Hold", "All start / stop");

static VirtualButton bt_setup = VirtualButton("b3", "Setup / Track")
    .Action("Tap", "Switch between the Play page and the Track page")
    .Action("Hold", "Show the Setup page");

static Manual manual = Manual()
    .Tagline("Five-track live looper, RC-505 style")
    .Preamble(
        "**Mark** is five loop tracks under one pair of buttons. Pick a track "
        "with the TRACK pot, tap B1 to record, tap again to play, again to "
        "overdub; B2 stops it. The five rings are the five tracks: red "
        "recording, green playing, yellow overdubbing, white stopped, with "
        "a playhead running round each. With nothing on J3 the first loop "
        "you record sets the bar, and TEMPO only tells Mark how many bars it "
        "was; with a clock on J3 every loop snaps to it. Hold B3 for the "
        "Setup page: quantize grid, what recording ends in, overdub or "
        "replace, single-track sections, tempo and master level. Tap B3 for "
        "the Track page, which belongs to the selected track: one-shot, "
        "reverse, an insert effect and its amount, and pan. B1 and B2 still "
        "record and stop there, and the TRACK pot is in the same place, so "
        "you can walk through all five tracks without leaving it.");

/* ---- surfaces ------------------------------------------------------------ */

static ControlLoop loop(hw);
Pager              pager = Pager(kNumAppPages, kNumPots)
                               .Shift(hw.buttons[kButtonB3], kPageSetup);

/*
 * This firmware's own preset slot. Every Alchemy firmware in the family
 * keeps its working state in a slot of its own -- Smack 12, Mark 13, Belt
 * 14, Relay 15 -- so switching firmware through the SD picker no longer
 * resets the one you left. Slot 0, where they all used to autosave, is
 * read once at boot for migration only (see main). Slots 0-11 are left for
 * presets saved by hand; saving one by hand into 12-15 overwrites that
 * firmware's working state.
 */
static constexpr uint8_t kHomeSlot = 13u;
static Presets     presets(hw.seed.qspi);
static Settings    settings(hw, &pager);
static CvMatrix    cv_matrix(kNumCvInputs);
static SdCard      sd;
static hostlink::FsExtension fs_ext(sd);
static hostlink::Host host(presets, "mark_alchemy", "Mark",
                           MARK_VERSION, MARK_GIT_HASH);
static MarkExtras    extras;
static SelectorHandle dry_input, clock_ratio, usb_port;

/* The front USB-C this boot: false = HostLink to the Mac (the default),
 * true = USB host for a Launchpad. One role per boot (libDaisy). */
static bool g_lp_mode = false;

/* Launchpad start-up, staged so a hang shows where it happened. The host
 * starts 2 s after boot, once the module is otherwise running; B2 shows the
 * stage in Launchpad mode:
 *   blue      about to start the USB host (a hang here = in Init)
 *   cyan      host running, no device seen on the wire
 *   yellow    a device is attached, enumeration in progress or stuck
 *   red       the host gave up on it (abort, unsupported, error)
 *   green     a MIDI device is active
 * Hold B1 while powering up to boot in Mac mode whatever the setting says. */
static uint8_t  g_lp_stage   = 0;
static uint32_t g_lp_boot_ms = 0;

#ifdef MARK_BENCH_USB
/* HostLink on the Seed's own micro-USB instead of the front panel, for the
 * bench-bootloader setup where the front port is not serving anything. */
static daisy::UsbHandle          bench_usb;
static hostlink::CdcUsbTransport bench_cdc;
#endif

/* ---- host shim (the entire host surface mark_core needs) ---------------- */

static float g_free_bpm = 120.0f;
static float host_get_bpm(void) { return g_free_bpm; }

/* ---- parameter dispatch ------------------------------------------------- */

/*
 * The engine takes strings, so a parameter is only sent when its quantized
 * value actually changes. Stepped ones also need hysteresis: a pot parked on
 * a step boundary would otherwise chatter across it on ADC noise, and a
 * TRACK select that chatters sends the next tap to the wrong loop. The SDK
 * leaves zone-commit debouncing to the app (docs/pages-and-layers.md).
 */
#define HYST 0.02f /* ~2% of travel */

struct Knob
{
    const char* key;      /* engine parameter, or nullptr for a local one */
    int         lo, hi;   /* inclusive integer range */
    bool        zones;    /* floor(norm * N), matching the SDK's selector
                             rings, else nearest */
    float       dead;     /* deadband in normalised units; 0 = none */
    int         last      = -32768;   /* -32768 = never dispatched */
    float       last_norm = 0.0f;
};

static Knob k_lvl[MARK_TRACKS] = {
    {"t1_level", 0, 100, false, 0.0f},
    {"t2_level", 0, 100, false, 0.0f},
    {"t3_level", 0, 100, false, 0.0f},
    {"t4_level", 0, 100, false, 0.0f},
    {"t5_level", 0, 100, false, 0.0f},
};
static Knob k_track  {nullptr,      0,   4, true,  HYST};
static Knob k_quant  {nullptr,      0,   4, true,  HYST};
static Knob k_recact {"rec_action", 0,   1, true,  HYST};
static Knob k_dub    {"dub_mode",   0,   1, true,  HYST};
static Knob k_play   {"play_mode",  0,   1, true,  HYST};
static Knob k_master {"master",     0, 200, false, 0.0f};

static int knob_quantize(const Knob& k, float norm)
{
    const int span = k.hi - k.lo;
    int       v;
    if (k.zones)
    {
        v = (int)(norm * (float)(span + 1));
        if (v > span) v = span;
    }
    else
    {
        v = (int)(norm * (float)span + 0.5f);
    }
    if (v < 0)    v = 0;
    if (v > span) v = span;
    return k.lo + v;
}

/* True, with *out set, when the quantized value moved far enough to count. */
static bool knob_changed(Knob& k, float norm, int* out)
{
    if (norm < 0.0f) norm = 0.0f;
    if (norm > 1.0f) norm = 1.0f;
    const int v = knob_quantize(k, norm);
    if (v == k.last) return false;
    if (k.dead > 0.0f && k.last != -32768)
    {
        const float moved = fabsf(norm - k.last_norm);
        if (moved < k.dead) return false;
    }
    k.last      = v;
    k.last_norm = norm;
    *out        = v;
    return true;
}

static void set_param_int(const char* key, int v)
{
    char b[16];
    snprintf(b, sizeof b, "%d", v);
    mark_set_param(M, key, b);
}

static bool dispatch(Knob& k, float norm)
{
    int v;
    if (!knob_changed(k, norm, &v)) return false;
    if (k.key) set_param_int(k.key, v);
    return true;
}

/* A gesture on the selected track: "t<n>_btn", "t<n>_stop", "t<n>_clear". */
static void track_gesture(const char* what)
{
    char key[24];
    snprintf(key, sizeof key, "t%d_%s", g_track + 1, what);
    mark_set_param(M, key, "1");
}

/* ---- TRACK page ------------------------------------------------------------ */

/*
 * Five values per track, kept in the extras (so they survive a power cycle)
 * and mirrored into the engine. The page is a window on the selected track:
 *
 *   a knob moves      -> extras.trk[g_track] and the engine, together
 *   the track changes -> the page is reloaded from the extras; SetStored
 *                        re-arms catch, so a pot never drags the new track
 *                        to wherever it was left for the old one
 *   a preset loads    -> extras.loads moves; every value is pushed to the
 *                        engine again and the page reloaded
 *
 * g_applied is what the engine last heard. 0xFF = nothing yet, which is how
 * the first frame after boot pushes all twenty-five.
 */
static_assert(kExtrasTracks == MARK_TRACKS, "extras.h carries one row per engine track");

static Knob k_tp[kNumTrackParams] = {
    {nullptr, 0,   1, true,  HYST},   /* one-shot  */
    {nullptr, 0,   1, true,  HYST},   /* reverse   */
    {nullptr, 0,   6, true,  HYST},   /* fx        */
    {nullptr, 0, 100, false, 0.0f},   /* fx amount */
    {nullptr, 0, 100, false, 0.0f},   /* pan       */
};

static const uint8_t kTrackPot[kNumTrackParams] = {
    kPotTopLeft, kPotTopRight, kPotMiddleLeft, kPotMiddleRight, kPotBottomLeft,
};

static VirtualKnob* const kTrackKnob[kNumTrackParams] = {
    &trk_shot, &trk_rev, &trk_fx, &trk_fxamt, &trk_pan,
};

static uint8_t  g_applied[MARK_TRACKS][kNumTrackParams];
static uint32_t g_seen_loads = 0;

static void track_param_int(int ti, const char* what, int v)
{
    char key[24];
    snprintf(key, sizeof key, "t%d_%s", ti + 1, what);
    set_param_int(key, v);
}

static void apply_track_param(int ti, uint8_t which, int v)
{
    switch (which)
    {
        case kTpShot:  track_param_int(ti, "shot", v); break;
        /* The engine ends an overdub when a track is reversed (the RC's
         * rule: no dub while reversed). */
        case kTpRev:   track_param_int(ti, "rev", v);  break;
        /* One knob for two engine parameters: zone 0 switches the insert
         * off and leaves its type alone, zones 1..6 are the tfx_t codes. */
        case kTpFx:
            if (v > 0) track_param_int(ti, "fx", v);
            track_param_int(ti, "fx_on", v > 0 ? 1 : 0);
            break;
        case kTpFxAmt: track_param_int(ti, "fxp", v);  break;
        case kTpPan:   track_param_int(ti, "pan", v);  break;
        default: break;
    }
}

/* The stored position that reads back as v: a zone's centre, or the point
 * on a linear scale. The inverse of knob_quantize. */
static float knob_norm_of(const Knob& k, int v)
{
    const int span = k.hi - k.lo;
    if (k.zones) return ((float)(v - k.lo) + 0.5f) / (float)(span + 1);
    return (float)(v - k.lo) / (float)span;
}

static void load_track_page(int ti, const float* phys)
{
    for (uint8_t w = 0; w < kNumTrackParams; w++)
    {
        const int   v = extras.trk[ti][w];
        const float n = knob_norm_of(k_tp[w], v);
        pager.SetStored(kPageTrack, kTrackPot[w], n, phys);
        /* Prime the change detector, or the next frame would "discover" the
         * new track's own value and send it straight back. */
        k_tp[w].last      = v;
        k_tp[w].last_norm = n;
    }
}

/* Extras -> engine, for whatever the engine has not heard yet. */
static void sync_tracks(const float* phys)
{
    if (extras.loads != g_seen_loads)
    {
        g_seen_loads = extras.loads;
        memset(g_applied, 0xFF, sizeof g_applied);
    }

    bool reload = false;
    for (int ti = 0; ti < MARK_TRACKS; ti++)
        for (uint8_t w = 0; w < kNumTrackParams; w++)
        {
            const uint8_t v = extras.trk[ti][w];
            if (v == g_applied[ti][w]) continue;
            apply_track_param(ti, w, v);
            g_applied[ti][w] = v;
            if (ti == g_track) reload = true;
        }
    if (reload) load_track_page(g_track, phys);
}

/* The page's own pots. True when one moved (worth a flash write). */
static bool dispatch_track_page(void)
{
    bool moved = false;
    for (uint8_t w = 0; w < kNumTrackParams; w++)
    {
        float n = kTrackKnob[w]->Norm();
        /* A pan pot wants a centre it can find. */
        if (w == kTpPan && fabsf(n - 0.5f) < 0.03f) n = 0.5f;

        int v;
        if (!knob_changed(k_tp[w], n, &v)) continue;
        extras.trk[g_track][w] = (uint8_t)v;
        g_applied[g_track][w]  = (uint8_t)v;
        apply_track_param(g_track, w, v);
        moved = true;
    }
    return moved;
}

/* ---- gate jacks: J3 clock, J4 record, J5 stop ------------------------------ */

/*
 * Read as raw ADC in the audio callback, not through the SDK's
 * AnalogControl: that path is low-passed at ~20 Hz for pots, which would
 * smear a millisecond pulse into nothing. The board's calibration record
 * gives each jack's 0 V code and the ADC reference, so the thresholds are
 * in volts. The deviation MAGNITUDE is thresholded, so a pulse fires
 * whichever way the ADC counts -- the sign of the front end is documented
 * (higher code = more negative jack volts) but not verified here.
 */
struct GateIn
{
    uint8_t  jack;
    uint16_t zero  = 32768u;
    int      hi    = 4915;   /* +1.5 V at VDDA 3.30: assert  */
    int      lo    = 1638;   /* +0.5 V:               release */
    bool     state = false;

    void Calibrate(const V2Calibration& cal, float vdda)
    {
        uint16_t z = cal.jack[jack].adc_zero_code;
        if (z < 20000u || z > 45000u) z = 32768u;   /* design mid-scale */
        const float cpv = kV2CvInGainDesign * 65535.0f / vdda;  /* counts per volt */
        zero = z;
        hi   = (int)(1.5f * cpv);
        lo   = (int)(0.5f * cpv);
    }

    /* True on the rising edge. */
    inline bool Poll()
    {
        const uint16_t raw = hw.seed.adc.Get(kCvAdcOffset + jack);
        int            dev = (int)zero - (int)raw;
        if (dev < 0) dev = -dev;
        if (!state)
        {
            if (dev > hi) { state = true; return true; }
        }
        else if (dev < lo) state = false;
        return false;
    }
};

static GateIn g_clk  {kClockJack};
static GateIn g_rec  {kRecJack};
static GateIn g_stop {kStopJack};

static void gates_calibrate(void)
{
    const V2Calibration& cal = hw.Calibration();
    float vdda = cal.vdda_at_cal;
    if (!(vdda > 2.5f && vdda < 3.6f)) vdda = kV2VddaDesign;
    g_clk.Calibrate(cal, vdda);
    g_rec.Calibrate(cal, vdda);
    g_stop.Calibrate(cal, vdda);
}

/*
 * Clock feed. The adapter free-runs when nothing is patched (Smack wants
 * that), but Mark does not: with no clock, the first loop you record IS the
 * bar, and TEMPO only says how many bars it was (mark_core.c schedule /
 * finalize_record). So the engine only hears the adapter after a pulse has
 * arrived on J3, and hears a MIDI Stop when the adapter loses the pulse
 * train, after which it free-runs again on its own grid.
 */
static volatile bool g_clk_feed   = false;
static bool          g_was_locked = false;

static void emit_to_engine(void* ctx, uint8_t byte)
{
    if (g_clk_feed) mark_on_midi((mark_t*)ctx, &byte, 1, 3); /* source 3 = host */
}

/* ---- buttons (1 ms poll) -------------------------------------------------- */

/*
 * B1 is the RC's [REC/PLAY] for the selected track, B2 its [STOP]:
 *
 *   B1 tap                 record -> play -> overdub -> play ...
 *   B1 hold  > 600 ms      undo / redo the last recording or overdub
 *   B1 hold  > 2000 ms     clear the track (fires while held)
 *   B2 tap                 stop the track
 *   B2 hold  > 600 ms      ALL start / stop (fires while held)
 *
 *   B3 tap   < 300 ms      PLAY <-> TRACK page
 *
 * Taps act on RELEASE, so a hold never fires the tap first; the cost is a
 * release-time delay on the first loop's downbeat, which the grid then
 * carries consistently. Edges come from Pressed(), not RisingEdge() (see
 * smack-versio's note on the debouncer's own edge flag at a 1 ms poll).
 *
 * B3 belongs to the Pager (hold = the SETUP layer, one binding per button),
 * so its tap is arbitrated here the way pager.h intends: a short press that
 * edited nothing (HoldUsed) and never met B2 (the Settings chord) is a tap.
 * The layer engages on the press edge, so SETUP shows on the rings for the
 * length of the tap; GoToPage then cancels it. The jump itself needs the
 * physical pots, so it is only flagged here and made in OnFrame.
 */
#define UNDO_PRESS_MS  600u
#define CLEAR_PRESS_MS 2000u
#define ALL_PRESS_MS   600u
#define NAV_TAP_MS     300u

struct Press
{
    bool     down  = false;
    bool     fired = false;   /* a hold action already fired; release is inert */
    uint32_t t0    = 0;
};

static Press b1, b2, b3;   /* b3.fired = the press met B2: a chord, not a tap */

static volatile bool G_NAV_TAP  = false;
static uint8_t       g_nav_page = kPagePlay;   /* the page B3 returns to */

/* ---- Launchpad Mini MK3 -----------------------------------------------------
 *
 *   rows 1-5 = tracks 1-5
 *     pad 1   REC/PLAY: tap = record -> play -> overdub (the track's B1),
 *             hold 0.6 s = undo/redo, hold 2 s = clear. Colour = the track:
 *             red rec, green play, amber dub, dim green stopped, dim red
 *             empty; blinks while a change waits for the bar.
 *     pad 2   STOP the track
 *     pads 3-8 the playhead, in the track's colour
 *   top 1     ALL start/stop       top 2  UNDO/REDO (amber undo, green redo)
 */
struct LpHold { bool down; bool fired; uint32_t t0; };
static LpHold g_lp_rec[MARK_TRACKS];

static void track_key(int ti, const char* what)
{
    char key[24];
    snprintf(key, sizeof key, "t%d_%s", ti + 1, what);
    mark_set_param(M, key, "1");
}

static void lp_poll(uint32_t now)
{
    lp::Poll(now);
    lp::Event e;
    const bool live = !settings.IsActive();
    while (lp::PopEvent(&e))
    {
        if (!live) continue;
        if (e.kind == lp::Kind::Grid && e.y < MARK_TRACKS)
        {
            const int ti = e.y;
            if (e.x == 0)
            {
                LpHold& h = g_lp_rec[ti];
                if (e.down) { h.down = true; h.fired = false; h.t0 = now; }
                else if (h.down)
                {
                    h.down = false;
                    if (!h.fired)
                    {
                        if (now - h.t0 > UNDO_PRESS_MS) mark_set_param(M, "undo", "1");
                        else                            track_key(ti, "btn");
                    }
                }
            }
            else if (e.x == 1 && e.down)
                track_key(ti, "stop");
        }
        else if (e.kind == lp::Kind::Top && e.down)
        {
            if (e.x == 0) mark_set_param(M, "all_btn", "1");
            if (e.x == 1) mark_set_param(M, "undo", "1");
        }
    }
    for (int ti = 0; ti < MARK_TRACKS; ti++)
    {
        LpHold& h = g_lp_rec[ti];
        if (h.down && !h.fired && now - h.t0 > CLEAR_PRESS_MS)
        {
            track_key(ti, "clear");
            h.fired = true;
        }
    }
}

/* Write the host report to /lpdiag.txt once, 15 s after the host started,
 * unless a Launchpad came up. Buffers where SDMMC DMA can reach them. */
alignas(32) static ALCHEMY_SDMMC_BSS FIL  s_lpdiag_fil;
alignas(32) static ALCHEMY_SDMMC_BSS char s_lpdiag_buf[4096];
static bool g_lpdiag_done = false;

static void lp_write_report(uint32_t now)
{
    if (g_lpdiag_done || g_lp_stage < 2 || now - g_lp_boot_ms < 17000u) return;
    if (lp::Connected()) { g_lpdiag_done = true; return; }
    if (picker::Busy() || !sd.EnsureMounted(now)) return;
    g_lpdiag_done = true;
    const int n = lp::Report(s_lpdiag_buf, (int)sizeof s_lpdiag_buf);
    if (f_open(&s_lpdiag_fil, "/lpdiag.txt", FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    UINT w = 0;
    f_write(&s_lpdiag_fil, s_lpdiag_buf, (UINT)n, &w);
    f_close(&s_lpdiag_fil);
}

static void lp_paint(uint32_t t_ms)
{
    if (!lp::Connected()) return;
    const bool blink = (t_ms / 150u) & 1u;
    for (int ti = 0; ti < MARK_TRACKS; ti++)
    {
        uint8_t c;
        switch (G_TSTATE[ti])
        {
            case MK_REC:  c = lp::kRed;      break;
            case MK_PLAY: c = lp::kGreen;    break;
            case MK_DUB:  c = lp::kAmber;    break;
            case MK_STOP: c = lp::kGreenDim; break;
            default:      c = lp::kRedDim;   break;
        }
        if (G_TPEND[ti] && blink) c = lp::kOff;
        lp::SetGrid(0, (uint8_t)ti, c);
        const bool has = G_TSTATE[ti] != MK_EMPTY;
        lp::SetGrid(1, (uint8_t)ti, has ? lp::kRedDim : lp::kOff);
        const bool moving = G_TSTATE[ti] == MK_PLAY || G_TSTATE[ti] == MK_DUB
                            || G_TSTATE[ti] == MK_REC;
        const int head = moving ? 2 + (G_TPOS[ti] * 6) / 128 : -1;
        for (int x = 2; x < 8; x++)
            lp::SetGrid((uint8_t)x, (uint8_t)ti, x == head ? c : lp::kOff);
    }
    lp::SetLogo(lp::kGreen);
    lp::SetTop(0, lp::kGreenDim);
    lp::SetTop(1, G_UNDO == 1 ? lp::kAmber : (G_UNDO == 2 ? lp::kGreen : lp::kOff));
}

static void OnPoll(uint32_t now)
{
    if (g_lp_mode)
    {
        if (g_lp_stage == 0 && now - g_lp_boot_ms > 2000u) g_lp_stage = 1;
        else if (g_lp_stage == 1 && now - g_lp_boot_ms > 2300u)
        {
            lp::Init();   /* the blue B2 has been on screen for 300 ms */
            g_lp_stage = 2;
        }
        else if (g_lp_stage >= 2)
        {
            lp_poll(now);
            g_lp_stage = (uint8_t)(2 + lp::Stage()); /* 2 cyan .. 5 green */
        }
    }

    if (settings.IsActive())
    {
        b1.down = b2.down = b3.down = false;
        G_TRIG_REC = G_TRIG_STOP = false;
        G_NAV_TAP = false;
        return;
    }

    /* B3 */
    {
        const bool pressed = hw.buttons[kButtonB3].Pressed();
        if (pressed && !b3.down) { b3.down = true; b3.fired = false; b3.t0 = now; }
        if (b3.down && pressed && hw.buttons[kButtonB2].Pressed()) b3.fired = true;
        if (b3.down && !pressed)
        {
            b3.down = false;
            if (!b3.fired && now - b3.t0 < NAV_TAP_MS
                && !pager.HoldUsed(hw.buttons[kButtonB3]))
                G_NAV_TAP = true;
        }
    }

    /* Trigger jacks, consumed here so set_param never runs in the callback. */
    if (G_TRIG_REC)  { G_TRIG_REC  = false; track_gesture("btn");  }
    if (G_TRIG_STOP) { G_TRIG_STOP = false; track_gesture("stop"); }

    /* B1 */
    {
        const bool pressed = hw.buttons[kButtonB1].Pressed();
        if (pressed && !b1.down) { b1.down = true; b1.fired = false; b1.t0 = now; }
        const uint32_t held = now - b1.t0;
        if (b1.down && pressed && !b1.fired && held > CLEAR_PRESS_MS)
        {
            track_gesture("clear");
            b1.fired = true;
        }
        if (b1.down && !pressed)
        {
            b1.down = false;
            if (!b1.fired)
            {
                if (held > UNDO_PRESS_MS) mark_set_param(M, "undo", "1");
                else                      track_gesture("btn");
            }
        }
    }

    /* B2. Stands down while B3 is held: B2+B3 held two seconds is the
     * Settings chord, and the hold-for-ALL threshold is 600 ms, so every
     * trip into Settings would otherwise stop (or start) every loop on the
     * way. While B3 is down B2 is ignored and its release fires nothing. */
    if (hw.buttons[kButtonB3].Pressed())
    {
        b2.down = false;
    }
    else
    {
        const bool pressed = hw.buttons[kButtonB2].Pressed();
        if (pressed && !b2.down) { b2.down = true; b2.fired = false; b2.t0 = now; }
        const uint32_t held = now - b2.t0;
        if (b2.down && pressed && !b2.fired && held > ALL_PRESS_MS)
        {
            mark_set_param(M, "all_btn", "1");
            b2.fired = true;
        }
        if (b2.down && !pressed)
        {
            b2.down = false;
            if (!b2.fired) track_gesture("stop");
        }
    }
}

/* ---- control frame (~60 Hz) ------------------------------------------------ */

static bool     g_dirty       = false;
static uint32_t g_dirty_since = 0;
static float    g_saved_peak  = 0.0f;
static bool     prev_settings = false;
static int      applied_dry   = -1;
static int      applied_ratio = -1;
static float    applied_bpm   = 0.0f;

static void mark_dirty(uint32_t now)
{
    g_dirty       = true;
    g_dirty_since = now;
}

/* "a,b,c,d,e" -> five ints. */
static void read_csv5(const char* key, int* out)
{
    char buf[64];
    if (mark_get_param(M, key, buf, sizeof buf) <= 0) return;
    const char* p = buf;
    for (int i = 0; i < MARK_TRACKS; i++)
    {
        char* end;
        out[i] = (int)strtol(p, &end, 10);
        if (*end != ',') break;
        p = end + 1;
    }
}

static void OnFrame(void)
{
    const uint32_t now = System::GetNow();
    int            v;

    /* The PLAY page is worth a flash write too. It used to follow the pots
     * at every boot, so saving it bought nothing; now that a saved PLAY page
     * is restored (the pots catch it), a change there must reach the slot or
     * it is lost at the next firmware switch or power cycle. The stored
     * values move only when a caught pot moves, and the 1 % step keeps ADC
     * noise from re-arming the autosave. The first frame only primes. */
    {
        static float play_seen[kNumPots];
        static bool  primed = false;
        for (uint8_t p = 0; p < kNumPots; p++)
        {
            const float s = pager.Stored(kPagePlay, p);
            if (!primed) { play_seen[p] = s; continue; }
            if (fabsf(s - play_seen[p]) > 0.01f)
            {
                play_seen[p] = s;
                mark_dirty(now);
            }
        }
        primed = true;
    }

    float phys[kNumPots];
    for (uint8_t p = 0; p < kNumPots; p++) phys[p] = hw.pots[p].Value();

    /* B3 tap: PLAY <-> TRACK. The Pager has already handled the release by
     * now (ControlLoop runs its Update before this), and GoToPage cancels
     * the layer outright if it has not. */
    if (G_NAV_TAP)
    {
        G_NAV_TAP = false;
        if (!settings.IsActive())
        {
            g_nav_page = (g_nav_page == kPageTrack) ? kPagePlay : kPageTrack;
            pager.GoToPage(g_nav_page, phys);
        }
    }

    /* TRACK select is P6 on both pages. Whichever page is showing owns the
     * pot; its stored value is copied to the other, where the same physical
     * position catches at once (InitCatch's proximity window). g_track is
     * always read from the PLAY page's knob, which is the one J6 modulates. */
    {
        const float sp = pager.Stored(kPagePlay,  kPotBottomRight);
        const float st = pager.Stored(kPageTrack, kPotBottomRight);
        if (fabsf(sp - st) > 1.0e-4f)
        {
            if (pager.Page() == kPageTrack) pager.SetStored(kPagePlay,  kPotBottomRight, st, phys);
            else                            pager.SetStored(kPageTrack, kPotBottomRight, sp, phys);
        }
    }

    /* PLAY page. Values are catch + locks + CV, already mixed by the SDK. */
    dispatch(k_lvl[0], lvl1.Norm());
    dispatch(k_lvl[1], lvl2.Norm());
    dispatch(k_lvl[2], lvl3.Norm());
    dispatch(k_lvl[3], lvl4.Norm());
    dispatch(k_lvl[4], lvl5.Norm());
    if (knob_changed(k_track, track.Norm(), &v))
    {
        g_track = v;
        load_track_page(g_track, phys);
    }

    /* TRACK page. sync first: after a preset load the page holds whatever
     * track was showing when the preset was saved, and must be reloaded
     * before its pots are read as this track's. */
    sync_tracks(phys);
    if (dispatch_track_page()) mark_dirty(now);

    /* SETUP page. These are worth a flash write when they move. */
    if (knob_changed(k_quant, quant.Norm(), &v))
    {
        /* Zone 0 is off; zones 1..4 are the engine's rec_grid 0..3. */
        set_param_int("quantize", v > 0 ? 1 : 0);
        if (v > 0) set_param_int("rec_grid", v - 1);
        mark_dirty(now);
    }
    if (dispatch(k_recact, recact.Norm())) mark_dirty(now);
    if (dispatch(k_dub,    dubmode.Norm())) mark_dirty(now);
    if (dispatch(k_play,   playmode.Norm())) mark_dirty(now);
    if (dispatch(k_master, master.Norm())) mark_dirty(now);

    /* TEMPO: the free-run tempo. With a clock on J3 it is the adapter's
     * fallback only; unclocked it is what the engine divides the first loop
     * by to find the bar. */
    {
        const float bpm = tempo.Value();
        if (fabsf(bpm - applied_bpm) >= 0.5f)
        {
            applied_bpm = bpm;
            g_free_bpm  = bpm;
            clk_set_free_bpm(&CLK, bpm);
            set_param_int("bpm_override", (int)lroundf(bpm));
            mark_dirty(now);
        }
    }

    /* Settings: dry input through or muted; clock ratio. */
    {
        const int d = (int)dry_input.Value();
        if (d != applied_dry)
        {
            applied_dry = d;
            set_param_int("monitor", d ? 0 : 1);
        }
        const int r = (int)clock_ratio.Value();
        if (r != applied_ratio)
        {
            static const int ticks[3] = {CLK_TICKS_DIV2, CLK_TICKS_1X, CLK_TICKS_2X};
            applied_ratio = r;
            clk_set_ratio(&CLK, ticks[r < 0 ? 1 : (r > 2 ? 2 : r)]);
        }
    }

    /* Engine readbacks. mark_get_param is an snprintf -- fine at frame rate,
     * never in the callback. */
    read_csv5("tstates", G_TSTATE);
    read_csv5("tpend",   G_TPEND);
    read_csv5("tpos",    G_TPOS);
    {
        char buf[16];
        if (mark_get_param(M, "undo_avail", buf, sizeof buf) > 0) G_UNDO = atoi(buf);
    }
    if (g_lp_mode) { lp_paint(System::GetNow()); lp_write_report(System::GetNow()); }

    /* Worst block this session. Clamped: an overrunning callback can report
     * over 100%, and the readout wants 0..1. */
    {
        float mx = cpu.GetMaxCpuLoad();
        if (mx > 1.0f) mx = 1.0f;
        if (mx > extras.cpu_peak) extras.cpu_peak = mx;
        if (extras.cpu_peak > g_saved_peak + 0.05f) mark_dirty(now);
    }

    /* A visit to Settings is worth saving (brightness, dry input, ratio). */
    const bool sact = settings.IsActive();
    if (prev_settings && !sact) mark_dirty(now);
    prev_settings = sact;

    /*
     * Autosave slot 0, the working state, a few seconds after the last
     * change and only with hands off: Save() erases the sectors the record
     * covers (one, for this payload) from this thread, and the 1 ms button
     * poll is paused for the duration. Loop audio is never saved; this is
     * the Setup page and the settings screen.
     */
    /* The USB port only takes effect at power-up, so a change is saved as
     * soon as Settings closes -- nobody should have to wait before cycling. */
    static int saved_usb = -1;
    if (saved_usb < 0) saved_usb = (int)usb_port.Value();
    const bool usb_changed = !sact && (int)usb_port.Value() != saved_usb;

    if (g_dirty && (now - g_dirty_since >= 5000u || usb_changed) && !sact && !picker::Busy()
        && !hw.buttons[kButtonB1].Pressed() && !hw.buttons[kButtonB2].Pressed()
        && !hw.buttons[kButtonB3].Pressed())
    {
        presets.Save(kHomeSlot);
        saved_usb    = (int)usb_port.Value();
        g_dirty      = false;
        g_saved_peak = extras.cpu_peak;
    }
}

/* ---- LEDs ------------------------------------------------------------------ */

/*
 * The SDK draws the rings from the knobs (the five LEVEL rings through
 * TrackRing above). What is added:
 *   B1 pair   the selected track's state colour, blinking while pending;
 *             white for a frame when undo is available and B1 is held past
 *             the undo threshold, so you know before you let go
 *   B2 pair   CLOCK: blue locked to J3, grey free-running,
 *             red when the audio callback is above 80%
 *   B3 pair   dim Setup tint (the SDK paints it while the page is held)
 *   P1 ring at boot  the previous session's worst CPU load, for 2.5 s
 */
static uint32_t g_readout_until = 0;
static float    g_boot_peak     = 0.0f;

static void paint_fill(uint8_t pot, float frac, const LedPanel::Rgb& c)
{
    const ArcGeometry& geo = hw.Arc();
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    const int n = (int)lroundf(frac * (float)geo.arc_leds);
    for (int i = 0; i < n; i++)
        hw.leds.SetRingByHour(pot, fmodf(geo.start_hour + geo.step_hours * (float)i, 12.0f),
                              hw.leds.ScaleGlobal(c));
}

/* Launchpad forensics on the rings while the host is stuck (yellow/red B2):
 *   P1 last host state before the first abort (lit LEDs = state number)
 *   P2 enumeration step at that abort (LEDs = ENUM state + 1)
 *   P3 all green = the device descriptor said Novation, all red = not read
 *   P4 aborts so far (capped at a full ring) */
static void lp_forensics(void)
{
    const lp::Diag d = lp::Diagnostics();
    const float    n = (float)hw.Arc().arc_leds;
    for (uint8_t p = 0; p < 4; p++) hw.leds.ClearRing(p);
    paint_fill(kPotTopLeft,     d.state_before_abort / n, kWhite);
    paint_fill(kPotTopRight,    d.enum_at_abort / n,      kAmber);
    paint_fill(kPotMiddleLeft,  1.0f, d.vid_ok ? LedPanel::Rgb{0x00, 0xFF, 0x00}
                                               : LedPanel::Rgb{0xFF, 0x00, 0x00});
    paint_fill(kPotMiddleRight, d.aborts / n,             kRed);
}

static void OnRender(uint32_t t_ms)
{
    LedPanel& L = hw.leds;

    if (t_ms < g_readout_until)
    {
        L.ClearRing(kPotTopLeft);
        if (g_boot_peak <= 0.0f)
        {
            /* No data: first boot after a flash. Distinct from "measured,
             * and low" so the two never get confused. */
            L.SetRingByHour(kPotTopLeft, hw.Arc().start_hour, L.ScaleGlobal(kIdle));
        }
        else
        {
            const LedPanel::Rgb c = g_boot_peak >= 0.90f ? kRed
                                  : g_boot_peak >= 0.75f ? kAmber
                                                         : kGreen;
            paint_fill(kPotTopLeft, g_boot_peak, c);
        }
    }

    if (settings.IsActive()) return;   /* Settings owns the buttons */

    LedPanel::Rgb st;
    switch (G_TSTATE[g_track])
    {
        case MK_REC:  st = kColRec;  break;
        case MK_PLAY: st = kColPlay; break;
        case MK_DUB:  st = kColDub;  break;
        case MK_STOP: st = kColStop; break;
        default:      st = kIdle;    break;
    }
    if (G_TPEND[g_track] && ((t_ms / 150u) & 1u)) st = kColDim;
    if (b1.down && !b1.fired && G_UNDO && (t_ms - b1.t0) > UNDO_PRESS_MS) st = kWhite;
    L.SetButtonPair(kButtonB1, L.ScaleGlobal(st));

    LedPanel::Rgb ck;
    if (cpu.GetAvgCpuLoad() > 0.80f) ck = kRed;
    else if (clk_locked(&CLK))       ck = kBlue;
    else                             ck = kGrey;
    if (g_lp_mode)
    {
        static const LedPanel::Rgb kStage[6] = {
            {0x40, 0x00, 0x40},   /* 0 waiting for the 2 s start */
            {0x00, 0x00, 0xFF},   /* 1 about to start the host   */
            {0x00, 0xC0, 0xC0},   /* 2 no device on the wire     */
            {0xFF, 0xC0, 0x00},   /* 3 attached, enumerating     */
            {0xFF, 0x00, 0x00},   /* 4 host gave up              */
            {0x00, 0xFF, 0x00}};  /* 5 MIDI device active        */
        ck = kStage[g_lp_stage < 6 ? g_lp_stage : 0];
    }
    L.SetButtonPair(kButtonB2, L.ScaleGlobal(ck));

    /* Which page a release of B3 lands on: dim violet PLAY, amber TRACK. */
    if (pager.Page() != kPageSetup)
        L.SetButtonPair(kButtonB3, L.ScaleGlobal(g_nav_page == kPageTrack ? kColTrkPg
                                                                         : kColSetup));

    if (g_lp_mode && (g_lp_stage == 3 || g_lp_stage == 4) && !settings.IsActive())
        lp_forensics();
}

/* ---- audio ----------------------------------------------------------------- */

static int16_t bufi[kBlockSize * 2];

static inline int16_t f2i(float v)
{
    if (v > 0.999969f) v = 0.999969f;
    if (v < -1.0f)     v = -1.0f;
    return (int16_t)(v * 32767.0f);
}

static void AudioCallback(daisy::AudioHandle::InputBuffer  in,
                          daisy::AudioHandle::OutputBuffer out,
                          size_t                           size)
{
    cpu.OnBlockStart();

    /* Controls are NOT read here; set_param stays on the control thread.
     * What is per-block: the three gate jacks need frame-accurate edges,
     * and the clock advance is what the engine's grid is built on. */
    if (g_clk.Poll())
    {
        g_clk_feed = true;      /* before the edge, so its Start gets through */
        clk_gate_edge(&CLK);
    }
    if (g_rec.Poll())  G_TRIG_REC  = true;
    if (g_stop.Poll()) G_TRIG_STOP = true;

    clk_advance(&CLK, (int)size, emit_to_engine, M);

    const bool locked = clk_locked(&CLK) != 0;
    if (g_was_locked && !locked)
    {
        /* The pulse train stopped: tell the engine, then stop feeding it the
         * adapter's free-run ticks. It falls back to its own grid. */
        const uint8_t stop = 0xFC;
        mark_on_midi(M, &stop, 1, 3);
        g_clk_feed = false;
    }
    g_was_locked = locked;

    /* Refuse to run rather than corrupt memory if the block is ever not what
     * Init() asked for. */
    if (size > kBlockSize)
    {
        for (size_t i = 0; i < size; i++)
        {
            out[0][i] = in[0][i];
            out[1][i] = in[1][i];
        }
        cpu.OnBlockEnd();
        return;
    }

    /* float -1..1 -> interleaved int16, which is what the engine takes.
     * Converting at the boundary keeps the engine bit-identical to the Move
     * build, so any difference in sound is a shim bug. The engine mixes the
     * dry input into its output itself (monitor), so nothing is blended here. */
    for (size_t i = 0; i < size; i++)
    {
        bufi[2 * i]     = f2i(in[0][i]);
        bufi[2 * i + 1] = f2i(in[1][i]);
    }

    mark_process(M, bufi, bufi, (int)size);

    const float k = 1.0f / 32768.0f;
    for (size_t i = 0; i < size; i++)
    {
        out[0][i] = (float)bufi[2 * i] * k;
        out[1][i] = (float)bufi[2 * i + 1] * k;
    }

    cpu.OnBlockEnd();
}

/* ---- boot ------------------------------------------------------------------ */

static void fault_forever(void)
{
    /* Refuse to run half-initialised: every button pair red, no audio. */
    for (;;)
    {
        hw.leds.Clear();
        for (uint8_t b = 0; b < kNumButtons; b++)
            hw.leds.SetButtonPair(b, {0x80, 0x00, 0x00});
        hw.leds.Show();
        System::Delay(200);
    }
}

int main(void)
{
    hw.Init(daisy::SaiHandle::Config::SampleRate::SAI_48KHZ, kBlockSize);
    cpu.Init(hw.SampleRate(), (int)hw.BlockSize());
    versio_alloc_init(g_pool, VERSIO_POOL_BYTES);
    gates_calibrate();

    memset(&HOST, 0, sizeof HOST);
    HOST.api_version      = 1;
    HOST.sample_rate      = MARK_SR;
    HOST.frames_per_block = kBlockSize;
    HOST.get_bpm          = host_get_bpm;
    clk_init(&CLK, MARK_SR, 120.0f);
    clk_set_mode(&CLK, CLK_EXTERNAL);   /* a pulse on J3 is always a clock */

    /* mark_create, not mark_create_in_dir: no module directory means no FX
     * catalogue scan and no loader thread is ever attempted. */
    M = mark_create(&HOST);
    if (!M || versio_alloc_failed()) fault_forever();

    /* Dry input passes to the output (the RC does the same); the Move's
     * feedback guard has no microphone here to protect. */
    mark_set_param(M, "monitor",  "1");
    mark_set_param(M, "hw_input", "1");
    mark_set_param(M, "follow",   "0");

    /* Storage + settings. sd.Init() registers the SDRAM volume; no card I/O
     * until something opens it. */
    sd.Init();
    picker::Install(settings, kSettingsFirmware, sd, hw);
    settings.UseBrightness();
    settings.UsePresets(presets);
    dry_input = settings.Page(kSettingsMain).Pot(1)
        .Selector(kDryLabels).Default(0)
        .Ident("dry").Name("Dry Input")
        .Help("**Through** mixes what comes in on J1/J2 into the output, as "
              "the RC-505 does. **Muted** outputs the loops only, for a mixer "
              "that already has the dry signal.");
    usb_port = settings.Page(kSettingsMain).Pot(5)
        .Selector(kUsbLabels).Default(0)
        .Ident("usb").Name("USB port")
        .Help("**Mac**: the front USB-C is HostLink, for the web programmer, "
              "presets and the card. **Launchpad**: the Lab is the USB host "
              "for a Launchpad Mini MK3 (5 V from a passive injector, not a hub). "
              "Takes effect when the module next powers up.");
    clock_ratio = settings.Page(kSettingsMain).Pot(4)
        .Selector(kRatioLabels).Default(1)
        .Ident("clk.ratio").Name("Clock Ratio")
        .Help("What one pulse on J3 is worth: a half note, a quarter note, "
              "or an eighth.");

    /* CV: J3-J5 are gates (read raw in the callback), the rest modulate. */
    cv_matrix.Jack(0).Off();
    cv_matrix.Jack(1).Off();
    cv_matrix.Jack(2).Off();
    cv_matrix.Jack(3).To(track);
    cv_matrix.Jack(4).To(lvl1);
    cv_matrix.Jack(5).To(lvl2);

    /* HostLink: identity, descriptor, SD file access. The transport is the
     * front USB-C unless this is a bench build. Must all be declared before
     * BootLoad(), which is where the host starts. */
    host.Product("Mark")
        .BootSlot(kHomeSlot)
        .Pages(play_page, setup_page, track_page)
        .Jacks(jk_in_l, jk_in_r, jk_clk, jk_rec, jk_stop, jk_cv_trk,
               jk_cv_l1, jk_cv_l2, jk_out_l, jk_out_r)
        .Buttons(bt_track, bt_stop, bt_setup)
        .Attach(manual)
        .Extend(fs_ext);
#ifdef MARK_BENCH_USB
    bench_cdc.Init(bench_usb, daisy::UsbHandle::FS_INTERNAL, "Mark (bench)");
    host.Transport(bench_cdc);
#endif

    /* Nothing on the TRACK page has reached the engine yet; the first frame
     * pushes all five tracks from the extras (sync_tracks). */
    memset(g_applied, 0xFF, sizeof g_applied);

    /* Preset payload: the pages, the settings screen, and the extras. */
    presets.Manage(pager);
    presets.Manage(settings);
    presets.Manage(extras);
    presets.Init();
    /* BootLoad() still runs: it fires HostLink's pre-boot hook, and it
     * restores slot 0 when slot 0 holds THIS firmware's settings (the
     * schema gate refuses any other), which carries a module's settings
     * over from the builds that autosaved there. The home slot, once it
     * holds anything, wins. */
    const bool had_home = presets.HasValid(kHomeSlot);
    const bool had_boot = had_home || presets.HasValid(0);
    presets.BootLoad();
    if (had_home) presets.Load(kHomeSlot);

    /* Physical pot positions, primed. */
    float phys[kNumPots];
    for (int i = 0; i < 8; i++)
    {
        hw.ProcessAllControls();
        System::Delay(1);
    }
    for (uint8_t p = 0; p < kNumPots; p++) phys[p] = hw.pots[p].Value();

    if (!had_boot)
    {
        /* First boot after a flash: the RC's defaults. Zone i of N sits at
         * (i + 0.5) / N. */
        pager.SetStored(kPageSetup, kPotTopLeft,     1.5f / 5.0f, phys); /* bar      */
        pager.SetStored(kPageSetup, kPotTopRight,    0.25f,       phys); /* rec->play*/
        pager.SetStored(kPageSetup, kPotMiddleLeft,  0.25f,       phys); /* overdub  */
        pager.SetStored(kPageSetup, kPotMiddleRight, 0.25f,       phys); /* multi    */
        pager.SetStored(kPageSetup, kPotBottomLeft,  70.0f / 150.0f, phys); /* 120 BPM */
        pager.SetStored(kPageSetup, kPotBottomRight, 0.5f,        phys); /* 100 %    */
    }

    /* The PLAY page. With nothing saved (first boot after a flash) it
     * adopts the pots, so whatever a pot points at is what the module
     * does. With a saved state it keeps the saved values, and each pot
     * must catch its value before it takes over: the pots are shared by
     * every firmware on the card, so after a picker switch they point
     * wherever the last firmware left them, and adopting them would
     * overwrite this one's settings with another's. BootLoad() left the
     * Pager a deferred re-arm for its first Update(), which arms the
     * catch against phys without touching the stored values. */
    if (!had_boot)
        for (uint8_t p = 0; p < kNumPots; p++)
            pager.SetStored(kPagePlay, p, phys[p], phys);

    /* Report last session's peak on the P1 ring, then start recording this
     * one. Audio is already passing through during the readout. */
    g_boot_peak      = extras.cpu_peak;
    extras.cpu_peak  = 0.0f;
    g_saved_peak     = 0.0f;
    g_readout_until  = System::GetNow() + 2500u;

    /* USB: one role per boot on the front port, decided by the saved
     * setting. HostLink only starts from its first Poll, so leaving it out
     * of the loop keeps the device stack off the port. */
    /* B1 held through power-up: Mac mode this boot. Read over ~20 ms so
     * the debouncer has settled whatever came before. */
    bool force_mac = false;
    for (int i = 0; i < 20; i++)
    {
        hw.ProcessAllControls();
        System::Delay(1);
    }
    force_mac = hw.buttons[kButtonB1].Pressed();
    g_lp_mode    = (int)usb_port.Value() == 1 && !force_mac;
    g_lp_boot_ms = System::GetNow();

    hw.StartAudio(AudioCallback);
    cpu.Reset();

    loop.Use(pager)
        .Use(settings)
        .Use(cv_matrix)
        .Use(play_page)
        .Use(setup_page)
        .Use(track_page)
        .OnFrame(OnFrame)
        .OnPoll(OnPoll)
        .OnRender(OnRender);
    if (!g_lp_mode) loop.Use(host);

    for (;;) loop.Tick();
}
