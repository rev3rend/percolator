# Percolator for MIDItoSpeech

`Percolator.json` is a device file for **MIDItoSpeech**, part of [synthaccess](https://github.com/IDMNYU/synthaccess)
(IDM NYU): software on a computer that speaks what a synthesizer sends over MIDI. With it, MIDItoSpeech says what
you change on the FM-1, e.g. "decay 80", "frequency 64", "pattern 7, rain".

## Using it

1. Connect the FM-1 by USB. On GLOBAL 2 (press GLO twice), keep **MIDI OUT** on: that is what sends your changes.
2. Copy `Percolator.json` into MIDItoSpeech's `devices` folder.
   - Web version: also add the file name to `WebMIDI/devlist.txt` (or run `makedevlist.sh`), or load it with the
     page's file control.
   - Max version: open it from the patch.
3. Choose the FM-1's MIDI port, "Percolator FM-1", as MIDItoSpeech's input (macOS may keep showing a name it saw
   before for the same USB device). Choose the FM-1's MIDI channel (GLOBAL page, MIDI CHANNEL); OMNI, the default,
   sends on **channel 1**.

## What it covers

Everything MIDI OUT sends that MIDItoSpeech reads (`firmware/app/sound.c`, `ctl_out`):

| Message | Control | Spoken |
| --- | --- | --- |
| CC 2 | KNOB 1: the selected track's ENERGY, 0..127 | "energy 100" |
| CC 11 | KNOB 2: its DECAY, 0..127 | "decay 80" |
| CC 4 | KNOB 3: its OBJECTS, 0..127 | "objects 64" |
| CC 1 | KNOB 4: its FREQ, 0..127 | "frequency 64" |
| Program Change 0..31 | the pattern starting (P01..P32) | "pattern 7, rain" |

- The four sounds are sent when a knob (or the SOUND page) changes them, as the numbers the screen shows. Selecting
  another track or kit sends nothing, and a CC that came in over MIDI is not sent back.
- The 24 factory patterns are spoken by name; P25..P32 by number. A pattern you rename on the FM-1 is still spoken
  by its slot's factory name (the file cannot know your names).
- Not spoken: each track's hit (a note: base note + track) and the MIDI clock. The FM-1's screen reader
  (fm1-reader) says the rest: tracks, steps, locks, pages, patterns and messages, in full words.

## Keeping it current

The file is generated from the firmware's own tables and the screen reader's wording, so it matches the FM-1's
screen and what its screen reader says:

```sh
cc -O1 -Ibuild/gen -Ifirmware/src -Ifirmware/app -Itests -o build/host/gen_synthaccess tools/gen_synthaccess.c -lm
build/host/gen_synthaccess > docs/synthaccess/Percolator.json
```

`tests/run_tests.sh` fails if the committed file is not what the firmware generates now, and checks the CC numbers
against the ones the firmware sends and the patterns against the factory set (`tests/synthaccess_check.py`).
