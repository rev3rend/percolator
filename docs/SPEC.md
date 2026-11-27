# Percolator: specification

Status: from the setup interview, 2026-11-26; milestone 1 (the skeleton) built 2026-11-26.

## What it is

Percolator is a 23-track percussion sequencer for the M-VAVE FM-1, in the spirit of the Roland TR-727, built on
Perry Cook's PhISEM physical models as implemented in the `Shakers` class of the Synthesis ToolKit (STK, Perry R.
Cook and Gary P. Scavone, https://github.com/thestk/stk). Each of STK's 23 Shakers instruments (Maraca through Tuned
Bamboo Chimes) is one track. Every track has the class's four parameters: shake energy, system decay, number of
objects and resonance frequency. They are set per track on the knobs and can be overridden per step as accents and
parameter locks. The sound comes first: a faithful port of the models, playable from the keys and MIDI. Then comes a
16-step sequencer on the white keys with modern options (accents, probability, ratchets, conditional trigs, nudge,
per-track length, swing), 16 stored patterns chained into songs, MIDI clock in and out, and a send-effects section
(delay, reverb, chorus, master filter and drive). The firmware is accessible: it speaks through the fm1-reader
screen reader, and it ships a MIDItoSpeech sidecar for IDM NYU's synthaccess. The GitHub repository publishes the
browser emulator and a web installer on GitHub Pages, as Solinaesque and Looching 2026 do.

## The 23 tracks

Track n plays STK Shakers type n − 1 and is played by key n (F3 = track 1). The short names are shown on the grid.

| # | Key | STK type | Name | Short | Resonators |
| --- | --- | --- | --- | --- | --- |
| 1 | F3 | 0 | Maraca | MAR | 1 |
| 2 | F#3 | 1 | Cabasa | CAB | 1 |
| 3 | G3 | 2 | Sekere | SEK | 1 |
| 4 | G#3 | 3 | Tambourine | TAM | 3 |
| 5 | A3 | 4 | Sleigh Bells | SLB | 5 |
| 6 | A#3 | 5 | Bamboo Chimes | BMB | 3 |
| 7 | B3 | 6 | Sand Paper | SND | 1 |
| 8 | C4 | 7 | Coke Can | COK | 5 |
| 9 | C#4 | 8 | Sticks | STX | 1 |
| 10 | D4 | 9 | Crunch | CRN | 1 |
| 11 | D#4 | 10 | Big Rocks | BRK | 1 |
| 12 | E4 | 11 | Little Rocks | LRK | 1 |
| 13 | F4 | 12 | Next Mug | MUG | 4 |
| 14 | F#4 | 13 | Penny + Mug | PNY | 7 |
| 15 | G4 | 14 | Nickel + Mug | NKL | 7 |
| 16 | G#4 | 15 | Dime + Mug | DIM | 7 |
| 17 | A4 | 16 | Quarter + Mug | QTR | 7 |
| 18 | A#4 | 17 | Franc + Mug | FRC | 7 |
| 19 | B4 | 18 | Peso + Mug | PES | 7 |
| 20 | C5 | 19 | Guiro | GUI | 2 (ratchet) |
| 21 | C#5 | 20 | Wrench | WRN | 2 (ratchet) |
| 22 | D5 | 21 | Water Drops | WTR | 3 (sweeping) |
| 23 | D#5 | 22 | Tuned Bamboo Chimes | ANG | 7 |

The four keys above the pads are performance keys: E5 MUTE, F5 SOLO, F#5 ACCENT, G5 SONG.

## Controls

Two modes, toggled by SEQ: **PLAY mode** (keys are pads) and **STEP mode** (white keys are the 16 steps). SONG
mode (G5) is a third screen on top of either.

### Keys

| Control | PLAY mode | STEP mode |
| --- | --- | --- |
| Keys 1..23 (F3..D#5) | play track n (fixed energy, or ACCENT energy while F#5 held); also select that track for the knobs; while REC and running, record the hit on the nearest step (quantized) | white keys (the 16 white keys F3..G5, left to right) = steps 1..16 of the selected track: tap toggles the trig; hold + encoders edits the step (below). Black keys: no step function (they still play their track while EDIT is not held) |
| EDIT held + any key 1..23 | select that track (no sound) | select that track (no sound) |
| E5 (MUTE) held + pad | toggle that track's mute | E5 is step 14; EDIT + E5 toggles the selected track's mute |
| F5 (SOLO) held + pad | toggle that track's solo (several tracks can be soloed) | F5 is step 15; EDIT + F5 toggles the selected track's solo |
| F#5 (ACCENT) held | pads and recorded hits are accented | F#5 is black, so it stays ACCENT: held + tap a step key = trig with accent (or toggles the accent of a set trig) |
| G5 (SONG) | open / close the SONG screen | G5 is step 16; EDIT + G5 opens the SONG screen |

The 16 white keys F3..G5 include E5, F5 and G5, so in STEP mode those three are steps 14..16 and their performance
functions move behind EDIT. Mute and solo are also on the TRACK 2 page (ENV twice).

MUTE and SOLO count as held only when they went down before the pad (a chord or a forearm across the keys that
includes E5 / F5 toggles nothing: found on the unit, where a forearm soloed tracks and silenced the sequence).
PLAY mode: E5 held, then F5 (or F5, then E5) turns every mute and solo off. The state is always visible: the header
shows SOLO (any track soloed) or MUTE (any muted); the footer and the TRACK view say MUTED, SOLO or
"SILENT: SOLO ON" for the selected track, and its trigs are drawn dim in both views.

### Holding a step (STEP mode)

| Control while a step key is held | Effect on that step |
| --- | --- |
| KNOB 1..4 | parameter locks: energy, decay, objects, resonance frequency (turned all the way to the left, past 0, back to "--" = no lock; EDIT clears all four at once). Turning a step off and on keeps its locks |
| SELECT | probability 0..100 % (steps of 5) |
| ALGORITHM | ratchet 1..4 (and the ratchet ramp: flat, up, down) |
| PRESETS | condition: always, 1:2, 2:2, 1:3, 2:3, 3:3, 1:4 .. 4:4, FILL, NOT FILL, FIRST, NOT FIRST |
| OCT− / OCT+ (tap) | nudge −/+ 1/12 of a step (−5..+5) |
| EDIT (press) | clear every lock of that step (its trig and other settings stay) |
| F#5 (ACCENT) tapped | toggle that step's accent |
| Two step keys held | the second copies the first step's settings (a later convenience, milestone 9) |

### Buttons

| Button | Action |
| --- | --- |
| PLAY | start / stop; held + PLAY again within 0.3 s (double tap) = restart from step 1 |
| REC | live record on / off (pads into the current pattern while running, quantized; with nudge from the timing once nudge exists) |
| SEQ | PLAY mode ↔ STEP mode |
| EDIT | held: the next key selects a track; held + SAVE twice within 2.5 s = clear the selected track's steps |
| HOME | the main screen; pressed again on it, toggles GRID (all tracks) ↔ TRACK (selected track zoomed) |
| SAVE | the SAVE screen: save the pattern / kit / song to a slot (PRESETS picks the slot, SAVE confirms, HOME cancels) |
| FX | the FX page(s) |
| ENV | the TRACK page (the selected track's mix and length) |
| LFO | the RANDOM page |
| SCL | the TIME page |
| GLO | the GLOBAL page |
| ARP | held: FILL (FILL conditions true while held); held + pad: note repeat of that pad at the ARP rate (TIME page) |
| OCT− / OCT+ | previous / next pattern, queued to the end of the current pattern while running (immediate when stopped). Held together 5 s: the platform's recovery (taken) |

### Encoders

| Encoder | On HOME (GRID / TRACK) | On a page |
| --- | --- | --- |
| SELECT | scroll the selected track (1..23) | move between the pages |
| ALGORITHM | tempo 40..240 BPM, 1 BPM a detent (the pattern stores half BPM: room for a finer control later) | as HOME |
| PRESETS | pattern select (1..16), queued while running | as HOME (on SAVE: the slot) |
| KNOB 1..4 | the selected track's energy, decay, objects, resonance frequency | the page's four parameters |

## Screens

All 240 × 240 in the kit's AMBER palette (dark brown-black background, cream text, amber theme, cyan accent; its
contrast is checked by `tools/gen_ui_palettes.py`): trigs in the selection amber, accented trigs in the brighter
theme amber, the playhead and the sounding marks cyan, muted tracks dim. Steps with a lock in magenta (#D648BA, the
complement of the amber and the cyan; accented #FF80E8), in both the GRID and the TRACK view; locks on a step that is
off show as a magenta stub (GRID) or a magenta edge (TRACK). The white dot marks probability, ratchet or a condition. Lazy drawing: each screen area has a
signature of what it shows and redraws only what changed; the GRID's playhead and sounding marks are sent cell by
cell (a 16th changes ~4 KB of pixels, not the ~90 KB grid). A 1.2 s splash at power-on shows the name (its letters
yellow, cyan, magenta in turn: the theme, the playhead's and the locks' colours), the version and the STK / Perry
Cook credit.

### HOME: GRID

- Header (top 20 px): pattern number (P01..P16, the queued one blinking beside it), tempo, play / stop / REC state,
  PLAY or STEP mode, SONG position when a song plays, MIDI clock source (INT / EXT) when external.
- Grid (23 rows × 16 columns, 8 px per row): a 12 px name does not fit an 8 px row, so the names go in pairs: the
  even track's name at the left and the odd track's at the right, both centred on the pair of rows (MAR left / CAB
  right on rows 1-2, and so on). Trigs amber, accented trigs brighter, steps with locks / probability / ratchets /
  conditions marked by a small dot, steps beyond the track's length blank. A cyan playhead per track (lengths
  differ), shown while running. A cyan mark left of a row while the track sounds. The selected track's row tinted and
  its name cyan; muted (or not soloed) rows dim; soloed names in the theme colour.
- Footer: the selected track's full name and its four values (E D O F).

### HOME: TRACK

The selected track zoomed: its name and STK type, 16 large step cells with all per-step data drawn (accent,
probability as a bar, ratchet count, condition as text, nudge as an offset, lock markers). While a step is held, its
values are shown in full. A level meter strip of all 23 tracks along the bottom.

### TRACK page (ENV)

The selected track: LEVEL (0..100), PAN (L50..R50), DELAY send, REVERB send (0..100); sub-page 2: LENGTH (1..16),
CLOCK DIVIDE (1/1, 1/2, 1/4, 2/1...), MUTE, SOLO.

### TIME page (SCL)

TEMPO, SWING (50..75 %), ACCENT amount (0..100: how much an accent adds to energy, and to the locked values), ARP
(note repeat) rate (1/8, 1/16, 1/32, 1/16T).

### RANDOM page (LFO)

HUMANIZE timing (0..10 ms), HUMANIZE energy (0..30 %), CHANCE BIAS (−50..+50 %: added to every step whose
probability is below 100 %; plain trigs stay solid; live since milestone 5), and RANDOMIZE (randomize the selected
track's trigs, with a DENSITY on KNOB 4). Details may change in the milestone.

### FX pages (FX)

FX 1: DELAY time (tempo-synced divisions or ms), FEEDBACK, DELAY TONE, DELAY level. FX 2: REVERB size, damping,
REVERB level, CHORUS depth (master). FX 3: master FILTER (low-pass ↔ high-pass, one knob centred = off), RESONANCE,
DRIVE, MASTER level.

### GLOBAL page (GLO)

MIDI channel (1..16, OMNI), base note (default 53 = F3), CLOCK source (INT / EXT / AUTO), TRS MIDI IN (on / off,
default on); GLOBAL 2: CLOCK OUT (on / off), NOTES OUT (on / off), and the CPU meter (read-only rows): CPU LOAD, the
audio ISR's average share of its 2.9 ms half (the platform's `audio_cpu_q8`, ~93 ms), and CPU PEAK, the worst single
half in the last 2..4 s (`audio_max_us`: what decides a crackle), with LATE n when halves have missed their deadline
since power-on (`felucca_dbg.late`: audible glitches). HOME's header rule is the same meter, always visible: the
average as a fill, the peak as a tick, cyan, red from 80 % or after a late half. (The emulator shows 0: it does not
measure; the screen-reader status comes with milestone 10.)

### SAVE

The thing to save (PATTERN / KIT / SONG), the slot (1..16, 1..4, 1..4) and whether it is used; SAVE again confirms,
HOME cancels. A message confirms "PATTERN 05 SAVED".

### SONG (G5)

A list of the selected song's chain (up to 64 entries of pattern + repeats 1..16), the selected entry highlighted.
SELECT moves through the entries (the last position, END, adds an entry with PRESETS), PRESETS sets the entry's
pattern, KNOB 1 its repeats, KNOB 2 right inserts a copy / left deletes, KNOB 3 LOOP or ONCE, KNOB 4 the song (1..4);
PLAY plays the song from the selected entry (milestone 8).

### Messages

A one-line overlay for 1.5 s: saves, pattern queued, clock source changes, "SOON" for features not yet built.

## Sound

- **Engine:** a port of STK's `Shakers` (PhISEM: a stochastic collision process exciting noise into a bank of
  two-pole resonators, with the per-instrument system decay, sound decay, gain, number of objects, resonance
  frequencies, radii and gains and frequency variation, the equalization zeros, the ratchet mode of Guiro and Wrench
  and the sweeping resonators of Water Drops), with STK's per-instrument tables. Integer (Q15 / Q31) or the FPU
  (`FM1_FPU`), whichever measures faster with the same sound; the port is checked against STK itself (built on the
  host in the tests) for spectrum and decay of every instrument.
- **Voices:** 23 tracks, one model each, each always the same instrument. A retrigger adds energy to the running
  model (as STK's `noteOn`: 0.1 × amplitude, capped at 1). One deliberate difference: STK stops ticking the
  resonators when the energy falls below 0.001, which cuts a ringing coin; here they ring out, and the voice goes idle
  (its state zeroed: exact silence) only when everything is near zero. The resonators' feedback rounds towards zero
  (a floor left a DC limit cycle in the low resonances, Crunch and Water Drops, found by the tests). A track whose
  energy and filter state have fallen below a threshold is idle and costs nothing. If the worst case (all 23 ringing,
  84 resonators) does not fit the audio ISR, a cap on simultaneously sounding tracks is added (the quietest is
  released), set from measurements.
- **The full model (milestone 2):** every resonance with its gain; the frequency variation at each collision
  (Tambourine's jingles, Sleigh Bells, Bamboo Chimes, the Rocks); Tuned Bamboo Chimes excite one of seven tubes per
  collision; Guiro and Wrench are STK's ratchet; Water Drops are STK's drops (three resonances around the second base
  frequency, sweeping upwards while their gain decays). Further deliberate differences from STK: FREQ also moves the
  varied and the sweeping frequencies (STK recomputes them from the base frequencies, so its control is undone at the
  next collision); every Water hit starts with a drop (STK's drop chance while a hit's energy lasts, ~25 ms, leaves
  two hits in three silent); Guiro and Wrench: DECAY sets the scrape, 1..8 ratchet sweeps a hit (64: 4; STK adds one
  sweep per noteOn and spends it at once, as it expects CC 2 scraping), and the hit's amplitude scales the scrape
  (STK ignores it); they use no system decay (STK sets none for them).
- **Checked against STK itself:** `tests/stk/` holds STK's Shakers source, unmodified; the suite builds it and compares
  every instrument at its defaults and with DECAY, OBJECTS and FREQ moved (32 hits each): the spectrum's centroid
  within 15 %, the decay time within 25 %, the level within 3 dB. Shown but not compared where the port differs on
  purpose (FREQ on varied frequencies; Water's decay, colour and level, which STK cuts at ~25 ms while a drop is still
  building; the coins at low DECAY, which STK cuts while they still ring). Holds on macOS and Linux (CI).
- **Parameters per track (0..127, as STK's controls):** ENERGY (shake energy per hit; STK CC 2), DECAY (system
  decay; CC 11), OBJECTS (number of objects; CC 4), FREQ (resonance frequency ×4^(v/128 − 0.5); CC 1). STK's code
  maps CC 11 to the system decay and CC 4 to the number of objects (`__SK_ShakerDamping_`, `__SK_ShakerNumObjects_`);
  its class comment says the reverse. Found by the reference test; Percolator follows the code. Defaults are
  STK's per-instrument baselines (64 = the instrument's own values; energy default from STK's examples).
- **Accent:** an accented hit adds the ACCENT amount to the hit's energy (up to a full hit's energy at 100 %), and
  moves each locked DECAY / OBJECTS / FREQ value towards 127 by ACCENT % (ACCENT 50: a DECAY lock of 40 plays as 83).
- **Locks:** a step's locks shape the sound it hits for as long as that sound rings (a knob turned meanwhile does
  not change a locked parameter, but does change the unlocked ones); the track's next hit without locks plays the
  knobs again. Pads and MIDI notes always play the knobs (the kit). An ENERGY lock sets the hit's amplitude.
- **Mix:** per track level, pan, delay send, reverb send. Master: chorus (insert), master filter (LP/HP) with
  resonance, drive, level. Pan is a balance law (centre: both channels at the track's level; towards a side the other
  channel falls to zero). After the MASTER level (FX 3) and the MASTER pot, a soft limit: straight up to -6 dBFS
  (where a busy pattern peaks at the defaults), then a quadratic knee into full scale (slope 0 at +3.5 dB over), so
  overload bends instead of clipping: everything at maximum (every level 100, MASTER 100, the pot full, accents on
  every 16th of all 23 tracks) reaches full scale in 0.07 % of samples (a hard clip: 1.2 %).
- **Effects:** a tempo-synced stereo delay (up to ~1 s; the RAM pool decides), a reverb (Freeverb-like or lighter,
  sized to the budget), a stereo chorus, the master filter and drive. The effects come after the sound is right
  (milestones), and the reverb is the first to be lightened if the ISR budget is tight.
- **No audio input** (the FM-1 has none).

## Timing and sequencing

- A sample-counting clock in the audio ISR: 4 ticks per 16th (MIDI's 24 PPQN in milestone 7); tempo 40..240 BPM.
- **Pattern:** 23 tracks × 16 steps. Per track: length 1..16, clock divide (1/1 16ths, 1/2 8ths, 1/4, 1/8, 2/1
  32nds, 4/1 64ths: a step every 4, 8, 16, 32, 2, 1 ticks), mute, solo (mute / solo are performance state, also
  saved with the pattern). Per step: trig, accent, probability, ratchet (1..4 with a ramp, spread over the track's
  step), condition, nudge (−5..+5 twelfths of a 16th: a fixed time on every divide), four locks (or none).
- **Conditions** count each track's loops (its length at its divide) from PLAY or from the last pattern change
  (where the tracks restart from step 1): A:B plays on loop A of every B; FIRST only the first loop, NOT FIRST the
  others; FILL while ARP is held, NOT FILL while it is not. Decided with the probability when the step is due.
- **Nudge** without latency: a step nudged early is decided half a 16th ahead and scheduled; every other step plays
  the moment it is due (a step 1 nudged early plays at once at PLAY, early from the next loop on).
- **Swing** 50..75 % (TIME page, per pattern): every second step of a pair is late by (SWING − 50) / 50 of a step
  (66 %: a triplet feel, 75 %: dotted), on tracks at 16ths or slower (not on 32nds and 64ths).
- **Note repeat:** ARP held + a pad held repeats the pad at REPEAT RATE (TIME page: 1/8, 1/16, 1/32, 1/16T) until
  either is let go, running or stopped; recorded while REC runs (rolls).
- **Live record** (REC while running): pad hits go to the nearest step of the pressed track; accent from F#5. A hit
  recorded ahead of the playhead (nearer the next 16th) is heard as the pad and skipped once when the playhead gets
  there (no double hit); the next pass plays it.
- **Pattern change** queued to the end of the pattern (the longest track's 16 steps) while running.
- **Songs:** 4 songs of up to 64 entries (pattern 1..16, repeats 1..16); song playback follows the chain and stops or
  loops at the end (a song setting).
- **MIDI clock** (GLOBAL: CLOCK INT / EXT / AUTO; GLOBAL 2: CLOCK OUT):
  - EXT follows 24 PPQN in (USB or TRS): the tempo is the smoothed interval between clocks (~16 clocks), and the
    sequencer runs smoothly at it but locked: each clock grants one clock's worth of time, so it never runs more than
    a clock ahead (a paused clock pauses the steps) and catches up at once if it falls behind. Swing, nudge, ratchets
    and note repeat follow the clock's tempo; the header shows "EXT" and that tempo. FA starts from step 1, FC stops,
    FB continues from the next 16th (or from a song position pointer, F2, in 16ths). The FM-1's PLAY does not start
    while following ("CLOCK EXT: THE MASTER PLAYS"). An interval longer than 1/4 s (a pause) is not measured; until
    two clocks have been, the pattern's tempo runs.
  - AUTO is EXT while a clock has arrived in the last half second, INT otherwise.
  - INT with CLOCK OUT on sends 24 PPQN over USB all the time (a slave always knows the tempo), FA at PLAY (and a
    clock at once), FC at stop. Nothing is sent while following (no loops).

## What is stored

The store has 36 records of 4 KB, `APP_ID` per firmware. All 16 patterns and the 4 kits are also kept in RAM (the
pool, ~43 KB of ~380 KB): changing patterns is a pointer swap at the bar line in the audio ISR (no flash read while
playing), and edits survive changing patterns until power-off; SAVE writes the shown pattern (or kit, or the songs)
to flash, and "save as" another slot copies it there and plays from that slot.

| Record | Count | Contents |
| --- | --- | --- |
| Settings | 1 | global page, last pattern / kit / song, screen choices |
| Patterns | 16 | 23 tracks × 16 steps × ~7 bytes (trig / accent / condition, probability, ratchet + nudge, 4 locks) ≈ 2.6 KB, plus per-track length, divide, mute, solo, the pattern's tempo and swing, a record version |
| Kits | 4 | per track: the four parameters, level, pan, delay and reverb sends; the FX settings |
| Songs | 4 | up to 64 × (pattern, repeats), loop flag (four songs can share one record) |

That is 25 records at most (fewer if songs share one), leaving room for later. Damaged or foreign records are
ignored and replaced by defaults; every value is range-checked on load.

## MIDI

- **In (USB and TRS; TRS MIDI IN on by default; the platform always listens on TRS, the GLOBAL switch makes the app
  ignore what comes from it):** notes on the chosen channel: base note + 0..22 trigger tracks
  1..23 (default base 53, the FM-1's own keys F3..D#5), velocity → energy; CC 2 / 11 / 4 / 1 set the selected
  track's (or, with a per-track channel mode later, a track's) energy, decay, objects, frequency (STK's numbers);
  MIDI clock, start, stop, continue and song position as the clock source says (Timing, "MIDI clock"). Note-offs are ignored
  (the models decay on their own), but an all-notes-off / all-sound-off (CC 123 / 120) silences every track.
- **Out (USB only):** clock, start and stop when INT and CLOCK OUT is on; every played trig and pad as a
  note (base note + track, velocity from energy, a short note-off after) when NOTES OUT is on.
- **Screen reader (fm1-reader):** the FM-1 speaks through the fm1-reader screen reader
  (https://charlesvestal.github.io/fm1-reader/, protocol version 1), as in Solinaesque: USB-MIDI SysEx
  `F0 7D 53 52 <cmd> ... F7`, silent until a reader's HELLO, a HELLO answer naming "PERCOLATOR v0.1", SAY messages
  for what the player caused (pages and their values, knob turns debounced, track selection with the track's full
  name, step edits "step 5 on, accent, probability 50 percent", pattern changes, saves, play / stop / record) and
  WHERE reading the whole screen (the grid as a summary: "pattern 3, playing, 120 BPM, selected Tambourine, steps 1,
  5, 9, 13"). Wording per fm1-reader's RULES.md. Needs the platform's `app_sysex()` hook, ported from Solinaesque.
- **MIDItoSpeech sidecar** (`docs/synthaccess/Percolator.json`, for https://github.com/IDMNYU/synthaccess
  MIDItoSpeech): what the FM-1 sends (the notes as track names, the CCs as parameter names and values), generated
  from the firmware's tables (`tools/gen_synthaccess.c`) and checked by the suite against what is sent.
- **USB name:** "Percolator FM-1" (the USB product string, firmware/src/usb.c, changed on request 2026-11-26 as in
  Solinaesque and Looching 2026); it keeps "FM-1", by which fm1_install.py and the web installer find the unit. The
  update loader stays "Felucca Update"; the package identity stays FM-1_900. Release builds are named
  `percolator-X.Y.fwsc`. A host that knew the unit as "Felucca" (or an earlier firmware's name) may show the new
  name only after the cable is plugged in again; a DAW must select the new port (and enable sync on it for the
  clock: many hide clock by default).

## The repository and the web

- GitHub Actions CI (build and host tests), the code graph (graphify).
- A GitHub Pages site (`web/make_site.py`, a `pages.yml` workflow, as in Solinaesque and Looching 2026): the browser
  emulator, the web installer (`web/installer.html`) with the released `.fwsc`, and the docs.
- Licensing: the new code GPL-3.0-only, Copyright (C) 2026 R. Luke DuBois. The ported Shakers code and tables keep
  STK's copyright and MIT-style license notice (Perry R. Cook and Gary P. Scavone), recorded in `LICENSING.md` and
  `LICENSES/`.

## Open questions

- The CPU cost of the effects on top of the full model (41 % peak measured in milestone 2): a cap on sounding tracks
  only if the effects push the peak towards 80 %.
- MUTE / SOLO in STEP mode: EDIT + E5 / F5 (above) may feel awkward; revisit once it is played.
- The encoders have no push switches (settled in the build): mute is on MUTE + pad and the TRACK page; tempo moves
  in whole BPM.
- Choosing another kit to play: SAVE KIT to a slot switches to it, but there is no "load kit" control yet. Candidates:
  a KIT row on GLOBAL 2, or PRESETS on the SAVE screen with a LOAD action. Decide before milestone 8.
- The GRID's paired names (left / right on two rows) read well in the screenshots; confirm on the unit.
- Whether tracks should be re-assignable to another STK instrument (two maracas). Not in v1: track n is type n − 1.
- The exact accent law (energy add vs scale) and the ratchet ramp, by ear.
- Patterns longer than 16 steps (pages of 16): out of v1, the record has room for a second page later.

## Out of scope for v1

Audio input (none on the FM-1), samples, other STK instruments, per-track effects beyond the sends, MIDI out over
TRS (the FM-1 has TRS in only), patterns longer than 16 steps, re-assignable tracks, wireless.

## Milestones

Each can be tested on the host and heard in the emulator.

1. **Skeleton (done, 2026-11-26):** every control from the tables wired to a visible effect ("SOON" where not
   built); HOME GRID / TRACK, all nine pages, SAVE, SONG; all 23 tracks sound through a skeleton PhISEM voice (STK's
   collision process and energy decays, the first resonance and the equalizer, levels balanced by simulation in
   `tools/gen_shakers.py`) from the pads and MIDI; a basic step clock already plays trigs, accents, mute, solo and
   per-track length, records live, changes patterns at the bar line and sends notes out; every per-step setting is
   edited, shown and stored (played from milestone 5); patterns, kits, songs and settings saved and loaded.
   Measured: image 129 KB of 582 KB, RAM 7.5 KB of 50 KB, pool 111 KB of 391 KB; the audio ISR's static cost 2211
   (the start app's 867); the emulator renders the worst case (all 23 pads re-hit every ~23 ms) at ~300× real time.
   On the unit (2026-11-26): the sequence plays, levels even, tempo steady, no crackles; the CPU meter (added after
   the first install) reads ~10..15 % both with a sequence running and with a forearm across all the pads while it
   runs. That is the baseline for milestone 2's full model.
2. **The Shakers port (built 2026-11-26):** all 23 instruments, every resonance, the variation, the tubes, the ratchet
   and the drops (Sound, "The full model"); the four parameters on the knobs; checked against STK built on the host
   (`tests/shakers_test.c`: every instrument and case passes; the comparison found STK's swapped CC 4 / 11 and an
   overflow in the ratchet). Cost: the audio ISR's static cost 19302 (the skeleton's 2211: a loop over up to seven
   resonances inside the sample loop); the emulator renders the worst case at 92× real time (the skeleton 319×).
   On the unit (2026-11-26): CPU PEAK 41 % at most (a busy sequence with the coins and the chimes, and a forearm
   across all the pads while it runs), no late half, everything sounds right. No voice cap needed; ~40 % of the
   audio ISR is left for the effects (milestone 9).
3. **Sound tuning (done 2026-11-26):** levels balanced across the 23 (by simulation in gen_shakers.py; "levels feel
   good" on the unit), per-track level and pan, energy from MIDI velocity, the accent law (ACCENT adds up to a full
   hit's energy); added the master's soft limit (dense patterns at full volume clipped). By ear
   in the emulator and on the unit.
4. **The step sequencer (done 2026-11-26):** 16 steps × 23 tracks, trigs, accent, the clock, PLAY / STOP (a double
   tap restarts), track selection, the grid's playhead, mute / solo, per-track length, patterns saved and loaded (16
   slots, all kept in RAM), queued pattern change at the bar line, live record (no double hit when recorded ahead of
   the playhead). Built mostly in the skeleton; checked on the unit (steady tempo).
5. **Per-step features (done 2026-11-26):** parameter locks (all four, held while the sound rings, moved by the
   accent), probability (one roll per step, so a ratcheted step plays all its repeats or none; CHANCE BIAS on the
   RANDOM page), ratchets x2..x4 spread evenly over the step (FLAT, UP from 40 % to 100 % of the step's amplitude,
   DOWN the other way; stopping drops the repeats left). Tests prove each against a broken version.
6. **Conditions and time (done 2026-11-26):** conditional trigs (A:B, FIRST, NOT FIRST, FILL, NOT FILL), FILL on
   ARP held, nudge early and late (a lookahead for the early ones), per-track clock divide, swing, note repeat. The
   clock now runs at 4 ticks per 16th; each piece tested to the sample block and proved against a broken version.
   (The first install of it was a stale image: the link failed on a 64-bit divide the pi32v2 toolchain lacks, and
   the old firmware was installed. tests/run_tests.sh now refuses a firmware older than its sources. The STK
   comparison was also made to hold on Linux: STK's dice are the C library's rand(), different per platform.)
   Confirmed on the unit (2026-11-26): conditions, divide, nudge, swing and note repeat all work.
7. **MIDI sync (done 2026-11-26):** clock in (EXT / AUTO) with a locked smooth follower, clock out, start / stop /
   continue / song position, PLAY deferring to the master. Tests drive a clock at 120 and 150 BPM, pause it, stop,
   continue, set a song position, and check AUTO's fall-back and that nothing is sent while following; each proved
   against a broken version. (Found by them: a first clock's interval, measured from boot, overflowed; and continue
   mid-step played nothing until the next step: now it resumes on the next 16th, as MIDI counts.)
8. **Songs and kits:** the SONG screen, chains, 4 songs, 4 kits, the SAVE screen for all three.
9. **Effects:** chorus, master filter and drive, delay, reverb (in that order, each measured).
10. **Accessibility:** fm1-reader (the `app_sysex` hook from Solinaesque, `sr.c`, `speak.c`), the MIDItoSpeech
    sidecar.
11. **The web:** GitHub Pages with the emulator and the web installer, release builds.
