# RHYTM-SEC-1

A generative drum and bass module for the M-VAVE FM-1, running on [fm1-dock](https://github.com/joakimeriksson/fm1-dock).

It uses the drums and bass from deamonSLINKY's [PERF-1](https://github.com/deamonSLINKY/PERF-1). Patterns are drawn from each genre's hit probabilities instead of fixed presets, so they keep evolving while you play. It's built for working up ideas rather than full songs.

- 19 styles, with density, complexity and evolve controls
- Bass that follows a root note and scale, with chord moves
- Sections, fills, breaks and builds, all shown on the screen grid
- Per-drum level, tune (in semitones), decay and kit
- A DJ-style low/high-pass filter
- MIDI clock sync over USB, including the DAW's song position
- USB audio stems (with the bundled firmware): a stereo master plus mono bass, kick, snare + clap, and hats + perc, at full level whatever the MASTER knob is set to

The full control reference is in [docs/cheatsheet.html](docs/cheatsheet.html).

## Install

Download both files from the [latest release](../../releases/latest):

1. **`fm1-dock-0.7.1-rhytmsec.fwsc`**: install with your usual FM-1 firmware flash tool. This is fm1-dock 0.7.1 with:
   - support for the newer UC25HQ80 flash chip
   - the 6-channel USB audio layout
   - song position over USB MIDI
   - full-level USB audio
   - FiMba-1 built in in place of Scintilla
2. **`rhytmsec1.dock`**: add it from fm1-dock's module page, like any other module.

The module also runs on stock fm1-dock 0.7.1. There, USB audio is plain stereo and follows MASTER, and a DAW's Continue restarts from the top instead of the song position.

If you used the earlier PEEP-1 build, remove it from the module list. RHYTM-SEC-1 is a new name, so fm1-dock treats it as a separate module and doesn't carry over PEEP-1's saved settings.

## USB audio channels (bundled firmware)

| Input | Signal |
|---|---|
| 1–2 | Master (stereo) |
| 3 | Bass |
| 4 | Kick |
| 5 | Snare + clap |
| 6 | Hats + perc |

The stems are mono and follow mutes and the DJ filter. On Windows, a DAW needs WASAPI, FlexASIO or ASIO4ALL to see all six channels.

## Build from source

You need fm1-dock's build setup: Docker, the JieLi toolchain and the AC79 SDK (see fm1-dock's README).

```bash
git clone https://github.com/joakimeriksson/fm1-dock && cd fm1-dock
git checkout 3ec04e5
git apply ../rhytm-sec-1/patches/fm1-dock-host.patch
cp -R ../rhytm-sec-1/rhytmsec1 modules/
python3 tools/hbmod.py modules/rhytmsec1                 # -> build/modules/rhytmsec1.dock
./build.sh --release 0.7.1-rhytmsec --modules cs1,fimba,obervave --uac-stems 4 --uac-full
```

The firmware options:
- `--uac-stems 4` turns on the six-channel USB audio.
- `--uac-full` takes USB audio before MASTER.

`patches/fm1-dock-host.patch` holds every change to fm1-dock itself:
- the flash chip ID
- the stems interface for modules (`hb_host_t.stem`)
- the USB audio layout and the song position forwarding
- the build options
- a fix to `tools/hbmod.py` for modules with no RAM code

### Engine tests (on your computer)

These run every genre and check evolve, breaks, the filter, stems and the MIDI clock:

```bash
cc -O2 -ffp-contract=off -Imodules/rhytmsec1/perf modules/rhytmsec1/perf/test_engine.c -o build/rhytmsec1_test
build/rhytmsec1_test /tmp/rhytmsec1-wavs
```

### Regenerating the style data

`rhytmsec1/perf/styles.h` is generated from PERF-1 (commit 848b3c6) by `gen/dump.c`. Run it with PERF-1 checked out next to this repo as `perf1`:

```bash
cd gen
clang -O0 -w -DEN_HOST -I../perf1/firmware/src/dsp -I../perf1/firmware/src dump.c -o dump -Wl,-undefined,dynamic_lookup
./dump > ../rhytmsec1/perf/styles.h
```

## Status

Tested with the native engine tests and on one FM-1 (a UC25HQ80 unit). The CPU headroom while recording stems hasn't been measured on hardware.

## Licence

GPL-3.0-only, as are PERF-1 and fm1-dock. See [LICENSING.md](LICENSING.md) for where each part comes from.

"M-VAVE" and "FM-1" are trademarks of their owners. This project isn't affiliated with or endorsed by them.
