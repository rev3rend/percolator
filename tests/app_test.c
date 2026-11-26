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

/* ------------------------------------------------------------------- screens --- */
static void screens(void)
{
    uint32_t t, s, p;
    host_nor_erase();
    host_boot();
    host_frame();
    check("boot: the splash (name, version) drawn", ink_rows(80, 170) > 30u);
    {   /* the large face is sparse (tools/gen_aa_font.py L_CHARS): a character it lacks draws as a space */
        const char *c = APP_NAME;
        int all = 1;
        for (; *c; c++)
            all &= glyph_at(&AF_L, (uint8_t)*c) >= 0;
        check("boot: every character of APP_NAME is in the large font", all);
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

    host_press(B_EDIT, 1);                       /* EDIT + a key: select, no sound */
    host_frame();
    check("EDIT held: the pads are silent (they select)", snd.pad_mask == 0u);
    key_tap(7);
    host_press(B_EDIT, 0);
    host_frame();
    check("EDIT + C4: track 8 (Coke Can) selected", sel() == 7u);
    check("STEP mode: black keys still play, white keys do not", snd.pad_mask == (PAD_MASK & BLACK_KEYS));
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
    check("HOME again: the GRID", set.view == 0u);

    /* every page, its values at the widest */
    {
        static const struct { uint32_t button, presses, page; const char *name; } PG[] = {
            {B_ENV, 1, PG_TRACK, "page_track"}, {B_ENV, 1, PG_TRACK2, "page_track2"}, {B_SCL, 1, PG_TIME, "page_time"},
            {B_LFO, 1, PG_RANDOM, "page_random"}, {B_FX, 1, PG_FX1, "page_fx1"}, {B_FX, 1, PG_FX2, "page_fx2"},
            {B_FX, 1, PG_FX3, "page_fx3"}, {B_GLO, 1, PG_GLO, "page_global"}, {B_GLO, 1, PG_GLO2, "page_global2"}};
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
        check("SELECT on a page: the page before", ui.page == PG_FX3);
    }
    press(B_ENV);
    host_turn(EN_K1, -200);
    host_turn(EN_K2, 300);
    host_frame();
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

    key_tap(KEY_SONG);
    check("SONG (G5): the SONG screen", ui.song_open == 1u);
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
    key_tap(KEY_SONG);
    check("SONG again: closed", ui.song_open == 0u);

    /* the widest GRID: every step set with everything, mixed mutes and solos, a long message */
    for (t = 0; t < NT; t++) {
        pat.t[t].flags = (uint8_t)(t % 5u == 0u ? TP_MUTE : t % 7u == 0u ? TP_SOLO : 0u);
        pat.t[t].len = (uint8_t)(t % 3u ? 16u : 12u);
        for (s = 0; s < NSTEP; s++) {
            step_t *st = &pat.t[t].s[s];
            st->f = (uint8_t)(((s + t) % 3u ? ST_TRIG : 0u) | ((s + t) % 4u == 0u ? ST_ACC : 0u));
            st->prob = (uint8_t)((s * t) % 5u == 0u ? 6u : 0u);
        }
    }
    msg("A MESSAGE THAT IS FAR TOO LONG FOR THE FOOTER OF THE SCREEN");
    track_select(22);
    frames(2);
    shot("home_grid_widest");
    lint("HOME GRID at its widest");
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
    check("a fresh unit: pattern 1, kit 1, empty", pat_index() == 0u && kit_index() == 0u && !pat.t[0].s[0].f);
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
        store_save(SLOT_KIT0, 0x12345678u, other, 400);
        power_cycle();
        check("another app's records: ignored (defaults)", !pat.t[0].s[0].f && kit.t[0].level == 80u &&
                                                            set.base_note == 53u);
    }
    {   /* records of ours with values out of range (an older version, a bug, damage): clamped */
        static pattern_t bad;
        settings_t sbad;
        kit_t kbad;
        memset(&bad, 0xFF, sizeof bad);
        store_save(SLOT_PAT0, APP_ID, &bad, sizeof bad);
        memset(&kbad, 0xEE, sizeof kbad);
        store_save(SLOT_KIT0, APP_ID, &kbad, sizeof kbad);
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

static void sound(void)
{
    uint32_t t, pkt, h0;
    int all_ok = 1, all_quiet = 1, all_in = 1;
    double db[NT];
    power_cycle();
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
    host_midi(0xB0, 2, 11);
    host_midi(0xB0, 4, 22);
    host_midi(0xB0, 11, 33);
    host_midi(0xB0, 1, 44);
    host_render(buf, 64);
    check("CC 2 / 4 / 11 / 1: the selected track's four parameters", kit.t[6].p[0] == 11u && kit.t[6].p[1] == 22u &&
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
    snd.rec = 0;
    while (host_midi_out(&pkt))
        ;
    set.notes_out = 1;
    fm1_in.notes = 1u << 2;
    host_render(buf, 64);
    fm1_in.notes = 0;
    check("NOTES OUT: a pad sends its note-on (base + 2)", host_midi_out(&pkt) && ((pkt >> 8) & 0xF0u) == 0x90u &&
                                                           ((pkt >> 16) & 0x7Fu) == 55u);
    host_render(buf, 64u * 32u + 64u);
    {
        int off = 0;
        while (host_midi_out(&pkt))
            off |= ((pkt >> 8) & 0xF0u) == 0x80u && ((pkt >> 16) & 0x7Fu) == 55u;
        check("NOTES OUT: its note-off ~46 ms later", off);
    }
    set.notes_out = 0;
    pat.t[0].s[0].f = ST_TRIG;
    fm1_in.notes = PAD_MASK;                     /* everything at once */
    host_render(buf, 64);
    fm1_in.notes = 0;
    host_render(buf, 8192);
    check("23 at once: the mix saturates within Q15", peak(buf, 8192) <= 32767);
    app_audio_stop();
    host_render(buf, 64);
    host_render(buf, 64);
    check("app_audio_stop: stopped and silent at once", !snd.running_pub && peak(buf, 64) == 0 && snd.active == 0u);
    while (host_midi_out(&pkt))
        ;
    midi_out(0x90, 60, 100);
    check("midi_out: a USB-MIDI note-on packet", host_midi_out(&pkt) && pkt == (0x09u | 0x90u << 8 | 60u << 16 | 100u << 24));
}

int main(void)
{
    if (system("mkdir -p build/shots"))
        return 1;
    screens();
    persistence();
    sound();
    puts(failed ? "APP TEST FAILED" : "app test passed");
    return failed ? 1 : 0;
}
