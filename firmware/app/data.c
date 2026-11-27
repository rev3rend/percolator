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
#include "factory.h"             /* build/gen: tools/gen_factory.py (the factory patterns, kits, songs) */

#define NT 23u                   /* tracks: STK Shakers types 0..22, played by keys 0..22 (F3..D#5) */
#define NSTEP 16u
#define NPAT 32u                 /* P01..P24 factory-filled (until saved over), P25..P32 blank */
#define NAME_LEN 12u             /* pattern and kit names: A-Z 0-9 space - . / # (NAME_CHARS), 0 ends a shorter one */
#define NKIT 12u                 /* K1..K8 factory, K9..K12 the defaults */
#define NSONG 8u                 /* S1..S4 factory tours, S5..S8 blank */
#define SONG_LEN 64u
#define SLOT_SET 0u
#define SLOT_PAT0 1u             /* 1..32: a pattern each */
#define SLOT_KITS 33u            /* all 12 kits in one record */
#define SLOT_SONGS 34u           /* all 8 songs in one record */
#define OLD_SLOT_KIT0 17u        /* before 2026-11-27: 4 kits in 17..20, 4 songs in 21 (migrated once at power-on) */
#define OLD_SLOT_SONG 21u
#define OLD_NKIT 4u
#define OLD_NSONG 4u
#define KEY_MUTE 23u             /* the performance keys above the pads (fm1_in.notes bits) */
#define KEY_SOLO 24u
#define KEY_ACCENT 25u
#define KEY_STEP_MUTE 20u        /* STEP mode, where the black keys play nothing: C#5 mutes, D#5 solos the selected track */
#define KEY_STEP_SOLO 22u
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
static int st_locked(const step_t *s) { return s->lock[0] || s->lock[1] || s->lock[2] || s->lock[3]; }
static int st_marks(const step_t *s) { return s->prob || s->rat || st_cond(s); }   /* the white dot (locks: colour) */

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
    uint8_t kitno;               /* (appended 2026-11-27) the kit saved with it, 1..12 (0: none, the kit stays) */
    uint8_t spare2[3];
    char name[NAME_LEN];         /* (appended 2026-11-27) its name (all 0: none) */
} pattern_t;
#define PAT_VER 1u
#define PAT_SIZE_V1 (4u + NT * sizeof(tpat_t))   /* a pattern record before the kit link (still read) */
_Static_assert(sizeof(pattern_t) <= 4076u, "a pattern fits one record");

static pattern_t pats[NPAT] __attribute__((section(".pool")));   /* all 32, as edited */
static pattern_t *volatile pat_cur = &pats[0];  /* the one being played and edited (the ISR moves it at a bar line) */
#define pat (*pat_cur)
static uint32_t pat_stored;      /* bit i: pattern i has a record in flash */
static uint32_t pat_index(void) { return (uint32_t)(pat_cur - pats); }
static uint32_t kit_index(void);
static void pat_switch(uint32_t i);              /* (below the kits: a pattern starting, with its kit) */

static uint32_t tp_len(uint32_t t) { uint32_t l = pat.t[t].len; return l && l <= NSTEP ? l : NSTEP; }
static uint32_t pat_bpm2(void) { uint32_t b = pat.tempo2; return b >= 80u && b <= 480u ? b : 240u; }
static uint32_t pat_swing(void) { uint32_t s = pat.swing; return s >= 50u && s <= 75u ? s : 50u; }

static const char NAME_CHARS[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-./#";   /* the name editor's set */

static void name_check(char *n)                  /* a stored name: its set only, ended by 0 (else cut there) */
{
    uint32_t i, k, end = 0;
    for (i = 0; i < NAME_LEN && !end; i++) {
        for (k = 0; NAME_CHARS[k] && NAME_CHARS[k] != n[i]; k++)
            ;
        if (!n[i] || !NAME_CHARS[k])
            end = 1;
    }
    for (i = end ? i - 1u : NAME_LEN; i < NAME_LEN; i++)
        n[i] = 0;
}

static void name_copy(char *d, const char *s)    /* a C string into a name field (at most NAME_LEN) */
{
    uint32_t i;
    for (i = 0; i < NAME_LEN; i++)
        d[i] = s && *s ? *s++ : 0;
}

static void pat_check(pattern_t *q)   /* after a load: everything in range (an old or damaged record) */
{
    uint32_t t, s, i;
    q->ver = PAT_VER;
    q->tempo2 = (uint16_t)(q->tempo2 >= 80u && q->tempo2 <= 480u ? q->tempo2 : 240u);
    q->kitno = q->kitno > NKIT ? 0u : q->kitno;
    name_check(q->name);
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
/* kit.fx[]: the order is stored (append only); the FX pages show them in the chain's order */
enum { FX_DTIME, FX_DFB, FX_DTONE, FX_DLVL, FX_RSIZE, FX_RDAMP, FX_RLVL, FX_CHORUS, FX_FILT, FX_RES, FX_DRIVE,
       FX_MASTER, FX_FPOS, FX_CRATE, NFX };      /* FX_FPOS: filter + drive 0 PRE / 1 POST; FX_CRATE: chorus rate */
typedef struct {
    uint8_t ver;
    uint8_t spare[3];
    ktrack_t t[NT];
    uint8_t fx[NFX];
    char name[NAME_LEN];         /* (appended 2026-11-27) its name (all 0: none) */
} kit_t;
#define KIT_VER 1u
static kit_t kits[NKIT] __attribute__((section(".pool")));
static kit_t *volatile kit_cur = &kits[0];
#define kit (*kit_cur)
static uint16_t kit_stored;      /* bit i: kit i has been saved (else factory or default) */
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
    q->fx[FX_CRATE] = 40;        /* ~0.5 Hz (FX_FPOS 0: PRE); every effect off: LEVELs, DEPTH, DRIVE 0 */
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
    uint8_t kit_follow;          /* (appended 2026-11-27) a pattern starting brings its kit: 1 on, 0 off */
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
    set.kit_follow = 1;
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
    set.kit_follow = set.kit_follow > 1u ? 1u : set.kit_follow;
}

/* pattern i starts playing (the ISR at a bar line, or at once when stopped; a song's entry): its kit with it when it
 * has one and KIT FOLLOWS PATTERN is on (a pointer each: the ISR reads them whole) */
static void pat_switch(uint32_t i)
{
    pat_cur = &pats[i % NPAT];
    if (set.kit_follow && pat_cur->kitno >= 1u && pat_cur->kitno <= NKIT)
        kit_cur = &kits[pat_cur->kitno - 1u];
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
        pat_stored |= 1u << i;
    return r;
}

/* the kits' record: each kit in its own fixed stride (so a kit_t that grows later keeps every kit in its place) and a
 * mask of the kits saved; kept in RAM as stored, so saving one kit never stores another's unsaved edits */
#define KIT_STRIDE 256u
typedef struct {
    uint8_t ver;
    uint8_t n;                   /* kits in the record */
    uint16_t saved;              /* bit i: kit i holds a saved kit */
    uint8_t k[NKIT][KIT_STRIDE];
} kits_rec_t;
_Static_assert(sizeof(kit_t) <= KIT_STRIDE, "a kit fits its stride");
_Static_assert(sizeof(kits_rec_t) <= 4076u, "the kits fit one record");
static kits_rec_t kits_rec __attribute__((section(".pool")));

static int32_t kits_write(void) { return store_save(SLOT_KITS, APP_ID, &kits_rec, sizeof kits_rec); }

static int32_t data_save_kit(uint32_t i)
{
    int32_t r;
    memset(kits_rec.k[i], 0, KIT_STRIDE);
    memcpy(kits_rec.k[i], &kits[i], sizeof kits[i]);
    kits_rec.ver = 1;
    kits_rec.n = NKIT;
    kits_rec.saved |= (uint16_t)(1u << i);
    r = kits_write();
    if (!r)
        kit_stored |= (uint16_t)(1u << i);
    return r;
}

static int32_t data_save_songs(void) { return store_save(SLOT_SONGS, APP_ID, &songs, sizeof songs); }

/* ---------------------------------------------------------------- factory --- */
static void factory_pattern(uint32_t i, pattern_t *q)   /* P(i + 1) as shipped (build/gen/factory.h) */
{
    uint32_t t, k;
    memset(q, 0, sizeof *q);
    if (i >= FP_N)
        return;
    q->tempo2 = FP_HDR[i][0];
    q->swing = (uint8_t)FP_HDR[i][1];
    q->kitno = (uint8_t)FP_HDR[i][2];
    name_copy(q->name, FP_NAME[i]);
    for (t = 0; t < NT; t++) {
        q->t[t].len = FP_TRK[i][t][0];
        q->t[t].div = FP_TRK[i][t][1];
    }
    for (k = FP_IDX[i]; k < FP_IDX[i + 1u]; k++) {
        const uint8_t *e = FP_STEP[k];
        step_t *st = &q->t[e[0] % NT].s[e[1] % NSTEP];
        st->f = e[2];
        st->prob = e[3];
        st->rat = e[4];
        st->lock[0] = e[5];
        st->lock[1] = e[6];
        st->lock[2] = e[7];
        st->lock[3] = e[8];
    }
}

static void factory_kit(uint32_t i, kit_t *q)   /* K(i + 1) as shipped; K9..K12: the defaults */
{
    uint32_t t, f;
    kit_default(q);
    if (i >= FK_N)
        return;
    for (t = 0; t < NT; t++) {
        const uint8_t *e = FK_TRK[i][t];
        q->t[t].p[0] = e[0];
        q->t[t].p[1] = e[1];
        q->t[t].p[2] = e[2];
        q->t[t].p[3] = e[3];
        q->t[t].level = e[4];
        q->t[t].pan = e[5];
    }
    for (f = 0; f < NFX; f++)
        q->fx[f] = FK_FX[i][f];
    name_copy(q->name, FK_NAME[i]);
}

static void factory_songs(void)                  /* S1..S4 the tours, S5..S8 blank */
{
    uint32_t i, e;
    memset(&songs, 0, sizeof songs);
    for (i = 0; i < FS_N && i < NSONG; i++) {
        song_t *sg = &songs.s[i];
        sg->loop = FS_LOOP[i];
        for (e = 0; e < 8u && FS_ENT[i][e][0]; e++) {
            sg->e[e][0] = (uint8_t)(FS_ENT[i][e][0] - 1u);
            sg->e[e][1] = FS_ENT[i][e][1];
            sg->n++;
        }
    }
}

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
    q->fx[FX_FPOS] = q->fx[FX_FPOS] > 1u ? 0u : q->fx[FX_FPOS];
    name_check(q->name);
}

static void data_load(void)      /* power-on: settings, the 32 patterns, the 12 kits, the 8 songs */
{
    static kit_t tmp;
    uint32_t i;
    int32_t n;
    set_default();
    if (load_into(SLOT_SET, &set, sizeof set) <= 0)
        set_default();
    set_check();
    pat_stored = 0;                              /* patterns: saved, else factory (P01..P16) or blank */
    for (i = 0; i < NPAT; i++) {
        memset(&pats[i], 0, sizeof pats[i]);
        n = load_into(SLOT_PAT0 + i, &pats[i], sizeof pats[i]);
        if (n >= (int32_t)PAT_SIZE_V1)           /* (a shorter record in 17..21 is the old layout's kit or songs) */
            pat_stored |= 1u << i;
        else
            factory_pattern(i, &pats[i]);
        pat_check(&pats[i]);
    }
    kit_stored = 0;                              /* kits: saved, else factory (K1..K8) or the defaults */
    memset(&kits_rec, 0, sizeof kits_rec);
    if (load_into(SLOT_KITS, &kits_rec, sizeof kits_rec) <= 0) {
        memset(&kits_rec, 0, sizeof kits_rec);
        for (i = 0; i < OLD_NKIT; i++) {         /* the old layout's 4 kits, once: into K1..K4, written at once */
            memset(&tmp, 0, sizeof tmp);
            n = load_into(OLD_SLOT_KIT0 + i, &tmp, sizeof tmp);
            if (n >= (int32_t)(sizeof tmp - 2u * (NFX - FX_FPOS)) && n <= (int32_t)sizeof tmp && tmp.ver == KIT_VER &&
                !((pat_stored >> (OLD_SLOT_KIT0 - SLOT_PAT0 + i)) & 1u)) {
                memcpy(kits_rec.k[i], &tmp, sizeof tmp);
                kits_rec.saved |= (uint16_t)(1u << i);
            }
        }
        if (kits_rec.saved) {
            kits_rec.ver = 1;
            kits_rec.n = NKIT;
            kits_write();
        }
    }
    for (i = 0; i < NKIT; i++) {
        factory_kit(i, &kits[i]);
        if ((kits_rec.saved >> i) & 1u) {
            memcpy(&kits[i], kits_rec.k[i], sizeof kits[i]);
            if (kits[i].fx[FX_CRATE] == 0 && kits[i].fx[FX_FPOS] == 0)
                kits[i].fx[FX_CRATE] = 40;       /* (a kit saved before the chorus rate existed) */
            kit_stored |= (uint16_t)(1u << i);
        }
        kit_check(&kits[i]);
    }
    memset(&songs, 0, sizeof songs);             /* songs: saved, else the old layout's (once), else factory */
    if (load_into(SLOT_SONGS, &songs, sizeof songs) <= 0) {
        memset(&songs, 0, sizeof songs);
        n = load_into(OLD_SLOT_SONG, &songs, sizeof songs);
        if (n == (int32_t)(4u + OLD_NSONG * sizeof(song_t)) &&
            !((pat_stored >> (OLD_SLOT_SONG - SLOT_PAT0)) & 1u)) {
            songs_check();
            data_save_songs();
        } else
            factory_songs();
    }
    songs_check();
    pat_cur = &pats[set.last_pat];
    kit_cur = &kits[set.last_kit];
    song_index = set.last_song;
}

static void data_erase_slot(uint32_t slot)       /* both copies of a record (FACTORY RESTORE) */
{
    st_erase(store_sector(slot, 0));
    st_erase(store_sector(slot, 1));
}

/* ------------------------------------------------------ page parameters --- */
enum { K_NUM, K_PCT, K_PAN, K_LIST, K_BPM2, K_OFF50, K_STEPS, K_CH, K_NOTE, K_ONOFF, K_MS, K_FILT, K_LOAD, K_PEAK,
       K_ACT };                                  /* K_ACT: an action, done with SAVE (its row shows how) */
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

enum { PG_SOUND, PG_TRACK, PG_TRACK2, PG_TIME, PG_RANDOM, PG_FX1, PG_FX2, PG_FX3, PG_FX4, PG_GLO, PG_GLO2,
       PG_GLO3, NPAGES, PG_HOME = 0xF0 };
static const char *const PAGE_NAME[NPAGES] = {"SOUND", "TRACK", "TRACK 2", "TIME", "RANDOM", "FX 1 FILTER",
                                              "FX 2 CHORUS", "FX 3 DELAY", "FX 4 REVERB", "GLOBAL", "GLOBAL 2",
                                              "GLOBAL 3"};
static const char *const FPOS_NAME[2] = {"PRE", "POST"};
static const char *const CLOCK_NAME[3] = {"INT", "EXT", "AUTO"};
static const char *const DLY_NAME[8] = {"1/32", "1/16", "1/16D", "1/8", "1/8D", "1/4", "1/4D", "1/2"};

static const param_t PP[NPAGES][4] = {
    {{"ENERGY", 0, 127, K_NUM, 1, 0}, {"DECAY", 0, 127, K_NUM, 1, 0},     /* the selected track's four (HOME's knobs) */
     {"OBJECTS", 0, 127, K_NUM, 1, 0}, {"FREQ", 0, 127, K_NUM, 1, 0}},
    {{"LEVEL", 0, 100, K_NUM, 1, 0}, {"PAN", 0, 100, K_PAN, 1, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}},
    {{"LENGTH", 1, 16, K_STEPS, 1, 0}, {"CLOCK DIVIDE", 0, NDIV - 1, K_LIST, 1, DIV_NAME},
     {"MUTE", 0, 1, K_ONOFF, 1, 0}, {"SOLO", 0, 1, K_ONOFF, 1, 0}},
    {{"TEMPO", 80, 480, K_BPM2, 1, 0}, {"SWING", 50, 75, K_PCT, 1, 0},
     {"ACCENT", 0, 100, K_PCT, 1, 0}, {"REPEAT RATE", 0, NREP - 1, K_LIST, 1, REP_NAME}},
    {{"HUMAN TIME", 0, 10, K_MS, 0, 0}, {"HUMAN ENERGY", 0, 30, K_PCT, 0, 0},
     {"CHANCE BIAS", 0, 100, K_OFF50, 1, 0}, {"DENSITY", 0, 100, K_PCT, 0, 0}},
    {{"FILTER", 0, 100, K_FILT, 1, 0}, {"RESONANCE", 0, 100, K_PCT, 1, 0},     /* the chain's order */
     {"DRIVE", 0, 100, K_PCT, 1, 0}, {"POSITION", 0, 1, K_LIST, 1, FPOS_NAME}},
    {{"DEPTH", 0, 100, K_PCT, 1, 0}, {"RATE", 0, 100, K_NUM, 1, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}},
    {{"TIME", 0, 7, K_LIST, 1, DLY_NAME}, {"FEEDBACK", 0, 95, K_PCT, 1, 0},
     {"TONE", 0, 100, K_PCT, 1, 0}, {"LEVEL", 0, 100, K_PCT, 1, 0}},
    {{"SIZE", 0, 100, K_PCT, 1, 0}, {"DAMPING", 0, 100, K_PCT, 1, 0},
     {"LEVEL", 0, 100, K_PCT, 1, 0}, {"MASTER", 0, 100, K_NUM, 1, 0}},
    {{"MIDI CHANNEL", 0, 16, K_CH, 1, 0}, {"BASE NOTE", 24, 104, K_NOTE, 1, 0},
     {"CLOCK", 0, 2, K_LIST, 1, CLOCK_NAME}, {"TRS MIDI IN", 0, 1, K_ONOFF, 1, 0}},
    {{"CLOCK OUT", 0, 1, K_ONOFF, 1, 0}, {"NOTES OUT", 0, 1, K_ONOFF, 1, 0},
     {"CPU LOAD", 0, 100, K_LOAD, 1, 0}, {"CPU PEAK", 0, 100, K_PEAK, 1, 0}},
    {{"KIT FOLLOWS PATTERN", 0, 1, K_ONOFF, 1, 0}, {"FACTORY RESTORE", 0, 0, K_ACT, 1, 0},
     {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}},
};

static uint8_t mute_tmp, solo_tmp;               /* TRACK 2's MUTE / SOLO rows edit the pattern's flags through these */

/* where a page row's value lives (the selected track's for TRACK pages); 0 for an empty row */
static void *pv_ptr(uint32_t page, uint32_t row, uint32_t t, int *wide)
{
    *wide = 0;
    switch (page) {
    case PG_SOUND: return &kit.t[t].p[row];      /* ENERGY DECAY OBJECTS FREQ = SP_* */
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
    case PG_FX1: return &kit.fx[row == 0 ? FX_FILT : row == 1 ? FX_RES : row == 2 ? FX_DRIVE : FX_FPOS];
    case PG_FX2: return row == 0 ? &kit.fx[FX_CHORUS] : row == 1 ? &kit.fx[FX_CRATE] : 0;
    case PG_FX3: return &kit.fx[FX_DTIME + row];
    case PG_FX4: return &kit.fx[row == 0 ? FX_RSIZE : row == 1 ? FX_RDAMP : row == 2 ? FX_RLVL : FX_MASTER];
    case PG_GLO: return row == 0 ? &set.midi_ch : row == 1 ? &set.base_note : row == 2 ? &set.clock : &set.trs_in;
    case PG_GLO2: return row == 0 ? &set.clock_out : row == 1 ? &set.notes_out : row == 2 ? &cpu_now : &cpu_peak;
    case PG_GLO3: {
        static uint8_t none;
        return row == 0 ? &set.kit_follow : row == 1 ? &none : 0;
    }
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
            str_cpy(b, "CENTER", max);
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
    case K_FILT:                                 /* one knob: LP below the middle, HP above, OFF in it */
        if (v == 50)
            str_cpy(b, "OFF", max);
        else {
            str_cpy(b, v < 50 ? "LOW-PASS " : "HIGH-PASS ", max);
            fmt_int(t, v < 50 ? 50 - v : v - 50);
            cat(b, t, max);
        }
        break;
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
    case K_ACT: str_cpy(b, "SAVE", max); break;
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
