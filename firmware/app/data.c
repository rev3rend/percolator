/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 R. Luke DuBois */
/* Percolator's data (docs/SPEC.md "What is stored"): the 23 tracks' sounds and mix (the kit), the pattern being played
 * (23 tracks x 16 steps with every per-step field), the settings, the songs, and their records in the store. Plus
 * the page parameters: one table row per value a page shows (name, range, how it is printed, whether it works yet).
 *
 * Records (store.c slots, APP_ID): 0 settings, 1..16 patterns, 17..20 kits, 21 the four songs. Every record starts
 * with a version byte; formats only grow at the end (a shorter record leaves the newer fields at their defaults), and
 * every loaded value is range-checked (another version, a damaged sector). All 16 patterns and the 4 kits live in
 * the pool (~43 KB): a pattern change is a pointer swap at the bar line (the ISR), edits survive changing patterns, and
 * SAVE writes the one shown to flash. All-zero is a valid empty pattern: length 0 reads as 16, divide 0 as 1/1, tempo 0 as 120 BPM, a step of zeros is
 * off with no locks, probability 100 % and no nudge. */

#include "shk_tables.h"          /* build/gen: tools/gen_shakers.py (STK's Shakers data, fixed point) */

#define NT 23u                   /* tracks: STK Shakers types 0..22, played by keys 0..22 (F3..D#5) */
#define NSTEP 16u
#define NPAT 16u
#define NKIT 4u
#define NSONG 4u
#define SONG_LEN 64u
#define SLOT_SET 0u
#define SLOT_PAT0 1u             /* 1..16 */
#define SLOT_KIT0 17u            /* 17..20 */
#define SLOT_SONG 21u
#define KEY_MUTE 23u             /* the performance keys above the pads (fm1_in.notes bits) */
#define KEY_SOLO 24u
#define KEY_ACCENT 25u
#define KEY_SONG 26u
#define PAD_MASK ((1u << NT) - 1u)
#define BLACK_KEYS 0x0252A52Au   /* bit k: key k is black (k % 12 in 1 3 5 8 10: F#3 G#3 A#3 C#4 D#4 ... F#5) */
_Static_assert(NT == SK_N, "a track per STK Shakers instrument");

enum { SP_EN, SP_DEC, SP_OBJ, SP_FRQ };          /* the four Shakers parameters (STK's code: CC 2, 11, 4, 1) */
static const char *const SP_NAME[4] = {"ENERGY", "DECAY", "OBJECTS", "FREQ"};
static const char SP_LETTER[4] = {'E', 'D', 'O', 'F'};

/* --------------------------------------------------------- the pattern --- */
/* a step, 7 bytes. f: bit 0 trig, bit 1 accent, bits 2..6 condition (COND_NAME). prob: the chance taken away in 5 %
 * steps (0 = 100 %, 20 = never). rat: bits 0..1 ratchet - 1 (1..4 hits), bits 2..3 ramp (RAMP_NAME), bits 4..7 nudge
 * in twelfths of a step, two's complement (-5..+5). lock[i]: 0 = none, else the value + 1 (1..128) */
typedef struct {
    uint8_t f, prob, rat, lock[4];
} step_t;
#define ST_TRIG 1u
#define ST_ACC 2u
#define NCOND 18u
static const char *const COND_NAME[NCOND] = {"ALWAYS", "1:2", "2:2", "1:3", "2:3", "3:3", "1:4", "2:4", "3:4",
                                             "4:4", "FILL", "NOT FILL", "FIRST", "NOT FIRST", "1:5", "1:6", "1:7",
                                             "1:8"};
static const char *const RAMP_NAME[3] = {"FLAT", "UP", "DOWN"};
static uint32_t st_cond(const step_t *s) { uint32_t c = (s->f >> 2) & 31u; return c < NCOND ? c : 0u; }
static uint32_t st_ratchet(const step_t *s) { return (s->rat & 3u) + 1u; }
static uint32_t st_ramp(const step_t *s) { uint32_t r = (s->rat >> 2) & 3u; return r < 3u ? r : 0u; }
static int32_t st_nudge(const step_t *s)
{
    int32_t n = (int32_t)(s->rat >> 4);
    n = n >= 8 ? n - 16 : n;
    return n < -5 ? -5 : n > 5 ? 5 : n;
}
static uint32_t st_prob(const step_t *s) { return s->prob > 20u ? 0u : 100u - 5u * s->prob; }   /* percent */
static int st_extra(const step_t *s)           /* anything beyond trig and accent: the grid marks it */
{
    return s->prob || s->rat || st_cond(s) || s->lock[0] || s->lock[1] || s->lock[2] || s->lock[3];
}

typedef struct {
    step_t s[NSTEP];
    uint8_t len;                 /* 1..16, 0 = 16 */
    uint8_t div;                 /* DIV_NAME index, 0 = 1/1 */
    uint8_t flags;               /* bit 0 mute, bit 1 solo */
    uint8_t spare;
} tpat_t;
#define TP_MUTE 1u
#define TP_SOLO 2u
#define NDIV 6u
static const char *const DIV_NAME[NDIV] = {"1/1", "1/2", "1/4", "1/8", "2/1", "4/1"};

typedef struct {
    uint8_t ver;
    uint8_t swing;               /* 50..75 %, 0 = 50 */
    uint16_t tempo2;             /* BPM x 2 (80..480), 0 = 120 BPM */
    tpat_t t[NT];
} pattern_t;
#define PAT_VER 1u
_Static_assert(sizeof(pattern_t) <= 4076u, "a pattern fits one record");

static pattern_t pats[NPAT] __attribute__((section(".pool")));   /* all 16, as edited */
static pattern_t *volatile pat_cur = &pats[0];  /* the one being played and edited (the ISR moves it at a bar line) */
#define pat (*pat_cur)
static uint16_t pat_stored;      /* bit i: pattern i has a record in flash */
static uint32_t pat_index(void) { return (uint32_t)(pat_cur - pats); }

static uint32_t tp_len(uint32_t t) { uint32_t l = pat.t[t].len; return l && l <= NSTEP ? l : NSTEP; }
static uint32_t pat_bpm2(void) { uint32_t b = pat.tempo2; return b >= 80u && b <= 480u ? b : 240u; }
static uint32_t pat_swing(void) { uint32_t s = pat.swing; return s >= 50u && s <= 75u ? s : 50u; }

static void pat_check(pattern_t *q)   /* after a load: everything in range (an old or damaged record) */
{
    uint32_t t, s, i;
    q->ver = PAT_VER;
    q->tempo2 = (uint16_t)(q->tempo2 >= 80u && q->tempo2 <= 480u ? q->tempo2 : 240u);
    q->swing = (uint8_t)(q->swing >= 50u && q->swing <= 75u ? q->swing : 50u);
    for (t = 0; t < NT; t++) {
        tpat_t *p = &q->t[t];
        p->len = (uint8_t)(p->len && p->len <= NSTEP ? p->len : NSTEP);
        p->div = p->div < NDIV ? p->div : 0u;
        p->flags &= TP_MUTE | TP_SOLO;
        for (s = 0; s < NSTEP; s++) {
            step_t *st = &p->s[s];
            st->f &= 0x7Fu;
            if (((st->f >> 2) & 31u) >= NCOND)
                st->f &= 3u;
            st->prob = st->prob > 20u ? 0u : st->prob;
            if (((st->rat >> 2) & 3u) == 3u)
                st->rat &= 0xF3u;
            {
                int32_t n = st_nudge(st);
                st->rat = (uint8_t)((st->rat & 15u) | (((uint32_t)n & 15u) << 4));
            }
            for (i = 0; i < 4u; i++)
                st->lock[i] = st->lock[i] > 128u ? 0u : st->lock[i];
        }
    }
}

static void pat_clear(pattern_t *q)
{
    memset(q, 0, sizeof *q);
    pat_check(q);
}

/* ------------------------------------------------------------- the kit --- */
typedef struct {
    uint8_t p[4];                /* ENERGY, DECAY, OBJECTS, FREQ: 0..127 (64 = STK's own value for DECAY, OBJECTS, FREQ) */
    uint8_t level;               /* 0..100 */
    uint8_t pan;                 /* 0..100, 50 = centre */
    uint8_t dsend, rsend;        /* delay and reverb sends, 0..100 */
} ktrack_t;
enum { FX_DTIME, FX_DFB, FX_DTONE, FX_DLVL, FX_RSIZE, FX_RDAMP, FX_RLVL, FX_CHORUS, FX_FILT, FX_RES, FX_DRIVE,
       FX_MASTER, NFX };
typedef struct {
    uint8_t ver;
    uint8_t spare[3];
    ktrack_t t[NT];
    uint8_t fx[NFX];
} kit_t;
#define KIT_VER 1u
static kit_t kits[NKIT] __attribute__((section(".pool")));
static kit_t *volatile kit_cur = &kits[0];
#define kit (*kit_cur)
static uint8_t kit_stored;       /* bit i: kit i has a record */
static uint32_t kit_index(void) { return (uint32_t)(kit_cur - kits); }

static void kit_default(kit_t *q)
{
    uint32_t t;
    memset(q, 0, sizeof *q);
    q->ver = KIT_VER;
    for (t = 0; t < NT; t++) {
        ktrack_t *k = &q->t[t];
        k->p[SP_EN] = 100;
        k->p[SP_DEC] = k->p[SP_OBJ] = k->p[SP_FRQ] = 64;
        k->level = 80;
        k->pan = 50;
    }
    q->fx[FX_DTIME] = 3;         /* 1/8 (DLY_NAME) */
    q->fx[FX_DFB] = 35;
    q->fx[FX_DTONE] = 60;
    q->fx[FX_RSIZE] = 50;
    q->fx[FX_RDAMP] = 50;
    q->fx[FX_FILT] = 50;         /* centred: off */
    q->fx[FX_MASTER] = 80;
}

/* ------------------------------------------------------------ settings --- */
enum { CLK_INT, CLK_EXT, CLK_AUTO };
typedef struct {
    uint8_t ver;
    uint8_t midi_ch;             /* 0 = OMNI, 1..16 */
    uint8_t base_note;           /* 24..104: the note of track 1 (MIDI in and out) */
    uint8_t clock;               /* CLK_* */
    uint8_t clock_out, notes_out, trs_in;   /* 0 / 1 */
    uint8_t accent;              /* 0..100: what an accent adds to a hit's energy, % of full */
    uint8_t rep_rate;            /* REP_NAME index (note repeat) */
    uint8_t hum_time, hum_en;    /* 0..10 ms, 0..30 % */
    uint8_t bias;                /* the chance bias, 0..100 = -50..+50 % */
    uint8_t density;             /* RANDOMIZE density, 0..100 % */
    uint8_t last_pat, last_kit, last_song;
    uint8_t view;                /* HOME: 0 GRID, 1 TRACK */
    uint8_t sel;                 /* the selected track */
} settings_t;
#define SET_VER 1u
#define NREP 4u
static const char *const REP_NAME[NREP] = {"1/8", "1/16", "1/32", "1/16T"};
static settings_t set;

static void set_default(void)
{
    memset(&set, 0, sizeof set);
    set.ver = SET_VER;
    set.base_note = 53;          /* F3: the FM-1's own keys */
    set.clock = CLK_AUTO;
    set.trs_in = 1;
    set.accent = 40;
    set.rep_rate = 1;
    set.bias = 50;
    set.density = 25;
}

static void set_check(void)
{
    set.ver = SET_VER;
    set.midi_ch = set.midi_ch > 16u ? 0u : set.midi_ch;
    set.base_note = set.base_note < 24u || set.base_note > 104u ? 53u : set.base_note;
    set.clock = set.clock > CLK_AUTO ? CLK_AUTO : set.clock;
    set.clock_out = set.clock_out ? 1u : 0u;
    set.notes_out = set.notes_out ? 1u : 0u;
    set.trs_in = set.trs_in ? 1u : 0u;
    set.accent = set.accent > 100u ? 40u : set.accent;
    set.rep_rate = set.rep_rate >= NREP ? 1u : set.rep_rate;
    set.hum_time = set.hum_time > 10u ? 0u : set.hum_time;
    set.hum_en = set.hum_en > 30u ? 0u : set.hum_en;
    set.bias = set.bias > 100u ? 50u : set.bias;
    set.density = set.density > 100u ? 25u : set.density;
    set.last_pat = set.last_pat >= NPAT ? 0u : set.last_pat;
    set.last_kit = set.last_kit >= NKIT ? 0u : set.last_kit;
    set.last_song = set.last_song >= NSONG ? 0u : set.last_song;
    set.view = set.view > 1u ? 0u : set.view;
    set.sel = set.sel >= NT ? 0u : set.sel;
}

/* --------------------------------------------------------------- songs --- */
typedef struct {
    uint8_t n;                   /* entries used, 0..64 */
    uint8_t loop;
    uint8_t e[SONG_LEN][2];      /* pattern 0..15, repeats 1..16 */
} song_t;
typedef struct {
    uint8_t ver;
    uint8_t spare[3];
    song_t s[NSONG];
} songs_t;
#define SONG_VER 1u
static songs_t songs;
static uint8_t song_index;

static void songs_check(void)
{
    uint32_t i, k;
    songs.ver = SONG_VER;
    for (i = 0; i < NSONG; i++) {
        song_t *s = &songs.s[i];
        s->n = s->n > SONG_LEN ? 0u : s->n;
        s->loop = s->loop ? 1u : 0u;
        for (k = 0; k < SONG_LEN; k++) {
            s->e[k][0] = s->e[k][0] >= NPAT ? 0u : s->e[k][0];
            s->e[k][1] = s->e[k][1] < 1u || s->e[k][1] > 16u ? 1u : s->e[k][1];
        }
    }
}

/* ------------------------------------------------------------- records --- */
static int32_t load_into(uint32_t slot, void *dst, uint32_t size)   /* bytes read (a shorter old record: fewer), -1 */
{
    return store_load(slot, APP_ID, dst, size);
}

static int32_t data_save_settings(void) { return store_save(SLOT_SET, APP_ID, &set, sizeof set); }
static int32_t data_save_pattern(uint32_t i)
{
    int32_t r = store_save(SLOT_PAT0 + i, APP_ID, &pats[i], sizeof pats[i]);
    if (!r)
        pat_stored |= (uint16_t)(1u << i);
    return r;
}

static int32_t data_save_kit(uint32_t i)
{
    int32_t r = store_save(SLOT_KIT0 + i, APP_ID, &kits[i], sizeof kits[i]);
    if (!r)
        kit_stored |= (uint8_t)(1u << i);
    return r;
}

static int32_t data_save_songs(void) { return store_save(SLOT_SONG, APP_ID, &songs, sizeof songs); }

static void kit_check(kit_t *q)
{
    uint32_t t, i;
    q->ver = KIT_VER;
    for (t = 0; t < NT; t++) {
        ktrack_t *k = &q->t[t];
        for (i = 0; i < 4u; i++)
            k->p[i] = k->p[i] > 127u ? 64u : k->p[i];
        k->level = k->level > 100u ? 80u : k->level;
        k->pan = k->pan > 100u ? 50u : k->pan;
        k->dsend = k->dsend > 100u ? 0u : k->dsend;
        k->rsend = k->rsend > 100u ? 0u : k->rsend;
    }
    for (i = 0; i < NFX; i++)
        q->fx[i] = q->fx[i] > 100u ? 50u : q->fx[i];
    q->fx[FX_DTIME] = q->fx[FX_DTIME] > 7u ? 3u : q->fx[FX_DTIME];
}

static void data_load(void)      /* power-on: settings, the 16 patterns, the 4 kits, the songs */
{
    uint32_t i;
    set_default();
    if (load_into(SLOT_SET, &set, sizeof set) <= 0)
        set_default();
    set_check();
    pat_stored = 0;
    for (i = 0; i < NPAT; i++) {
        pat_clear(&pats[i]);
        if (load_into(SLOT_PAT0 + i, &pats[i], sizeof pats[i]) > 0)
            pat_stored |= (uint16_t)(1u << i);
        else
            pat_clear(&pats[i]);
        pat_check(&pats[i]);
    }
    kit_stored = 0;
    for (i = 0; i < NKIT; i++) {
        kit_default(&kits[i]);
        if (load_into(SLOT_KIT0 + i, &kits[i], sizeof kits[i]) > 0)
            kit_stored |= (uint8_t)(1u << i);
        else
            kit_default(&kits[i]);
        kit_check(&kits[i]);
    }
    pat_cur = &pats[set.last_pat];
    kit_cur = &kits[set.last_kit];
    memset(&songs, 0, sizeof songs);
    if (load_into(SLOT_SONG, &songs, sizeof songs) <= 0)
        memset(&songs, 0, sizeof songs);
    songs_check();
    song_index = set.last_song;
}

/* ------------------------------------------------------ page parameters --- */
enum { K_NUM, K_PCT, K_PAN, K_LIST, K_BPM2, K_OFF50, K_STEPS, K_CH, K_NOTE, K_ONOFF, K_MS, K_LOAD, K_PEAK };
#define K_READONLY(k) ((k) >= K_LOAD)     /* measured, shown on a page, not set by its knob */

/* the CPU meter (ui.c cpu_update): the audio ISR's load, % of its 2.9 ms half */
static uint8_t cpu_now;          /* the platform's average (audio_cpu_q8, ~93 ms) */
static uint8_t cpu_peak;         /* the worst single half in the last 2..4 s (audio_max_us) */
static uint32_t cpu_late;        /* halves that missed their deadline since power-on (felucca_dbg.late): glitches */
typedef struct {
    const char *name;            /* 0: an empty row */
    int16_t lo, hi;
    uint8_t kind;
    uint8_t live;                /* 1: it works; 0: shown with SOON (a later milestone) */
    const char *const *list;     /* K_LIST: the names */
} param_t;

enum { PG_TRACK, PG_TRACK2, PG_TIME, PG_RANDOM, PG_FX1, PG_FX2, PG_FX3, PG_GLO, PG_GLO2, NPAGES, PG_HOME = 0xF0 };
static const char *const PAGE_NAME[NPAGES] = {"TRACK", "TRACK 2", "TIME", "RANDOM", "FX 1 DELAY", "FX 2 REVERB",
                                              "FX 3 MASTER", "GLOBAL", "GLOBAL 2"};
static const char *const CLOCK_NAME[3] = {"INT", "EXT", "AUTO"};
static const char *const DLY_NAME[8] = {"1/32", "1/16", "1/16D", "1/8", "1/8D", "1/4", "1/4D", "1/2"};

static const param_t PP[NPAGES][4] = {
    {{"LEVEL", 0, 100, K_NUM, 1, 0}, {"PAN", 0, 100, K_PAN, 1, 0},
     {"DELAY SEND", 0, 100, K_PCT, 0, 0}, {"REVERB SEND", 0, 100, K_PCT, 0, 0}},
    {{"LENGTH", 1, 16, K_STEPS, 1, 0}, {"CLOCK DIVIDE", 0, NDIV - 1, K_LIST, 0, DIV_NAME},
     {"MUTE", 0, 1, K_ONOFF, 1, 0}, {"SOLO", 0, 1, K_ONOFF, 1, 0}},
    {{"TEMPO", 80, 480, K_BPM2, 1, 0}, {"SWING", 50, 75, K_PCT, 0, 0},
     {"ACCENT", 0, 100, K_PCT, 1, 0}, {"REPEAT RATE", 0, NREP - 1, K_LIST, 0, REP_NAME}},
    {{"HUMAN TIME", 0, 10, K_MS, 0, 0}, {"HUMAN ENERGY", 0, 30, K_PCT, 0, 0},
     {"CHANCE BIAS", 0, 100, K_OFF50, 1, 0}, {"DENSITY", 0, 100, K_PCT, 0, 0}},
    {{"TIME", 0, 7, K_LIST, 0, DLY_NAME}, {"FEEDBACK", 0, 100, K_PCT, 0, 0},
     {"TONE", 0, 100, K_PCT, 0, 0}, {"LEVEL", 0, 100, K_PCT, 0, 0}},
    {{"SIZE", 0, 100, K_PCT, 0, 0}, {"DAMPING", 0, 100, K_PCT, 0, 0},
     {"LEVEL", 0, 100, K_PCT, 0, 0}, {"CHORUS", 0, 100, K_PCT, 0, 0}},
    {{"FILTER", 0, 100, K_OFF50, 0, 0}, {"RESONANCE", 0, 100, K_PCT, 0, 0},
     {"DRIVE", 0, 100, K_PCT, 0, 0}, {"MASTER", 0, 100, K_NUM, 1, 0}},
    {{"MIDI CHANNEL", 0, 16, K_CH, 1, 0}, {"BASE NOTE", 24, 104, K_NOTE, 1, 0},
     {"CLOCK", 0, 2, K_LIST, 0, CLOCK_NAME}, {"TRS MIDI IN", 0, 1, K_ONOFF, 1, 0}},
    {{"CLOCK OUT", 0, 1, K_ONOFF, 0, 0}, {"NOTES OUT", 0, 1, K_ONOFF, 1, 0},
     {"CPU LOAD", 0, 100, K_LOAD, 1, 0}, {"CPU PEAK", 0, 100, K_PEAK, 1, 0}},
};

static uint8_t mute_tmp, solo_tmp;               /* TRACK 2's MUTE / SOLO rows edit the pattern's flags through these */

/* where a page row's value lives (the selected track's for TRACK pages); 0 for an empty row */
static void *pv_ptr(uint32_t page, uint32_t row, uint32_t t, int *wide)
{
    *wide = 0;
    switch (page) {
    case PG_TRACK: return row == 0 ? &kit.t[t].level : row == 1 ? &kit.t[t].pan : row == 2 ? &kit.t[t].dsend
                                                                                           : &kit.t[t].rsend;
    case PG_TRACK2:
        if (row == 0)
            return &pat.t[t].len;
        if (row == 1)
            return &pat.t[t].div;
        mute_tmp = (pat.t[t].flags & TP_MUTE) ? 1u : 0u;
        solo_tmp = (pat.t[t].flags & TP_SOLO) ? 1u : 0u;
        return row == 2 ? &mute_tmp : &solo_tmp;
    case PG_TIME:
        if (row == 0) {
            *wide = 1;
            return &pat.tempo2;
        }
        return row == 1 ? &pat.swing : row == 2 ? &set.accent : &set.rep_rate;
    case PG_RANDOM: return row == 0 ? &set.hum_time : row == 1 ? &set.hum_en : row == 2 ? &set.bias : &set.density;
    case PG_FX1: return &kit.fx[FX_DTIME + row];
    case PG_FX2: return &kit.fx[FX_RSIZE + row];
    case PG_FX3: return &kit.fx[FX_FILT + row];
    case PG_GLO: return row == 0 ? &set.midi_ch : row == 1 ? &set.base_note : row == 2 ? &set.clock : &set.trs_in;
    case PG_GLO2: return row == 0 ? &set.clock_out : row == 1 ? &set.notes_out : row == 2 ? &cpu_now : &cpu_peak;
    }
    return 0;
}

static int32_t pv_get(uint32_t page, uint32_t row, uint32_t t)
{
    int wide;
    void *p = pv_ptr(page, row, t, &wide);
    if (!p)
        return 0;
    if (page == PG_TRACK2 && row == 0)
        return (int32_t)tp_len(t);
    if (page == PG_TIME && row == 0)
        return (int32_t)pat_bpm2();
    if (page == PG_TIME && row == 1)
        return (int32_t)pat_swing();
    return wide ? *(uint16_t *)p : *(uint8_t *)p;
}

static void pv_set(uint32_t page, uint32_t row, uint32_t t, int32_t v)   /* clamped to the row's range */
{
    int wide;
    const param_t *q = &PP[page][row];
    void *p = pv_ptr(page, row, t, &wide);
    if (!p || !q->name || K_READONLY(q->kind))
        return;
    v = v < q->lo ? q->lo : v > q->hi ? q->hi : v;
    if (page == PG_TRACK2 && row >= 2) {
        uint8_t bit = row == 2 ? TP_MUTE : TP_SOLO;
        pat.t[t].flags = (uint8_t)(v ? pat.t[t].flags | bit : pat.t[t].flags & ~bit);
        return;
    }
    if (wide)
        *(uint16_t *)p = (uint16_t)v;
    else
        *(uint8_t *)p = (uint8_t)v;
}

static void cat(char *d, const char *s, unsigned max) { str_cpy(d + str_len(d), s, max - str_len(d)); }

static void note_name(char *b, unsigned max, uint32_t n)
{
    static const char *const NN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    char t[8];
    str_cpy(b, NN[n % 12u], max);
    fmt_int(t, (int32_t)(n / 12u) - 1);
    cat(b, t, max);
}

static void pv_text(char *b, unsigned max, const param_t *q, int32_t v)   /* a value as the page prints it */
{
    char t[12];
    b[0] = 0;
    switch (q->kind) {
    case K_PCT: fmt_int(t, v); str_cpy(b, t, max); cat(b, "%", max); break;
    case K_PAN:
        if (v == 50)
            str_cpy(b, "CENTRE", max);
        else {
            str_cpy(b, v < 50 ? "L" : "R", max);
            fmt_int(t, v < 50 ? 50 - v : v - 50);
            cat(b, t, max);
        }
        break;
    case K_LIST: str_cpy(b, q->list[v < q->lo ? q->lo : v > q->hi ? q->hi : v], max); break;
    case K_BPM2:
        fmt_int(t, v / 2);
        str_cpy(b, t, max);
        if (v & 1)
            cat(b, ".5", max);
        cat(b, " BPM", max);
        break;
    case K_OFF50: fmt_int(t, v - 50); str_cpy(b, v > 50 ? "+" : "", max); cat(b, t, max); cat(b, "%", max); break;
    case K_STEPS: fmt_int(t, v); str_cpy(b, t, max); cat(b, v == 1 ? " STEP" : " STEPS", max); break;
    case K_CH:
        if (!v)
            str_cpy(b, "OMNI", max);
        else
            fmt_int(b, v);
        break;
    case K_NOTE: note_name(b, max, (uint32_t)v); break;
    case K_ONOFF: str_cpy(b, v ? "ON" : "OFF", max); break;
    case K_MS: fmt_int(t, v); str_cpy(b, t, max); cat(b, " MS", max); break;
    case K_LOAD: fmt_int(t, v); str_cpy(b, t, max); cat(b, "%", max); break;
    case K_PEAK:
        fmt_int(t, v);
        str_cpy(b, t, max);
        cat(b, "%", max);
        if (cpu_late) {                          /* a glitch happened: say how many */
            cat(b, "  LATE ", max);
            fmt_int(t, (int32_t)(cpu_late > 999u ? 999u : cpu_late));
            cat(b, t, max);
        }
        break;
    default: fmt_int(b, v); break;
    }
}
