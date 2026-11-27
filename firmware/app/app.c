/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 M-Vave-OP1 contributors
 * Copyright (C) 2026 R. Luke DuBois */
/* Percolator: a 23-track percussion sequencer on STK's Shakers (PhISEM) models (docs/SPEC.md). The app contract's
 * five hooks (app.h) and the parts, in dependency order:
 *   data.c    the tracks, the kit, the patterns, the settings, the songs, their records; the page parameters
 *   fx.c      the effects chain on the mix (FPU): filter / drive (pre or post), chorus, delay, reverb
 *   sound.c   the audio ISR: the 23 Shakers voices, the pads, MIDI in, notes out, the step clock, the chain
 *   ui.c      the panel: controls, LEDs, the HOME (GRID / TRACK), page, SAVE and SONG screens */
#ifndef APP_NAME
#define APP_NAME "PERCOLATOR"
#endif
#ifndef APP_VERSION
#define APP_VERSION "v0.1"       /* build.py --release X.Y */
#endif
#define APP_ID 0xEE398C08u       /* this app's records in the store (store.c): every app its own (setup/new_project.py
                                  * drew it), so another firmware's data in the same flash reads as empty */

#include "data.c"
#include "fx.c"
#include "sound.c"
#include "ui.c"

static void app_init(void)
{
    data_load();
    snd_init();
    ui_init();
    ui_draw();
}

static void app_frame(void) { ui_frame(); }

static const char *app_fault_name(void) { return APP_NAME " CRASH"; }
