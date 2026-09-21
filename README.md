# mark-alchemy

**Mark** — the RC-505-style five-track live looper — for the
[Hermetic Modular Alchemy Lab V2](https://hermeticmodular.com/modules/alchemy-lab),
built on the [Alchemy SDK](https://github.com/hermetic-modular/alchemy-sdk).

Same C engine as [schwung-mark](https://github.com/timncox/schwung-mark) for
the Ableton Move, recompiled for six pots on three pages, three buttons, six
CV jacks and 102 LEDs. Five loop tracks of 55 seconds each in the module's
SDRAM; pick a track, tap to record, tap to play, tap to overdub; the five
rings are the five tracks' displays. With nothing on the clock jack the
first loop you record is the bar; with a clock, everything snaps to it.
Tap B3 for the selected track's own page: one-shot, reverse, an insert
effect and its amount, pan.

**Status: builds and passes its native tests. Nothing has been heard
yet.** See `DESIGN.md` for the control map, what is verified and what is
not. The Move build's sessions and external FX modules are not on this
panel.

## Build

Requires `arm-none-eabi-gcc`, `make`, `dfu-util`.

```sh
git clone --recurse-submodules <this repo>
cd mark-alchemy
make libdaisy        # once
make                 # build/mark_alchemy.bin
make test            # native tests, no hardware
```

`MARK_SECONDS=<n>` on the make line changes the per-track capacity; the
native test then proves that capacity fits the SDRAM pool, which the
engine's own fallback cannot do on this allocator (see `src/versio_alloc.h`).

## Flash

Front USB-C with the factory bootloader: hold B3 during the ~2 s boot
window (rings spin a warm-white comet), the rings switch to a slow breathe,
then:

```sh
make program-dfu
```

or the [web programmer](https://hermeticmodular.com/program). Once this
firmware is running on the front port, `make program-live` reboots and
flashes without touching the module.

Bench setup (Hermetic's intdfu bootloader on the Seed's micro-USB): same
gesture on the micro-USB port; build with `BENCH_USB=1` if you want HostLink
there too.

## Firmware on the SD card

Put `.bin` files built for the Alchemy Lab bootloader in a folder named
`alchemy` on a FAT32 card. Hold B2+B3 for two seconds, tap B1 to the
Firmware page, pick a file with the top-left pot, then turn the top-right
pot down and all the way up. It writes, verifies, and reboots into the
chosen firmware. Every firmware in this family (Smack, Mark, Belt) carries
the same page, so the card is the module's library and any of them can
hand the module to any other.

## The site

`docs/index.html` is the operation manual, a single file meant for GitHub
Pages from `main:/docs`. Its panel drawing is generated from the SDK's KiCad
front-panel template by `tools/emit_panel_geometry.py`; run it with
`--check` before publishing and `--write` after the template changes, and
never hand-edit the coordinates. The repository URL the page links to is one
constant at the top of the file.

## Files on the card over USB

`tools/hostlink-fs.mjs` speaks HostLink's filesystem block, so images reach
the card without pulling it:

```sh
node tools/hostlink-fs.mjs info
node tools/hostlink-fs.mjs ls /alchemy
node tools/hostlink-fs.mjs put build/mark_alchemy.bin /alchemy/mark_alchemy.bin --overwrite
```

The picker lists files in on-disk order, one dot each, so the order you
upload them is the order the FILE pot walks.

## Family

`smack-alchemy`, `belt-alchemy` and this repository share, by copy, the
picker, the SDRAM allocator, the clock adapter, the linker script, the card
tool and the shape of the Makefile and main file. A fix in one belongs in
all three.

## Not a Hermetic Modular product

Custom firmware. Hermetic Modular did not write, test or endorse it; ask
here, not there.

## License

MIT (this firmware). The vendored engine is Tim Cox's; see the file headers.
