# Percolator

A 23-track percussion sequencer built on the PhISEM Shakers models of the Synthesis ToolKit (STK), with per-step accents, locks, probability, ratchets, songs, MIDI sync, effects and screen-reader support.

Custom firmware for the **M-VAVE FM-1** pocket synthesizer, version 1.0
([release](https://github.com/rev3rend/percolator/releases/tag/v1.0)). Every milestone of the spec is built, tested
on the host and in the browser emulator, and running on a real FM-1. The full description: [docs/SPEC.md](docs/SPEC.md).

**Try it in your browser:** [the Percolator emulator](https://claude.ai/artifact/MTT231c1nvNQsW93vnyYGh): press
*Switch on*, then play the pads (or your computer keyboard: <kbd>Z</kbd>–<kbd>/</kbd> and <kbd>Q</kbd>–<kbd>P</kbd>);
PLAY runs the factory pattern, PRESETS picks another.

**Install it on your FM-1:** [the web installer](https://rev3rend.github.io/percolator/webapp/installer/) (Chrome or
Edge, over USB), or `python3 tools/fm1_install.py percolator-1.0.fwsc` (needs `mido` and `python-rtmidi`). Read
[Safety](#safety) first. The way back to the official firmware is in both.

## What it does

- **23 tracks, one per Shakers instrument:** maraca, cabasa, sekere, tambourine, sleigh bells, bamboo chimes, sand
  paper, coke can, sticks, crunch, big and little rocks, the mug and its five coins, guiro, wrench, water drops and
  tuned bamboo chimes, each with energy, decay, objects and frequency on the knobs, level and pan.
- **A step sequencer:** 16 steps per track, accents, parameter locks, probability, ratchets (x2..x4, flat, up, down),
  conditions (A:B, FILL, FIRST), nudge, per-track length and clock divide, swing, note repeat, live record, mute and
  solo; a GRID of all 23 tracks and a TRACK view of one.
- **Patterns, kits, songs:** 32 named patterns (24 factory ones in four styles: hand percussion, ambient, metal and
  industrial, generative), 12 named kits (8 factory), 8 songs (4 factory tours); a pattern brings its kit; FACTORY
  RESTORE.
- **Effects on the mix:** filter and drive (before or after the rest), chorus, ping-pong delay that follows the
  tempo, a short room reverb, a soft limiter; saved with the kit.
- **MIDI:** notes and CCs in over USB and TRS; clock in and out (start, stop, continue, song position); each hit as
  a note, the knobs as CCs and the pattern as Program Change out over USB.
- **Accessible:** speaks through the [fm1-reader](https://github.com/charlesvestal/fm1-reader) screen reader: connect
  the FM-1 by USB, open <https://charlesvestal.github.io/fm1-reader/> in Chrome or Edge, and every screen, track,
  step, knob, pattern and message is spoken in full words. The FM-1 stays silent on USB until a reader asks. A
  MIDItoSpeech device file for [synthaccess](https://github.com/IDMNYU/synthaccess) speaks what it sends:
  [docs/synthaccess/Percolator.json](docs/synthaccess/Percolator.json).
- Shows up over USB as "Percolator FM-1".

The sound models are Perry Cook's PhISEM, from the `Shakers` class of the
[Synthesis ToolKit (STK)](https://github.com/thestk/stk) by Perry R. Cook and Gary P. Scavone (MIT-style licence:
[LICENSING.md](LICENSING.md)).

Built on the **FM-1 Firmware Kit**: the platform of [Felucca](https://github.com/hugelton/Felucca) (Leo Kuroshita,
Hügelton Instruments) as [M-Vave-OP1](https://github.com/janschulten/m-vave-op1) runs it.

```
firmware/
  hal/        header-only HAL: every register access lives here          (platform, from Felucca)
  loader/     the update loader that writes a new app                     (platform, from Felucca; do not touch)
  src/        boot, main loop, audio ISR, USB-MIDI, update entry, flash,  (platform)
              display and drawing, panel map, MIDI helpers, record store
  app/        app.h: the contract; app.c and its files: this firmware
docs/         SPEC.md (what the firmware does), PROJECT.json (how the repository was set up)
tests/        host tests: the platform's update path, input, USB, store; the app on a simulated FM-1
web/          the web installer (with "Return to official V15") and the browser emulator
tools/        build, package, install, font generators
.claude/      Claude Code skills: fm1-platform (rules, budgets, tests, safety), graphify (code graph)
```

## Build, test, try

```sh
sh .claude/skills/fm1-platform/scripts/setup_env.sh --emsdk   # once: toolchain, SDK files, Emscripten
./build.sh                                                     # build/firmware.fwsc
tests/run_tests.sh                                             # host tests, screenshots in build/shots/
sh web/emu/build.sh && python3 -m http.server -d build/emu 8000   # the emulator: http://localhost:8000/
```

Details: [BUILDING.md](BUILDING.md). With Claude Code, the `fm1-platform` skill holds the rules for every change.

## Safety

The FM-1 has **one firmware bank and no recovery button**. A firmware that does not start needs extra hardware to
recover. So the working order is always **host tests → browser emulator → careful install**, never the reverse.

- The platform keeps the ways back: the boot guard sends the unit to the chip's boot mode (`WL80UBOOT`) after two
  failed boots; OCT− and OCT+ held 5 s does the same on purpose; the update entry accepts a new package over USB
  while the firmware runs. Do not remove or bypass them, and do not change `firmware/loader/`, the boot guard, the
  package identity (`FM-1_900`) or the update path.
- Until this firmware has run on a real FM-1, have the recovery hardware at hand for an install:
  [FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter) (a Seeed XIAO RP2040 on the FM-1's USB lines,
  which reads and writes the flash from the boot mode). Back up the flash first.
- Way back while a firmware still starts: `python3 tools/fm1_install.py --stock`, the web installer's "Return to
  official V15", or M-VAVE's own updater.

## Licence

GPL-3.0-only, as Felucca. Copyright (C) 2026 R. Luke DuBois; the platform: Leo Kuroshita (Hügelton Instruments) and
the M-Vave-OP1 contributors. Keep the licence headers, credit Felucca (and M-Vave-OP1) wherever you publish, and give
your users the source. Details and third-party parts: [LICENSING.md](LICENSING.md).

M-VAVE and FM-1 are trademarks of their respective owners. This firmware is not affiliated with or endorsed by them,
nor by Hügelton Instruments.
