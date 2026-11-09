# RHYTM-SEC-1

Standalone firmware for the M-VAVE FM-1 that turns it into a generative drum machine and bass synth.

It uses the drums and bass from deamonSLINKY's [PERF-1](https://github.com/deamonSLINKY/PERF-1). Patterns are drawn from each genre's hit probabilities instead of fixed presets, so they keep evolving while you play. It's built for working up ideas rather than full songs.

<p>
<img src="docs/screens/02-house-playing.png" width="240" alt="House playing: the step grid, with the drum knob page">
<img src="docs/screens/07-break.png" width="240" alt="Dubstep in a break: steps that don't play are dimmed">
<img src="docs/screens/05-sound-page.png" width="240" alt="The sound page with the snare selected">
</p>

- 19 styles, with density, complexity and evolve controls
- Bass that follows a root note and scale, with chord moves
- Sections, fills, breaks and builds, all shown on the screen grid
- Per-drum level, tune (in semitones), decay and kit
- A DJ-style low/high-pass filter
- MIDI clock sync over USB, including the DAW's song position
- USB audio with stems: a stereo master plus mono bass, kick, snare + clap, and hats + perc, always at full level whatever the MASTER knob is set to

The full control reference is in [docs/cheatsheet.html](docs/cheatsheet.html).

## Install

1. Download **`rhytm-sec-1-1.1.fwsc`** from the [latest release](../../releases/latest).
2. Install it with your FM-1 firmware flash tool.

The FM-1 starts straight into RHYTM-SEC-1. To go back to another firmware, flash that firmware the same way.

The firmware identifies itself as `FM-1_947` to installers.

**Under the hood:** the firmware is built on [fm1-dock](https://github.com/joakimeriksson/fm1-dock)'s platform as a single-app build, with RHYTM-SEC-1 built in. You don't need fm1-dock or its web page to use it. Two fm1-dock features are still there:
- Holding **HOME** for 1.5 s opens a small menu with the master EQ and MIDI options. There's also a test sine. Pick RHYTM-SEC-1 there to go back.
- Holding **OCT+** at power-on starts in safe mode with the test sine only.

Bluetooth MIDI is left out of this build to make room for the module.

## Screens

| | |
|---|---|
| ![Stopped](docs/screens/01-stopped.png) | ![Playing](docs/screens/02-house-playing.png) |
| Stopped: style, key, tempo, the pattern and the drum knobs | Playing: steps that play light up, the white column is the current step |
| ![Generator page](docs/screens/03-page-2.png) | ![Bass page](docs/screens/04-page-3.png) |
| Tap SEL for the generator page: evolve, how often, fills, ghosts | The bass page: line, cutoff, resonance, sound |
| ![Sound page](docs/screens/05-sound-page.png) | ![New style](docs/screens/06-style-changed.png) |
| The sound page: press a drum's key to pick it (here the snare) | A new style changes at once |
| ![Break](docs/screens/07-break.png) | ![Filter](docs/screens/08-filter-fill.png) |
| A break: the steps that drop out are dimmed | The DJ filter (LP36 at top right) during a fill |

The screens are drawn by the module's own code on a computer, through fm1-dock's text and graphics code (`tools/shots.sh`). The top line is the one the firmware shows; its CPU figure here is a placeholder.

## USB audio channels

| Input | Signal |
|---|---|
| 1–2 | Master (stereo) |
| 3 | Bass |
| 4 | Kick |
| 5 | Snare + clap |
| 6 | Hats + perc |

The stems are mono and follow mutes and the DJ filter. On Windows, a DAW needs WASAPI, FlexASIO or ASIO4ALL to see all six channels.

## Build from source

You need what fm1-dock's build needs:
- Docker on macOS
- the JieLi toolchain (`fm1-dock/tools/get_toolchain.sh`)
- the AC79 SDK (see fm1-dock's README)

```bash
git clone --recursive https://github.com/hamperj/rhytm-sec-1 && cd rhytm-sec-1
./build.sh                      # -> build/rhytm-sec-1-1.1.fwsc (VERSION=X.Y ./build.sh for another number)
```

How the build works:
- `fm1-dock/` is a submodule pinned to fm1-dock 0.7.1 (commit 3ec04e5).
- `build.sh` applies `patches/fm1-dock-host.patch`, copies `rhytmsec1/` into fm1-dock's modules, and builds fm1-dock as a solo app with that one module.
- The release `.fwsc` comes from exactly this script.

The patch holds every change to fm1-dock itself:
- the UC25HQ80 flash chip ID, for newer FM-1s
- the stems interface for modules (`hb_host_t.stem`)
- the USB audio layout and the song position forwarding
- the `--uac-stems` and `--uac-full` build options
- a fix to `tools/hbmod.py` for modules with no RAM code

### Engine tests (on your computer)

These run every genre and check evolve, breaks, the filter, stems and the MIDI clock:

```bash
cc -O2 -ffp-contract=off -Irhytmsec1/perf rhytmsec1/perf/test_engine.c -o build/rhytmsec1_test
build/rhytmsec1_test /tmp/rhytmsec1-wavs
```

### Screenshots

`tools/shots.sh` renders `docs/screens/` (needs Pillow).

### Regenerating the style data

`rhytmsec1/perf/styles.h` is generated from PERF-1 (commit 848b3c6) by `gen/dump.c`. Run it with PERF-1 checked out next to this repo as `perf1`:

```bash
cd gen
clang -O0 -w -DEN_HOST -I../perf1/firmware/src/dsp -I../perf1/firmware/src dump.c -o dump -Wl,-undefined,dynamic_lookup
./dump > ../rhytmsec1/perf/styles.h
```

### As an fm1-dock module

If you'd rather run it alongside other synths on fm1-dock, the release also has `rhytmsec1.dock`. Add it from fm1-dock's module page. On stock fm1-dock the USB audio is plain stereo and follows MASTER.

## Status

Tested with the native engine tests and on one FM-1 (a UC25HQ80 unit) as an fm1-dock module. The single-app firmware itself hasn't been run on hardware yet. The CPU headroom while recording stems hasn't been measured on hardware.

## Licence

GPL-3.0-only, as are PERF-1 and fm1-dock. See [LICENSING.md](LICENSING.md) for where each part comes from.

"M-VAVE" and "FM-1" are trademarks of their owners. This project isn't affiliated with or endorsed by them.
