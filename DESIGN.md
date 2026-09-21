# Mark on the Alchemy Lab — Design

Port of [schwung-mark](../mark) (Mark for the Ableton Move) onto the
Hermetic Modular **Alchemy Lab V2**, on the Alchemy SDK, in the shape of
[smack-alchemy](../smack-alchemy). Written 2026-09-10 before anything was
heard. Claims are marked **✅ verified** (with the source) or **⚠️ assumed /
unheard**; keep that discipline.

## 1. What carries over, and how

- **The engine** (`src/vendor/mark_core.[ch]`, `plugin_api_v1.h`,
  `audio_fx_api_v2.h`): the schwung-mark `main` copy at 30090bd (module
  0.5.0). `mark_core.c` is byte-identical to upstream; the header has two
  edits: `MARK_SR` 44100 → 48000 (libDaisy offers no 44.1 kHz; every
  tempo, delay and LFO constant follows it), and `MARK_MAX_SECONDS` wrapped
  in `#ifndef` so the Makefile sets it (§ 2). Engine changes go upstream.
- **Its Move-only host needs are stubbed, not removed.** The engine
  includes `<pthread.h>`, `<dlfcn.h>`, `<dirent.h>` and `<sched.h>` for two
  features that have no home here: sessions (WAV files on a worker thread)
  and hosting external Schwung FX modules (`dlopen`). `src/stubs/` shadows
  those four headers (the toolchain has no `dlfcn.h`, and its `dirent.h`
  is an `#error`; its `pthread.h` is empty without `_POSIX_THREADS`), and
  `src/host_stubs.c` makes every such call fail the way the real one fails
  when the resource is unavailable: `pthread_create` → EAGAIN, `dlopen` →
  NULL, `opendir` → NULL, `mkdir`/`rmdir` → −1, `usleep` → returns.
  `fopen`/`stat`/`unlink` reach newlib's nosys layer and return −1. The
  engine's own error paths then leave sessions and external FX inert. Safe
  for the rest of the build because the toolchain's thread model is
  `single` (✅ `arm-none-eabi-gcc -v`), so nothing in libstdc++ or the SDK
  pulls the real `<pthread.h>`. `mark_create()` rather than
  `mark_create_in_dir()`, so no loader thread is ever attempted.
- **The clock adapter** (`clock_adapter.[ch]`): gate edges → synthetic
  24 ppqn MIDI clock, byte-identical to smack-alchemy's, where its suite
  runs. Here the engine only hears it after a pulse (§ 3).
- **The allocator** (`versio_alloc.[ch]`): a bump allocator whose `free`
  is a no-op. That defeats the engine's capacity ladder (60 → 45 → 30 s: a
  failed rung's buffers would never come back), so the capacity is fixed
  at build time and proven to fit on the first rung natively.
- **48 kHz, 128-frame blocks** (`MARK_BLOCK_FRAMES`); float in/out, int16
  interleaved into the engine. The engine mixes the dry input into its
  output itself (`monitor`), so nothing is blended in the callback.
- **The picker, the linker script, the Makefile, `tools/hostlink-fs.mjs`**:
  copies of smack-alchemy's. A fix in one belongs in all three.

## 2. Memory

Five tracks and the undo buffer, stereo int16 at 48 kHz: 1,152,000 bytes
per second of capacity. With five 256 KB delay lines and the engine struct,
**55 s per track = 64,727,856 B** in a **62 MiB pool** (`versio_alloc.h`),
99.6 % of it. The SDK keeps ~64 KB of SDRAM for HostLink (✅ smack-alchemy
`.map`), so the build's SDRAM total is 65,077,184 B of 64 MiB (97 %). ✅
`test/test_alloc.c` links the firmware's own wrapper unit with the
firmware's pool and capacity and asserts the first rung fits with
`track_seconds` = 55; `make MARK_SECONDS=n` changes both together.

## 3. Control surface

Mark on the Move has five pad columns, 16 steps and eight knobs. Here: six
pots on three pages, three buttons, and the rings do the work the pads did.

### PLAY page (base)

| Pot | Parameter | Engine | Ring |
|---|---|---|---|
| P1–P5 | **LEVEL** of tracks 1–5, 0–100 | `t<n>_level` | **the track's display** (§ LEDs) |
| P6 | **TRACK** 1–5: which one B1, B2, J4 and J5 act on | local | 5-zone selector |

### SETUP page (hold B3)

| Pot | Parameter | Engine | Ring |
|---|---|---|---|
| P1 | **QUANTIZE** Off / Bar / Beat / 8th / 16th | `quantize` 0/1 + `rec_grid` 0..3 | 5-zone selector |
| P2 | **REC →** Play / Overdub: what ending a recording lands in | `rec_action` | 2-zone |
| P3 | **DUB** Overdub / Replace | `dub_mode` | 2-zone |
| P4 | **PLAY** Multi / Single (starting a track stops the others: song sections) | `play_mode` | 2-zone |
| P5 | **TEMPO** 50–200 BPM | `bpm_override` + the adapter's free-run | level |
| P6 | **MASTER** 0–200 % | `master` | level |

First boot after a flash: quantize Bar, rec → Play, Overdub, Multi,
120 BPM, 100 %. The PLAY page adopts the physical pots at boot.

### TRACK page (tap B3) — added 2026-09-21, ⚠️ unseen on hardware

What the engine keeps per track, for the track P6 selects. B1 and B2 are
still record and stop here: a looper's transport never depends on the page.

| Pot | Parameter | Engine | Ring |
|---|---|---|---|
| P1 | **ONE-SHOT** Loop / One-shot | `t<n>_shot` | 2-zone |
| P2 | **REVERSE** Forward / Reverse (the engine ends an overdub when a track is reversed: the RC's rule) | `t<n>_rev` | 2-zone |
| P3 | **FX** Off / Low-pass / High-pass / Crush / Delay / Phaser / Ring mod | zone 0 → `t<n>_fx_on` 0; zones 1–6 → `t<n>_fx` n + `fx_on` 1 | 7-zone |
| P4 | **FX AMOUNT** 0–100 | `t<n>_fxp` | level |
| P5 | **PAN** L50–R50, with a ±3 % centre snap | `t<n>_pan` 0–100 | bipolar |
| P6 | **TRACK** 1–5, the same pot as on PLAY | local | five dots in the tracks' state colours, the selected one bright (the LEVEL rings are out of sight here; it reads the selection, so it follows J6) |

First boot: loop, forward, FX off at 50, centre.

**How one page serves five tracks.** The Pager stores one value per
(page, pot) (✅ `pager.h` `states_[kMaxPages][kMaxPots]`), so the five
tracks' values live in `MarkExtras::trk` and the page is a window on the
selected one:

- a pot moves → `extras.trk[g_track]` and the engine, together;
- the selection moves → the page is reloaded with `SetStored()`, which
  re-arms catch (✅ `pager.cpp` `SetStored` → `InitCatch`), so a pot never
  drags the new track to where it was left for the old one; the change
  detectors are primed with the loaded values so nothing is echoed back;
- a preset loads → `extras.loads` moves (the SDK has no load hook; ✅
  `presets.h`), every value is pushed to the engine again and the page
  reloaded. At boot the same path pushes all twenty-five.

**TRACK select on both pages.** `g_track` is always read from the PLAY
page's knob, the one J6 modulates. The TRACK page's P6 writes through: each
frame the showing page's stored P6 is copied to the other, where the same
physical position catches at once (✅ `InitCatch` proximity window, 0.005).

**Reaching it.** The Pager allows one navigation binding per button and B3
is the SETUP Shift, so the tap is arbitrated in the app the way `pager.h`
intends (`HoldUsed`): a B3 press under 300 ms that edited nothing and never
met B2 toggles PLAY ↔ TRACK with `GoToPage()`. The Shift engages on the
press edge, so ⚠️ SETUP shows on the rings for the length of the tap. The
B3 pair says where a release lands: dim violet PLAY, amber TRACK.

Not on this page and why: **loop length**. `t<n>_len16` is sixteenths of a
*measure*, capped at one measure (✅ `track_setlen16`), so a knob on it
would cut any longer loop to a bar; halving a loop (½ ¼ ⅛ of what was
recorded) wants a small engine parameter, upstream. SPEED takes P1 when the
engine has it, and ONE-SHOT moves to a later page with SEED; a HEADS page
follows. Plan: `~/tim-os/docs/alchemy-loopex-integration.md`.

### Settings (B2+B3 held 2 s)

Page 0: brightness (SDK, P1), **DRY INPUT** Through / Muted (P2, `monitor`),
preset slot + action (SDK, P3/P4), **CLOCK RATIO** ÷2 / =1 / ×2 (P5, what
one pulse on J3 is worth). Page 1 **FIRMWARE**: the SD picker
(smack-alchemy DESIGN.md § 4, unchanged).

### Buttons

| Button | Gesture | Does |
|---|---|---|
| **B1 REC / PLAY** | tap | the RC's track button: record → play → overdub → play … (`t<n>_btn`) |
| | hold > 0.6 s | undo / redo the last recording or overdub (`undo`; the pair lights white when a hold has reached it) |
| | hold > 2 s | clear the track (`t<n>_clear`, fires while held) |
| **B2 STOP** | tap | stop the track (`t<n>_stop`) |
| | hold > 0.6 s | ALL start / stop (`all_btn`, fires while held) |
| **B3 SETUP / TRACK** | tap < 0.3 s | PLAY ↔ TRACK page |
| | hold | the SETUP page |
| B2 + B3 | hold 2 s | Settings (SDK). B2 stands down while B3 is held, so the chord never fires ALL and its release stops nothing; a B3 press that met B2 is never a tap |

Taps act on release so a hold never fires the tap first; the cost is a
release-time delay on the first loop's downbeat, which the grid then
carries consistently.

### The clock (J3)

The engine's grid: a running MIDI clock wins; otherwise the first finalized
loop anchors the bar (`grid_unit`); otherwise the tempo. Unclocked, the
first loop is NOT quantized (✅ `schedule()`: quantize needs a playing base
track or a running clock), so it is free length, and `finalize_record`
divides it by the TEMPO's bar to find how many bars it was. That is the
RC-505 with no rhythm: the first loop sets the bar, TEMPO says how many.

So the adapter's free-run ticks (which Smack wants) must NOT reach the
engine: `emit_to_engine` forwards bytes only after a pulse has arrived on
J3, and when the adapter loses the train (`clk_locked` falls) the callback
sends one MIDI Stop and stops forwarding; the engine then free-runs on its
own grid again (`follow` is 0, so Stop pauses nothing). ⚠️ Unheard. A
single pulse with no second one leaves the feed on at the free-run tempo
until the adapter's idle timeout; harmless, and a trigger into J3 is not a
use this firmware offers.

### LEDs

| Where | Shows |
|---|---|
| LEVEL rings 1–5 | **the RC-505's five track displays**: the level as an arc in the state colour — red recording, green playing, yellow overdubbing, white stopped, near-dark empty — blinking while an action waits for the bar line, with a white playhead pip once around per loop pass. A custom ring (colour follows state), so the SDK's catch pip is not drawn on these five |
| TRACK ring (PLAY) | 5-zone selector |
| TRACK ring (TRACK page) | five dots in the five tracks' state colours, the selected one bright, the rest at a quarter, blinking while pending; an empty selected track shows the selector blue |
| B1 pair | the selected track's state colour, blinking while pending; white once a hold has reached the undo threshold and undo is available |
| B2 pair | CLOCK: blue locked to J3, grey free-running; **red = callback over 80%** |
| B3 pair | where a release lands: dim violet PLAY, amber TRACK |
| P1 ring, first 2.5 s | last session's worst CPU load: dim blue = no data; green < 75%, amber < 90%, red = it did not fit |

The rings read `tstates`, `tpend`, `tpos` and `undo_avail` once per frame.

### CV (J3–J8 = CV 0–5)

| Jack | Role |
|---|---|
| J3 | **CLOCK** — raw ADC in the callback, magnitude-thresholded (smack-alchemy's reader), one pulse per quarter at ratio =1 |
| J4 | **REC** trigger: the selected track's button, from a sequencer |
| J5 | **STOP** trigger: the selected track's stop |
| J6 | → TRACK select |
| J7 | → LEVEL 1 |
| J8 | → LEVEL 2 |

Triggers are edge-detected in the callback and consumed at the 1 ms poll,
so `mark_set_param` never runs on the audio thread. Provisional routing;
the SDK's runtime CV routing through Settings is the v0.2 answer.

## 4. Persistence

The SDK's **Presets** manage the pager (all three pages), Settings
(brightness, dry input, ratio) and `MarkExtras` {`cpu_peak`, `trk[5][5]`:
the TRACK page for every track}. Slot 0 is the working state, **autosaved**
5 s after the last SETUP / TRACK / Settings / extras change with hands off
the buttons and the picker idle. Loop audio is never saved (the Move's
sessions need a directory and a thread; § 1). `MarkExtras` carries its own
schema tag ('MRK'), so a slot 0 written by Smack or Belt on the same module
fails the slot's schema gate (✅ `presets.cpp`: XOR of every managed
component) and lands in first-boot defaults.

**The TRACK page changed the blob twice over:** the Pager's hash covers its
page count (✅ `pager.cpp` `SchemaHash`: pages × pots) and the extras tag
moved to 0x02. A slot 0 written by v0.1.0 therefore fails the gate and the
first boot of this build comes up in first-boot defaults. That is the gate
working, not a regression. ✅ `test/test_extras.cpp`: round trip, one bad
byte resets that track only, a garbage peak is refused, loads are counted.

## 5. Budgets (✅ from the 2026-09-21 build, TRACK page in)

| Region | Used | Of | v0.1.0 |
|---|---|---|---|
| SRAM (code + data, BOOT_SRAM) | 353,568 B | 480 KB (72 %) | 349,472 B |
| DTCM | 76,300 B | 128 KB | 74,676 B |
| SDRAM | 65,077,184 B | 64 MB (97 %) | unchanged |
| `.bin` | 345,644 B | | 341,552 B |

CPU: ⚠️ unmeasured. Five int16 loop reads and writes per sample from
SDRAM plus the built-in FX is far below what Smack asks of the same
silicon; the boot bar will say.

## 6. Open questions

1. ⚠️ Does anything play? First rack / Erica-case power-on answers it.
2. ⚠️ The clock feed's lock / unlock behaviour against a real clock.
3. ⚠️ Trigger and clock jack polarity (magnitude-thresholded).
4. ⚠️ Whether an inert session/FX code path is ever reached at runtime and
   what it does with a −1 (it should log `io_error` and stop).
5. The SDK is beta; the pinned SHAs are what this was built against.
6. ⚠️ The TRACK page, all of it: whether a 300 ms B3 tap feels right, how
   distracting the SETUP flash on the rings is during it, whether P6 really
   stays live across the page change, and what the six insert effects cost
   on five tracks at once (the boot bar will say).
7. ⚠️ J6 moving the selection while a TRACK-page pot is in hand: the page
   reloads and the pot has to catch the new track's value. Intended; unfelt.
