/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 R. Luke DuBois */
/* The panel (main loop): docs/SPEC.md's controls table turned into actions, the LEDs, and the screens.
 *
 * Screens: HOME as GRID (all 23 tracks x 16 steps) or TRACK (the selected track zoomed), the parameter pages (TRACK,
 * TRACK 2, TIME, RANDOM, FX 1..3, GLOBAL, GLOBAL 2: four rows, one per knob), SAVE and SONG. Lazy drawing: every area
 * keeps a signature of what it shows and is drawn only when that changes, everything when the screen changes or the
 * platform sets app_redraw. The GRID's step cells have a fast path: the playhead and the sounding marks change every
 * 16th, so only the cells that changed are sent (a few hundred bytes), not the grid (~90 KB, ~60 ms of SPI).
 *
 * The GRID: rows of 8 px for 23 tracks; a 12 px name does not fit a row, so the names go in pairs of rows, the even
 * track's at the left and the odd track's at the right, both centred on the pair (the pairs are also the drawing
 * strips). Warm and dark: the AMBER palette (amber trigs, brighter accents, a cyan playhead). */

#define PAL_AMBER 2u             /* tools/gen_ui_palettes.py PALETTES: GREY GREEN AMBER ... */
#define HOLD_MS 450u
#define MSG_MS 1400u
#define DTAP_MS 300u             /* PLAY twice within this: restart */
#define FLASH_MS 90u             /* a track's mark stays lit this long after a hit */
#define CONFIRM_MS 2500u         /* EDIT + SAVE twice within this clears the track */
#define SPLASH_MS 1200u          /* the name and version at power-on */
#define CPU_EVERY_MS 100u        /* the CPU meter's numbers change at most this often */
#define CPU_WIN_MS 2000u         /* the peak: the worst half of this window and the one before */
#define CPU_HALF_US ((uint32_t)HALF_FRAMES * 1000000u / FS)   /* 2902 us: the time an audio half may take */
#define CPU_HOT 80u              /* % above which the meter turns red */

/* the GRID's geometry */
#define G_TOP 22
#define G_ROW 8
#define G_X0 32
#define G_PITCH 11
#define G_CW 9
#define G_MARK_X 27              /* the sounding mark, left of the cells */
#define G_FOOT 214
static int32_t cell_x(uint32_t s) { return G_X0 + (int32_t)s * G_PITCH + (int32_t)(s >> 2) * 2; }

/* a step with a lock: magenta, the complement of the amber trigs and the cyan playhead (outside the palette: the
 * firmware has one look, AMBER); brighter when accented, as amber trigs are */
#define RGB565(r, g, b) ((uint16_t)(((r) >> 3) << 11 | ((g) >> 2) << 5 | ((b) >> 3)))
#define C_LOCK RGB565(214, 72, 186)
#define C_LOCK_ACC RGB565(255, 128, 232)

static uint16_t step_fill(const step_t *st, int silent)   /* a trig's colour: silent dim, locked magenta, else amber */
{
    if (silent)
        return T_DIM;
    if (st_locked(st))
        return (st->f & ST_ACC) ? C_LOCK_ACC : C_LOCK;
    return (st->f & ST_ACC) ? T_THEME : T_SEL;
}

enum { SCR_HOME, SCR_PAGE, SCR_SAVE, SCR_SONG };
enum { SV_PAT, SV_KIT, SV_SONG };
static const char *const SV_NAME[3] = {"PATTERN", "KIT", "SONG"};

static struct {
    uint8_t mode;                /* 0 PLAY (pads), 1 STEP */
    uint8_t page;                /* PG_* or PG_HOME */
    uint8_t saving, save_what, save_slot;
    uint8_t song_open, song_cur;
    uint8_t rec;
    uint8_t queued;              /* the pattern queued, + 1 (0: none) */
    int8_t held_step;            /* STEP mode: the step key held (-1: none) */
    uint8_t step_used;           /* the held step was edited: no trig toggle on release */
    uint8_t set_dirty;
    uint32_t clear_until;        /* EDIT + SAVE: armed until then */
    uint32_t last_buttons, last_keys;
    uint32_t down_ms[NB];
    uint8_t held_fired[NB];
    uint32_t play_tap_ms;
    char msg[40];
    uint32_t msg_until;
    uint8_t drawn_screen;        /* the screen (and page / view) on the panel: a change draws everything */
    uint32_t sig[16];
    uint8_t drawn_pos[NT];       /* the GRID's playhead as drawn (0xFF none) */
    uint32_t drawn_flash;        /* the GRID's sounding marks as drawn */
    uint8_t seen_hits[NT];
    uint32_t flash_until[NT];
    uint32_t frames;
    uint32_t splash_until;
    uint32_t cpu_next, cpu_win_end, cpu_late0;
    uint8_t cpu_win, cpu_prev;   /* the worst half (%) of this window and of the one before */
} ui;

static void msg(const char *s)
{
    str_cpy(ui.msg, s, sizeof ui.msg);
    ui.msg_until = fm1_ms + MSG_MS;
}

static void msg2(const char *a, const char *b)
{
    str_cpy(ui.msg, a, sizeof ui.msg);
    cat(ui.msg, b, sizeof ui.msg);
    ui.msg_until = fm1_ms + MSG_MS;
}

static const char *msg_now(void) { return (int32_t)(ui.msg_until - fm1_ms) > 0 ? ui.msg : ""; }

static uint32_t fnv(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }
static uint32_t fnv_s(uint32_t h, const char *s)
{
    while (*s)
        h = fnv(h, (uint8_t)*s++);
    return h;
}
#define FNV0 2166136261u

static int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

static uint32_t sel(void) { return set.sel < NT ? set.sel : 0u; }

static int white_step(uint32_t k)                /* key k -> step 0..15 if it is a white key, else -1 */
{
    uint32_t s = 0, i;
    if (k > 26u || ((BLACK_KEYS >> k) & 1u))
        return -1;
    for (i = 0; i < k; i++)
        s += !((BLACK_KEYS >> i) & 1u);
    return (int)s;
}

static uint32_t step_key(uint32_t s)             /* step 0..15 -> its white key */
{
    uint32_t k;
    for (k = 0; k < 27u; k++)
        if (white_step(k) == (int)s)
            return k;
    return 0;
}

static void label2(char *b, unsigned max, char c, uint32_t n)   /* "P01", "K3" style */
{
    char t[8];
    b[0] = c;
    b[1] = 0;
    if (c == 'P' && n < 10u)
        cat(b, "0", max);
    fmt_int(t, (int32_t)n);
    cat(b, t, max);
}

static void track_select(uint32_t t)
{
    set.sel = (uint8_t)t;
    snd.sel = (uint8_t)t;
}

/* ---------------------------------------------------------- pattern change, saving --- */
static void queue_pattern(uint32_t i)
{
    char b[8];
    if (i >= NPAT)
        return;
    label2(b, sizeof b, 'P', i + 1u);
    if (i == pat_index() && !ui.queued) {
        msg2(b, " PLAYING");
        return;
    }
    ui.queued = (uint8_t)(i + 1u);
    snd.next_req = ui.queued;
    msg2(snd.running_pub ? "NEXT " : "PATTERN ", b);
}

static void settings_keep(void)
{
    set.last_pat = (uint8_t)pat_index();
    set.last_kit = (uint8_t)kit_index();
    set.last_song = song_index;
    ui.set_dirty = 0;
    if (data_save_settings())
        msg("SETTINGS NOT SAVED");
}

static void save_store(void)
{
    int32_t r;
    char b[24];
    if (ui.save_what == SV_PAT) {
        if (ui.save_slot != pat_index())
            pats[ui.save_slot] = pat;            /* save as: the slot gets a copy, and is played from now on */
        pat_cur = &pats[ui.save_slot];
        r = data_save_pattern(ui.save_slot);
        label2(b, sizeof b, 'P', ui.save_slot + 1u);
    } else if (ui.save_what == SV_KIT) {
        if (ui.save_slot != kit_index())
            kits[ui.save_slot] = kit;
        kit_cur = &kits[ui.save_slot];
        r = data_save_kit(ui.save_slot);
        label2(b, sizeof b, 'K', ui.save_slot + 1u);
    } else {
        song_index = ui.save_slot;
        r = data_save_songs();
        label2(b, sizeof b, 'S', ui.save_slot + 1u);
    }
    settings_keep();
    if (r)
        msg("SAVE FAILED");
    else {
        cat(b, " SAVED", sizeof b);
        msg2(SV_NAME[ui.save_what], " ");
        cat(ui.msg, b, sizeof ui.msg);
    }
    ui.saving = 0;
}

static void page_open(uint32_t page)
{
    if (ui.page != page && ui.set_dirty && (ui.page == PG_GLO || ui.page == PG_GLO2 || ui.page == PG_TIME ||
                                             ui.page == PG_RANDOM))
        settings_keep();                         /* leaving a settings page: keep what changed */
    ui.page = (uint8_t)page;
    ui.saving = 0;
    ui.song_open = 0;
}

/* ------------------------------------------------------------------- drawing --- */
static void draw_bar_h(int32_t x, int32_t y, int32_t w, int32_t h, int32_t num, int32_t den, int bip)
{
    int32_t f;
    cv_rect(x, y, w, h, T_LINE);
    if (!bip) {
        f = den ? w * num / den : 0;
        if (f > 0)
            cv_rect(x, y, f, h, T_SEL);
    } else {                                     /* from the middle */
        int32_t mid = x + w / 2;
        f = den ? (w / 2) * (num < 0 ? -num : num) / den : 0;
        cv_rect(num < 0 ? mid - f : mid, y, f, h, T_SEL);
        cv_rect(mid, y - 1, 1, h + 2, T_MID);
    }
}

static uint32_t solo_any(void);
static uint32_t any_mute(void);

static void draw_head(void)                      /* HOME's top line: pattern, queue, tempo, state */
{
    char b[40], t[12];
    int32_t x, left, gap;
    cv_begin(240, G_TOP - 2, T_BG);
    label2(b, sizeof b, 'P', pat_index() + 1u);
    left = x = cv_text(6, 2, &AF_M, b, T_THEME);
    if (ui.queued) {
        label2(t, sizeof t, 'P', ui.queued);
        str_cpy(b, ">", sizeof b);
        cat(b, t, sizeof b);
        left = cv_text(x + 6, 4, &AF_S, b, (fm1_ms / 250u) & 1u ? T_ACCENT : T_MID);
    }
    x = 234;                                     /* the state from the right, then the tempo in what is left */
    if (snd.running_pub)
        x = cv_text_r(x, 4, &AF_S, "RUN", T_ACCENT, T_BG) - 6;
    else
        x = cv_text_r(x, 4, &AF_S, "STOP", T_MID, T_BG) - 6;
    if (ui.rec)
        x = cv_text_r(x, 4, &AF_S, "REC", T_REC, T_BG) - 6;
    if (panel_held(B_ARP))
        x = cv_text_r(x, 4, &AF_S, "FILL", T_THEME, T_BG) - 6;
    if (solo_any())
        x = cv_text_r(x, 4, &AF_S, "SOLO", T_THEME, T_BG) - 6;
    else if (any_mute())
        x = cv_text_r(x, 4, &AF_S, "MUTE", T_MID, T_BG) - 6;
    if (ui.mode)
        x = cv_text_r(x, 4, &AF_S, "STEP", T_MID, T_BG) - 6;
    {   /* "120 BPM" (EXT: the clock's, "EXT 120 BPM"), or just the number when crowded */
        uint32_t bpm2 = snd.ext_pub ? snd.ext_bpm2_pub : pat_bpm2();
        pv_text(t, sizeof t, &PP[PG_TIME][0], (int32_t)bpm2);
        str_cpy(b, snd.ext_pub ? "EXT " : "", sizeof b);
        cat(b, t, sizeof b);
        gap = x - left - 8;
        if (text_w(&AF_S, b) > gap) {
            str_cpy(b, snd.ext_pub ? "EXT " : "", sizeof b);
            fmt_int(t, (int32_t)bpm2 / 2);
            cat(b, t, sizeof b);
            if (text_w(&AF_S, b) > gap)
                b[0] = 0;
        }
    }
    if (b[0])
        cv_text_in(left + 4, 4, x - left - 4, &AF_S, b, T_TEXT, T_BG);
    cv_blit(0, 0);
}

/* the CPU meter under HOME's header (its 2 px rule): the average as a fill, the peak as a tick; red when hot */
static void draw_cpu_bar(void)
{
    int32_t w = 240 * cpu_now / 100, pk = 240 * cpu_peak / 100;
    uint16_t c = cpu_peak >= CPU_HOT || cpu_late ? T_REC : T_ACCENT;
    cv_begin(240, 2, T_BG);
    cv_rect(0, 1, 240, 1, T_LINE);
    if (w > 0)
        cv_rect(0, 0, w, 2, c);
    if (pk > 0)
        cv_rect(pk > 238 ? 238 : pk, 0, 2, 2, cpu_peak >= CPU_HOT ? T_REC : T_TEXT);
    cv_blit(0, G_TOP - 2);
}

static void cpu_update(void)                     /* every frame: the platform's measurements into the meter */
{
    uint32_t us = audio_max_us, pk;
    audio_max_us = 0;                            /* (only the ISR raises it: a raise lost to this reset is one frame) */
    pk = (us * 100u + CPU_HALF_US / 2u) / CPU_HALF_US;     /* (rounded) */
    pk = pk > 100u ? 100u : pk;
    ui.cpu_win = (uint8_t)(pk > ui.cpu_win ? pk : ui.cpu_win);
    if ((int32_t)(fm1_ms - ui.cpu_win_end) >= 0) {
        ui.cpu_prev = ui.cpu_win;
        ui.cpu_win = 0;
        ui.cpu_win_end = fm1_ms + CPU_WIN_MS;
    }
    if ((int32_t)(fm1_ms - ui.cpu_next) < 0)
        return;
    ui.cpu_next = fm1_ms + CPU_EVERY_MS;
    {
        uint32_t now = (audio_cpu_q8 * 100u + 128u) / 256u;
        cpu_now = (uint8_t)(now > 100u ? 100u : now);
    }
    cpu_peak = ui.cpu_win > ui.cpu_prev ? ui.cpu_win : ui.cpu_prev;
    cpu_peak = cpu_peak < cpu_now ? cpu_now : cpu_peak;
    cpu_late = felucca_dbg.late - ui.cpu_late0;
}

static uint32_t solo_any(void)
{
    uint32_t t, a = 0;
    for (t = 0; t < NT; t++)
        a |= pat.t[t].flags & TP_SOLO;
    return a;
}

static uint32_t any_mute(void)
{
    uint32_t t, a = 0;
    for (t = 0; t < NT; t++)
        a |= pat.t[t].flags & TP_MUTE;
    return a;
}

static uint32_t shown_pos(uint32_t t) { return snd.running_pub ? snd.pos_pub[t] : 0xFFu; }

static uint32_t flash_bits(void)                 /* tracks hit within FLASH_MS */
{
    uint32_t t, b = 0;
    for (t = 0; t < NT; t++) {
        if (snd.hits[t] != ui.seen_hits[t]) {
            ui.seen_hits[t] = snd.hits[t];
            ui.flash_until[t] = fm1_ms + FLASH_MS;
        }
        if ((int32_t)(ui.flash_until[t] - fm1_ms) > 0)
            b |= 1u << t;
    }
    return b;
}

/* one step cell of track t, step s, into the canvas at row top y (canvas coordinates) */
static void draw_cell(uint32_t t, uint32_t s, int32_t x, int32_t y)
{
    const step_t *st = &pat.t[t].s[s];
    uint32_t muted = !audible(t, solo_any());
    int head = shown_pos(t) == s;
    if (t == sel())
        cv_rect(x - 1, y, G_CW + 2, G_ROW, T_TINT);
    if (s >= tp_len(t))
        return;                                  /* beyond the track's length: nothing */
    if (st->f & ST_TRIG) {
        cv_rect(x, y + 1, G_CW, G_ROW - 2, head ? T_ACCENT : step_fill(st, (int)muted));
        if (st_marks(st))
            cv_rect(x + G_CW - 2, y + 1, 2, 2, T_TEXT);
    } else                                       /* off: a stub (magenta: locks waiting for a trig) */
        cv_rect(x, y + 3, G_CW, 2, head ? T_ACCENT : st_locked(st) ? C_LOCK : (s & 3u) ? T_LINE : T_DIM);
}

static void draw_mark(uint32_t t, int32_t y, uint32_t flash)
{
    if ((flash >> t) & 1u)
        cv_rect(G_MARK_X, y + 1, 3, G_ROW - 2, T_ACCENT);
}

static void draw_pair(uint32_t pr, uint32_t flash)   /* tracks 2 pr and 2 pr + 1: one 16 px strip */
{
    uint32_t k, solo = solo_any();
    cv_begin(240, 2u * G_ROW, T_BG);
    for (k = 0; k < 2u; k++) {
        uint32_t t = 2u * pr + k, s;
        uint16_t c;
        if (t >= NT)
            break;
        for (s = 0; s < NSTEP; s++)
            draw_cell(t, s, cell_x(s), (int32_t)(k * G_ROW));
        draw_mark(t, (int32_t)(k * G_ROW), flash);
        c = t == sel() ? T_ACCENT : !audible(t, solo) ? T_DIM : (pat.t[t].flags & TP_SOLO) ? T_THEME : T_MID;
        if (k == 0u)
            cv_text(3, 2, &AF_S, SK_SHORT[t], c);
        else
            cv_text_r(237, 2, &AF_S, SK_SHORT[t], c, T_BG);
        ui.drawn_pos[t] = (uint8_t)shown_pos(t);
    }
    cv_blit(0, (uint32_t)(G_TOP + (int32_t)pr * 2 * G_ROW));
}

static uint32_t sig_pair(uint32_t pr)            /* what a pair strip shows, but the playhead and the marks */
{
    uint32_t h = fnv(fnv(FNV0, pr), sel()), k, s;
    h = fnv(h, solo_any());
    for (k = 0; k < 2u; k++) {
        uint32_t t = 2u * pr + k;
        if (t >= NT)
            break;
        h = fnv(fnv(h, tp_len(t)), pat.t[t].flags);
        for (s = 0; s < NSTEP; s++) {
            const step_t *st = &pat.t[t].s[s];
            h = fnv(h, (st->f & 3u) | (uint32_t)st_marks(st) << 2 | (uint32_t)st_locked(st) << 3);
        }
    }
    return h;
}

static void grid_fast(uint32_t flash)            /* the playhead and the marks: only the cells that changed */
{
    uint32_t t;
    for (t = 0; t < NT; t++) {
        uint32_t now = shown_pos(t), was = ui.drawn_pos[t];
        int32_t y = G_TOP + (int32_t)t * G_ROW;
        if (now != was) {
            uint32_t i, c[2] = {was, now};
            for (i = 0; i < 2u; i++)
                if (c[i] < NSTEP) {
                    cv_begin(G_PITCH, G_ROW, T_BG);
                    draw_cell(t, c[i], 1, 0);
                    cv_blit((uint32_t)(cell_x(c[i]) - 1), (uint32_t)y);
                }
            ui.drawn_pos[t] = (uint8_t)now;
        }
        if (((flash ^ ui.drawn_flash) >> t) & 1u) {
            cv_begin(3, G_ROW, T_BG);
            if ((flash >> t) & 1u)
                cv_rect(0, 1, 3, G_ROW - 2, T_ACCENT);
            cv_blit(G_MARK_X, (uint32_t)y);
        }
    }
    ui.drawn_flash = flash;
}

static void params_line(char *b, unsigned max, uint32_t t)   /* "E100 D64 O64 F64" */
{
    char n[8];
    uint32_t i;
    b[0] = 0;
    for (i = 0; i < 4u; i++) {
        char l[2] = {SP_LETTER[i], 0};
        if (i)
            cat(b, " ", max);
        cat(b, l, max);
        fmt_int(n, kit.t[t].p[i]);
        cat(b, n, max);
    }
}

/* a track's mute / solo state: 0 plays, 1 soloed, 2 muted, 3 silent because other tracks are soloed */
static const char *const STATE_TEXT[4] = {"", "SOLO", "MUTED", "SILENT: SOLO ON"};
static uint32_t track_state(uint32_t t)
{
    uint32_t f = pat.t[t].flags;
    return (f & TP_MUTE) ? 2u : (f & TP_SOLO) ? 1u : solo_any() ? 3u : 0u;
}

static void draw_foot_home(void)
{
    char b[32];
    const char *m = msg_now();
    cv_begin(240, 240 - G_FOOT, T_BG);
    cv_rect(0, 0, 240, 1, T_LINE);
    if (m[0])
        cv_text_fit(6, 7, &AF_S, m, T_TEXT, T_BG, 228);
    else if (track_state(sel())) {               /* silenced or soloed: say so, it is not visible otherwise */
        cv_text_fit(6, 7, &AF_S, SK_NAME[sel()], T_TEXT, T_BG, 100);
        cv_text_r(234, 7, &AF_S, STATE_TEXT[track_state(sel())], track_state(sel()) == 1u ? T_THEME : T_ACCENT, T_BG);
    } else {
        params_line(b, sizeof b, sel());
        cv_text_r(234, 7, &AF_S, b, T_THEME, T_BG);
        cv_text_fit(6, 7, &AF_S, SK_NAME[sel()], T_TEXT, T_BG, 234 - text_w(&AF_S, b) - 14);
    }
    cv_blit(0, G_FOOT);
}

/* HOME, TRACK view: the selected track's 16 steps large, the held step's settings, the tracks sounding */
static void step_desc(char *b, unsigned max, const step_t *st, uint32_t s)
{
    char t[12];
    str_cpy(b, "STEP ", max);
    fmt_int(t, (int32_t)s + 1);
    cat(b, t, max);
    cat(b, (st->f & ST_TRIG) ? ((st->f & ST_ACC) ? "  ON  ACCENT" : "  ON") : "  OFF", max);
    cat(b, "  ", max);
    fmt_int(t, (int32_t)st_prob(st));
    cat(b, t, max);
    cat(b, "%", max);
}

static void step_desc2(char *b, unsigned max, const step_t *st)
{
    char t[12];
    uint32_t i;
    str_cpy(b, "X", max);
    fmt_int(t, (int32_t)st_ratchet(st));
    cat(b, t, max);
    if (st_ratchet(st) > 1u) {
        cat(b, " ", max);
        cat(b, RAMP_NAME[st_ramp(st)], max);
    }
    cat(b, "  ", max);
    cat(b, COND_NAME[st_cond(st)], max);
    cat(b, "  N", max);
    fmt_int(t, st_nudge(st));
    if (st_nudge(st) > 0)
        cat(b, "+", max);
    cat(b, t, max);
    for (i = 0; i < 4u; i++) {
        char l[3] = {' ', SP_LETTER[i], 0};
        cat(b, l, max);
        if (st->lock[i]) {
            fmt_int(t, st->lock[i] - 1);
            cat(b, t, max);
        } else
            cat(b, "--", max);
    }
}

static void draw_track_top(void)                 /* 22 .. 122: the name and the first 8 steps */
{
    char b[48];
    uint32_t t = sel(), s, silent = track_state(t) >= 2u;
    cv_begin(240, 100, T_BG);
    cv_text_fit(8, 4, &AF_M, SK_NAME[t], T_THEME, T_BG, 150);
    if (track_state(t))
        cv_text_r(232, 7, &AF_S, STATE_TEXT[track_state(t)], track_state(t) == 1u ? T_THEME : T_ACCENT, T_BG);
    else {
        str_cpy(b, "TRACK ", sizeof b);
        fmt_int(b + 6, (int32_t)t + 1);
        cv_text_r(232, 7, &AF_S, b, T_MID, T_BG);
    }
    for (s = 0; s < 8u; s++) {
        const step_t *st = &pat.t[t].s[s];
        int32_t x = 10 + (int32_t)s * 28, y = 30;
        int on = (st->f & ST_TRIG) != 0, in = s < tp_len(t), head = shown_pos(t) == s;
        uint16_t fill = !in ? T_BG : on ? step_fill(st, (int)silent) : T_RAISE;
        cv_rrect(x, y, 24, 52, 3, fill, T_BG);
        if (head)
            cv_frame(x - 2, y - 2, 28, 56, T_ACCENT);
        if (in) {
            if (!on && st_locked(st))            /* locks on a step that is off: a magenta edge */
                cv_frame(x + 1, y + 1, 22, 50, C_LOCK);
            fmt_int(b, (int32_t)s + 1);
            cv_text_in(x, y + 4, 24, &AF_S, b, on ? T_INK : T_MID, fill);
            if (st_ratchet(st) > 1u) {
                str_cpy(b, "X", sizeof b);
                fmt_int(b + 1, (int32_t)st_ratchet(st));
                cv_text_in(x, y + 20, 24, &AF_S, b, on ? T_INK : T_TEXT, fill);
            }
            if (st->prob)
                cv_rect(x + 3, y + 46, 18 * (int32_t)st_prob(st) / 100, 3, T_ACCENT);
            if (st_cond(st))
                cv_rect(x + 10, y + 38, 4, 4, on ? T_INK : T_TEXT);
        }
    }
    cv_blit(0, G_TOP);
}

static void draw_track_bottom(void)              /* 122 .. 214: steps 9..16, the held step, the tracks sounding */
{
    char b[64];
    uint32_t t = sel(), s, act = snd.active_pub, silent = track_state(t) >= 2u;
    cv_begin(240, 92, T_BG);
    for (s = 8; s < NSTEP; s++) {
        const step_t *st = &pat.t[t].s[s];
        int32_t x = 10 + (int32_t)(s - 8u) * 28, y = 2;
        int on = (st->f & ST_TRIG) != 0, in = s < tp_len(t), head = shown_pos(t) == s;
        uint16_t fill = !in ? T_BG : on ? step_fill(st, (int)silent) : T_RAISE;
        cv_rrect(x, y, 24, 52, 3, fill, T_BG);
        if (head)
            cv_frame(x - 2, y - 2 < 0 ? 0 : y - 2, 28, 55, T_ACCENT);
        if (in) {
            if (!on && st_locked(st))
                cv_frame(x + 1, y + 1, 22, 50, C_LOCK);
            fmt_int(b, (int32_t)s + 1);
            cv_text_in(x, y + 4, 24, &AF_S, b, on ? T_INK : T_MID, fill);
            if (st_ratchet(st) > 1u) {
                str_cpy(b, "X", sizeof b);
                fmt_int(b + 1, (int32_t)st_ratchet(st));
                cv_text_in(x, y + 20, 24, &AF_S, b, on ? T_INK : T_TEXT, fill);
            }
            if (st->prob)
                cv_rect(x + 3, y + 46, 18 * (int32_t)st_prob(st) / 100, 3, T_ACCENT);
            if (st_cond(st))
                cv_rect(x + 10, y + 38, 4, 4, on ? T_INK : T_TEXT);
        }
    }
    if (ui.held_step >= 0) {
        const step_t *st = &pat.t[t].s[ui.held_step];
        step_desc(b, sizeof b, st, (uint32_t)ui.held_step);
        cv_text_fit(8, 58, &AF_S, b, T_TEXT, T_BG, 224);
        step_desc2(b, sizeof b, st);
        cv_text_fit(8, 72, &AF_S, b, T_THEME, T_BG, 224);
    } else {
        cv_text_fit(8, 58, &AF_S, ui.mode ? "HOLD A STEP + KNOBS: LOCKS" : "SEQ: STEP MODE", T_DIM, T_BG, 224);
        for (s = 0; s < NT; s++)                 /* the tracks sounding */
            cv_rect(8 + (int32_t)s * 10, 78, 8, 8, (act >> s) & 1u ? T_ACCENT : s == t ? T_SEL : T_RAISE);
    }
    cv_blit(0, 122);
}

/* the parameter pages */
static void draw_page_head(void)
{
    char b[40];
    uint32_t r, live = 0;
    for (r = 0; r < 4u; r++)
        live |= PP[ui.page][r].name && PP[ui.page][r].live;
    cv_begin(240, 30, T_BG);
    str_cpy(b, PAGE_NAME[ui.page], sizeof b);
    if (ui.page == PG_TRACK || ui.page == PG_TRACK2) {
        cat(b, "  ", sizeof b);
        cat(b, SK_SHORT[sel()], sizeof b);
    }
    cv_text(10, 6, &AF_M, b, T_THEME);
    fmt_int(b, ui.page + 1);
    cat(b, "/9", sizeof b);
    cv_text_r(230, 8, &AF_S, b, T_MID, T_BG);
    if (!live)
        cv_text_r(196, 8, &AF_S, "SOON", T_ACCENT, T_BG);
    cv_rect(10, 28, 220, 1, T_LINE);
    cv_blit(0, 0);
}

static void draw_page_rows(uint32_t r0)          /* rows r0, r0 + 1: 44 px each */
{
    uint32_t r, page_live = 0;
    for (r = 0; r < 4u; r++)
        page_live |= PP[ui.page][r].name && PP[ui.page][r].live;
    cv_begin(240, 88, T_BG);
    for (r = r0; r < r0 + 2u; r++) {
        const param_t *p = &PP[ui.page][r];
        int32_t y = (int32_t)(r - r0) * 44, x, v;
        char b[24], k[4] = {'K', (char)('1' + r), 0, 0};
        if (!p->name)
            continue;
        v = pv_get(ui.page, r, sel());
        cv_text(10, y + 6, &AF_S, k, T_DIM);
        x = cv_text(34, y + 4, &AF_M, p->name, p->live ? T_TEXT : T_MID);
        if (!p->live && page_live)
            cv_text(x + 8, y + 6, &AF_S, "SOON", T_ACCENT);
        pv_text(b, sizeof b, p, v);
        cv_text_r(230, y + 4, &AF_M, b, p->live ? T_THEME : T_MID, T_BG);
        if (p->kind == K_PAN || p->kind == K_OFF50)
            draw_bar_h(34, y + 28, 196, 6, v - 50, 50, 1);
        else
            draw_bar_h(34, y + 28, 196, 6, v - p->lo, p->hi - p->lo, 0);
    }
    cv_blit(0, 32u + r0 * 44u);
}

static void draw_page_foot(void)
{
    const char *m = msg_now();
    cv_begin(240, 26, T_BG);
    cv_rect(10, 0, 220, 1, T_LINE);
    cv_text_fit(10, 8, &AF_S, m[0] ? m : "SELECT: PAGE   HOME: BACK", m[0] ? T_TEXT : T_DIM, T_BG, 220);
    cv_blit(0, 214);
}

static uint32_t sig_page_rows(uint32_t r0)
{
    uint32_t h = fnv(fnv(FNV0, ui.page), sel()), r;
    for (r = r0; r < r0 + 2u; r++)
        h = fnv(h, (uint32_t)pv_get(ui.page, r, sel()));
    return h;
}

/* SAVE */
static int slot_used(uint32_t what, uint32_t i)
{
    return what == SV_PAT ? (pat_stored >> i) & 1u : what == SV_KIT ? (kit_stored >> i) & 1u : songs.s[i].n > 0u;
}

static void draw_save(void)
{
    char b[24];
    const char *m = msg_now();
    cv_begin(240, 124, T_BG);
    cv_text_in(0, 8, 240, &AF_M, "SAVE", T_MID, T_BG);
    cv_text_in(0, 30, 240, &AF_M, SV_NAME[ui.save_what], T_TEXT, T_BG);
    label2(b, sizeof b, ui.save_what == SV_PAT ? 'P' : ui.save_what == SV_KIT ? 'K' : 'S', ui.save_slot + 1u);
    cv_text_in(0, 56, 240, &AF_L, b, T_ACCENT, T_BG);
    cv_text_in(0, 96, 240, &AF_S, slot_used(ui.save_what, ui.save_slot) ? "REPLACES WHAT IS THERE" : "EMPTY SLOT",
               T_MID, T_BG);
    cv_blit(0, 0);
    cv_begin(240, 116, T_BG);
    cv_text_in(0, 14, 240, &AF_S, "SELECT: PATTERN / KIT / SONG", T_MID, T_BG);
    cv_text_in(0, 34, 240, &AF_S, "PRESETS: SLOT", T_MID, T_BG);
    cv_text_in(0, 54, 240, &AF_S, "SAVE: STORE", T_TEXT, T_BG);
    cv_text_in(0, 74, 240, &AF_S, "ANY OTHER BUTTON: CANCEL", T_MID, T_BG);
    cv_text_fit(10, 96, &AF_S, m, T_TEXT, T_BG, 220);
    cv_blit(0, 124);
}

/* SONG */
static void draw_song(void)
{
    const song_t *sg = &songs.s[song_index];
    char b[32], t[8];
    int32_t i, first = (int32_t)ui.song_cur - 3;
    const char *m = msg_now();
    first = first < 0 ? 0 : first;
    cv_begin(240, 120, T_BG);
    label2(b, sizeof b, 'S', song_index + 1u);
    str_cpy(t, b, sizeof t);
    str_cpy(b, "SONG ", sizeof b);
    cat(b, t + 1, sizeof b);
    cv_text(10, 6, &AF_M, b, T_THEME);
    cv_text_r(230, 8, &AF_S, sg->loop ? "LOOP" : "ONCE", T_MID, T_BG);
    cv_text_r(180, 8, &AF_S, "PLAY: SOON", T_ACCENT, T_BG);
    cv_rect(10, 28, 220, 1, T_LINE);
    for (i = 0; i < 4; i++) {                    /* entries first .. first + 3 */
        int32_t e = first + i, y = 34 + i * 21;
        if (e > (int32_t)sg->n || e >= (int32_t)SONG_LEN)
            break;
        if (e == (int32_t)ui.song_cur)
            cv_rrect(8, y - 2, 224, 20, 4, T_SEL, T_BG);
        fmt_int(t, e + 1);
        str_cpy(b, e + 1 < 10 ? "0" : "", sizeof b);
        cat(b, t, sizeof b);
        cv_text(16, y, &AF_S, b, e == (int32_t)ui.song_cur ? T_INK : T_MID);
        if (e == (int32_t)sg->n)
            cv_text(56, y, &AF_S, "END (PRESETS: ADD)", e == (int32_t)ui.song_cur ? T_INK : T_DIM);
        else {
            label2(b, sizeof b, 'P', sg->e[e][0] + 1u);
            cat(b, "  X", sizeof b);
            fmt_int(t, sg->e[e][1]);
            cat(b, t, sizeof b);
            cv_text(56, y, &AF_S, b, e == (int32_t)ui.song_cur ? T_INK : T_TEXT);
        }
    }
    cv_blit(0, 0);
    cv_begin(240, 120, T_BG);
    cv_text_in(0, 20, 240, &AF_S, "SELECT: ENTRY   PRESETS: PATTERN", T_MID, T_BG);
    cv_text_in(0, 40, 240, &AF_S, "K1 REPEATS  K2 ADD/DEL  K3 LOOP", T_MID, T_BG);
    cv_text_in(0, 60, 240, &AF_S, "K4 SONG 1..4   G5: CLOSE", T_MID, T_BG);
    cv_text_fit(10, 94, &AF_S, m, T_TEXT, T_BG, 220);
    cv_blit(0, 120);
}

static uint32_t sig_song(void)
{
    const song_t *sg = &songs.s[song_index];
    uint32_t h = fnv(fnv(fnv(fnv(FNV0, song_index), ui.song_cur), sg->n), sg->loop), i;
    for (i = 0; i < sg->n; i++)
        h = fnv(fnv(h, sg->e[i][0]), sg->e[i][1]);
    return fnv_s(h, msg_now());
}

static uint32_t screen_id(void)
{
    if (ui.saving)
        return 0xF1u;
    if (ui.song_open)
        return 0xF2u;
    if (ui.page != PG_HOME)
        return ui.page;
    return 0xE0u + set.view;
}

static void draw_splash(void)                    /* the name, its letters yellow, cyan, magenta in turn */
{
    static const char *const name = APP_NAME;
    char pre[24], one[2] = {0, 0};
    uint32_t i, n = str_len(name), k = 0;
    int32_t x0;
    lcd_fill(0, 0, 240, 240, T_BG);
    x0 = (240 - text_w(&AF_L, name)) / 2;
    cv_begin(240, AF_L.h, T_BG);
    for (i = 0; i < n && i + 1u < sizeof pre; i++) {
        uint16_t c = k % 3u == 0u ? T_THEME : k % 3u == 1u ? T_ACCENT : C_LOCK;
        one[0] = name[i];
        str_cpy(pre, name, i + 2u);              /* its place: the kerned word up to it, less its own width */
        if (name[i] != ' ') {
            cv_text(x0 + text_w(&AF_L, pre) - text_w(&AF_L, one), 0, &AF_L, one, c);
            k++;                                 /* (a space takes no colour) */
        }
    }
    cv_blit(0, 82);
    draw_text_line(0, 122, 240, &AF_S, APP_VERSION, T_MID, T_BG, 1);
    draw_text_line(0, 150, 240, &AF_S, "STK SHAKERS (PHISEM) - PERRY COOK", T_DIM, T_BG, 1);
}

static void ui_draw(void)
{
    uint32_t scr = screen_id(), all, h, i, flash;
    const char *m = msg_now();
    if ((int32_t)(ui.splash_until - fm1_ms) > 0) {   /* the splash: nothing else until it ends */
        if (app_redraw) {
            app_redraw = 0;
            draw_splash();
        }
        return;
    }
    all = app_redraw || scr != ui.drawn_screen;
    flash = flash_bits();
    if (all) {
        app_redraw = 0;
        lcd_fill(0, 0, 240, 240, T_BG);
        ui.drawn_screen = (uint8_t)scr;
        for (i = 0; i < 16u; i++)
            ui.sig[i] = ~0u;
    }
    if (ui.saving) {
        h = fnv_s(fnv(fnv(fnv(FNV0, ui.save_what), ui.save_slot), (uint32_t)slot_used(ui.save_what, ui.save_slot)), m);
        if (h != ui.sig[0])
            draw_save();
        ui.sig[0] = h;
        return;
    }
    if (ui.song_open) {
        h = sig_song();
        if (h != ui.sig[0])
            draw_song();
        ui.sig[0] = h;
        return;
    }
    if (ui.page != PG_HOME) {
        uint32_t s[4];
        s[0] = fnv(fnv(FNV0, ui.page), sel());
        s[1] = sig_page_rows(0);
        s[2] = sig_page_rows(2);
        s[3] = fnv_s(FNV0, m);
        if (s[0] != ui.sig[0])
            draw_page_head();
        if (s[1] != ui.sig[1])
            draw_page_rows(0);
        if (s[2] != ui.sig[2])
            draw_page_rows(2);
        if (s[3] != ui.sig[3])
            draw_page_foot();
        for (i = 0; i < 4u; i++)
            ui.sig[i] = s[i];
        return;
    }
    h = fnv(fnv(fnv(fnv(fnv(fnv(FNV0, pat_index()), ui.queued ? ui.queued * 2u + ((fm1_ms / 250u) & 1u) : 0u),
                        pat_bpm2()), snd.running_pub), ui.rec), ui.mode);
    h = fnv(fnv(h, snd.ext_pub), snd.ext_pub ? snd.ext_bpm2_pub : 0u);
    h = fnv(fnv(fnv(h, (uint32_t)panel_held(B_ARP)), solo_any()), any_mute());
    if (h != ui.sig[0])
        draw_head();
    ui.sig[0] = h;
    h = fnv(fnv(fnv(FNV0, cpu_now), cpu_peak), cpu_late != 0u);
    if (h != ui.sig[14])
        draw_cpu_bar();
    ui.sig[14] = h;
    h = fnv_s(fnv(sel(), kit.t[sel()].p[0] | kit.t[sel()].p[1] << 8 | kit.t[sel()].p[2] << 16 |
                             (uint32_t)kit.t[sel()].p[3] << 24), m);
    h = fnv(h, track_state(sel()));
    if (set.view == 0u) {
        uint32_t pr;
        for (pr = 0; pr < 12u; pr++) {
            uint32_t sp = sig_pair(pr);
            if (sp != ui.sig[1u + pr])
                draw_pair(pr, flash);
            ui.sig[1u + pr] = sp;
        }
        grid_fast(flash);
        if (h != ui.sig[13])
            draw_foot_home();
        ui.sig[13] = h;
    } else {
        uint32_t t = sel(), s, a = fnv(fnv(FNV0, t), shown_pos(t)), b;
        a = fnv(fnv(a, tp_len(t)), track_state(t));
        for (s = 0; s < NSTEP; s++) {
            const step_t *st = &pat.t[t].s[s];
            a = fnv(fnv(fnv(fnv(a, st->f), st->prob), st->rat), st->lock[0] | st->lock[1] << 8 |
                    st->lock[2] << 16 | (uint32_t)st->lock[3] << 24);
        }
        b = fnv(fnv(fnv(a, (uint32_t)ui.held_step), snd.active_pub), ui.mode);
        if (a != ui.sig[1])
            draw_track_top();
        if (b != ui.sig[2])
            draw_track_bottom();
        if (h != ui.sig[13])
            draw_foot_home();
        ui.sig[1] = a;
        ui.sig[2] = b;
        ui.sig[13] = h;
    }
}

/* -------------------------------------------------------------------- input --- */
static void step_edit_enc(uint32_t role, int32_t s)   /* an encoder while a step key is held */
{
    step_t *st = &pat.t[sel()].s[ui.held_step];
    ui.step_used = 1;
    if (role >= EN_K1) {
        uint32_t i = role - EN_K1;
        int32_t cur = st->lock[i] ? st->lock[i] : kit.t[sel()].p[i] + 1;
        st->lock[i] = (uint8_t)clampi(cur + s, 0, 128);   /* 0: no lock (turned all the way down) */
    } else if (role == EN_SELECT)
        st->prob = (uint8_t)clampi(st->prob - s, 0, 20);   /* clockwise: more likely */
    else if (role == EN_ALGO) {                  /* 1x, then 2x / 3x / 4x each FLAT UP DOWN */
        int32_t r = st_ratchet(st), idx = r == 1 ? 0 : 1 + (r - 2) * 3 + (int32_t)st_ramp(st);
        idx = clampi(idx + s, 0, 9);
        r = idx == 0 ? 1 : 2 + (idx - 1) / 3;
        st->rat = (uint8_t)((st->rat & 0xF0u) | (uint32_t)(r - 1) | (uint32_t)(idx == 0 ? 0 : (idx - 1) % 3) << 2);
    } else if (role == EN_PRESET) {
        int32_t c = clampi((int32_t)st_cond(st) + s, 0, NCOND - 1);
        st->f = (uint8_t)((st->f & 3u) | (uint32_t)c << 2);
    }
}

static void nudge(int32_t d)
{
    step_t *st = &pat.t[sel()].s[ui.held_step];
    int32_t n = clampi(st_nudge(st) + d, -5, 5);
    st->rat = (uint8_t)((st->rat & 15u) | ((uint32_t)n & 15u) << 4);
    ui.step_used = 1;
}

static void song_enc(uint32_t role, int32_t s)
{
    song_t *sg = &songs.s[song_index];
    uint32_t c = ui.song_cur;
    if (role == EN_SELECT)
        ui.song_cur = (uint8_t)clampi((int32_t)c + s, 0, sg->n < SONG_LEN ? sg->n : SONG_LEN - 1);
    else if (role == EN_PRESET) {
        if (c == sg->n && sg->n < SONG_LEN) {    /* on END: a new entry, the pattern playing */
            sg->e[c][0] = (uint8_t)pat_index();
            sg->e[c][1] = 1;
            sg->n++;
        } else if (c < sg->n)
            sg->e[c][0] = (uint8_t)clampi(sg->e[c][0] + s, 0, NPAT - 1);
    } else if (role == EN_K1 && c < sg->n)
        sg->e[c][1] = (uint8_t)clampi(sg->e[c][1] + s, 1, 16);
    else if (role == EN_K2) {
        uint32_t i;
        if (s > 0 && sg->n < SONG_LEN && c < sg->n) {   /* insert a copy of the entry */
            for (i = sg->n; i > c; i--) {
                sg->e[i][0] = sg->e[i - 1u][0];
                sg->e[i][1] = sg->e[i - 1u][1];
            }
            sg->n++;
        } else if (s < 0 && c < sg->n) {         /* delete it */
            for (i = c; i + 1u < sg->n; i++) {
                sg->e[i][0] = sg->e[i + 1u][0];
                sg->e[i][1] = sg->e[i + 1u][1];
            }
            sg->n--;
        }
    } else if (role == EN_K3)
        sg->loop = s > 0;
    else if (role == EN_K4) {
        song_index = (uint8_t)clampi(song_index + s, 0, NSONG - 1);
        ui.song_cur = 0;
    }
}

static void ui_encoders(void)
{
    uint32_t role;
    for (role = 0; role < NE; role++) {
        int32_t s = panel_enc(role);
        if (!s)
            continue;
        if (ui.held_step >= 0 && ui.mode) {
            step_edit_enc(role, s);
            continue;
        }
        if (ui.saving) {
            if (role == EN_SELECT) {
                ui.save_what = (uint8_t)clampi(ui.save_what + s, 0, 2);
                ui.save_slot = (uint8_t)(ui.save_what == SV_PAT ? pat_index() : ui.save_what == SV_KIT ? kit_index()
                                                                                                    : song_index);
            } else if (role == EN_PRESET)
                ui.save_slot = (uint8_t)clampi(ui.save_slot + s, 0,
                                               ui.save_what == SV_PAT ? NPAT - 1 : ui.save_what == SV_KIT ? NKIT - 1
                                                                                                         : NSONG - 1);
            continue;
        }
        if (ui.song_open) {
            song_enc(role, s);
            continue;
        }
        if (role == EN_ALGO) {                   /* tempo, 1 BPM a detent */
            pat.tempo2 = (uint16_t)clampi((int32_t)pat_bpm2() + 2 * s, 80, 480);
            continue;
        }
        if (role == EN_PRESET) {
            int32_t base = ui.queued ? ui.queued - 1 : (int32_t)pat_index();
            queue_pattern((uint32_t)clampi(base + s, 0, NPAT - 1));
            continue;
        }
        if (role == EN_SELECT) {
            if (ui.page == PG_HOME)
                track_select((uint32_t)clampi((int32_t)sel() + s, 0, NT - 1));
            else
                page_open((uint32_t)clampi(ui.page + s, 0, NPAGES - 1));
            continue;
        }
        if (ui.page == PG_HOME) {                /* KNOB 1..4: the selected track's sound */
            uint32_t i = role - EN_K1;
            kit.t[sel()].p[i] = (uint8_t)clampi(kit.t[sel()].p[i] + s, 0, 127);
        } else {
            uint32_t r = role - EN_K1;
            const param_t *p = &PP[ui.page][r];
            if (!p->name || K_READONLY(p->kind))
                continue;
            pv_set(ui.page, r, sel(), pv_get(ui.page, r, sel()) + (p->kind == K_BPM2 ? 2 * s : s));
            if (ui.page == PG_TIME || ui.page == PG_RANDOM || ui.page == PG_GLO || ui.page == PG_GLO2)
                ui.set_dirty = 1;
            if (ui.page == PG_GLO && r == 0u)
                snd.silence_req = 1;             /* notes out on the old channel must not hang */
        }
    }
}

static void key_press(uint32_t k, uint32_t keys)
{
    int edit = panel_held(B_EDIT), ws = white_step(k);
    if (edit) {                                  /* EDIT + a key: select (STEP mode's E5 / F5 / G5 too) */
        if (k < NT && !(ui.mode && (k == KEY_MUTE || k == KEY_SOLO)))
            track_select(k);
        else if (k == KEY_MUTE || k == KEY_SOLO) {
            uint8_t bit = k == KEY_MUTE ? TP_MUTE : TP_SOLO;
            pat.t[sel()].flags ^= bit;
            msg2(SK_SHORT[sel()], (pat.t[sel()].flags & bit) ? (bit == TP_MUTE ? " MUTED" : " SOLO") :
                                                                (bit == TP_MUTE ? " UNMUTED" : " SOLO OFF"));
        } else if (k == KEY_SONG) {
            ui.song_open ^= 1u;
            ui.saving = 0;
        }
        ui.held_fired[B_EDIT] = 1;
        return;
    }
    if (ui.mode && ws >= 0) {                    /* STEP mode: a white key is a step */
        if ((uint32_t)ws >= tp_len(sel()))
            return;
        if (ui.held_step < 0) {
            ui.held_step = (int8_t)ws;
            ui.step_used = 0;
            if ((keys >> KEY_ACCENT) & 1u) {     /* ACCENT held: an accented trig */
                step_t *st = &pat.t[sel()].s[ws];
                st->f = (st->f & ST_TRIG) ? st->f ^ ST_ACC : st->f | ST_TRIG | ST_ACC;
                ui.step_used = 1;
            }
        }
        return;
    }
    if (ui.mode && k == KEY_ACCENT && ui.held_step >= 0) {   /* ACCENT tapped on a held step */
        pat.t[sel()].s[ui.held_step].f ^= ST_ACC;
        ui.step_used = 1;
        return;
    }
    if (k == KEY_SONG && !ui.mode) {
        ui.song_open ^= 1u;
        ui.saving = 0;
        return;
    }
    /* MUTE and SOLO count as held only if they were down before this frame: a chord (a forearm across the keys)
     * that includes E5 or F5 must not toggle every pad in it. E5 held, then F5 (or the other way): all off. */
    if (!ui.mode && ((k == KEY_SOLO && ((ui.last_keys & keys) >> KEY_MUTE & 1u)) ||
                     (k == KEY_MUTE && ((ui.last_keys & keys) >> KEY_SOLO & 1u)))) {
        uint32_t t;
        for (t = 0; t < NT; t++)
            pat.t[t].flags &= (uint8_t)~(TP_MUTE | TP_SOLO);
        msg("ALL MUTES AND SOLOS OFF");
        return;
    }
    if (k < NT && !ui.mode) {
        if ((ui.last_keys >> KEY_MUTE) & 1u) {
            pat.t[k].flags ^= TP_MUTE;
            msg2(SK_SHORT[k], (pat.t[k].flags & TP_MUTE) ? " MUTED" : " UNMUTED");
        } else if ((ui.last_keys >> KEY_SOLO) & 1u) {
            pat.t[k].flags ^= TP_SOLO;
            msg2(SK_SHORT[k], (pat.t[k].flags & TP_SOLO) ? " SOLO" : " SOLO OFF");
        } else {
            track_select(k);                     /* (the ISR played it, and repeats it while ARP is held) */
        }
    }
}

static void key_release(uint32_t k)
{
    if (ui.mode && ui.held_step >= 0 && white_step(k) == ui.held_step) {
        if (!ui.step_used)
            pat.t[sel()].s[ui.held_step].f ^= ST_TRIG;
        ui.held_step = -1;
    }
}

static void ui_keys(void)
{
    uint32_t keys = fm1_in.notes, ch = keys ^ ui.last_keys, k, i;
    for (i = 0; i < 28u; i++) {                  /* ACCENT first: pressed with a step in one frame, it is a modifier */
        k = i == 0u ? KEY_ACCENT : i - 1u;
        if (i && k == KEY_ACCENT)
            continue;
        if ((ch >> k) & 1u) {
            if ((keys >> k) & 1u)
                key_press(k, keys);
            else
                key_release(k);
        }
    }
    ui.last_keys = keys;
    if (ui.held_step >= 0 && !((keys >> step_key((uint32_t)ui.held_step)) & 1u))
        ui.held_step = -1;                       /* (a release missed between frames) */
    /* which keys the ISR plays: PLAY mode the 23 pads, STEP mode the black ones; none while EDIT, MUTE or SOLO
     * is held (those keys select or toggle) */
    snd.fill = (uint8_t)panel_held(B_ARP);       /* FILL conditions, note repeat */
    if (panel_held(B_EDIT) || (!ui.mode && (keys & (1u << KEY_MUTE | 1u << KEY_SOLO))))
        snd.pad_mask = 0;
    else
        snd.pad_mask = ui.mode ? PAD_MASK & BLACK_KEYS : PAD_MASK;
}

static void button_press(uint32_t b)
{
    if (ui.saving) {
        if (b == B_SAVE)
            save_store();
        else {
            ui.saving = 0;
            msg("SAVE CANCELLED");
        }
        ui.held_fired[b] = 1;
        return;
    }
    if (ui.held_step >= 0 && ui.mode && (b == B_OCTDN || b == B_OCTUP)) {   /* nudge the held step */
        nudge(b == B_OCTUP ? 1 : -1);
        ui.held_fired[b] = 1;
        return;
    }
    if (ui.held_step >= 0 && ui.mode && b == B_EDIT) {   /* a held step + EDIT: all its locks off */
        char t[8];
        memset(pat.t[sel()].s[ui.held_step].lock, 0, 4);
        ui.step_used = 1;
        ui.held_fired[b] = 1;
        fmt_int(t, ui.held_step + 1);
        msg2("LOCKS CLEARED: STEP ", t);
        return;
    }
    switch (b) {
    case B_PLAY:
        if (snd.ext_pub) {                       /* following a MIDI clock: its master starts and stops */
            msg("CLOCK EXT: THE MASTER PLAYS");
            break;
        }
        if (fm1_ms - ui.play_tap_ms < DTAP_MS) {
            snd.run_req = 3;
            msg("RESTART");
        } else
            snd.run_req = snd.running_pub ? 2u : 1u;
        ui.play_tap_ms = fm1_ms;
        break;
    case B_REC:
        ui.rec ^= 1u;
        snd.rec = ui.rec;
        msg(ui.rec ? "RECORD ON" : "RECORD OFF");
        break;
    case B_SEQ:
        ui.mode ^= 1u;
        ui.held_step = -1;
        msg(ui.mode ? "STEP MODE" : "PAD MODE");
        break;
    case B_HOME:
        if (ui.page == PG_HOME && !ui.song_open) {
            set.view ^= 1u;
            ui.set_dirty = 1;
        }
        page_open(PG_HOME);
        break;
    case B_SAVE:
        if (panel_held(B_EDIT)) {                /* EDIT + SAVE: clear the selected track (twice) */
            ui.held_fired[B_EDIT] = 1;
            if ((int32_t)(ui.clear_until - fm1_ms) > 0) {
                memset(pat.t[sel()].s, 0, sizeof pat.t[sel()].s);
                ui.clear_until = fm1_ms;
                msg2(SK_SHORT[sel()], " CLEARED");
            } else {
                ui.clear_until = fm1_ms + CONFIRM_MS;
                msg("EDIT+SAVE AGAIN: CLEAR TRACK");
            }
            break;
        }
        ui.saving = 1;
        ui.song_open = 0;
        ui.save_what = SV_PAT;
        ui.save_slot = (uint8_t)pat_index();
        break;
    case B_FX: page_open(ui.page == PG_FX1 ? PG_FX2 : ui.page == PG_FX2 ? PG_FX3 : PG_FX1); break;
    case B_ENV: page_open(ui.page == PG_TRACK ? PG_TRACK2 : PG_TRACK); break;
    case B_LFO: page_open(PG_RANDOM); break;
    case B_SCL: page_open(PG_TIME); break;
    case B_GLO: page_open(ui.page == PG_GLO ? PG_GLO2 : PG_GLO); break;
    case B_OCTDN:
    case B_OCTUP: {
        int32_t base = ui.queued ? ui.queued - 1 : (int32_t)pat_index();
        queue_pattern((uint32_t)clampi(base + (b == B_OCTUP ? 1 : -1), 0, NPAT - 1));
        break;
    }
    }
}

static void ui_buttons(void)
{
    uint32_t now = fm1_in.buttons, b;
    for (b = 0; b < NB; b++) {
        uint32_t m = panel_mask(b), down = now & m, was = ui.last_buttons & m;
        if (down && !was) {
            ui.down_ms[b] = fm1_ms;
            ui.held_fired[b] = 0;
            button_press(b);
        } else if (down && !ui.held_fired[b] && fm1_ms - ui.down_ms[b] >= HOLD_MS) {
            ui.held_fired[b] = 1;
        }
    }
    ui.last_buttons = now;
}

static void ui_master(void)                      /* the MASTER pot as the output level, 0 .. 256 */
{
    static int32_t avg = -1;
    int32_t v, q;
    if (ui.frames & 3u)
        return;
    v = fm1_adc_read(FM1_ADC_MASTER);
    if (v < 0)
        return;
    avg = avg < 0 ? v << 4 : avg + v - (avg >> 4);
    q = (avg >> 4) * 257 >> 10;
    if (q - snd.master_q8 > 2 || snd.master_q8 - q > 2 || q == 0 || q == 256)
        snd.master_q8 = q > 256 ? 256 : q;
}

static void ui_leds(void)
{
    uint32_t b, k, blink = (fm1_ms / 250u) & 1u, act = snd.active_pub, t = sel();
    for (b = 0; b < NB; b++) {
        int on = panel_held(b);
        switch (b) {
        case B_PLAY: on |= snd.running_pub; break;
        case B_REC: on |= ui.rec; break;
        case B_SEQ: on |= ui.mode; break;
        case B_HOME: on |= ui.page == PG_HOME && !ui.saving && !ui.song_open; break;
        case B_SAVE: on |= ui.saving && blink; break;
        case B_ENV: on |= ui.page == PG_TRACK || ui.page == PG_TRACK2; break;
        case B_SCL: on |= ui.page == PG_TIME; break;
        case B_LFO: on |= ui.page == PG_RANDOM; break;
        case B_FX: on |= ui.page == PG_FX1 || ui.page == PG_FX2 || ui.page == PG_FX3; break;
        case B_GLO: on |= ui.page == PG_GLO || ui.page == PG_GLO2; break;
        case B_OCTDN:
        case B_OCTUP: on |= ui.queued && blink; break;
        }
        fm1_led_key(panel.btn[b], on);
    }
    for (k = 0; k < 27u; k++) {
        int on = 0, ws = white_step(k);
        if (ui.mode && ws >= 0) {                /* STEP mode: the selected track's trigs, the playhead inverted */
            if ((uint32_t)ws < tp_len(t)) {
                on = (pat.t[t].s[ws].f & ST_TRIG) != 0;
                if (shown_pos(t) == (uint32_t)ws)
                    on = !on;
            }
        } else if (k < NT)
            on = (act >> k) & 1u || k == t;
        else if (k == KEY_SONG)
            on = ui.song_open;
        if ((fm1_in.notes >> k) & 1u)
            on = 1;
        fm1_led_key(14u + k, on);
    }
}

static void ui_init(void)
{
    palette_set(PAL_AMBER);
    ui.page = PG_HOME;
    ui.held_step = -1;
    ui.drawn_screen = 0xFF;
    snd.sel = (uint8_t)sel();
    snd.pad_mask = PAD_MASK;
    ui.splash_until = fm1_ms + SPLASH_MS;
    ui.cpu_late0 = felucca_dbg.late;             /* (.noinit: it counts across resets; the meter counts from here) */
}

static void ui_frame(void)
{
    ui.frames++;
    if (snd.swapped) {                           /* the queued pattern took over */
        snd.swapped = 0;
        ui.queued = 0;
    }
    ui_buttons();
    ui_keys();
    ui_encoders();
    ui_master();
    cpu_update();
    ui_leds();
    ui_draw();
}
