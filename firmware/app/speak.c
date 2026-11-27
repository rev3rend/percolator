/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 R. Luke DuBois */
/* What the screen reader says (main loop, after ui_frame, only while a reader listens: sr.c). Each frame compares
 * what the screen shows with the last frame and says what changed, in full words (fm1-reader's RULES.md: no
 * abbreviations or symbols, the important thing first, short):
 *   HOME, GRID      the pattern, the kit, the tempo, the transport, the selected track and its four sounds
 *   HOME, TRACK     the selected track, its length and the steps that are on
 *   a track         its name (and its state: muted, solo), when SELECT or EDIT + a key picks it
 *   a knob          its name and value, when it settles ("decay 80")
 *   a step          STEP mode, a step key held: the step's settings; each edit as it happens; a step key tapped:
 *                   "step 5 on"
 *   a pattern       when it starts while playing (a queue, a song): "pattern 7, rain"
 *   the transport   "playing", "stopped", "following MIDI clock"
 *   a page          its name and its values ("effects 3, delay page. time eighth note, ...")
 *   SAVE            what, the slot, what it replaces, the name and how to edit it; each edit as it happens
 *   SONG            the song and its entry under the cursor; each edit
 *   a message       the line at the bottom ("next pattern 7, rain", "sticks muted"); a failure as a warning (KEEP)
 *   WHERE           the whole screen
 * The words come from the screen's own texts through spk(), which spells out what the small screen abbreviates. */

/* ------------------------------------------------------------ the screen's words, spoken --- */
static const char *const SPK_TOK[][2] = {
    {"BPM", "beats per minute"}, {"MS", "milliseconds"},     {"MIDI", "MIDI"},       {"CPU", "C P U"},
    {"TRS", "T R S"},            {"FX", "effects"},          {"INT", "internal"},    {"EXT", "external"},
    {"EXT:", "external,"},       {"FREQ", "frequency"},      {"LOW-PASS", "low pass"}, {"HIGH-PASS", "high pass"},
    {"EDIT+SAVE", "edit and save"}, {"TAMB", "tambourine"},  {"PROB", "probability"}, {"OMNI", "omni"},
    {"SOLO", "solo"},            {"PRE", "pre"},             {"POST", "post"},       {"#", "number"},
};
#define NSPK (sizeof SPK_TOK / sizeof SPK_TOK[0])

static int spk_digits(const char *t)             /* t is all digits (and at least one) */
{
    if (!*t)
        return 0;
    for (; *t; t++)
        if (*t < '0' || *t > '9')
            return 0;
    return 1;
}

static void spk_num(char *d, unsigned max, const char *num)   /* "07" -> "7" */
{
    while (num[0] == '0' && num[1])
        num++;
    cat(d, num, max);
}

static void spk_track(char *d, unsigned max, uint32_t t)   /* "Penny + Mug" -> "penny and mug" */
{
    const char *s = SK_NAME[t % NT];
    char w[32];
    uint32_t i, k = 0;
    for (i = 0; s[i] && k + 4u < sizeof w; i++) {
        char c = s[i];
        if (c == '+') {
            w[k++] = 'a';
            w[k++] = 'n';
            w[k++] = 'd';
            continue;
        }
        w[k++] = c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
    }
    w[k] = 0;
    cat(d, w, max);
}

static void spk_note_value(char *d, unsigned max, const char *t)   /* "1/16T" -> "sixteenth note triplets" */
{
    static const char *const V[6][2] = {{"1", "whole note"}, {"2", "half note"}, {"4", "quarter note"},
                                        {"8", "eighth note"}, {"16", "sixteenth note"}, {"32", "thirty second note"}};
    char n[4];
    uint32_t i, k = 0;
    if (t[0] != '1' || t[1] != '/') {
        cat(d, t, max);
        return;
    }
    for (t += 2; *t >= '0' && *t <= '9' && k < 3u; t++)
        n[k++] = *t;
    n[k] = 0;
    for (i = 0; i < 6u; i++)
        if (str_eq(n, V[i][0])) {
            if (*t == 'D')
                cat(d, "dotted ", max);
            cat(d, V[i][1], max);
            cat(d, *t == 'T' ? " triplets" : "s", max);
            return;
        }
    cat(d, n, max);
}

/* one token of screen text, spoken */
static void spk_tok(char *d, unsigned max, const char *t)
{
    char w[48];
    uint32_t i, n = str_len(t);
    if (!n)
        return;
    for (i = 0; i < NSPK; i++)
        if (str_eq(t, SPK_TOK[i][0])) {
            cat(d, SPK_TOK[i][1], max);
            return;
        }
    for (i = 0; i < NT; i++)                     /* MAR STX ...: the track's name */
        if (str_eq(t, SK_SHORT[i])) {
            spk_track(d, max, i);
            return;
        }
    if ((t[0] == 'P' || t[0] == 'K' || t[0] == 'S') && n >= 2u && n <= 3u && spk_digits(t + 1)) {   /* P07 K3 S2 */
        cat(d, t[0] == 'P' ? "pattern " : t[0] == 'K' ? "kit " : "song ", max);
        spk_num(d, max, t + 1);
        return;
    }
    if (n >= 3u && t[n - 1u] == ':' ) {          /* "CLEARED:" -> "cleared," */
        str_cpy(w, t, sizeof w);
        w[n - 1u] = 0;
        spk_tok(d, max, w);
        cat(d, ",", max);
        return;
    }
    if (n >= 3u && t[n - 1u] == ',') {
        str_cpy(w, t, sizeof w);
        w[n - 1u] = 0;
        spk_tok(d, max, w);
        cat(d, ",", max);
        return;
    }
    if (t[n - 1u] == '%' && n > 1u) {            /* 100% -> 100 percent */
        str_cpy(w, t, sizeof w);
        w[n - 1u] = 0;
        spk_tok(d, max, w);
        cat(d, " percent", max);
        return;
    }
    if ((t[0] == '+' || t[0] == '-') && n > 1u && t[1] >= '0' && t[1] <= '9') {   /* +12 -2: plus / minus */
        cat(d, t[0] == '+' ? "plus " : "minus ", max);
        spk_tok(d, max, t + 1);
        return;
    }
    if (n == 3u && t[0] >= '1' && t[0] <= '9' && t[1] == ':' && t[2] >= '1' && t[2] <= '9') {   /* 1:2 */
        char c[2] = {t[0], 0};
        cat(d, c, max);
        cat(d, " of ", max);
        c[0] = t[2];
        cat(d, c, max);
        return;
    }
    if (t[0] == '1' && t[1] == '/') {            /* note values */
        spk_note_value(d, max, t);
        return;
    }
    for (i = 0; t[i] && i + 1u < sizeof w; i++) {   /* as written, small letters, no symbols */
        char c = t[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == ',' || c == '\'' ||
                   c == '-'))
            c = ' ';
        w[i] = c;
    }
    w[i] = 0;
    cat(d, w, max);
}

/* screen text -> spoken text, appended to d ("KIT", "PATTERN", "SONG" before a label: the label's own word) */
static void spk(char *d, unsigned max, const char *s)
{
    char t[48];
    uint32_t k;
    while (*s) {
        while (*s == ' ')
            s++;
        for (k = 0; *s && *s != ' ' && k + 1u < sizeof t; s++)
            t[k++] = *s;
        t[k] = 0;
        if (!k)
            continue;
        if (s[0] == ' ' && s[1] && s[2] >= '0' && s[2] <= '9' &&   /* (in this order: never past the end) */
            ((str_eq(t, "KIT") && s[1] == 'K') || (str_eq(t, "PATTERN") && s[1] == 'P') ||
             (str_eq(t, "SONG") && s[1] == 'S')))
            continue;                            /* "KIT K7 CLOCKWORK" -> "kit 7 clockwork" (the label says it) */
        if (d[0] && d[str_len(d) - 1u] != ' ')
            cat(d, " ", max);
        spk_tok(d, max, t);
    }
}

static void spk_name(char *d, unsigned max, const char *field)   /* a pattern's or kit's name, if it has one */
{
    char n[NAME_LEN + 1];
    name_str(n, field);
    if (n[0]) {
        cat(d, ", ", max);
        spk(d, max, n);
    }
}

/* ------------------------------------------------------------ values --- */
#define SPK_LEN 400u             /* a whole readout (sr_say_long cuts it into SAYs) */

static void spk_value(char *d, unsigned max, const param_t *p, int32_t v)   /* a page value, in words */
{
    static const char *const DIV_WORD[NDIV] = {"normal speed", "half speed", "quarter speed", "eighth speed",
                                               "double speed", "4 times speed"};
    char b[24], t[12];
    switch (p->kind) {
    case K_PAN:
        if (v == 50)
            cat(d, "center", max);
        else {
            cat(d, v < 50 ? "left " : "right ", max);
            fmt_int(t, v < 50 ? 50 - v : v - 50);
            cat(d, t, max);
        }
        return;
    case K_BPM2:
        fmt_int(t, v / 2);
        cat(d, t, max);
        cat(d, v & 1 ? " and a half beats per minute" : " beats per minute", max);
        return;
    case K_STEPS:
        fmt_int(t, v);
        cat(d, t, max);
        cat(d, v == 1 ? " step" : " steps", max);
        return;
    case K_NOTE: {                               /* "C#3" -> "C sharp 3", "C-1" -> "C minus 1" */
        static const char *const NN[12] = {"C", "C sharp", "D", "D sharp", "E", "F", "F sharp", "G", "G sharp", "A",
                                           "A sharp", "B"};
        int32_t o = v / 12 - 1;
        cat(d, NN[v % 12], max);
        cat(d, o < 0 ? " minus " : " ", max);
        fmt_int(t, o < 0 ? -o : o);
        cat(d, t, max);
        return;
    }
    case K_ACT: cat(d, "press save", max); return;
    case K_LIST:
        if (p->list == DIV_NAME) {
            cat(d, DIV_WORD[v < 0 ? 0 : v >= (int32_t)NDIV ? NDIV - 1 : v], max);
            return;
        }
        break;
    default: break;
    }
    pv_text(b, sizeof b, p, v);
    spk(d, max, b);
}

static void spk_param(char *d, unsigned max, const param_t *p, int32_t v)   /* "decay 80" */
{
    spk(d, max, p->name);
    cat(d, " ", max);
    spk_value(d, max, p, v);
}

/* ------------------------------------------------------------ what each screen says --- */
static void spk_track_state(char *d, unsigned max, uint32_t t)
{
    static const char *const W[4] = {"", ", solo", ", muted", ", silent, other tracks are soloed"};
    cat(d, W[track_state(t)], max);
}

static void spk_track_line(char *d, unsigned max, uint32_t t)   /* "track 9, sticks, muted" */
{
    char n[8];
    cat(d, "track ", max);
    fmt_int(n, (int32_t)t + 1);
    cat(d, n, max);
    cat(d, ", ", max);
    spk_track(d, max, t);
    spk_track_state(d, max, t);
}

static void spk_sounds(char *d, unsigned max, uint32_t t)   /* "energy 100, decay 64, objects 64, frequency 64" */
{
    uint32_t i;
    for (i = 0; i < 4u; i++) {
        if (i)
            cat(d, ", ", max);
        spk_param(d, max, &PP[PG_SOUND][i], kit.t[t].p[i]);
    }
}

static void spk_pattern(char *d, unsigned max, uint32_t i)   /* "pattern 7, rain" */
{
    char n[8];
    cat(d, "pattern ", max);
    fmt_int(n, (int32_t)(i % NPAT) + 1);
    cat(d, n, max);
    spk_name(d, max, pats[i % NPAT].name);
}

static void spk_kit(char *d, unsigned max, uint32_t i)   /* "kit 3, rain room" */
{
    char n[8];
    cat(d, "kit ", max);
    fmt_int(n, (int32_t)(i % NKIT) + 1);
    cat(d, n, max);
    spk_name(d, max, kits[i % NKIT].name);
}

static void spk_transport(char *d, unsigned max)  /* "76 beats per minute, stopped" */
{
    if (snd.ext_pub)
        cat(d, "following MIDI clock", max);
    else
        spk_value(d, max, &PP[PG_TIME][0], (int32_t)pat_bpm2());
    cat(d, snd.running_pub ? ", playing" : ", stopped", max);
    if (snd.song_on_pub) {
        char n[4];
        cat(d, ", song ", max);
        fmt_int(n, (int32_t)song_n_shown() + 1);
        cat(d, n, max);
    }
    if (ui.mode)
        cat(d, ", step mode", max);
    if (ui.rec)
        cat(d, ", recording", max);
}

static void spk_steps_on(char *d, unsigned max, uint32_t t)   /* "steps on: 1 accent, 5, 9 locked" */
{
    uint32_t s, n = 0, len = tp_len(t);
    char b[8];
    for (s = 0; s < len; s++) {
        const step_t *st = &pat.t[t].s[s];
        if (!(st->f & ST_TRIG))
            continue;
        cat(d, n++ ? ", " : "steps on: ", max);
        fmt_int(b, (int32_t)s + 1);
        cat(d, b, max);
        if (st->f & ST_ACC)
            cat(d, " accent", max);
        if (st_locked(st))
            cat(d, " locked", max);
    }
    if (!n)
        cat(d, "no steps on", max);
}

static void spk_home(char *d, unsigned max)
{
    uint32_t t = sel();
    char b[16];
    d[0] = 0;
    if (set.view == 0u) {                        /* GRID */
        cat(d, "grid. ", max);
        spk_pattern(d, max, pat_index());
        cat(d, ". ", max);
        spk_kit(d, max, kit_index());
        cat(d, ". ", max);
        spk_transport(d, max);
        cat(d, ". ", max);
        spk_track_line(d, max, t);
        cat(d, ". ", max);
        spk_sounds(d, max, t);
        cat(d, ".", max);
    } else {                                     /* TRACK */
        cat(d, "track view. ", max);
        spk_track_line(d, max, t);
        cat(d, ". length ", max);
        fmt_int(b, (int32_t)tp_len(t));
        cat(d, b, max);
        cat(d, " steps. ", max);
        spk_steps_on(d, max, t);
        cat(d, ". ", max);
        spk_pattern(d, max, pat_index());
        cat(d, ", ", max);
        spk_transport(d, max);
        cat(d, ".", max);
    }
}

static void spk_page(char *d, unsigned max)       /* "effects 3, delay page. time eighth notes, ..." */
{
    static const char *const NAME[NPAGES] = {"sound", "track", "track 2", "time", "random", "effects 1, filter",
                                             "effects 2, chorus", "effects 3, delay", "effects 4, reverb", "global",
                                             "global 2", "global 3"};
    uint32_t r, n = 0;
    str_cpy(d, NAME[ui.page], max);
    cat(d, " page", max);
    if (ui.page == PG_SOUND || ui.page == PG_TRACK || ui.page == PG_TRACK2) {
        cat(d, ", ", max);
        spk_track(d, max, sel());
    }
    cat(d, ". ", max);
    for (r = 0; r < 4u; r++) {
        const param_t *p = &PP[ui.page][r];
        if (!p->name)
            continue;
        if (n++)
            cat(d, ", ", max);
        spk_param(d, max, p, pv_get(ui.page, r, sel()));
    }
    cat(d, ".", max);
}

static void spk_name_char(char *d, unsigned max)  /* the SAVE editor at its cursor: "position 3, letter c" */
{
    char t[4], c = ui.name[ui.name_cur];
    fmt_int(t, ui.name_cur + 1);
    cat(d, "position ", max);
    cat(d, t, max);
    cat(d, !c || c == ' ' ? ", space" : c >= '0' && c <= '9' ? ", digit " : c == '-' ? ", dash" : c == '.' ? ", dot"
           : c == '/' ? ", slash" : c == '#' ? ", number sign" : ", letter ", max);
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        t[0] = c;
        t[1] = 0;
        cat(d, t, max);
    }
}

static void spk_save_name(char *d, unsigned max)  /* "name rain" or "name empty" */
{
    char n[NAME_LEN + 1];
    uint32_t k;
    str_cpy(n, ui.name, sizeof n);
    for (k = str_len(n); k && n[k - 1u] == ' ';)
        n[--k] = 0;
    cat(d, "name ", max);
    if (k)
        spk(d, max, n);
    else
        cat(d, "empty", max);
}

static void spk_save_slot(char *d, unsigned max)  /* "pattern 26, empty slot" */
{
    char b[32];
    label2(b, sizeof b, ui.save_what == SV_PAT ? 'P' : ui.save_what == SV_KIT ? 'K' : 'S', ui.save_slot + 1u);
    spk(d, max, b);
    cat(d, ", ", max);
    spk(d, max, slot_state(ui.save_what, ui.save_slot));
    if (ui.save_what != SV_SONG) {
        cat(d, ". ", max);
        spk_save_name(d, max);
    }
}

static void spk_save(char *d, unsigned max)
{
    str_cpy(d, "save ", max);
    spk(d, max, SV_NAME[ui.save_what]);
    cat(d, " to ", max);
    spk_save_slot(d, max);
    cat(d, ". ", max);
    if (ui.save_what != SV_SONG)
        cat(d, "knob 1 moves the cursor, knob 2 changes the letter. ", max);
    cat(d, "select picks pattern, kit or song, presets picks the slot. press save to store, any other button "
           "cancels.", max);
}

static void spk_song_entry(char *d, unsigned max)   /* "entry 3, pattern 3, african bell, 2 times" */
{
    const song_t *sg = &songs.s[song_index];
    char b[8];
    uint32_t c = ui.song_cur;
    if (c >= sg->n) {
        cat(d, "end, presets adds the pattern playing", max);
        return;
    }
    cat(d, "entry ", max);
    fmt_int(b, (int32_t)c + 1);
    cat(d, b, max);
    cat(d, ", ", max);
    spk_pattern(d, max, sg->e[c][0]);
    cat(d, ", ", max);
    fmt_int(b, sg->e[c][1]);
    cat(d, b, max);
    cat(d, sg->e[c][1] == 1u ? " time" : " times", max);
}

static void spk_song_head(char *d, unsigned max)   /* "song 1, loop, 6 entries" */
{
    const song_t *sg = &songs.s[song_index];
    char b[8];
    cat(d, "song ", max);
    fmt_int(b, (int32_t)song_index + 1);
    cat(d, b, max);
    cat(d, sg->loop ? ", loop, " : ", once, ", max);
    fmt_int(b, (int32_t)sg->n);
    cat(d, b, max);
    cat(d, sg->n == 1u ? " entry" : " entries", max);
    if (snd.song_on_pub && song_n_shown() == song_index)
        cat(d, ", playing", max);
}

static void spk_song(char *d, unsigned max)
{
    str_cpy(d, "song screen. ", max);
    spk_song_head(d, max);
    cat(d, ". ", max);
    spk_song_entry(d, max);
    cat(d, ". select picks the entry, presets its pattern, knob 1 the repeats, knob 2 adds or deletes, knob 3 loop "
           "or once, knob 4 the song. play starts, home goes back.", max);
}

static void spk_restore(char *d, unsigned max)
{
    str_cpy(d, ui.restore == 1u ? "factory restore. erases every saved pattern, kit, song and setting and brings "
                                  "back the factory set. press save to restore, any other button cancels."
                                : "factory restore, erasing.", max);
}

static void spk_screen(char *d, unsigned max)     /* the whole screen */
{
    d[0] = 0;
    if (ui.restore)
        spk_restore(d, max);
    else if (ui.saving)
        spk_save(d, max);
    else if (ui.song_open)
        spk_song(d, max);
    else if (ui.page == PG_HOME)
        spk_home(d, max);
    else
        spk_page(d, max);
}

/* the held step: all of it, or what changed since o */
static void spk_step(char *d, unsigned max, const step_t *st, uint32_t s, const step_t *o)
{
    char b[12];
    uint32_t i;
    if (!o) {
        cat(d, "step ", max);
        fmt_int(b, (int32_t)s + 1);
        cat(d, b, max);
        cat(d, (st->f & ST_TRIG) ? ((st->f & ST_ACC) ? ", on, accent" : ", on") : ", off", max);
    }
    if (!o || o->prob != st->prob) {
        cat(d, d[0] ? ", chance " : "chance ", max);
        fmt_int(b, (int32_t)st_prob(st));
        cat(d, b, max);
        cat(d, " percent", max);
    }
    if (!o || (o->rat & 15u) != (st->rat & 15u)) {
        if (st_ratchet(st) > 1u) {
            cat(d, d[0] ? ", ratchet " : "ratchet ", max);
            fmt_int(b, (int32_t)st_ratchet(st));
            cat(d, b, max);
            cat(d, st_ramp(st) == 1u ? " up" : st_ramp(st) == 2u ? " down" : "", max);
        } else if (o)
            cat(d, "no ratchet", max);
    }
    if (!o || st_cond(o) != st_cond(st)) {
        if (st_cond(st) || o) {
            cat(d, d[0] ? ", condition " : "condition ", max);
            spk(d, max, COND_NAME[st_cond(st)]);
        }
    }
    if (!o || st_nudge(o) != st_nudge(st)) {
        if (st_nudge(st) || o) {
            cat(d, d[0] ? ", nudge " : "nudge ", max);
            if (st_nudge(st)) {
                cat(d, st_nudge(st) > 0 ? "late " : "early ", max);
                fmt_int(b, st_nudge(st) > 0 ? st_nudge(st) : -st_nudge(st));
                cat(d, b, max);
            } else
                cat(d, "none", max);
        }
    }
    for (i = 0; i < 4u; i++)
        if (o ? o->lock[i] != st->lock[i] : st->lock[i] != 0u) {
            cat(d, d[0] ? ", " : "", max);
            spk(d, max, SP_NAME[i]);
            cat(d, " lock ", max);
            if (st->lock[i]) {
                fmt_int(b, st->lock[i] - 1);
                cat(d, b, max);
            } else
                cat(d, "off", max);
        }
}

/* ------------------------------------------------------------ what changed --- */
static struct {
    uint32_t screen;             /* screen_id() */
    uint32_t msg_until;
    int32_t v[4];                /* the four values shown (a page's rows, HOME's sounds) */
    uint8_t sel, patn, kitn, running, ext, song_i, song_cur, song_n, song_loop;
    uint16_t bpm2, trig, acc;
    int8_t held;
    step_t step;
    uint8_t song_e[2];
    uint8_t what, slot, cur;
    char name[NAME_LEN + 1];
    uint8_t valid;
} sk;

static void sk_values(int32_t *v)                /* the four values the screen shows now */
{
    uint32_t r;
    for (r = 0; r < 4u; r++)
        v[r] = ui.page == PG_HOME ? kit.t[sel()].p[r] : pv_get(ui.page, r, sel());
}

static uint32_t sk_mask(uint32_t bit)            /* the selected track's steps with this flag */
{
    uint32_t s, m = 0;
    for (s = 0; s < NSTEP; s++)
        m |= (pat.t[sel()].s[s].f & bit) ? 1u << s : 0u;
    return m;
}

static int msg_warn(const char *m)               /* a failure: heard whole */
{
    uint32_t n = str_len(m);
    return (n >= 6u && str_eq(m + n - 6u, "FAILED")) || (n >= 9u && str_eq(m + n - 9u, "NOT SAVED"));
}

static void speak_home(char *d, unsigned max, uint32_t now, const int32_t *v, int said)
{
    uint32_t t = sel(), r, trig = sk_mask(ST_TRIG), acc = sk_mask(ST_ACC);
    d[0] = 0;
    if (ui.held_step >= 0 && ui.mode) {          /* a step key held: the step, then each edit */
        const step_t *st = &pat.t[t].s[ui.held_step];
        if (ui.held_step != sk.held)
            spk_step(d, max, st, (uint32_t)ui.held_step, 0);
        else if (memcmp(st, &sk.step, sizeof *st))
            spk_step(d, max, st, (uint32_t)ui.held_step, &sk.step);
        if (d[0])
            sr_later(d, SR_NOW, ui.held_step != sk.held ? 60u : 120u, now);
        return;
    }
    if (t != sk.sel) {                           /* another track */
        spk_track_line(d, max, t);
        cat(d, ". ", max);
        if (set.view == 0u)
            spk_sounds(d, max, t);
        else
            spk_steps_on(d, max, t);
        sr_later(d, SR_NOW, 200u, now);
        return;
    }
    if (pat_index() != sk.patn) {                /* another pattern started: playing (a queue, a song) in full; */
        if (snd.running_pub && !said)            /* stopped, the message said it: only the kit it brought */
            spk_pattern(d, max, pat_index());
        if (kit_index() != sk.kitn) {
            cat(d, d[0] ? ", " : "", max);
            spk_kit(d, max, kit_index());
        }
        if (d[0])
            sr_later(d, 0, 200u, now);
        return;
    }
    if ((trig != sk.trig || acc != sk.acc) && !(ui.rec && snd.running_pub)) {   /* a step key tapped */
        uint32_t ch = (trig ^ sk.trig) | (acc ^ sk.acc), s;
        char b[8];
        if (ch && !(ch & (ch - 1u))) {           /* one step */
            for (s = 0; !((ch >> s) & 1u); s++)
                ;
            cat(d, "step ", max);
            fmt_int(b, (int32_t)s + 1);
            cat(d, b, max);
            cat(d, !((trig >> s) & 1u) ? " off" : ((acc >> s) & 1u) ? " accent" : " on", max);
            sr_say(d, SR_NOW);
        }
        return;
    }
    for (r = 0; r < 4u; r++)                     /* KNOB 1..4: the track's sound */
        if (v[r] != sk.v[r]) {
            spk_param(d, max, &PP[PG_SOUND][r], v[r]);
            sr_later(d, SR_NOW, 120u, now);
            return;
        }
    if (pat_bpm2() != sk.bpm2 && !snd.ext_pub) {   /* ALGORITHM: the tempo */
        spk_value(d, max, &PP[PG_TIME][0], (int32_t)pat_bpm2());
        sr_later(d, SR_NOW, 120u, now);
    }
}

static void speak_frame(void)
{
    char d[SPK_LEN];
    uint32_t now = fm1_ms, r, screen = screen_id();
    int32_t v[4];
    int said = 0;
    const song_t *sg = &songs.s[song_index];
    if (!sr_on()) {
        sk.valid = 0;
        sr_tick(now);
        return;
    }
    sk_values(v);
    if (sr_where() || !sk.valid) {               /* a new reader, or WHERE: the whole screen */
        spk_screen(d, sizeof d);
        sr_say_long(d, SR_NOW);
    } else {
        d[0] = 0;
        if (ui.msg_until != sk.msg_until && msg_now()[0]) {   /* a new message */
            spk(d, sizeof d, ui.msg);
            sr_say(d, SR_NOW | (msg_warn(ui.msg) ? SR_KEEP : 0u));
            said = 1;
        }
        if (screen != sk.screen) {               /* another screen */
            spk_screen(d, sizeof d);
            if (str_len(d) <= SR_MAX)
                sr_later(d, SR_NOW, 200u, now);
            else
                sr_say_long(d, SR_NOW);
        } else if (ui.restore) {
            ;                                    /* (its progress: silent; the end is a message) */
        } else if (ui.saving) {                  /* SAVE: each edit */
            d[0] = 0;
            if (ui.save_what != sk.what || ui.save_slot != sk.slot) {
                if (ui.save_what != sk.what) {
                    spk(d, sizeof d, SV_NAME[ui.save_what]);
                    cat(d, ", ", sizeof d);
                }
                spk_save_slot(d, sizeof d);
            } else if (ui.name_cur != sk.cur)
                spk_name_char(d, sizeof d);
            else if (!str_eq(ui.name, sk.name)) {
                uint32_t diff = 0, la = str_len(ui.name), lb = str_len(sk.name);
                for (r = 0; r < NAME_LEN; r++)
                    diff += (r < la ? ui.name[r] : ' ') != (r < lb ? sk.name[r] : ' ');
                if (diff <= 1u)
                    spk_name_char(d, sizeof d);
                else
                    spk_save_name(d, sizeof d);
            }
            if (d[0])
                sr_later(d, SR_NOW, 120u, now);
        } else if (ui.song_open) {               /* SONG: each edit */
            d[0] = 0;
            if (song_index != sk.song_i || sg->loop != sk.song_loop) {
                spk_song_head(d, sizeof d);
                cat(d, ". ", sizeof d);
                spk_song_entry(d, sizeof d);
            } else if (ui.song_cur != sk.song_cur || sg->n != sk.song_n ||
                       (ui.song_cur < sg->n && (sg->e[ui.song_cur][0] != sk.song_e[0] ||
                                                sg->e[ui.song_cur][1] != sk.song_e[1])))
                spk_song_entry(d, sizeof d);
            if (d[0])
                sr_later(d, SR_NOW, 120u, now);
        } else if (ui.page == PG_HOME)
            speak_home(d, sizeof d, now, v, said);
        else
            for (r = 0; r < 4u; r++) {           /* a page's knob */
                const param_t *p = &PP[ui.page][r];
                if (v[r] != sk.v[r] && p->name && !K_READONLY(p->kind)) {
                    d[0] = 0;
                    spk_param(d, sizeof d, p, v[r]);
                    sr_later(d, SR_NOW, 120u, now);
                    break;
                }
            }
        if (snd.ext_pub != sk.ext)               /* MIDI clock came or went (not the player's doing: queued) */
            sr_say(snd.ext_pub ? "following MIDI clock" : "internal clock", 0);
        else if (snd.running_pub != sk.running && !said)
            sr_say(snd.running_pub ? "playing" : "stopped", snd.ext_pub ? 0u : SR_NOW);
    }
    sk.screen = screen;
    sk.msg_until = ui.msg_until;
    memcpy(sk.v, v, sizeof sk.v);
    sk.sel = (uint8_t)sel();
    sk.patn = (uint8_t)pat_index();
    sk.kitn = (uint8_t)kit_index();
    sk.running = snd.running_pub;
    sk.ext = snd.ext_pub;
    sk.bpm2 = (uint16_t)pat_bpm2();
    sk.trig = (uint16_t)sk_mask(ST_TRIG);
    sk.acc = (uint16_t)sk_mask(ST_ACC);
    sk.held = ui.mode ? ui.held_step : -1;
    if (sk.held >= 0)
        sk.step = pat.t[sel()].s[sk.held];
    sk.song_i = song_index;
    sk.song_cur = ui.song_cur;
    sk.song_n = (uint8_t)sg->n;
    sk.song_loop = sg->loop;
    sk.song_e[0] = ui.song_cur < sg->n ? sg->e[ui.song_cur][0] : 0u;
    sk.song_e[1] = ui.song_cur < sg->n ? sg->e[ui.song_cur][1] : 0u;
    sk.what = ui.save_what;
    sk.slot = ui.save_slot;
    sk.cur = ui.name_cur;
    str_cpy(sk.name, ui.name, sizeof sk.name);
    sk.valid = 1;
    sr_tick(now);
}
