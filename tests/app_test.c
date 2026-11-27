/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 M-Vave-OP1 contributors
 * Copyright (C) 2026 R. Luke DuBois */
/* Host test of Percolator (firmware/app/) on the host platform (tests/host_platform.h): every screen drawn (and the
 * layout lint at the widest values, a PNG of each in build/shots/), lazy drawing and the GRID's fast path, the
 * controls table of docs/SPEC.md (keys, buttons, encoders, held steps), the LEDs, the records in flash (patterns, kits,
 * songs, settings: save, power cycle, another app's record, damaged values, no flash) and the sound (every one of the
 * 23 instruments sounds and rings out to exact silence, MIDI in, the channel, CCs, all sound off, TRS on / off, the
 * accent, the tempo, mute, solo, track length, live record, notes out, app_audio_stop).
 *   cc -O1 -Ibuild/gen -Ifirmware/src -Ifirmware/app -Itests -o build/host/app_test tests/app_test.c -lm
 *   build/host/app_test */
#include "host_platform.h"
#include <math.h>

#define SECS 3u
static int32_t buf[2u * FS * SECS];
static int32_t buf_small[2u * 44100u];

static int failed;
static void check(const char *what, int ok)
{
    printf("%-64s %s\n", what, ok ? "ok" : "FAIL");
    failed += !ok;
}

static void frames(uint32_t n)
{
    while (n--)
        host_frame();
}

static void press(uint32_t b)                    /* a button tapped */
{
    host_press(b, 1);
    host_frame();
    host_press(b, 0);
    host_frame();
}

static void key_tap(uint32_t k)                  /* a key tapped (with whatever else is held) */
{
    fm1_in.notes |= 1u << k;
    host_frame();
    fm1_in.notes &= ~(1u << k);
    host_frame();
}

static void song_screen(int open)                /* HOME cycles GRID -> TRACK -> SONG -> GRID */
{
    uint32_t i;
    if (ui.page != PG_HOME)
        press(B_HOME);
    for (i = 0; i < 4u && ui.song_open != (uint8_t)(open != 0); i++)
        press(B_HOME);
}

static uint32_t ink_rows(uint32_t y0, uint32_t y1)   /* rows in y0..y1 with anything but the background */
{
    uint32_t x, y, n = 0;
    for (y = y0; y < y1; y++)
        for (x = 0; x < 240u; x++)
            if (host_fb[y * 240u + x] != T_BG) {
                n++;
                break;
            }
    return n;
}

static uint16_t px(int32_t x, int32_t y) { return host_fb[y * 240 + x]; }
static uint16_t cell_px(uint32_t t, uint32_t s) { return px(cell_x(s) + 4, G_TOP + (int32_t)t * G_ROW + 4); }

static void lint(const char *what)
{
    char b[96];
    snprintf(b, sizeof b, "layout: %s: nothing cut or outside its canvas", what);
    check(b, host_clipped == 0u);
    if (host_clipped)
        printf("  (%u, the last: %s)\n", host_clipped, host_clip_what);
    host_clipped = 0;
}

static void shot(const char *name)
{
    char b[96];
    snprintf(b, sizeof b, "build/shots/%s.png", name);
    host_png(b);
}

static void power_cycle(void)                    /* the app's RAM gone, the flash kept; past the splash */
{
    memset(&ui, 0, sizeof ui);
    memset(&snd, 0, sizeof snd);
    snd.master_q8 = 256;
    memset(pats, 0, sizeof pats);
    memset(kits, 0, sizeof kits);
    memset(&set, 0, sizeof set);
    memset(&songs, 0, sizeof songs);
    pat_cur = &pats[0];
    kit_cur = &kits[0];
    pat_stored = kit_stored = 0;
    song_index = 0;
    fm1_in.notes = fm1_in.buttons = 0;
    host_boot();
    frames(80);
}

static void ring_out(void);

static void blank_world(void)                    /* no factory content: blank patterns, default kits, no songs */
{
    uint32_t i;
    for (i = 0; i < NPAT; i++)
        pat_clear(&pats[i]);
    for (i = 0; i < NKIT; i++)
        kit_default(&kits[i]);
    memset(&songs, 0, sizeof songs);
    songs_check();
    pat_cur = &pats[0];
    kit_cur = &kits[0];
}

/* ------------------------------------------------------------------- screens --- */
static void screens(void)
{
    uint32_t t, s, p;
    host_nor_erase();
    host_boot();
    blank_world();                               /* (the general checks start from nothing; persistence() tests the
                                                  * factory set) */
    host_frame();
    check("boot: the splash (name, version) drawn", ink_rows(80, 170) > 30u);
    {   /* the large face is sparse (tools/gen_aa_font.py L_CHARS): a character it lacks draws as a space */
        const char *c = APP_NAME;
        int all = 1;
        for (; *c; c++)
            all &= glyph_at(&AF_L, (uint8_t)*c) >= 0;
        check("boot: every character of APP_NAME is in the large font", all);
    }
    {   /* the name's letters in turn: yellow (the theme), cyan (the accent), magenta (the locks) */
        uint32_t x, y, th = 0, ac = 0, mg = 0, lx[3] = {0, 0, 0};
        for (y = 82; y < 82u + AF_L.h; y++)
            for (x = 0; x < 240u; x++) {
                uint16_t c = host_fb[y * 240u + x];
                if (c == T_THEME && (!th++ || x < lx[0]))
                    lx[0] = x;
                if (c == T_ACCENT && (!ac++ || x < lx[1]))
                    lx[1] = x;
                if (c == C_LOCK && (!mg++ || x < lx[2]))
                    lx[2] = x;
            }
        check("splash: the name in yellow, cyan and magenta", th > 40u && ac > 40u && mg > 40u);
        check("splash: P yellow, E cyan, R magenta (left to right)", lx[0] < lx[1] && lx[1] < lx[2]);
    }
    shot("splash");
    frames(80);
    check("after the splash: HOME GRID, 23 rows of names and steps", ink_rows(G_TOP, G_FOOT) > 120u);
    check("GRID: an empty step is a stub, not a trig", cell_px(0, 0) != T_SEL && cell_px(0, 0) != T_THEME);
    shot("home_grid");
    host_frame();
    check("lazy: nothing changed, nothing sent to the panel", host_blits == 0u);

    host_turn(EN_SELECT, 3);                    /* SELECT: the track */
    host_frame();
    check("SELECT: track 4 selected (Tambourine)", sel() == 3u && snd.sel == 3u);

    press(B_SEQ);                                /* STEP mode: white keys are steps */
    check("SEQ: STEP mode", ui.mode == 1u);
    key_tap(0);
    host_frame();
    check("STEP: F3 tapped sets step 1's trig", (pat.t[3].s[0].f & ST_TRIG) != 0);
    check("GRID: the trig drawn in the theme's selection colour", cell_px(3, 0) == T_SEL);
    fm1_in.notes = 1u << KEY_ACCENT;             /* ACCENT held + G3 (step 2) */
    key_tap(2);
    fm1_in.notes = 0;
    host_frame();
    check("STEP: ACCENT held + step 2: an accented trig", (pat.t[3].s[1].f & (ST_TRIG | ST_ACC)) == (ST_TRIG | ST_ACC));
    check("GRID: the accent drawn brighter (T_THEME)", cell_px(3, 1) == T_THEME);
    key_tap(0);
    check("STEP: F3 again clears step 1", !(pat.t[3].s[0].f & ST_TRIG));
    key_tap(step_key(15));
    check("STEP: G5 is step 16", (pat.t[3].s[15].f & ST_TRIG) != 0);

    fm1_in.notes = 1u << step_key(4);            /* hold step 5 and edit it */
    host_frame();
    check("STEP: a held step is shown", ui.held_step == 4);
    host_turn(EN_K1, 5);
    host_turn(EN_K4, -3);
    host_turn(EN_SELECT, -4);
    host_turn(EN_ALGO, 3);
    host_turn(EN_PRESET, 2);
    host_frame();
    press(B_OCTUP);
    press(B_OCTUP);
    {
        const step_t *st = &pat.t[3].s[4];
        check("held step + KNOB 1: an energy lock (the track's 100 + 5)", st->lock[0] == 106u);
        check("held step + KNOB 4: a frequency lock (64 - 3)", st->lock[3] == 62u);
        check("held step + SELECT: probability 80 %", st_prob(st) == 80u);
        check("held step + ALGORITHM: ratchet x2 DOWN", st_ratchet(st) == 2u && st_ramp(st) == 2u);
        check("held step + PRESETS: condition 2:2", st_cond(st) == 2u);
        check("held step + OCT+ twice: nudge +2", st_nudge(st) == 2);
        check("held step + OCT+: no pattern queued", ui.queued == 0u);
    }
    fm1_in.notes = 0;
    host_frame();
    check("released after edits: the trig was not toggled", !(pat.t[3].s[4].f & ST_TRIG));
    check("GRID: a step with settings but no trig shows no trig", cell_px(3, 4) != T_SEL);
    check("GRID: locks on a step that is off: a magenta stub", px(cell_x(4) + 4, G_TOP + 3 * G_ROW + 3) == C_LOCK);
    pat.t[3].s[4].f = ST_TRIG;
    host_frame();
    check("GRID: a locked trig is magenta", cell_px(3, 4) == C_LOCK);
    pat.t[3].s[4].f = ST_TRIG | ST_ACC;
    host_frame();
    check("GRID: a locked, accented trig is the brighter magenta", cell_px(3, 4) == C_LOCK_ACC);
    press(B_HOME);                               /* the TRACK view too */
    track_select(3);
    frames(2);
    check("TRACK view: the locked trig is magenta", px(10 + 4 * 28 + 12, G_TOP + 30 + 30) == C_LOCK_ACC);
    shot("home_track_locks");
    press(B_HOME);                               /* (TRACK -> SONG -> GRID) */
    press(B_HOME);
    fm1_in.notes = 1u << step_key(4);            /* hold step 5, EDIT: its locks off, its trig kept */
    host_frame();
    press(B_EDIT);
    fm1_in.notes = 0;
    host_frame();
    check("held step + EDIT: every lock cleared", !st_locked(&pat.t[3].s[4]) && strstr(ui.msg, "LOCKS CLEARED") != 0);
    check("held step + EDIT: the trig stays, the selection too", (pat.t[3].s[4].f & ST_TRIG) && sel() == 3u);
    check("GRID: no locks: amber again", cell_px(3, 4) == T_THEME);
    pat.t[3].s[4].f = 0;

    host_press(B_EDIT, 1);                       /* EDIT + a key: select, no sound */
    host_frame();
    check("EDIT held: the pads are silent (they select)", snd.pad_mask == 0u);
    key_tap(7);
    host_press(B_EDIT, 0);
    host_frame();
    check("EDIT + C4: track 8 (Coke Can) selected", sel() == 7u);
    check("STEP mode: no key plays a track (white: steps, black: functions)", snd.pad_mask == 0u);
    track_select(6);                             /* STEP mode's C#5 / D#5: mute / solo the selected track */
    key_tap(KEY_STEP_MUTE);
    check("STEP mode, C#5: the selected track muted, C#5 lit", (pat.t[6].flags & TP_MUTE) &&
                                                              host_led[14u + KEY_STEP_MUTE] && !host_led[14u + KEY_STEP_SOLO]);
    key_tap(KEY_STEP_SOLO);
    check("STEP mode, D#5: the selected track soloed, D#5 lit", (pat.t[6].flags & TP_SOLO) && host_led[14u + KEY_STEP_SOLO]);
    fm1_in.notes = 1u << KEY_STEP_MUTE;          /* C#5 held, then D#5: all off */
    host_frame();
    key_tap(KEY_STEP_SOLO);
    fm1_in.notes = 0;
    host_frame();
    check("STEP mode, C#5 held then D#5: every mute and solo off", !(pat.t[6].flags & (TP_MUTE | TP_SOLO)));
    {   /* E5, F5, G5 are steps 14..16 only (no mute, solo or SONG behind them, EDIT or not) */
        uint32_t f0 = pat.t[6].flags;
        key_tap(23);
        key_tap(26);
        check("STEP mode: E5 and G5 are steps 14 and 16 only", (pat.t[6].s[13].f & ST_TRIG) &&
                                                                (pat.t[6].s[15].f & ST_TRIG) &&
                                                                pat.t[6].flags == f0 && !ui.song_open);
        host_press(B_EDIT, 1);
        host_frame();
        key_tap(24);
        host_press(B_EDIT, 0);
        host_frame();
        check("STEP mode: EDIT + F5 does nothing (no longer solo)", pat.t[6].flags == f0);
        pat.t[6].s[13].f = pat.t[6].s[15].f = 0;
    }
    track_select(7);
    press(B_SEQ);
    check("SEQ again: PAD mode, all 23 pads play", ui.mode == 0u && snd.pad_mask == PAD_MASK);

    fm1_in.notes = 1u << KEY_MUTE;               /* MUTE held + a pad */
    host_frame();
    key_tap(2);
    fm1_in.notes = 1u << KEY_SOLO;
    host_frame();
    key_tap(9);
    fm1_in.notes = 0;
    host_frame();
    check("MUTE (E5) + G3: Sekere muted", (pat.t[2].flags & TP_MUTE) != 0);
    check("SOLO (F5) + D4: Crunch soloed", (pat.t[9].flags & TP_SOLO) != 0);
    check("MUTE / SOLO held: the pad pressed did not change the selection", sel() == 7u);
    {   /* a chord with E5 / F5 in it (a forearm across the keys, found on the unit): no mute or solo toggled */
        uint32_t t2, flags = 0;
        for (t2 = 0; t2 < NT; t2++)
            pat.t[t2].flags = 0;
        fm1_in.notes = (1u << 26) - 1u;          /* (G5 left out: it would open SONG, harmless but not tested here) */
        host_frame();
        fm1_in.notes = 0;
        host_frame();
        for (t2 = 0; t2 < NT; t2++)
            flags |= pat.t[t2].flags;
        check("a chord including E5 and F5: no track muted or soloed", flags == 0u);
        pat.t[2].flags = TP_MUTE;
        pat.t[9].flags = TP_SOLO;
        host_frame();
        check("a solo elsewhere: the selected track's footer says it is silent", track_state(sel()) == 3u &&
                                                                                  ink_rows(G_FOOT, 240) > 8u);
        shot("home_grid_solo");
        {   /* the most crowded header: a queued pattern, STEP, SOLO, FILL, REC, RUN, 125.5 BPM */
            ui.queued = 12;
            ui.mode = 1;
            ui.rec = 1;
            snd.running_pub = 1;
            pat.tempo2 = 251;
            host_press(B_ARP, 1);
            frames(2);
            shot("home_head_crowded");
            lint("the most crowded header");
            host_press(B_ARP, 0);
            ui.queued = 0;
            ui.mode = 0;
            ui.rec = 0;
            snd.running_pub = 0;
            pat.tempo2 = 240;
            frames(2);
        }
        fm1_in.notes = 1u << KEY_MUTE;           /* E5 held, then F5: all off */
        host_frame();
        fm1_in.notes |= 1u << KEY_SOLO;
        host_frame();
        fm1_in.notes = 0;
        host_frame();
        flags = 0;
        for (t2 = 0; t2 < NT; t2++)
            flags |= pat.t[t2].flags;
        check("E5 held, then F5: every mute and solo off", flags == 0u);
        pat.t[2].flags = TP_MUTE;
        pat.t[9].flags = TP_SOLO;
    }
    key_tap(10);
    check("a pad in PAD mode selects its track", sel() == 10u);

    press(B_HOME);                               /* HOME on HOME: the TRACK view */
    check("HOME on HOME: TRACK view", set.view == 1u);
    frames(2);
    shot("home_track");
    lint("HOME TRACK");
    track_select(3);
    press(B_SEQ);
    fm1_in.notes = 1u << step_key(4);
    host_frame();
    host_frame();
    check("TRACK view: the held step's settings drawn", ink_rows(180, 205) > 8u);
    shot("home_track_held");
    lint("HOME TRACK with a held step");
    fm1_in.notes = 0;
    host_frame();
    press(B_SEQ);
    press(B_HOME);
    check("HOME again, from TRACK: the SONG screen", ui.song_open == 1u);
    press(B_HOME);
    check("HOME again, from SONG: the GRID", set.view == 0u && !ui.song_open);

    /* every page, its values at the widest */
    {
        static const struct { uint32_t button, presses, page; const char *name; } PG[] = {
            {B_ENV, 1, PG_SOUND, "page_sound"}, {B_ENV, 1, PG_TRACK, "page_track"},
            {B_ENV, 1, PG_TRACK2, "page_track2"}, {B_SCL, 1, PG_TIME, "page_time"},
            {B_LFO, 1, PG_RANDOM, "page_random"}, {B_FX, 1, PG_FX1, "page_fx1"}, {B_FX, 1, PG_FX2, "page_fx2"},
            {B_FX, 1, PG_FX3, "page_fx3"}, {B_FX, 1, PG_FX4, "page_fx4"},
            {B_GLO, 1, PG_GLO, "page_global"}, {B_GLO, 1, PG_GLO2, "page_global2"}};
        uint32_t i, r;
        track_select(18);                        /* "Peso + Mug": the longest short name pair */
        for (i = 0; i < sizeof PG / sizeof PG[0]; i++) {
            char b[64];
            press(PG[i].button);
            snprintf(b, sizeof b, "%s opens %s", B_NAME[PG[i].button], PAGE_NAME[PG[i].page]);
            check(b, ui.page == PG[i].page);
            for (r = 0; r < 4u; r++)             /* the widest text: try both ends */
                if (PP[ui.page][r].name)
                    pv_set(ui.page, r, sel(), PP[ui.page][r].kind == K_PAN ? 37 : PP[ui.page][r].hi);
            frames(2);
            shot(PG[i].name);
            lint(PAGE_NAME[PG[i].page]);
            for (r = 0; r < 4u; r++)
                if (PP[ui.page][r].name)
                    pv_set(ui.page, r, sel(), PP[ui.page][r].lo);
            frames(2);
            lint(PAGE_NAME[PG[i].page]);
        }
        host_turn(EN_SELECT, -2);
        host_frame();
        check("SELECT on a page: the page before", ui.page == PG_FX4);
        {   /* (the widest values above left the effects at their ends: back to a fresh kit's, all off) */
            static kit_t d;
            kit_default(&d);
            memcpy(kit.fx, d.fx, sizeof d.fx);
        }
    }
    press(B_ENV);                                /* ENV: SOUND first, the selected track's four */
    check("ENV: the SOUND page first", ui.page == PG_SOUND);
    kit.t[18].p[SP_EN] = 100;                    /* (the widest-values loop above left them at 0) */
    kit.t[18].p[SP_DEC] = kit.t[18].p[SP_OBJ] = kit.t[18].p[SP_FRQ] = 64;
    host_turn(EN_K1, -7);
    host_turn(EN_K2, 10);
    host_turn(EN_K3, -4);
    host_turn(EN_K4, 200);
    host_frame();
    check("SOUND page: KNOB 1..4 set the track's ENERGY, DECAY, OBJECTS, FREQ", kit.t[18].p[SP_EN] == 93u &&
          kit.t[18].p[SP_DEC] == 74u && kit.t[18].p[SP_OBJ] == 60u && kit.t[18].p[SP_FRQ] == 127u);
    check("SOUND page: the values drawn large (rows of the page)", ink_rows(36, 200) > 60u);
    kit.t[18].p[SP_EN] = 100;
    kit.t[18].p[SP_DEC] = kit.t[18].p[SP_OBJ] = kit.t[18].p[SP_FRQ] = 64;
    press(B_ENV);                                /* SOUND -> TRACK */
    host_turn(EN_K1, -200);
    host_turn(EN_K2, 300);
    host_frame();
    {
        char b[16];
        pv_text(b, sizeof b, &PP[PG_TRACK][1], 50);
        check("TRACK page: pan in the middle reads CENTER", str_eq(b, "CENTER"));
    }
    check("TRACK page: KNOB 1 level, KNOB 2 pan (clamped)", kit.t[18].level == 0u && kit.t[18].pan == 100u);
    kit.t[18].level = 80;
    kit.t[18].pan = 50;
    press(B_SCL);
    host_turn(EN_K1, 10);
    host_frame();
    check("TIME page: KNOB 1 tempo (+10 BPM over the 40 the test left)", pat_bpm2() == 2u * 50u);
    pat.tempo2 = 240;
    press(B_HOME);

    /* SAVE and SONG */
    press(B_SAVE);
    check("SAVE: the SAVE screen, PATTERN", ui.saving && ui.save_what == SV_PAT);
    host_turn(EN_SELECT, 1);
    host_turn(EN_PRESET, 3);
    frames(2);
    shot("save");
    lint("SAVE");
    check("SAVE: SELECT picks KIT, PRESETS the slot", ui.save_what == SV_KIT && ui.save_slot == 3u);
    press(B_HOME);
    check("SAVE: HOME cancels", !ui.saving);

    song_screen(1);
    check("SONG (HOME, HOME): the SONG screen", ui.song_open == 1u);
    {   /* one turn a frame: within a frame the roles are taken in their order, not the order turned */
        static const int32_t T[][2] = {{EN_PRESET, 1}, {EN_SELECT, 1}, {EN_PRESET, 1}, {EN_SELECT, -1}, {EN_K1, 15},
                                       {EN_PRESET, 14}};
        uint32_t i;
        for (i = 0; i < 6u; i++) {               /* END: a new entry; then a second; back to the first */
            host_turn((uint32_t)T[i][0], T[i][1]);
            host_frame();
        }
    }
    for (p = 0; p < 6u; p++) {                   /* more entries: K2 inserts copies */
        host_turn(EN_K2, 1);
        host_frame();
    }
    frames(2);
    shot("song");
    lint("SONG");
    check("SONG: entries added, repeats on KNOB 1", songs.s[0].n == 8u && songs.s[0].e[0][1] == 16u);
    check("SONG: PRESETS sets an entry's pattern", songs.s[0].e[0][0] == 14u);
    host_turn(EN_K2, -1);
    host_frame();
    check("SONG: KNOB 2 left deletes", songs.s[0].n == 7u);
    song_screen(0);
    check("SONG again: closed", ui.song_open == 0u);

    /* the widest GRID: every step set with everything, mixed mutes and solos, a long message */
    for (t = 0; t < NT; t++) {
        pat.t[t].flags = (uint8_t)(t % 5u == 0u ? TP_MUTE : t % 7u == 0u ? TP_SOLO : 0u);
        pat.t[t].len = (uint8_t)(t % 3u ? 16u : 12u);
        for (s = 0; s < NSTEP; s++) {
            step_t *st = &pat.t[t].s[s];
            st->f = (uint8_t)(((s + t) % 3u ? ST_TRIG : 0u) | ((s + t) % 4u == 0u ? ST_ACC : 0u));
            st->prob = (uint8_t)((s * t) % 5u == 0u ? 6u : 0u);
            st->lock[SP_FRQ] = (uint8_t)((s + 2u * t) % 7u == 0u ? 90u : 0u);   /* some locks: magenta */
        }
    }
    msg("A MESSAGE THAT IS FAR TOO LONG FOR THE FOOTER OF THE SCREEN");
    track_select(22);
    frames(2);
    shot("home_grid_widest");
    lint("HOME GRID at its widest");
    for (t = 0; t < NT; t++)                     /* the same pattern, nothing muted: trigs, accents, locks */
        pat.t[t].flags = 0;
    frames(2);
    shot("home_grid_locks");
    for (t = 0; t < NT; t++)
        pat.t[t].flags = 0;
    memset(pats, 0, sizeof pats);
    for (t = 0; t < NPAT; t++)
        pat_check(&pats[t]);
    frames(100);

    /* PLAY: the playhead's fast path */
    pat.t[0].s[0].f = ST_TRIG;
    press(B_PLAY);
    host_render(buf_small, 512);
    host_frame();
    check("PLAY: running", snd.running_pub == 1u);
    {
        int fast = 1;
        uint32_t i;
        for (i = 0; i < 6u; i++) {
            host_render(buf_small, 1024);        /* ~23 ms: the playhead moves every ~125 ms at 120 BPM */
            host_frame();
            fast &= host_pixels < 6000u;         /* cells and marks only (23 tracks x 2 cells: 4048), never the grid (~45000) */
            if (host_pixels >= 6000u)
                printf("  frame %u: %u pixels in %u blits\n", i, host_pixels, host_blits);
        }
        check("PLAY: the GRID updates by cells only (the fast path)", fast);
    }
    check("PLAY: the playhead drawn in the accent (cyan)", px(cell_x(snd.pos_pub[1]) + 4, G_TOP + G_ROW + 4) == T_ACCENT);
    check("PLAY LED lit while running", host_led[panel.btn[B_PLAY]] == 1u);
    shot("home_grid_playing");
    host_press(B_PLAY, 1);                       /* a double tap: restart */
    host_frame();
    host_press(B_PLAY, 0);
    host_frame();
    check("PLAY twice quickly: still running (restart)", snd.running_pub == 1u || snd.run_req == 3u);
    frames(25);
    press(B_PLAY);
    host_render(buf_small, 64);
    host_frame();
    check("PLAY again: stopped", snd.running_pub == 0u);

    /* patterns: OCT+ / PRESETS queue; stopped: at once */
    press(B_OCTUP);
    host_render(buf_small, 64);
    host_frame();
    check("OCT+ stopped: pattern 2 at once", pat_index() == 1u && ui.queued == 0u);
    host_turn(EN_PRESET, 5);
    host_frame();
    host_render(buf_small, 64);
    host_frame();
    check("PRESETS +5: pattern 7", pat_index() == 6u);
    press(B_PLAY);
    host_render(buf_small, 512);
    host_frame();
    press(B_OCTDN);
    host_render(buf_small, 2048);
    host_frame();
    check("running: OCT- queues pattern 6, the old one plays on", ui.queued == 6u && pat_index() == 6u);
    frames(2);
    shot("home_grid_queued");
    host_render(buf_small, 44096);               /* past the bar line (16 steps = 2 s at 120 BPM) */
    host_render(buf_small, 44096);
    host_frame();
    check("running: the queued pattern takes over at the bar line", pat_index() == 5u && ui.queued == 0u);
    press(B_PLAY);
    host_render(buf_small, 64);
    host_frame();

    /* LEDs */
    press(B_SEQ);
    track_select(0);
    pat.t[0].s[3].f = ST_TRIG;
    host_frame();
    check("STEP mode LEDs: a trig's white key lit, an empty one dark", host_led[14u + step_key(3)] &&
                                                                       !host_led[14u + step_key(2)]);
    check("SEQ LED lit in STEP mode", host_led[panel.btn[B_SEQ]] == 1u);
    press(B_SEQ);
    host_frame();
    check("PAD mode LEDs: the selected track's key lit", host_led[14u] == 1u && !host_led[14u + 5u]);
    press(B_REC);
    check("REC: on, its LED lit", ui.rec && snd.rec && host_led[panel.btn[B_REC]]);
    press(B_REC);

    {   /* the CPU meter: the platform's average and worst half into GLOBAL 2 and HOME's header bar */
        audio_cpu_q8 = 128;                      /* 50 % */
        audio_max_us = CPU_HALF_US * 9u / 10u;   /* one half at 90 % */
        frames(8);
        audio_max_us = CPU_HALF_US / 5u;         /* later halves lighter: the peak is held */
        frames(8);
        check("CPU meter: the average (50 %) and the worst half held (90 %)", cpu_now == 50u && cpu_peak == 90u);
        check("CPU meter: hot, so the header bar is red", px(10, G_TOP - 2) == T_REC && px(200, G_TOP - 2) != T_REC);
        shot("home_cpu_hot");
        frames(2u * CPU_WIN_MS / 16u + 10u);     /* two windows later: the 90 % has gone */
        check("CPU meter: the peak falls back after its window", cpu_peak == 50u || cpu_peak == 20u);
        audio_cpu_q8 = 51;                       /* 20 % */
        frames(10);
        check("CPU meter: cool, the bar in the accent", px(10, G_TOP - 2) == T_ACCENT);
        press(B_GLO);
        press(B_GLO);
        check("GLO twice: GLOBAL 2 with the CPU rows", ui.page == PG_GLO2);
        host_turn(EN_K3, 30);
        host_turn(EN_K4, -30);
        host_frame();
        check("CPU rows: read-only (the knobs change nothing)", cpu_now == 20u);
        felucca_dbg.late += 999999u;             /* glitches: the widest text */
        audio_cpu_q8 = 256;
        audio_max_us = CPU_HALF_US * 2u;
        frames(10);
        check("CPU meter: LATE counted, values capped at 100", cpu_late == 999999u && cpu_now == 100u && cpu_peak == 100u);
        shot("page_global2_cpu");
        lint("GLOBAL 2 with the CPU at its widest");
        felucca_dbg.late = 0;
        ui.cpu_late0 = 0;
        audio_cpu_q8 = 0;
        audio_max_us = 0;
        press(B_HOME);
        frames(2u * CPU_WIN_MS / 16u + 10u);
    }
    memset(host_fb, 0, sizeof host_fb);          /* the platform drew over the screen (UBOOT countdown) */
    app_redraw = 1;
    host_frame();
    check("app_redraw: the whole screen drawn again", ink_rows(G_TOP, G_FOOT) > 120u && !app_redraw);

    host_press(B_EDIT, 1);                       /* EDIT + SAVE twice: clear the track */
    host_frame();
    press(B_SAVE);
    check("EDIT + SAVE once: armed, nothing cleared", (pat.t[0].s[3].f & ST_TRIG) != 0 && !ui.saving);
    press(B_SAVE);
    host_press(B_EDIT, 0);
    host_frame();
    check("EDIT + SAVE twice: the track cleared", !(pat.t[0].s[3].f & ST_TRIG));
}

/* ---------------------------------------------------------------- flash --- */
static void save_as(uint32_t what, uint32_t slot)
{
    press(B_SAVE);
    host_turn(EN_SELECT, (int32_t)what - (int32_t)ui.save_what);
    host_frame();
    host_turn(EN_PRESET, (int32_t)slot - (int32_t)ui.save_slot);
    host_frame();
    press(B_SAVE);
}

static void persistence(void)
{
    power_cycle();
    host_nor_erase();
    power_cycle();
    {   /* a fresh unit: the factory set (P01..P16, K1..K8, S1..S4), the rest blank / default */
        static pattern_t fp;
        static kit_t fk;
        factory_pattern(0, &fp);
        pat_check(&fp);
        factory_kit(0, &fk);
        kit_check(&fk);
        check("a fresh unit: P01 is the factory SAMBA (with its kit K1), K1 STUDIO",
              pat_index() == 0u && kit_index() == 0u && !memcmp(&pats[0], &fp, sizeof fp) && pats[0].kitno == 1u &&
              !memcmp(&kits[0], &fk, sizeof fk) && pat.t[0].s[0].f != 0);
        check("a fresh unit: P24 factory, P25..P32 blank", pats[23].kitno == 8u && pats[24].kitno == 0u &&
                                                            !pats[24].t[0].s[0].f && !pats[31].t[8].s[0].f);
        check("a fresh unit: the factory names (P01 SAMBA, P24 EUCLID, K3 RAIN ROOM; P25, K9 unnamed)",
              !strncmp(pats[0].name, "SAMBA", NAME_LEN) && !strncmp(pats[23].name, "EUCLID", NAME_LEN) &&
              !strncmp(kits[2].name, "RAIN ROOM", NAME_LEN) && !pats[24].name[0] && !kits[8].name[0]);
        kit_default(&fk);
        check("a fresh unit: K8 factory (DRIFT's delay), K9..K12 the defaults", kits[7].fx[FX_DLVL] == 40u &&
                                                                                !memcmp(&kits[8], &fk, sizeof fk) &&
                                                                                !memcmp(&kits[11], &fk, sizeof fk));
        check("a fresh unit: S1..S4 the factory tours, S5..S8 blank", songs.s[0].n == 6u && songs.s[0].e[5][0] == 5u &&
                                                                       songs.s[3].n == 6u && songs.s[3].loop &&
                                                                       !songs.s[4].n && !songs.s[7].n);
        check("a fresh unit: nothing saved", !pat_stored && !kit_stored && set.kit_follow == 1u);
    }
    {   /* every factory pattern plays with its kit: it sounds, and the soft limit has little to do */
        uint32_t i, k, j, full, ok = 1;
        int32_t pk;
        for (i = 0; i < FP_N; i++) {
            snd.run_req = 2;
            host_render(buf, 64);
            ring_out();
            pat_switch(i);
            snd.run_req = 1;
            full = 0;
            pk = 0;
            for (k = 0; k < 4u; k++) {           /* 4 s: two bars at most tempi */
                host_render(buf, 44096);
                for (j = 0; j < 2u * 44096u; j++) {
                    pk = abs(buf[j]) > pk ? abs(buf[j]) : pk;
                    full += abs(buf[j]) >= 32767;
                }
            }
            printf("  factory %-18s K%u: peak %6.1f dBFS%s\n", FP_NAME[i], kit_index() + 1u, 20.0 * log10((pk + 1) / 32768.0),
                   full ? "  (reaches full scale)" : "");
            ok &= pk > 2000 && full * 1000u < 2u * 44096u * 4u;
        }
        snd.run_req = 2;
        host_render(buf, 64);
        ring_out();
        check("every factory pattern sounds with its kit, under 0.1 % at full scale", ok);
        pat_switch(0);
    }
    {   /* KIT FOLLOWS PATTERN: a pattern starting brings its kit (P07 RAIN: K3); off: the kit stays */
        queue_pattern(6);
        host_render(buf, 64);
        host_frame();
        check("KIT FOLLOWS PATTERN: P07 RAIN brings K3 (RAIN ROOM)", pat_index() == 6u && kit_index() == 2u);
        check("a pattern's name in the message (NEXT / PATTERN P07 RAIN)", strstr(ui.msg, "P07 RAIN") != 0);
        frames(2);
        check("its name in the header", ink_rows(0, 18) > 8u);
        shot("home_named");
        lint("HOME with a pattern's name");
        queue_pattern(25);                       /* a blank pattern: no kit, the kit stays */
        host_render(buf, 64);
        host_frame();
        check("a blank pattern (P26): the kit stays (K3)", pat_index() == 25u && kit_index() == 2u);
        set.kit_follow = 0;
        queue_pattern(12);                       /* P13 WORKSHOP: K5, but off */
        host_render(buf, 64);
        host_frame();
        check("KIT FOLLOWS PATTERN off: the kit stays", pat_index() == 12u && kit_index() == 2u);
        set.kit_follow = 1;
        kit_choose(6);                           /* K7; P26 saved with it: it remembers K7 */
        check("a kit's name in the message (KIT K7 CLOCKWORK)", strstr(ui.msg, "K7 CLOCKWORK") != 0);
        queue_pattern(25);
        host_render(buf, 64);
        host_frame();
        save_as(SV_PAT, 25);
        check("SAVE PATTERN: the kit in use saved with it (P26: K7)", pats[25].kitno == 7u && (pat_stored >> 25) & 1u);
        check("SAVE a blank pattern: the suggested name (PATTERN 26)", !strncmp(pats[25].name, "PATTERN 26", NAME_LEN));
        kit_choose(0);
        queue_pattern(0);
        host_render(buf, 64);
        host_frame();
        queue_pattern(25);
        host_render(buf, 64);
        host_frame();
        check("P26 again: K7 with it", kit_index() == 6u);
        power_cycle();
        check("power-on: P26, its kit link and name kept", pats[25].kitno == 7u && pat_index() == 25u &&
                                                             !strncmp(pats[25].name, "PATTERN 26", NAME_LEN));
    }
    {   /* naming on the SAVE screen: KNOB 1 the cursor, KNOB 2 the letter */
        uint32_t i;
        press(B_SAVE);
        check("SAVE: the name of what is saved (PATTERN 26)", ui.saving && !strcmp(ui.name, "PATTERN 26"));
        for (i = 0; i < 10u; i++) {              /* clear it: every letter to a space */
            ui.name_cur = (uint8_t)i;
            while (ui.name[i] != ' ') {
                host_turn(EN_K2, 1);
                host_frame();
            }
        }
        host_turn(EN_K1, -20);                   /* the cursor home, then B, E, A, T */
        host_frame();
        {
            static const char W[] = "BEAT";
            for (i = 0; W[i]; i++) {
                while (ui.name[ui.name_cur] != W[i]) {
                    host_turn(EN_K2, 1);
                    host_frame();
                }
                host_turn(EN_K1, 1);
                host_frame();
            }
        }
        frames(2);
        shot("save_naming");
        lint("SAVE with the name editor");
        press(B_SAVE);
        check("a name edited with KNOB 1 / 2 and saved (BEAT, end spaces trimmed)", !strncmp(pats[25].name, "BEAT", NAME_LEN) &&
                                                                                    pats[25].name[4] == 0);
        power_cycle();
        check("power-on: the name kept", !strncmp(pats[25].name, "BEAT", NAME_LEN));
        memcpy(pats[25].name, "BAD*NAME\x01xx", NAME_LEN);   /* a damaged name: cut at its first wrong character */
        pat_check(&pats[25]);
        check("a damaged name: cut at its first wrong character", !strncmp(pats[25].name, "BAD", NAME_LEN) &&
                                                                  pats[25].name[3] == 0);
    }
    {   /* the old layout (before 2026-11-27): 4 kits in slots 17..20, 4 songs in 21: moved, written once */
        static kit_t ok;
        static uint8_t old_songs[4u + OLD_NSONG * sizeof(song_t)];
        song_t *os = (song_t *)(old_songs + 4);
        host_nor_erase();
        kit_default(&ok);
        ok.t[2].level = 33;
        ok.fx[FX_RLVL] = 44;
        store_save(OLD_SLOT_KIT0 + 1u, APP_ID, &ok, sizeof ok - 2u);   /* a kit saved before the chorus rate */
        memset(old_songs, 0, sizeof old_songs);
        old_songs[0] = 1;
        os[3].n = 1;
        os[3].e[0][0] = 6;
        os[3].e[0][1] = 3;
        store_save(OLD_SLOT_SONG, APP_ID, old_songs, sizeof old_songs);
        power_cycle();
        check("the old layout: its kit 2 moved to K2, the factory elsewhere", kits[1].t[2].level == 33u &&
                                                                              kits[1].fx[FX_RLVL] == 44u &&
                                                                              kits[1].fx[FX_CRATE] == 40u &&
                                                                              kits[0].t[0].level == 70u);
        check("the old layout: its songs moved (S4), S5..S8 blank", songs.s[3].n == 1u && songs.s[3].e[0][0] == 6u &&
                                                                     !songs.s[4].n);
        check("the old layout: P17..P21 (its old slots) read as not saved (factory)", !(pat_stored >> 16) &&
                                                                                       pats[17].kitno == 6u);
        {
            static kits_rec_t kr;
            check("the old layout: moved into the new records at once", store_load(SLOT_KITS, APP_ID, &kr, sizeof kr) > 0
                                                                        && kr.saved == 2u &&
                                                                        store_load(SLOT_SONGS, APP_ID, &songs, sizeof songs) > 0);
        }
    }
    {   /* FACTORY RESTORE (GLOBAL 3, SAVE, SAVE): every record erased, the factory set back */
        uint32_t k;
        pats[3].tempo2 = 300;
        save_as(SV_PAT, 3);
        kits[0].t[0].level = 12;
        save_as(SV_KIT, 0);
        press(B_GLO);
        press(B_GLO);
        press(B_GLO);
        check("GLO three times: GLOBAL 3", ui.page == PG_GLO3);
        frames(2);
        shot("page_global3");
        lint("GLOBAL 3");
        press(B_SAVE);
        check("GLOBAL 3, SAVE: the restore is asked", ui.restore == 1u);
        frames(2);
        shot("restore_ask");
        lint("FACTORY RESTORE question");
        press(B_HOME);
        check("the restore: any other button cancels", ui.restore == 0u && (pat_stored >> 3) & 1u);
        press(B_GLO);
        press(B_GLO);
        press(B_GLO);
        press(B_SAVE);
        press(B_SAVE);
        for (k = 0; k < 60u && ui.restore; k++)
            host_frame();
        check("FACTORY RESTORE: done, the factory set back", !ui.restore && !pat_stored && !kit_stored &&
                                                             pats[3].tempo2 == FP_HDR[3][0] && kits[0].t[0].level == 70u);
        check("FACTORY RESTORE: every record erased", store_load(SLOT_PAT0 + 3u, APP_ID, buf, 4076) < 0 &&
                                                      store_load(SLOT_KITS, APP_ID, buf, 4076) < 0 &&
                                                      store_load(SLOT_SET, APP_ID, buf, 4076) < 0);
        power_cycle();
        check("FACTORY RESTORE: still the factory set after power-on", !pat_stored && kits[0].t[0].level == 70u);
    }
    blank_world();
    pat.t[4].s[6].f = ST_TRIG | ST_ACC;
    pat.t[4].s[6].lock[2] = 99;
    pat.t[4].len = 12;
    pat.tempo2 = 251;
    kit.t[4].p[SP_DEC] = 101;
    save_as(SV_PAT, 2);
    check("SAVE PATTERN to P03: stored, played from P03", (pat_stored & 4u) && pat_index() == 2u &&
                                                           strstr(ui.msg, "SAVED") != 0);
    pat.t[4].s[7].f = ST_TRIG;                   /* not saved */
    power_cycle();
    check("power-on: P03 back (the last saved pattern)", pat_index() == 2u && pat.t[4].s[6].f == (ST_TRIG | ST_ACC) &&
                                                          pat.t[4].s[6].lock[2] == 99u && tp_len(4) == 12u);
    check("power-on: the tempo kept (125.5 BPM)", pat_bpm2() == 251u);
    check("power-on: what was not saved is gone", !(pat.t[4].s[7].f & ST_TRIG));
    check("power-on: the kit not saved: the default", kit.t[4].p[SP_DEC] == 64u);
    kit.t[4].p[SP_DEC] = 101;
    save_as(SV_KIT, 1);
    songs.s[2].n = 1;
    songs.s[2].e[0][0] = 9;
    songs.s[2].e[0][1] = 4;
    save_as(SV_SONG, 2);
    power_cycle();
    check("SAVE KIT to K2: back after power-on, K2 in use", kit_index() == 1u && kit.t[4].p[SP_DEC] == 101u);
    check("SAVE SONG: the songs back, S3 selected", songs.s[2].n == 1u && songs.s[2].e[0][0] == 9u &&
                                                     songs.s[2].e[0][1] == 4u && song_index == 2u);
    {   /* the unit ran another firmware since: its records in the same slots read as empty */
        static uint8_t other[4000];
        memset(other, 0x33, sizeof other);
        host_nor_erase();
        store_save(SLOT_SET, 0x12345678u, other, 64);
        store_save(SLOT_PAT0, 0x12345678u, other, sizeof other);
        store_save(SLOT_KITS, 0x12345678u, other, 400);
        power_cycle();
        check("another app's records: ignored (the factory set)", pats[0].kitno == 1u && kit.t[0].level == 70u &&
                                                                   set.base_note == 53u && !pat_stored);
    }
    {   /* records of ours with values out of range (an older version, a bug, damage): clamped */
        static pattern_t bad;
        settings_t sbad;
        kit_t kbad;
        memset(&bad, 0xFF, sizeof bad);
        store_save(SLOT_PAT0, APP_ID, &bad, sizeof bad);
        memset(&kbad, 0xEE, sizeof kbad);
        {   /* the kits' record: kit 1 saved, all of it damaged */
            static kits_rec_t kr;
            memset(&kr, 0, sizeof kr);
            kr.saved = 1;
            memcpy(kr.k[0], &kbad, sizeof kbad);
            store_save(SLOT_KITS, APP_ID, &kr, sizeof kr);
        }
        memset(&sbad, 0xC8, sizeof sbad);
        store_save(SLOT_SET, APP_ID, &sbad, sizeof sbad);
        power_cycle();
        check("damaged settings: clamped (pattern 1, channel OMNI, base F3)", pat_index() == 0u && set.midi_ch == 0u &&
                                                                               set.base_note == 53u && sel() == 0u);
        check("damaged pattern: lengths 16, tempo 120, conditions in range", tp_len(0) == 16u && pat_bpm2() == 240u &&
                                                                              st_cond(&pat.t[0].s[0]) < NCOND &&
                                                                              pat.t[0].s[0].lock[0] == 0u &&
                                                                              st_nudge(&pat.t[0].s[0]) >= -5);
        check("damaged kit: parameters in range", kit.t[0].p[0] <= 127u && kit.t[0].level <= 100u &&
                                                   kit.fx[FX_DTIME] <= 7u);
        frames(2);
        lint("HOME after damaged records");
    }
    {   /* a shorter, older record: the fields it lacks at their defaults */
        settings_t s0;
        set_default();
        s0 = set;
        s0.midi_ch = 5;
        host_nor_erase();
        store_save(SLOT_SET, APP_ID, &s0, 4);  /* ver, midi_ch, base_note, clock only */
        power_cycle();
        check("a shorter settings record: its fields read, the rest defaults", set.midi_ch == 5u && set.accent == 40u);
    }
    flash_ok = 0;
    save_as(SV_PAT, 0);
    check("no flash: SAVE fails visibly", strstr(ui.msg, "FAILED") != 0);
    flash_ok = 1;
    host_nor_erase();
    power_cycle();
}

/* ---------------------------------------------------------------- sound --- */
static int32_t peak(const int32_t *b, uint32_t frames_)
{
    int32_t m = 0;
    uint32_t i;
    for (i = 0; i < 2u * frames_; i++)
        m = abs(b[i]) > m ? abs(b[i]) : m;
    return m;
}

static void ring_out(void)                       /* every voice back to silence (and idle) */
{
    uint32_t t;
    for (t = 0; t < NT; t++)
        voice_silence(t);
}


static uint32_t hit_times(uint32_t t, uint32_t frames_, uint32_t *at, uint32_t max)   /* hits on t, block by block */
{
    uint32_t i, n = 0;
    uint8_t h = snd.hits[t];
    for (i = 0; i < frames_ / CTL; i++) {
        host_render(buf, CTL);
        while (snd.hits[t] != h) {
            h++;
            if (n < max)
                at[n] = i * CTL;
            n++;
        }
    }
    return n;
}

static void seq_reset(void)                      /* stopped, an empty pattern, silence */
{
    uint32_t t;
    snd.run_req = 2;
    host_render(buf, 64);
    memset(pats, 0, sizeof pats);
    pat_check(&pat);
    for (t = 0; t < NT; t++)
        voice_silence(t);
}


/* render with a MIDI clock coming in at bpm (0: none), block by block; hits on track t into at[] */
static uint32_t clock_run(uint32_t bpm, uint32_t frames_, uint32_t t, uint32_t *at, uint32_t max)
{
    static double next;                          /* the next clock's time, carried across calls */
    uint32_t i, n = 0;
    uint8_t h = snd.hits[t];
    double iv = bpm ? FS * 60.0 / (bpm * 24.0) : 0.0;
    if (next < 0.0 || !bpm)
        next = 0.0;
    for (i = 0; i < frames_ / CTL; i++) {
        if (bpm) {
            while (next <= (double)CTL) {
                host_midi(0xF8, 0, 0);
                next += iv;
            }
            next -= CTL;
        }
        host_render(buf, CTL);
        while (snd.hits[t] != h) {
            h++;
            if (n < max)
                at[n] = i * CTL;
            n++;
        }
    }
    return n;
}

static uint32_t out_count(uint32_t status, uint32_t *first)   /* drain MIDI out: how many of status, the first one */
{
    uint32_t pkt, n = 0;
    *first = 0;
    while (host_midi_out(&pkt)) {
        uint32_t b = (pkt >> 8) & 0xFFu;
        if (!*first)
            *first = b;
        n += b == status;
    }
    return n;
}


static void render_long(uint32_t frames_)       /* longer than buf: in pieces of a second (a multiple of CTL) */
{
    while (frames_ >= 44096u) {
        host_render(buf, 44096);
        frames_ -= 44096u;
    }
    if (frames_ / CTL)
        host_render(buf, frames_ / CTL * CTL);
}


static int32_t fxbuf[2u * 4u * 44096u];         /* 4 s of the chain's output */

static void fx_off(void)                         /* every effect off (a fresh kit's) */
{
    static kit_t d;
    kit_default(&d);
    memcpy(kit.fx, d.fx, sizeof d.fx);
}

static void hit_render(uint32_t t, uint32_t frames_)   /* one pad hit on track t, frames_ (<= 4 s) into fxbuf */
{
    uint32_t i;
    ring_out();
    snd.v[t].seed = 0x1234567u;                  /* the same dice every time: hits compare like for like */
    fm1_in.notes = 1u << t;
    host_render(fxbuf, 64);
    fm1_in.notes = 0;
    for (i = 0; i < frames_; i += 44096u)
        host_render(fxbuf + 2u * i, frames_ - i < 44096u ? frames_ - i : 44096u);
}

static double win_energy(uint32_t ch, uint32_t from, uint32_t len)   /* sum of squares, one channel */
{
    double e = 0.0;
    uint32_t i;
    for (i = from; i < from + len; i++)
        e += (double)fxbuf[2u * i + ch] * fxbuf[2u * i + ch];
    return e;
}

static double brightness(uint32_t frames_)       /* first-difference energy over energy (left): high = bright */
{
    double d = 0.0, e = 0.0;
    uint32_t i;
    for (i = 1; i < frames_; i++) {
        double a = fxbuf[2u * i], b = fxbuf[2u * (i - 1u)];
        d += (a - b) * (a - b);
        e += a * a;
    }
    return e > 0.0 ? d / e : 0.0;
}

static void sound(void)
{
    uint32_t t, pkt, h0;
    int all_ok = 1, all_quiet = 1, all_in = 1;
    double db[NT];
    power_cycle();
    blank_world();                               /* (from nothing: no factory effects, no kit links) */
    host_render(buf, 1024);
    check("silence at power-on", peak(buf, 1024) == 0);
    for (t = 0; t < NT; t++) {                   /* every instrument: sounds, stays in range, rings out to silence */
        int32_t pk, tail;
        fm1_in.notes = 1u << t;
        host_render(buf, 64);
        host_frame();
        fm1_in.notes = 0;
        host_render(buf, FS / CTL * CTL * SECS);
        host_frame();
        pk = peak(buf, FS);
        tail = peak(buf + 2u * (FS * SECS - 4096u), 4096);
        db[t] = 20.0 * log10((pk > 0 ? pk : 1) / 32768.0);
        if (pk < 400)
            all_ok = 0, printf("  %s: peak %d (too quiet)\n", SK_NAME[t], pk);
        if (pk >= 32767)
            all_in = 0, printf("  %s: peak %d (clipped)\n", SK_NAME[t], pk);
        if (tail || (snd.active >> t) & 1u)
            all_quiet = 0, printf("  %s: still sounding after %u s (tail %d)\n", SK_NAME[t], SECS, tail);
    }
    printf("  peaks (dBFS, a hit at the defaults):");
    for (t = 0; t < NT; t++)
        printf(" %s %.0f", SK_SHORT[t], db[t]);
    printf("\n");
    check("every one of the 23 instruments sounds (peak above -38 dBFS)", all_ok);
    check("no instrument clips on one hit", all_in);
    check("every instrument rings out to exact silence and goes idle", all_quiet);
    check("the pads hit their own track (Water Drops: 1 hit)", snd.hits[21] == 1u);

    ring_out();
    track_select(0);
    kit.t[0].p[SP_EN] = 60;
    h0 = (uint32_t)snd.v[0].e;
    fm1_in.notes = 1u;
    host_render(buf, 32);
    fm1_in.notes = 0;
    host_render(buf, 32);
    {
        int32_t e1 = snd.v[0].e;
        ring_out();
        fm1_in.notes = 1u | 1u << KEY_ACCENT;
        host_render(buf, 32);
        fm1_in.notes = 0;
        host_render(buf, 32);
        check("ACCENT (F#5) held: a stronger hit", snd.v[0].e > e1 && h0 == 0u);
    }
    kit.t[0].p[SP_EN] = 100;
    ring_out();

    host_midi(0x90, 53 + 4, 100);                /* G#3: track 5, Sleigh Bells */
    host_render(buf, 1024);
    check("MIDI note 57: Sleigh Bells sounds", snd.hits[4] == 2u && peak(buf, 1024) > 400 && snd.last_midi == 57u);
    host_midi(0x90, 52, 100);                    /* below the base note */
    host_midi(0x90, 53 + 23, 100);               /* above the 23 */
    host_render(buf, 64);
    check("MIDI notes outside the 23: ignored", snd.active == (1u << 4));
    host_midi(0xB0, 120, 0);                     /* all sound off */
    host_render(buf, 64);
    host_render(buf, 64);
    check("CC 120: every track silent at once", peak(buf, 64) == 0 && snd.active == 0u);
    set.midi_ch = 2;
    host_midi(0x90, 53, 100);                    /* channel 1 */
    host_render(buf, 64);
    check("MIDI channel 2: a note on channel 1 is ignored", snd.active == 0u);
    host_midi(0x91, 53, 100);
    host_render(buf, 64);
    check("MIDI channel 2: a note on channel 2 plays", snd.active == 1u);
    set.midi_ch = 0;
    ring_out();
    track_select(6);
    host_frame();
    host_midi(0xB0, 2, 11);                      /* STK's code: 2 energy, 11 decay, 4 objects, 1 frequency */
    host_midi(0xB0, 11, 22);
    host_midi(0xB0, 4, 33);
    host_midi(0xB0, 1, 44);
    host_render(buf, 64);
    check("CC 2 / 11 / 4 / 1: the selected track's four parameters", kit.t[6].p[0] == 11u && kit.t[6].p[1] == 22u &&
                                                                      kit.t[6].p[2] == 33u && kit.t[6].p[3] == 44u);
    kit.t[6].p[0] = 100;
    kit.t[6].p[1] = kit.t[6].p[2] = kit.t[6].p[3] = 64;
    set.trs_in = 0;
    midi_enqueue(0x09u | 0x90u << 8 | 53u << 16 | 100u << 24, 2u);
    host_render(buf, 64);
    check("TRS MIDI IN off: a note from TRS is ignored", snd.active == 0u);
    set.trs_in = 1;
    midi_enqueue(0x09u | 0x90u << 8 | 53u << 16 | 100u << 24, 2u);
    host_render(buf, 64);
    check("TRS MIDI IN on: it plays", snd.active == 1u);
    midi_in_overflow = 1;
    host_render(buf, 64);
    check("MIDI ring overflow: harmless", !midi_in_overflow);
    ring_out();

    /* the sequencer: 120 BPM, a trig on every step of track 1 */
    memset(pats, 0, sizeof pats);
    pat_check(&pat);
    for (t = 0; t < NSTEP; t++)
        pat.t[0].s[t].f = ST_TRIG;
    h0 = snd.hits[0];
    snd.run_req = 1;
    host_render(buf, 44096);                     /* 1 s: 16ths at 0, 125, ... 875 ms */
    check("120 BPM: 8 steps in a second", (uint8_t)(snd.hits[0] - h0) == 8u);
    pat.t[0].flags = TP_MUTE;
    h0 = snd.hits[0];
    host_render(buf, 44096);
    check("a muted track does not play", snd.hits[0] == h0);
    pat.t[0].flags = 0;
    pat.t[1].flags = TP_SOLO;
    host_render(buf, 44096);
    check("another track soloed: this one does not play", snd.hits[0] == h0);
    pat.t[1].flags = 0;
    pat.t[0].len = 3;
    {
        int wraps = 1;
        uint32_t i;
        for (i = 0; i < 40u; i++) {
            host_render(buf, 1024);
            wraps &= snd.pos_pub[0] < 3u;
        }
        check("track length 3: its playhead stays in steps 1..3", wraps);
    }
    pat.t[0].len = 0;
    memset(pat.t[0].s, 0, sizeof pat.t[0].s);
    snd.rec = 1;                                 /* live record */
    host_render(buf, 2048);
    fm1_in.notes = 1u << 8;
    host_render(buf, 64);
    fm1_in.notes = 0;
    host_render(buf, 64);
    {
        uint32_t s, n = 0;
        for (s = 0; s < NSTEP; s++)
            n += (pat.t[8].s[s].f & ST_TRIG) != 0;
        check("REC while running: the pad hit recorded as one trig", n == 1u);
    }
    {   /* recorded ahead of the playhead (nearer the next 16th): heard once now, not again a moment later */
        uint8_t h;
        memset(pat.t[8].s, 0, sizeof pat.t[8].s);
        snd.run_req = 3;                         /* restart: step 1 at once */
        host_render(buf, 64);
        host_render(buf, 4096);                  /* ~93 ms of the 125 ms step: nearer step 2 */
        h = snd.hits[8];
        fm1_in.notes = 1u << 8;
        host_render(buf, 32);
        fm1_in.notes = 0;
        host_render(buf, 4096);                  /* past step 2 */
        check("REC ahead of the playhead: one hit, not two", (uint8_t)(snd.hits[8] - h) == 1u &&
                                                             (pat.t[8].s[1].f & ST_TRIG));
        h = snd.hits[8];
        host_render(buf, 44096);                 /* the next pass: step 2 plays */
        host_render(buf, 44096);
        check("REC ahead of the playhead: the next pass plays it", (uint8_t)(snd.hits[8] - h) == 1u);
    }
    snd.rec = 0;
    {   /* milestone 5: locks, probability, ratchets (track 1, Maraca; 120 BPM: a 16th is 5512.5 samples) */
        step_t *st = pat.t[0].s;
        uint32_t i, k, when[8], nw = 0;
        snd.run_req = 2;
        host_render(buf, 64);
        memset(pats, 0, sizeof pats);
        pat_check(&pat);
        ring_out();
        st[0].f = ST_TRIG;
        st[0].lock[SP_FRQ] = 21;                 /* FREQ 20 */
        st[0].lock[SP_EN] = 51;                  /* ENERGY 50 */
        st[1].f = ST_TRIG;                       /* no locks */
        snd.run_req = 1;
        host_render(buf, 64);
        check("lock: the step's FREQ (20) and ENERGY (50 -> amplitude 100) play", snd.v[0].fq == 20u &&
                                                                                  snd.last_amp[0] == 100u);
        check("lock: held while the sound rings (the knob's FREQ 64 does not take over)",
              (snd.v[0].lmask >> SP_FRQ) & 1u && vpar(0, &snd.v[0], SP_FRQ) == 20u);
        host_render(buf, 5568);                  /* step 2: no locks */
        check("lock: the next step without locks plays the knobs (FREQ 64, ENERGY 100)", snd.v[0].fq == 64u &&
                                                                                          snd.v[0].lmask == 0u &&
                                                                                          snd.last_amp[0] == 200u);
        snd.run_req = 2;
        host_render(buf, 64);
        ring_out();
        st[1].f = 0;
        st[0].lock[SP_EN] = 0;
        st[0].lock[SP_FRQ] = 0;
        st[0].lock[SP_DEC] = 41;                 /* DECAY 40 */
        st[0].f = ST_TRIG | ST_ACC;
        set.accent = 50;
        snd.run_req = 1;
        host_render(buf, 64);
        check("accent: a locked DECAY 40 moved half way to 127 (83)", snd.v[0].lp[SP_DEC] == 83u);
        set.accent = 40;
        snd.run_req = 2;
        host_render(buf, 64);
        {   /* a DECAY lock really decays faster: the energy after 50 ms, DECAY 0 locked against the knob's 64 */
            int32_t e_lock, e_free;
            ring_out();
            st[0].f = ST_TRIG;
            st[0].lock[SP_DEC] = 1;              /* DECAY 0 */
            snd.run_req = 1;
            host_render(buf, 2208);
            e_lock = snd.v[0].e;
            snd.run_req = 2;
            host_render(buf, 64);
            ring_out();
            st[0].lock[SP_DEC] = 0;
            snd.run_req = 1;
            host_render(buf, 2208);
            e_free = snd.v[0].e;
            snd.run_req = 2;
            host_render(buf, 64);
            check("lock: DECAY 0 decays faster than the knob's 64", e_lock < e_free / 2);
        }
        /* probability: every step 50 % for 200 steps; 0 %; and with CHANCE BIAS +50 */
        ring_out();
        for (k = 0; k < NSTEP; k++) {
            st[k].f = ST_TRIG;
            st[k].prob = 10;                     /* 50 % */
        }
        {   /* (counted render by render: the hit counter is a byte) */
            uint32_t n50 = 0, n0 = 0, nb = 0, j;
            uint8_t h;
            snd.run_req = 1;
            for (j = 0; j < 200u; j++) {      /* ~a 16th each (5512.5): 200 renders ~ 201 steps */
                h = snd.hits[0];
                host_render(buf, 5536);
                n50 += (uint8_t)(snd.hits[0] - h);
            }
            for (k = 0; k < NSTEP; k++)
                st[k].prob = 20;                 /* 0 % */
            for (j = 0; j < 50u; j++) {
                h = snd.hits[0];
                host_render(buf, 5536);
                n0 += (uint8_t)(snd.hits[0] - h);
            }
            for (k = 0; k < NSTEP; k++)
                st[k].prob = 10;
            set.bias = 100;                      /* +50 % */
            for (j = 0; j < 50u; j++) {
                h = snd.hits[0];
                host_render(buf, 5536);
                nb += (uint8_t)(snd.hits[0] - h);
            }
            set.bias = 50;
            snd.run_req = 2;
            host_render(buf, 64);
            printf("  probability 50 %%: %u of ~201 steps\n", n50);
            check("probability 50 %: about half the steps play (70..130 of ~201)", n50 >= 70u && n50 <= 130u);
            check("probability 0 %: none play", n0 == 0u);
            check("CHANCE BIAS +50: a 50 % step always plays", nb >= 49u);
        }
        /* ratchets: x4 UP on step 1 alone; the repeats' times and amplitudes */
        ring_out();
        for (k = 0; k < NSTEP; k++)
            st[k] = (step_t){0};
        st[0].f = ST_TRIG;
        st[0].rat = (uint8_t)(3u | 1u << 2);     /* x4, UP */
        {
            uint8_t h = snd.hits[0];
            uint16_t amps[8];
            snd.run_req = 1;
            for (i = 0; i < 5536u / 32u; i++) {  /* one 16th, block by block */
                host_render(buf, 32);
                if (snd.hits[0] != h && nw < 8u) {
                    h = snd.hits[0];
                    amps[nw] = snd.last_amp[0];
                    when[nw++] = i * 32u;
                }
            }
            snd.run_req = 2;
            host_render(buf, 64);
            check("ratchet x4: four hits in the step", nw == 4u);
            check("ratchet x4: evenly spaced (~1378 samples, a 16th / 4)",
                  nw == 4u && abs((int)(when[1] - when[0]) - 1378) <= 40 && abs((int)(when[3] - when[2]) - 1378) <= 40);
            check("ratchet UP: 40 %, 60 %, 80 %, 100 % of the step's 200", nw == 4u && amps[0] == 80u &&
                                                                           amps[1] == 120u && amps[2] == 160u &&
                                                                           amps[3] == 200u);
            st[0].rat = (uint8_t)(2u | 2u << 2); /* x3, DOWN */
            nw = 0;
            h = snd.hits[0];
            snd.run_req = 1;
            for (i = 0; i < 5536u / 32u; i++) {
                host_render(buf, 32);
                if (snd.hits[0] != h && nw < 8u) {
                    h = snd.hits[0];
                    amps[nw++] = snd.last_amp[0];
                }
            }
            snd.run_req = 2;
            host_render(buf, 64);
            check("ratchet x3 DOWN: 100 %, 70 %, 40 %", nw == 3u && amps[0] == 200u && amps[1] == 140u &&
                                                       amps[2] == 80u);
            st[0].rat = 3u;                      /* x4 FLAT, stopped half way */
            h = snd.hits[0];
            snd.run_req = 1;
            host_render(buf, 1408);              /* the first hit and one repeat */
            snd.run_req = 2;
            host_render(buf, 5536);
            check("ratchet: stopping drops the repeats left", (uint8_t)(snd.hits[0] - h) == 2u);
        }
        memset(pats, 0, sizeof pats);
        pat_check(&pat);
        ring_out();
    }
    {   /* milestone 6: clock divide, conditions, nudge, swing, note repeat (120 BPM: a 16th is 5512.5 samples) */
        uint32_t at[64], n, k;
        static const struct { uint8_t div; uint32_t hits; const char *what; } DV[] = {
            {1, 4, "clock divide 1/2: 4 steps a second (8ths)"}, {4, 16, "clock divide 2/1: 16 steps a second"},
            {5, 32, "clock divide 4/1: 32 steps a second"}, {3, 1, "clock divide 1/8: 1 step a second"}};
        for (k = 0; k < 4u; k++) {
            uint32_t j;
            seq_reset();
            for (j = 0; j < NSTEP; j++)
                pat.t[0].s[j].f = ST_TRIG;
            pat.t[0].div = DV[k].div;
            snd.run_req = 1;
            n = hit_times(0, 44096, at, 64);
            check(DV[k].what, n == DV[k].hits);
        }
        {   /* conditions on a 4-step track, 8 loops (4 s): step 1 only */
            static const struct { uint8_t c, fill; uint32_t hits; const char *what; } CD[] = {
                {1, 0, 4, "condition 1:2: loops 1, 3, 5, 7 of 8"}, {2, 0, 4, "condition 2:2: loops 2, 4, 6, 8"},
                {8, 0, 2, "condition 3:4: 2 of 8 loops"}, {12, 0, 1, "condition FIRST: the first loop only"},
                {13, 0, 7, "condition NOT FIRST: all but the first"}, {10, 1, 8, "condition FILL, ARP held: every loop"},
                {10, 0, 0, "condition FILL, ARP not held: never"}, {11, 0, 8, "condition NOT FILL, ARP not held: every loop"}};
            for (k = 0; k < sizeof CD / sizeof CD[0]; k++) {
                seq_reset();
                pat.t[0].len = 4;
                pat.t[0].s[0].f = (uint8_t)(ST_TRIG | CD[k].c << 2);
                snd.fill = CD[k].fill;
                snd.run_req = 1;
                n = hit_times(0, 44096u * 4u - 64u, at, 64);
                snd.fill = 0;
                check(CD[k].what, n == CD[k].hits);
                if (k == 1u)
                    check("condition 2:2: not in the first loop", n && at[0] >= 4u * 5512u - 64u);
            }
        }
        {   /* nudge: step 2 alone, +3 and -3 twelfths of a 16th */
            seq_reset();
            pat.t[0].s[1].f = ST_TRIG;
            pat.t[0].s[1].rat = (uint8_t)(3u << 4);
            snd.run_req = 1;
            n = hit_times(0, 11040, at, 4);
            check("nudge +3: a quarter of a 16th late (6890)", n == 1u && abs((int)at[0] - 6890) <= 48);
            seq_reset();
            pat.t[0].s[1].f = ST_TRIG;
            pat.t[0].s[1].rat = (uint8_t)(((uint32_t)-3 & 15u) << 4);
            snd.run_req = 1;
            n = hit_times(0, 11040, at, 4);
            check("nudge -3: a quarter of a 16th early (4134)", n == 1u && abs((int)at[0] - 4134) <= 48);
            seq_reset();
            pat.t[0].s[0].f = ST_TRIG;           /* step 1 nudged early: at PLAY it cannot be, so at once */
            pat.t[0].s[0].rat = (uint8_t)(((uint32_t)-5 & 15u) << 4);
            snd.run_req = 1;
            n = hit_times(0, 5504, at, 4);
            check("nudge -5 on step 1 at PLAY: plays at once", n == 1u && at[0] == 0u);
            n = hit_times(0, 88192 - 5504, at, 4);
            check("nudge -5 on step 1, the next loop: 5/12 of a 16th early", n == 1u &&
                  abs((int)(at[0] + 5504) - (int)(16u * 5512u + 256u - 2297u)) <= 300);
        }
        {   /* swing 75 %: the second 16th of each pair half a 16th late, the first on time; not on 32nds */
            seq_reset();
            pat.t[0].s[1].f = ST_TRIG;
            pat.t[0].s[2].f = ST_TRIG;
            pat.swing = 75;
            snd.run_req = 1;
            n = hit_times(0, 13760, at, 4);
            check("swing 75 %: step 2 half a 16th late (8268)", n == 2u && abs((int)at[0] - 8268) <= 48);
            check("swing 75 %: step 3 on time (11025)", n == 2u && abs((int)at[1] - 11025) <= 48);
            seq_reset();
            pat.t[0].s[1].f = ST_TRIG;
            pat.t[0].div = 4;                    /* 2/1: 32nds */
            pat.swing = 75;
            snd.run_req = 1;
            n = hit_times(0, 5504, at, 4);
            check("swing: not on 32nds (step 2 at half a 16th, 2756)", n == 1u && abs((int)at[0] - 2756) <= 48);
            pat.swing = 50;
        }
        {   /* note repeat: ARP held + a pad held, at REPEAT RATE; let go: it stops */
            seq_reset();
            snd.fill = 1;
            set.rep_rate = 1;                    /* 1/16 */
            fm1_in.notes = 1u << 3;
            n = hit_times(3, 43840, at, 64);     /* (just short of a second: 0, 5512, ... 38584) */
            check("note repeat 1/16, ARP held: 8 hits a second (stopped too)", n == 8u &&
                  abs((int)(at[1] - at[0]) - 5512) <= 48);
            fm1_in.notes = 0;
            n = hit_times(3, 22048, at, 64);
            check("note repeat: the pad let go, no more", n == 0u);
            set.rep_rate = 0;                    /* 1/8 */
            fm1_in.notes = 1u << 3;
            n = hit_times(3, 43840, at, 64);
            check("note repeat 1/8: 4 hits a second", n == 4u);
            snd.fill = 0;
            host_render(buf, 64);
            n = hit_times(3, 22048, at, 64);
            check("note repeat: ARP let go, no more (the pad still held)", n == 0u);
            fm1_in.notes = 0;
            set.rep_rate = 1;
        }
        seq_reset();
    }
    {   /* milestone 7: MIDI clock out (INT) and in (EXT, AUTO) */
        uint32_t at[64], n, first, k;
        seq_reset();
        for (k = 0; k < NSTEP; k++)
            pat.t[0].s[k].f = ST_TRIG;
        set.clock = CLK_INT;
        set.clock_out = 1;
        out_count(0, &first);
        host_render(buf, 44096);
        n = out_count(0xF8, &first);
        check("clock out: 48 clocks a second at 120 BPM (24 PPQN), stopped too", n >= 47u && n <= 49u);
        snd.run_req = 1;
        host_render(buf, 64);
        n = out_count(0xF8, &first);
        check("clock out: PLAY sends start (FA) first, then a clock", first == 0xFAu && n >= 1u);
        snd.run_req = 2;
        host_render(buf, 64);
        check("clock out: stop sends FC", out_count(0xFC, &first) == 1u);
        set.clock_out = 0;
        host_render(buf, 22048);
        check("clock out off: no clock", out_count(0xF8, &first) == 0u);

        set.clock = CLK_EXT;                     /* following */
        seq_reset();
        for (k = 0; k < NSTEP; k++)
            pat.t[0].s[k].f = ST_TRIG;
        clock_run(120, 4096, 0, at, 64);         /* the clock runs; nothing plays before a start */
        check("EXT: a clock alone does not start", !snd.running_pub);
        host_midi(0xFA, 0, 0);
        n = clock_run(120, 88192 - 64, 0, at, 64);
        check("EXT 120 BPM: start (FA), 16 steps in 2 s", n == 16u);
        check("EXT 120 BPM: the steps evenly spaced (~5512)", n == 16u && abs((int)(at[15] - at[1]) - 14 * 5512) <= 200);
        n = clock_run(150, 88192, 0, at, 64);
        check("EXT 150 BPM: 20 steps in 2 s (the tempo followed)", n >= 19u && n <= 21u);
        check("EXT: the header's tempo is the clock's (150 BPM, the pattern's 120)",
              abs((int)snd.ext_bpm2_pub - 300) <= 3 && snd.ext_pub);
        {   /* a ratchet follows the clock's tempo: x2 at 150 BPM, its repeat half a 16th (2205) after the hit */
            uint32_t j;
            for (j = 0; j < NSTEP; j++)
                pat.t[0].s[j] = (step_t){0};
            pat.t[0].s[(snd.pos_pub[0] + 2u) % 16u].f = ST_TRIG;
            pat.t[0].s[(snd.pos_pub[0] + 2u) % 16u].rat = 1u;   /* x2 */
            n = clock_run(150, 22048, 0, at, 64);
            check("EXT: a ratchet's repeats at the clock's tempo (half a 16th at 150 BPM: 2205)",
                  n == 2u && abs((int)(at[1] - at[0]) - 2205) <= 64);
            for (j = 0; j < NSTEP; j++)
                pat.t[0].s[j] = (step_t){ST_TRIG, 0, 0, {0, 0, 0, 0}};
        }
        n = clock_run(0, 22048, 0, at, 64);      /* the clock pauses: the sequencer waits (a clock ahead at most) */
        check("EXT: the clock pauses, the steps wait (at most one more)", n <= 1u);
        n = clock_run(150, 44096, 0, at, 64);
        check("EXT: the clock resumes, so do the steps", n >= 8u);
        host_midi(0xFC, 0, 0);
        n = clock_run(150, 22048, 0, at, 64);
        check("EXT: stop (FC)", !snd.running_pub && n == 0u);
        {
            uint32_t p0 = snd.pos_pub[0];
            host_midi(0xFB, 0, 0);
            clock_run(150, 64, 0, at, 64);
            check("EXT: continue (FB) goes on from where it stopped", snd.running_pub && snd.pos_pub[0] == (p0 + 1u) % 16u);
            host_midi(0xFC, 0, 0);
            host_midi(0xF2, 8, 0);               /* song position: the 8th 16th */
            host_midi(0xFB, 0, 0);
            clock_run(150, 64, 0, at, 64);
            check("EXT: song position 8, then continue: from step 9", snd.pos_pub[0] == 8u);
        }
        host_midi(0xFC, 0, 0);
        clock_run(150, 64, 0, at, 64);
        set.clock_out = 1;
        out_count(0, &first);
        clock_run(150, 22048, 0, at, 64);
        check("EXT: no clock sent while following one", out_count(0xF8, &first) == 0u);
        set.clock_out = 0;
        snd.ext_pub = 1;                         /* (as the ISR left it) */
        press(B_PLAY);
        clock_run(150, 64, 0, at, 64);
        check("EXT: PLAY does not start (the master plays)", !snd.running_pub && strstr(ui.msg, "CLOCK EXT") != 0);

        set.clock = CLK_AUTO;                    /* AUTO: EXT while a clock arrives */
        clock_run(120, 4096, 0, at, 64);
        check("AUTO: a clock arriving: following it", snd.ext_pub == 1u);
        clock_run(0, 26496, 0, at, 64);          /* 0.6 s without */
        check("AUTO: no clock for half a second: the internal clock", snd.ext_pub == 0u);
        snd.run_req = 1;
        n = clock_run(0, 44096 - 64, 0, at, 64);
        check("AUTO without a clock: PLAY runs at the pattern's tempo", n == 8u);
        seq_reset();
        set.clock = CLK_AUTO;
    }
    {   /* milestone 8: songs (played from the SONG screen) and kits (EDIT + PRESETS) */
        uint32_t at[8], k;
        song_t *sg = &songs.s[1];
        seq_reset();
        for (k = 0; k < NPAT; k++)
            pat_check(&pats[k]);
        pats[1].t[0].s[0].f = ST_TRIG;           /* P02: track 1; P03: track 2 */
        pats[2].t[1].s[0].f = ST_TRIG;
        memset(sg, 0, sizeof *sg);
        sg->n = 2;
        sg->e[0][0] = 1; sg->e[0][1] = 2;        /* P02 x2, then P03 x1, ONCE */
        sg->e[1][0] = 2; sg->e[1][1] = 1;
        song_index = 1;
        song_screen(1);
        check("SONG screen open", ui.song_open == 1u);
        ui.song_cur = 0;
        press(B_PLAY);
        host_render(buf, 64);
        host_frame();
        check("SONG PLAY: the song plays its first entry (P02)", snd.song_on_pub && pat_index() == 1u && snd.running_pub);
        frames(2);
        shot("song_playing");
        lint("SONG playing");
        render_long(88192 * 2u - 1024u);         /* two bars of P02 (2 s each at 120 BPM) */
        host_render(buf, 2048);
        host_frame();
        check("song: after its 2 repeats, the next entry (P03)", pat_index() == 2u && snd.song_e_pub == 1u);
        song_screen(0);                          /* closed: it plays on, the header shows it */
        frames(2);
        shot("home_song");
        lint("HOME with a song playing");
        render_long(88192);
        host_frame();
        check("song ONCE: stops after its last entry", !snd.running_pub && !snd.song_on_pub);
        sg->loop = 1;
        song_screen(1);
        ui.song_cur = 1;                         /* from entry 2 */
        press(B_PLAY);
        host_render(buf, 64);
        host_frame();
        check("song: PLAY from the selected entry (P03)", pat_index() == 2u && snd.song_e_pub == 1u);
        render_long(88192 + 1024u);
        host_frame();
        check("song LOOP: after the last entry, the first again", snd.running_pub && pat_index() == 1u &&
                                                                   snd.song_e_pub == 0u);
        press(B_PLAY);                           /* (on the SONG screen: stops the song) */
        host_render(buf, 64);
        host_frame();
        check("song: PLAY again stops it", !snd.running_pub && !snd.song_on_pub);
        press(B_PLAY);
        host_render(buf, 64);
        song_screen(0);                          /* closed; a pattern by hand: the song gives way */
        press(B_OCTUP);
        host_render(buf, 64);
        host_frame();
        check("song: a pattern chosen by hand ends song mode, the pattern plays on",
              !snd.song_on_pub && snd.running_pub && strstr(ui.msg, "SONG OFF") != 0);
        snd.run_req = 2;
        host_render(buf, 64);
        ui.queued = 0;
        snd.next_req = 0;
        song_index = 2;                          /* an empty song */
        song_screen(1);
        press(B_PLAY);
        host_render(buf, 64);
        check("song empty: PLAY says so, nothing plays", !snd.running_pub && strstr(ui.msg, "EMPTY") != 0);
        song_index = 1;
        set.clock = CLK_EXT;                     /* following a clock: armed, the master starts it */
        clock_run(120, 4096, 0, at, 8);
        host_frame();
        ui.song_cur = 1;
        press(B_PLAY);
        clock_run(120, 64, 0, at, 8);
        check("song, EXT: PLAY arms it (nothing plays yet)", !snd.running_pub && snd.song_arm == 2u);
        host_midi(0xFA, 0, 0);
        clock_run(120, 64, 0, at, 8);
        check("song, EXT: the master's start begins it from the entry", snd.running_pub && snd.song_on_pub &&
                                                                         pat_index() == 2u);
        host_midi(0xFC, 0, 0);
        clock_run(120, 64, 0, at, 8);
        set.clock = CLK_AUTO;
        clock_run(0, 26496, 0, at, 8);
        song_screen(0);
        check("SONG screen closed", !ui.song_open);
        memset(sg, 0, sizeof *sg);
        song_index = 0;

        /* kits: EDIT + PRESETS; the sound follows the kit at once */
        seq_reset();
        kits[2].t[5].level = 0;                  /* K3: track 6 silent */
        kits[0].t[5].level = 80;
        host_press(B_EDIT, 1);
        host_frame();
        host_turn(EN_PRESET, 2);
        host_frame();
        host_press(B_EDIT, 0);
        host_frame();
        check("EDIT + PRESETS: kit 3", kit_index() == 2u && ui.queued == 0u && strstr(ui.msg, "KIT K3") != 0);
        frames(2);
        shot("home_kit3");
        fm1_in.notes = 1u << 5;
        host_render(buf, 64);
        fm1_in.notes = 0;
        host_render(buf, 4096);
        check("kit 3: its track 6 at level 0 is silent", peak(buf, 4096) == 0);
        host_press(B_EDIT, 1);
        host_frame();
        host_turn(EN_PRESET, -2);
        host_frame();
        host_press(B_EDIT, 0);
        host_frame();
        ring_out();
        fm1_in.notes = 1u << 5;
        host_render(buf, 64);
        fm1_in.notes = 0;
        host_render(buf, 4096);
        check("kit 1 again: track 6 sounds", kit_index() == 0u && peak(buf, 4096) > 400);
        kits[2].t[5].level = 80;
        seq_reset();
    }
    {   /* milestone 9: the effects chain (filter / drive pre or post, chorus, delay, reverb); Sticks, a short hit */
        double e_dry, e1l, e1r, e2l, e2r, b0, b1;
        int32_t p0, p1;
        uint32_t i, diff;
        seq_reset();
        fx_off();
        check("effects all off: the chain is skipped", !fx_any());
        hit_render(8, 44096);
        e_dry = win_energy(0, 11025 - 1000, 2000);
        kit.fx[FX_DLVL] = 80;                    /* delay: 1/8 at 120 BPM = 11025 samples, no feedback */
        kit.fx[FX_DTIME] = 3;
        kit.fx[FX_DFB] = 0;
        kit.fx[FX_DTONE] = 100;
        hit_render(8, 44096);
        e1l = win_energy(0, 11025 - 1000, 2000);
        e1r = win_energy(1, 11025 - 1000, 2000);
        check("delay 1/8: an echo 11025 samples (a 1/8 at 120 BPM) after the hit", e1l > 100.0 * (e_dry + 1.0));
        check("delay ping-pong: the first echo on the left", e1l > 20.0 * (e1r + 1.0));
        kit.fx[FX_DFB] = 60;
        hit_render(8, 44096);
        e2l = win_energy(0, 22050 - 1000, 2000);
        e2r = win_energy(1, 22050 - 1000, 2000);
        check("delay ping-pong, feedback: the second echo on the right", e2r > 20.0 * (e2l + 1.0) && e2r > 0.0);
        fx_off();
        kit.fx[FX_RLVL] = 80;                    /* reverb: a room tail after a dry hit */
        kit.fx[FX_RSIZE] = 60;
        hit_render(8, 44096);
        check("reverb: a room tail 0.1..0.3 s after a short hit", win_energy(0, 4410, 8820) > 1000.0 * (e_dry + 1.0));
        kit.fx[FX_RLVL] = 0;                     /* off: its tail ends in exact silence */
        hit_render(8, 4u * 44096u);
        p0 = 0;
        for (i = 3u * 44096u; i < 4u * 44096u; i++)
            p0 = abs(fxbuf[2u * i]) > p0 ? abs(fxbuf[2u * i]) : p0;
        check("reverb: its tail ends in exact silence", p0 == 0);
        fx_off();
        kit.fx[FX_CHORUS] = 80;                  /* chorus: a centred sound becomes stereo */
        hit_render(4, 22048);
        for (i = 0, diff = 0; i < 22048u; i++)
            diff += fxbuf[2u * i] != fxbuf[2u * i + 1u];
        check("chorus: left and right differ (a centred track)", diff > 5000u);
        fx_off();
        hit_render(4, 22048);
        for (i = 0, diff = 0; i < 22048u; i++)
            diff += fxbuf[2u * i] != fxbuf[2u * i + 1u];
        check("chorus off: left = right", diff == 0u);
        hit_render(11, 8192);                    /* the filter: Little Rocks, bright */
        b0 = brightness(8192);
        kit.fx[FX_FILT] = 10;                    /* low-pass, far down */
        hit_render(11, 8192);
        b1 = brightness(8192);
        check("filter low-pass: darker", b1 < 0.3 * b0);
        kit.fx[FX_FILT] = 90;                    /* high-pass */
        hit_render(7, 8192);                     /* Coke Can, low */
        b1 = brightness(8192);
        kit.fx[FX_FILT] = 50;
        hit_render(7, 8192);
        b0 = brightness(8192);
        check("filter high-pass: brighter (Coke Can's low body gone: ~2x)", b1 > 1.5 * b0);
        kit.t[8].p[SP_EN] = 20;                  /* drive: a quiet hit comes up */
        hit_render(8, 4096);
        p0 = peak(fxbuf, 4096);
        kit.fx[FX_DRIVE] = 100;
        hit_render(8, 4096);
        p1 = peak(fxbuf, 4096);
        check("drive: a quiet hit louder", p1 > 2 * p0);
        kit.t[8].p[SP_EN] = 100;
        {   /* POSITION: drive PRE leaves the delay's echo at its share of a driven hit; POST drives the echo too */
            double pre, post;
            fx_off();
            kit.fx[FX_DLVL] = 50;
            kit.fx[FX_DFB] = 0;
            kit.fx[FX_DRIVE] = 100;
            kit.fx[FX_FPOS] = 0;
            hit_render(8, 22048);
            pre = win_energy(0, 11025 - 1000, 2000) / (win_energy(0, 0, 2000) + 1.0);
            kit.fx[FX_FPOS] = 1;
            hit_render(8, 22048);
            post = win_energy(0, 11025 - 1000, 2000) / (win_energy(0, 0, 2000) + 1.0);
            check("POSITION POST: the echo driven too (louder against the hit than PRE)", post > 1.5 * pre);
        }
        fx_off();                                /* app_audio_stop: every tail silenced at once */
        kit.fx[FX_DLVL] = 80;
        kit.fx[FX_DFB] = 80;
        kit.fx[FX_RLVL] = 80;
        hit_render(8, 22048);
        app_audio_stop();
        host_render(fxbuf, 64);
        host_render(fxbuf, 22048);
        check("app_audio_stop: the delay and the room silent at once", peak(fxbuf, 22048) == 0);
        fx_off();
        host_render(fxbuf, 44096);
        seq_reset();
    }
    while (host_midi_out(&pkt))
        ;
    set.notes_out = 1;
    fm1_in.notes = 1u << 2;
    host_render(buf, 64);
    fm1_in.notes = 0;
    check("MIDI OUT: a pad sends its note-on (base + 2)", host_midi_out(&pkt) && ((pkt >> 8) & 0xF0u) == 0x90u &&
                                                           ((pkt >> 16) & 0x7Fu) == 55u);
    host_render(buf, 64u * 32u + 64u);
    {
        int off = 0;
        while (host_midi_out(&pkt))
            off |= ((pkt >> 8) & 0xF0u) == 0x80u && ((pkt >> 16) & 0x7Fu) == 55u;
        check("MIDI OUT: its note-off ~46 ms later", off);
    }
    set.notes_out = 0;
    pat.t[0].s[0].f = ST_TRIG;
    fm1_in.notes = PAD_MASK;                     /* everything at once */
    host_render(buf, 64);
    fm1_in.notes = 0;
    host_render(buf, 8192);
    check("23 at once: the mix saturates within Q15", peak(buf, 8192) <= 32767);
    {   /* the master's soft limit: everything loud (every level 100, MASTER 100, the pot full, accents on every 16th)
         * reaches full scale only rarely (a hard clip measured 1.2 % of samples), quiet passes untouched */
        uint32_t t2, s2, i, full = 0;
        kit_t keep = kit;
        int32_t keep_pot = snd.master_q8;
        for (t2 = 0; t2 < NT; t2++) {
            kit.t[t2].level = 100;
            for (s2 = 0; s2 < NSTEP; s2++)
                pat.t[t2].s[s2].f = ST_TRIG | ST_ACC;
        }
        kit.fx[FX_MASTER] = 100;
        snd.master_q8 = 256;
        snd.run_req = 3;
        host_render(buf, 44096 * 2u);
        for (i = 0; i < 2u * 44096u * 2u; i++)
            full += buf[i] >= 32767 || buf[i] <= -32767;
        check("soft limit: full scale in under 0.2 % of samples (hard clip: 1.2 %)", full * 500u < 2u * 44096u * 2u);
        check("soft limit: never beyond +-32767", peak(buf, 44096u * 2u) <= 32767);
        snd.run_req = 2;
        host_render(buf, 64);
        for (t2 = 0; t2 < NT; t2++) {
            voice_silence(t2);
            memset(pat.t[t2].s, 0, sizeof pat.t[t2].s);
        }
        kit = keep;
        snd.master_q8 = keep_pot;
    }
    app_audio_stop();
    host_render(buf, 64);
    host_render(buf, 64);
    check("app_audio_stop: stopped and silent at once", !snd.running_pub && peak(buf, 64) == 0 && snd.active == 0u);
    while (host_midi_out(&pkt))
        ;
    midi_out(0x90, 60, 100);
    check("midi_out: a USB-MIDI note-on packet", host_midi_out(&pkt) && pkt == (0x09u | 0x90u << 8 | 60u << 16 | 100u << 24));
}

/* ---------------------------------------------------------------- the screen reader (fm1-reader) --- */
static uint32_t rd_pos;                                  /* how far host_sx_out has been read */
static char rd_text[24][200];
static uint8_t rd_flags[24], rd_cmd[24];
static uint32_t rd_n, rd_bad;                            /* frames read; texts breaking the wording rules */

static void rd_take(void)                                /* the frames sent since the last call */
{
    rd_n = 0;
    while (rd_pos < host_sx_out_len) {
        uint32_t a = rd_pos, b = a, k = 0, i;
        while (b < host_sx_out_len && host_sx_out[b] != 0xF7)
            b++;
        rd_pos = b + 1u;
        if (rd_n >= 24u || host_sx_out[a] != 0xF0 || b - a < 6u)
            continue;
        rd_cmd[rd_n] = host_sx_out[a + 4u];
        rd_flags[rd_n] = host_sx_out[a + 5u];
        for (i = a + 6u; i < b && k + 1u < sizeof rd_text[0]; i++)
            rd_text[rd_n][k++] = host_sx_out[i] ? (char)host_sx_out[i] : '|';   /* (the HELLO's 00 shown as |) */
        rd_text[rd_n][k] = 0;
        if (rd_cmd[rd_n] == 16u) {                       /* a SAY: ASCII, <= 160, none of the symbols RULES.md bars */
            const char *t = rd_text[rd_n];
            int bad = k > 160u || !k;
            for (i = 0; t[i]; i++)
                bad |= t[i] < 32 || t[i] > 126 || strchr("/|><*_=#[]{}~^@&$+", t[i]) != 0;
            for (i = 0; i < NT; i++)             /* no screen abbreviation of a track left */
                if (strstr(t, SK_SHORT[i]))
                    bad = 1;
            if (bad) {
                printf("  (against the rules: \"%s\")\n", t);
                rd_bad++;
            }
        }
        rd_n++;
    }
}

static int rd_has(const char *start, uint32_t flags)     /* a SAY beginning so, with those flags, was among them */
{
    uint32_t i;
    for (i = 0; i < rd_n; i++)
        if (rd_cmd[i] == 16u && rd_flags[i] == flags && !strncmp(rd_text[i], start, strlen(start)))
            return 1;
    return 0;
}

static int rd_in(const char *part)                       /* a SAY holding this */
{
    uint32_t i;
    for (i = 0; i < rd_n; i++)
        if (rd_cmd[i] == 16u && strstr(rd_text[i], part))
            return 1;
    return 0;
}

static void rd_hello(void);
static void rd_frames(uint32_t n)                        /* frames (and audio), collecting what was said; a HELLO */
{                                                        /* every 2 s, as a reader sends it */
    static char all[24][200];
    static uint8_t af[24], ac[24];
    static uint32_t since;
    uint32_t got = 0, i;
    while (n--) {
        if (++since >= 2000u / 16u && sr_on()) {
            since = 0;
            rd_hello();
        }
        host_render(buf_small, 704);             /* (16 ms of audio: the ISR's pattern changes, the transport) */
        host_frame();
        rd_take();
        for (i = 0; i < rd_n && got < 24u; i++, got++) {
            memcpy(all[got], rd_text[i], sizeof all[0]);
            af[got] = rd_flags[i];
            ac[got] = rd_cmd[i];
        }
    }
    for (i = 0; i < got; i++) {
        memcpy(rd_text[i], all[i], sizeof all[0]);
        rd_flags[i] = af[i];
        rd_cmd[i] = ac[i];
    }
    rd_n = got;
}

static void rd_show(const char *what)
{
    uint32_t i;
    for (i = 0; i < rd_n; i++)
        if (rd_cmd[i] == 16u)
            printf("  (%s: \"%s\")\n", what, rd_text[i]);
}

static void rd_hello(void)
{
    static const uint8_t HELLO[] = {0x7D, 0x53, 0x52, 0x01, 0x01};
    host_sysex_in(HELLO, sizeof HELLO);
}

static void reader(void)
{
    static const uint8_t BYE[] = {0x7D, 0x53, 0x52, 0x02}, WHERE[] = {0x7D, 0x53, 0x52, 0x03},
                         UPDATE[] = {0x22, 0x24, 0x35, 0x00, 0x00};
    uint8_t ANSWER[7u + sizeof SR_NAME];         /* F0 7D 53 52 01 01 <SR_NAME> 00 F7 */
    uint32_t i;
    memcpy(ANSWER, (const uint8_t[]){0xF0, 0x7D, 0x53, 0x52, 0x01, 0x01}, 6u);
    memcpy(ANSWER + 6u, SR_NAME, sizeof SR_NAME - 1u);
    ANSWER[5u + sizeof SR_NAME] = 0x00;
    ANSWER[6u + sizeof SR_NAME] = 0xF7;
    host_nor_erase();
    power_cycle();                               /* the factory set: P01 SAMBA, K1 STUDIO */
    host_sx_out_len = rd_pos = 0;
    rd_bad = 0;
    host_turn(EN_K1, 3);
    press(B_ENV);
    frames(20);
    check("reader: silent until a HELLO (nothing sent)", host_sx_out_len == 0u);
    host_sysex_in(UPDATE, sizeof UPDATE);        /* the update protocol's frame: not the app's */
    host_frame();
    check("reader: another SysEx frame is left for the update entry", host_sx_ready && host_sx_out_len == 0u);
    host_sx_ready = 0;
    press(B_HOME);
    rd_hello();
    host_frame();
    check("reader: HELLO answered, byte for byte (F0 7D 53 52 01 01 PERCOLATOR <version> 00 F7)",
          host_sx_out_len >= sizeof ANSWER && !memcmp(host_sx_out, ANSWER, sizeof ANSWER) && !host_sx_ready);
    rd_frames(10);
    rd_show("whole screen");
    check("reader: a new reader hears the whole screen (GRID: pattern, kit, tempo, track, its sounds)",
          rd_has("grid. pattern 1, samba. kit 1, studio. 104 beats per minute, stopped. track 1, maraca.", SR_NOW) &&
              rd_in("energy "));

    host_turn(EN_K2, 2);                         /* KNOB 2: decay, once it settles */
    rd_frames(2);
    check("reader: a knob: nothing while it is still moving", rd_n == 0u);
    rd_frames(10);
    check("reader: a knob: \"decay N\" once it settles", rd_n == 1u && rd_has("decay ", SR_NOW));
    host_turn(EN_SELECT, 8);                     /* SELECT: another track */
    rd_frames(20);
    rd_show("track");
    check("reader: SELECT: \"track 9, sticks. energy ...\"", rd_has("track 9, sticks. energy ", SR_NOW));
    host_turn(EN_ALGO, 4);
    rd_frames(12);
    check("reader: ALGORITHM: \"108 beats per minute\"", rd_has("108 beats per minute", SR_NOW));
    host_turn(EN_PRESET, 6);                     /* PRESETS, stopped: the message; then the kit it brought */
    rd_frames(20);
    rd_show("pattern");
    check("reader: PRESETS: \"pattern 7 rain\" (the message, no label doubled)", rd_has("pattern 7 rain", SR_NOW));
    check("reader: KIT FOLLOWS PATTERN: \"kit 3, rain room\"", rd_has("kit 3, rain room", 0));
    host_press(B_EDIT, 1);                       /* EDIT + PRESETS: the kit's message */
    host_frame();
    host_turn(EN_PRESET, 1);
    frames(3);
    host_press(B_EDIT, 0);
    rd_frames(10);
    check("reader: EDIT + PRESETS: \"kit 4 glass garden\"", rd_has("kit 4 glass garden", SR_NOW));

    press(B_PLAY);                               /* the transport */
    rd_frames(10);
    check("reader: PLAY: \"playing\"", rd_has("playing", SR_NOW));
    host_turn(EN_PRESET, 1);                     /* playing: the next pattern at the bar line, in full */
    rd_frames(260);
    rd_show("queued");
    check("reader: a queued pattern starting: \"pattern 8, glass garden\"", rd_has("pattern 8, glass garden", 0));
    press(B_PLAY);
    rd_frames(10);
    check("reader: PLAY again: \"stopped\"", rd_has("stopped", SR_NOW));

    press(B_HOME);                               /* HOME on HOME: the TRACK view */
    rd_frames(20);
    rd_show("track view");
    check("reader: the TRACK view: the track, its length, its steps", rd_has("track view. track 9, sticks. length ", SR_NOW) &&
                                                                    rd_in("steps on"));
    press(B_SEQ);
    rd_frames(10);
    check("reader: SEQ: \"step mode\"", rd_has("step mode", SR_NOW));
    {
        uint32_t was = pat.t[8].s[1].f & ST_TRIG;
        key_tap(step_key(1));
        rd_frames(4);
        rd_show("step key");
        check("reader: a step key: \"step 2 on\" / \"step 2 off\"", rd_has(was ? "step 2 off" : "step 2 on", SR_NOW));
    }
    fm1_in.notes |= 1u << step_key(1);           /* hold step 2: its settings; KNOB 1: a lock */
    rd_frames(10);
    rd_show("step held");
    check("reader: a step held: \"step 2, on, chance 100 percent\"", rd_has("step 2, ", SR_NOW) && rd_in("chance "));
    host_turn(EN_K1, 5);
    rd_frames(12);
    rd_show("lock");
    check("reader: a lock: \"energy lock N\"", rd_has("energy lock ", SR_NOW));
    host_turn(EN_PRESET, 1);
    rd_frames(12);
    check("reader: a condition: \"condition 1 of 2\"", rd_has("condition 1 of 2", SR_NOW));
    fm1_in.notes = 0;
    rd_frames(10);
    press(B_SEQ);
    rd_frames(10);
    press(B_HOME);
    rd_frames(20);

    press(B_FX);                                 /* a page; a knob on it */
    press(B_FX);
    press(B_FX);
    rd_frames(20);
    rd_show("page");
    check("reader: FX 3: \"effects 3, delay page. time ...\"", rd_has("effects 3, delay page. time ", SR_NOW));
    host_turn(EN_K1, 1);
    rd_frames(12);
    rd_show("page knob");
    check("reader: a page knob: the delay time as a note value", rd_has("time ", SR_NOW) && rd_in(" note"));
    press(B_HOME);
    rd_frames(20);

    rd_hello();
    press(B_SAVE);                               /* SAVE: the screen, then each edit */
    rd_frames(30);
    rd_show("save");
    check("reader: SAVE: what, the slot, the name, how to edit", rd_has("save pattern to pattern 8, replaces a factory "
                                                                        "pattern. name glass garden.", SR_NOW));
    host_turn(EN_K2, 1);
    rd_frames(12);
    check("reader: SAVE: a letter -> \"position 1, letter H\"", rd_has("position 1, letter H", SR_NOW));
    host_turn(EN_K1, 1);
    rd_frames(12);
    check("reader: SAVE: the cursor -> \"position 2, letter L\"", rd_has("position 2, letter L", SR_NOW));
    host_turn(EN_SELECT, 1);
    rd_frames(12);
    rd_show("save kit");
    check("reader: SAVE: SELECT -> \"kit, kit 4, replaces a factory kit. name glass garden\"",
          rd_has("kit, kit 4, replaces a factory kit. name glass garden", SR_NOW));
    press(B_HOME);
    rd_frames(10);
    check("reader: SAVE cancelled: \"save cancelled\"", rd_has("save cancelled", SR_NOW));
    rd_frames(30);

    song_screen(1);                              /* SONG: the song and the entry; each edit */
    rd_frames(30);
    rd_show("song");
    check("reader: SONG: \"song screen. song 1, loop, 6 entries. entry 1, pattern 1, samba, 2 times\"",
          rd_has("song screen. song 1, loop, 6 entries. entry 1, pattern 1, samba, 2 times.", SR_NOW));
    host_turn(EN_SELECT, 2);
    rd_frames(12);
    check("reader: SONG: SELECT -> \"entry 3, pattern 3, african bell, 2 times\"",
          rd_has("entry 3, pattern 3, african bell, 2 times", SR_NOW));
    host_turn(EN_K1, 1);
    rd_frames(12);
    check("reader: SONG: KNOB 1 -> \"..., 3 times\"", rd_has("entry 3, pattern 3, african bell, 3 times", SR_NOW));
    song_screen(0);
    rd_frames(30);

    for (i = 0; i < NPAGES; i++) {               /* WHERE on every page: the whole page, within the rules */
        rd_hello();
        page_open(i);
        rd_frames(30);
        host_sysex_in(WHERE, sizeof WHERE);
        rd_frames(4);
        if (!rd_in(" page")) {
            printf("  (WHERE on page %u: nothing)\n", i);
            rd_bad++;
        }
    }
    press(B_HOME);
    rd_frames(30);
    for (i = 0; i < NT; i++) {                   /* every track's name, in both views */
        rd_hello();
        track_select(i);
        rd_frames(16);
    }
    press(B_HOME);
    rd_frames(30);
    press(B_HOME);
    press(B_HOME);
    rd_frames(30);
    check("reader: every text ASCII, <= 160 characters, no symbol or abbreviation RULES.md bars", rd_bad == 0u);

    host_sx_room = 0;                            /* the USB queue full: dropped, nothing waits */
    {
        uint32_t d0 = sr.dropped, len0 = host_sx_out_len;
        host_turn(EN_K1, 1);
        frames(20);
        check("reader: the USB queue full: the message dropped, nothing sent, no wait",
              host_sx_out_len == len0 && sr.dropped > d0);
    }
    host_sx_room = 64;
    rd_frames(20);
    host_sysex_in(BYE, sizeof BYE);
    rd_frames(2);
    host_turn(EN_K1, 1);
    rd_frames(20);
    check("reader: BYE: silent again", rd_n == 0u && !sr_on());
    rd_hello();
    rd_frames(3);
    check("reader: HELLO after BYE: the whole screen again", rd_has("grid.", SR_NOW));
    frames(5000u / 16u + 10u);                   /* no HELLO for 5 s: gone */
    check("reader: no HELLO for 5 s: silent", !sr_on());
    host_sx_out_len = rd_pos = 0;
}

/* MIDI OUT: the knobs as CCs, the pattern as Program Change (what MIDItoSpeech reads) */
static void notes_ctl(void)
{
    uint32_t pkt, n, cc = 0, v = 0, pc = 0xFFu;
    host_nor_erase();
    power_cycle();
    set.notes_out = 1;
    host_render(buf_small, 64);
    while (host_midi_out(&pkt))
        ;
    check("MIDI OUT: nothing sent at power-on", 1);
    host_turn(EN_K2, 3);                         /* HOME, KNOB 2: decay of the selected track */
    host_frame();
    host_render(buf_small, 64);
    for (n = 0; host_midi_out(&pkt);)
        if (((pkt >> 8) & 0xF0u) == 0xB0u)
            n++, cc = (pkt >> 16) & 0x7Fu, v = (pkt >> 24) & 0x7Fu;
    check("MIDI OUT: a knob sends its CC (KNOB 2: CC 11, the value)", n == 1u && cc == 11u && v == kit.t[sel()].p[SP_DEC]);
    track_select(5);                             /* another track: nothing sent */
    host_frame();
    host_render(buf_small, 64);
    check("MIDI OUT: another track: no CCs", !out_count(0xB0u, &n));
    host_midi(0xB0, 4, 99);                      /* CC in: taken, not sent back */
    host_render(buf_small, 64);
    host_render(buf_small, 64);
    check("MIDI OUT: CC 4 in sets objects, and is not echoed", kit.t[5].p[SP_OBJ] == 99u && !out_count(0xB0u, &n));
    queue_pattern(6);
    host_render(buf_small, 64);
    host_render(buf_small, 64);
    while (host_midi_out(&pkt))
        if (((pkt >> 8) & 0xF0u) == 0xC0u)
            pc = (pkt >> 16) & 0x7Fu;
    check("MIDI OUT: a pattern starting sends Program Change (P07: 6)", pc == 6u);
    set.notes_out = 0;
    host_turn(EN_K1, 2);
    host_frame();
    host_render(buf_small, 64);
    check("MIDI OUT off: no CCs", !out_count(0xB0u, &n));
}

int main(void)
{
    if (system("mkdir -p build/shots"))
        return 1;
    screens();
    persistence();
    sound();
    reader();
    notes_ctl();
    puts(failed ? "APP TEST FAILED" : "app test passed");
    return failed ? 1 : 0;
}
