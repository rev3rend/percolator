/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 R. Luke DuBois */
/* The audio ISR: 23 Shakers voices, the pads, MIDI in and notes out, and the step clock.
 *
 * The voice is STK's PhISEM (Perry Cook; STK Shakers.cpp, Perry R. Cook and Gary P. Scavone, MIT-style licence) in
 * fixed point, as far as the skeleton goes: a hit adds 0.1 x amplitude to the shake energy (capped at 1, as STK's
 * noteOn); the energy decays by the system decay (here per 32-sample block, from SK_SD32); each sample a collision
 * happens with the chance nObjects / 1024 (SK_THR), adds the energy to the sound level and excites the resonator with
 * it; the sound level decays by the sound decay; the first resonance (SK_A1, SK_A2) and STK's three-tap equalizer
 * shape it. The four parameters index the generated tables directly, so a knob or a MIDI CC needs no recomputation.
 * Not yet (milestone 2): the other resonances, the frequency variation, the ratchet (Guiro, Wrench), the sweeping
 * drops (Water) and the one-tube-per-collision chimes. One deliberate difference from STK: STK stops ticking the
 * resonators when the energy falls below 0.001, which cuts a ringing coin; here they ring out and the voice goes idle
 * only when everything is near zero (then its state is zeroed: exact silence, no limit cycle).
 *
 * Formats: energy and sound level Q24; the resonator state Q20 (gen_shakers.py checks the headroom); coefficients
 * Q29, the feedback rounded towards zero (a floor leaves a DC limit cycle in the low resonances: Crunch, Water); out
 * Q15 through SK_OUTG. The step clock counts samples: phase += BPM x 2 per sample, a 16th every FS x 30.
 *
 * Main loop -> ISR: volatile requests (run_req, stop_req, silence_req, pad_mask, rec, sel, next_req) taken at the start
 * of a block. ISR -> main loop: the published playhead, hit counters, the running flag, the swapped pattern. The ISR
 * reads the kit and the pattern directly (single bytes: a torn read is a value either way) and, while recording,
 * sets trigs in the pattern. */

#define E_HIT_Q24 1677722        /* 0.1 in Q24: STK's energy per hit at amplitude 1 */
#define E_MAX_Q24 (1 << 24)
#define STEP_UNITS ((uint32_t)FS * 30u)          /* phase units per 16th (phase += BPM x 2 per sample) */
#define NOTE_OFF_BLOCKS 64u      /* notes out: the note-off ~46 ms after the note-on */

typedef struct {
    int32_t e, lvl, y1, y2;      /* energy, sound level (Q24); resonator outputs (Q20) */
    uint32_t seed;               /* the collision dice (an LCG per voice) */
} voice_t;

static struct {
    voice_t v[NT];
    uint32_t active;             /* bit t: voice t is sounding (rendered) */
    uint32_t last_keys;
    uint8_t note_off[NT];        /* notes out: blocks until the note-off, 0 = none pending */
    uint8_t out_note[NT];        /* the note sent (base note changes must not leave one hanging) */
    uint8_t out_ch;
    /* the clock */
    uint8_t running;
    uint32_t phase;              /* 0 .. STEP_UNITS */
    uint32_t gstep;              /* 16ths since start (the next to play) */
    /* main loop -> ISR */
    volatile uint8_t run_req;    /* 1 start, 2 stop, 3 restart */
    volatile uint8_t stop_req;   /* app_audio_stop: stop and silence */
    volatile uint8_t silence_req;
    volatile uint32_t pad_mask;  /* keys that play their track when pressed */
    volatile uint8_t rec;        /* live record: pads set trigs while running */
    volatile uint8_t sel;        /* the selected track: MIDI CCs 2 / 4 / 11 / 1 set its parameters */
    volatile uint8_t next_req;   /* the pattern to change to, + 1 (0: none): at the next bar line, or at once when stopped */
    volatile int32_t master_q8;  /* the MASTER pot, 0..256 */
    /* ISR -> main loop */
    volatile uint8_t running_pub;
    volatile uint8_t pos_pub[NT];   /* each track's last played step */
    volatile uint8_t hits[NT];      /* counts hits: the screen flashes the track */
    volatile uint8_t swapped;       /* the queued pattern took over (the main loop clears it) */
    volatile uint32_t active_pub;
    volatile uint8_t last_midi;     /* the last MIDI note-on received, 0 = none */
} snd = {.master_q8 = 256};

static void snd_init(void)
{
    uint32_t t;
    for (t = 0; t < NT; t++)
        snd.v[t].seed = 0x9E3779B9u * (t + 1u);
}

static void voice_silence(uint32_t t)
{
    voice_t *v = &snd.v[t];
    v->e = v->lvl = v->y1 = v->y2 = 0;
    snd.active &= ~(1u << t);
}

static void notes_out_flush(void)                /* every pending note-off now */
{
    uint32_t t;
    for (t = 0; t < NT; t++)
        if (snd.note_off[t]) {
            snd.note_off[t] = 0;
            midi_out(0x80u | snd.out_ch, snd.out_note[t], 0);
        }
}

/* a hit on track t: amp_q8 is the amplitude x 256 (STK's 0..1, here up to 2 with an accent) */
static __attribute__((noinline)) void snd_hit(uint32_t t, uint32_t amp_q8)
{
    voice_t *v = &snd.v[t];
    int32_t e = v->e + (int32_t)((uint32_t)E_HIT_Q24 / 256u * amp_q8);
    v->e = e > E_MAX_Q24 ? E_MAX_Q24 : e;
    snd.active |= 1u << t;
    snd.hits[t]++;
    if (set.notes_out) {
        uint32_t n = set.base_note + t, vel = amp_q8 / 2u;
        if (snd.note_off[t])
            midi_out(0x80u | snd.out_ch, snd.out_note[t], 0);
        snd.out_ch = (uint8_t)(set.midi_ch ? set.midi_ch - 1u : 0u);
        snd.out_note[t] = (uint8_t)(n > 127u ? 127u : n);
        midi_out(0x90u | snd.out_ch, snd.out_note[t], vel < 1u ? 1u : vel > 127u ? 127u : vel);
        snd.note_off[t] = NOTE_OFF_BLOCKS;
    }
}

static uint32_t amp_of(uint32_t t, int accent)   /* ENERGY 0..127 -> 0..254, plus the accent */
{
    uint32_t a = kit.t[t].p[SP_EN] * 2u;
    if (accent)
        a += set.accent * 655u >> 8;             /* ACCENT 0..100 % -> 0..256 */
    return a;
}

static int audible(uint32_t t, uint32_t solo_any)
{
    uint32_t f = pat.t[t].flags;
    return !(f & TP_MUTE) && (!solo_any || (f & TP_SOLO));
}

static uint32_t solo_mask(void)
{
    uint32_t t, any = 0;
    for (t = 0; t < NT; t++)
        any |= pat.t[t].flags & TP_SOLO;
    return any;
}

static __attribute__((noinline)) void seq_step(void)   /* the 16th gstep: every track's step */
{
    uint32_t t, solo = solo_mask();
    if (snd.next_req && snd.gstep && (snd.gstep % NSTEP) == 0u) {   /* a queued pattern: at the bar line */
        pat_cur = &pats[(snd.next_req - 1u) % NPAT];
        snd.next_req = 0;
        snd.swapped = 1;
        solo = solo_mask();
    }
    for (t = 0; t < NT; t++) {
        uint32_t pos = snd.gstep % tp_len(t);
        const step_t *s = &pat.t[t].s[pos];
        snd.pos_pub[t] = (uint8_t)pos;
        if ((s->f & ST_TRIG) && audible(t, solo))
            snd_hit(t, amp_of(t, (s->f & ST_ACC) != 0));
    }
    snd.gstep++;
}

static __attribute__((noinline)) void snd_record(uint32_t t, int accent)   /* a pad while REC runs */
{
    uint32_t len = tp_len(t), g = snd.gstep ? snd.gstep - 1u : 0u, pos;
    if (snd.phase >= STEP_UNITS / 2u)            /* nearer the next 16th than the one just played */
        g++;
    pos = g % len;
    pat.t[t].s[pos].f |= ST_TRIG | (accent ? ST_ACC : 0u);
}

static __attribute__((noinline)) void snd_midi(uint32_t pkt, uint32_t src)
{
    uint32_t st = (pkt >> 8) & 0xF0u, ch = (pkt >> 8) & 0x0Fu, d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu;
    if (pkt == MIDI_LOST || (src == 2u && !set.trs_in))
        return;                                  /* (no note-offs to lose: the models decay on their own) */
    if (st < 0x80u || st >= 0xF0u || (set.midi_ch && ch != set.midi_ch - 1u))
        return;                                  /* the clock comes in milestone 7 */
    if (st == 0x90u && d2) {
        snd.last_midi = (uint8_t)d1;
        if (d1 >= set.base_note && d1 - set.base_note < NT)
            snd_hit(d1 - set.base_note, d2 * 2u);
    } else if (st == 0xB0u) {
        uint32_t t = snd.sel < NT ? snd.sel : 0u;
        if (d1 == 2u)
            kit.t[t].p[SP_EN] = (uint8_t)d2;
        else if (d1 == 4u)
            kit.t[t].p[SP_DEC] = (uint8_t)d2;
        else if (d1 == 11u)
            kit.t[t].p[SP_OBJ] = (uint8_t)d2;
        else if (d1 == 1u)
            kit.t[t].p[SP_FRQ] = (uint8_t)d2;
        else if (d1 == 120u || d1 == 123u)
            snd.silence_req = 1;
    }
}

/* one voice into the mix: n samples, its gains Q15 */
static void voice_render(uint32_t t, int32_t *mix, uint32_t n, int32_t gl, int32_t gr)
{
    voice_t *v = &snd.v[t];
    const uint8_t *p = kit.t[t].p;
    uint32_t thr = SK_THR[t][p[SP_OBJ]], seed = v->seed, i;
    int32_t cg = (int32_t)SK_CG[t][p[SP_OBJ]], a1 = SK_A1[t][p[SP_FRQ]], a2 = SK_A2[t], snd_d = SK_SND[t];
    int32_t e0 = SK_EQ[t][0], e1 = SK_EQ[t][1], e2 = SK_EQ[t][2], outg = SK_OUTG[t];
    int32_t e = v->e, lvl = v->lvl, y1 = v->y1, y2 = v->y2;
    int live = e >= SK_MIN_ENERGY_Q24;
    if (live)                                    /* the system decay, once per block (SK_SD32 = decay^32) */
        e = (int32_t)((int64_t)e * SK_SD32[t][p[SP_DEC]] >> 30);
    for (i = 0; i < n; i++) {
        int32_t x = 0, y, o, s;
        int64_t acc;
        if (live) {
            seed = seed * 1664525u + 1013904223u;
            if ((seed >> 16) < thr) {
                lvl += e;
                x = (int32_t)((int64_t)lvl * cg >> 20);            /* Q24 x Q16 -> Q20 */
            }
        }
        lvl = (int32_t)((int64_t)lvl * snd_d >> 15);
        acc = (int64_t)a1 * y1 + (int64_t)a2 * y2;
        y = x - (int32_t)((acc + (acc < 0 ? (1 << 29) - 1 : 0)) >> 29);   /* towards zero: no limit cycle */
        o = e0 * y + e1 * y1 + e2 * y2;
        y2 = y1;
        y1 = y;
        s = (int32_t)((int64_t)o * outg >> 21);                    /* Q20 x Q16 -> Q15 */
        mix[2u * i] += s * gl >> 15;
        mix[2u * i + 1u] += s * gr >> 15;
    }
    v->e = e;
    v->lvl = lvl;
    v->y1 = y1;
    v->y2 = y2;
    v->seed = seed;
    if (e < SK_MIN_ENERGY_Q24 && lvl < 64 && y1 < 16 && y1 > -16 && y2 < 16 && y2 > -16)
        voice_silence(t);                        /* rung out: exact silence from here */
}

static void app_audio(int32_t *out, uint32_t n)
{
    static int32_t mix[2u * CTL];
    uint32_t pkt, src, i, t, keys = fm1_in.notes, pressed, act;
    int32_t master;
    if (snd.stop_req) {                          /* before an update or UBOOT: stop, silence, forget */
        snd.stop_req = 0;
        snd.running = 0;
        snd.silence_req = 1;
        snd.next_req = 0;
    }
    while (midi_in_take(&pkt, &src))
        snd_midi(pkt, src);
    if (snd.silence_req) {
        snd.silence_req = 0;
        for (t = 0; t < NT; t++)
            voice_silence(t);
        notes_out_flush();
    }
    if (snd.run_req) {
        uint32_t r = snd.run_req;
        snd.run_req = 0;
        if (r == 2u) {
            snd.running = 0;
        } else if (r == 3u || !snd.running) {
            snd.running = 1;
            snd.gstep = 0;
            snd.phase = STEP_UNITS;              /* the first 16th at once */
        }
    }
    if (snd.next_req && !snd.running) {          /* stopped: the queued pattern now */
        pat_cur = &pats[(snd.next_req - 1u) % NPAT];
        snd.next_req = 0;
        snd.swapped = 1;
    }
    pressed = keys & ~snd.last_keys & snd.pad_mask & PAD_MASK;
    snd.last_keys = keys;
    while (pressed) {                            /* pads: a hit at once, recorded while REC runs */
        uint32_t k = (uint32_t)__builtin_ctz(pressed);
        int acc = (keys >> KEY_ACCENT) & 1u;
        pressed &= pressed - 1u;
        snd_hit(k, amp_of(k, acc));
        if (snd.rec && snd.running)
            snd_record(k, acc);
    }
    if (snd.running) {
        snd.phase += pat_bpm2() * n;
        while (snd.phase >= STEP_UNITS) {
            snd.phase -= STEP_UNITS;
            seq_step();
        }
    }
    snd.running_pub = snd.running;
    for (t = 0; t < NT; t++)                     /* notes out: the note-offs due */
        if (snd.note_off[t] && !--snd.note_off[t])
            midi_out(0x80u | snd.out_ch, snd.out_note[t], 0);

    for (i = 0; i < 2u * n; i++)
        mix[i] = 0;
    master = snd.master_q8 * kit.fx[FX_MASTER] * 655 >> 16;   /* 0..256 (x / 100 as x 655 >> 16) */
    act = snd.active;
    while (act) {
        uint32_t k = (uint32_t)__builtin_ctz(act);
        const ktrack_t *kt = &kit.t[k];
        int32_t lev = kt->level * 328, pan = kt->pan;           /* level 0..100 -> Q15 */
        int32_t gl = pan <= 50 ? lev : (lev >> 4) * (100 - pan) * 1311 >> 12;   /* x / 50 as x 1311 >> 16 */
        int32_t gr = pan >= 50 ? lev : (lev >> 4) * pan * 1311 >> 12;
        act &= act - 1u;
        voice_render(k, mix, n, gl, gr);
    }
    snd.active_pub = snd.active;
    for (i = 0; i < 2u * n; i++) {               /* the master level, saturated to Q15 */
        int32_t s = mix[i] * master >> 8;
        out[i] = s > 32767 ? 32767 : s < -32767 ? -32767 : s;
    }
}

static void app_audio_stop(void) { snd.stop_req = 1; }
