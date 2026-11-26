/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 R. Luke DuBois */
/* The audio ISR: 23 Shakers voices, the pads, MIDI in and notes out, and the step clock.
 *
 * The voice is STK's Shakers (Perry Cook's PhISEM; STK Shakers.cpp, Perry R. Cook and Gary P. Scavone, MIT-style
 * licence) in fixed point, every part of it: a hit adds 0.1 x amplitude to the shake energy (capped at 1, as STK's
 * noteOn); the energy decays by the system decay (per 32-sample block, SK_SD32); each sample a collision happens with
 * the chance nObjects / 1024 (SK_THR), adds the energy to the sound level and excites the resonances with it, each
 * through its gain x currentGain (SK_CG); the varying resonances (Tambourine's jingles, Sleigh Bells, Bamboo, the
 * Rocks) take a new random frequency at each collision; Tuned Bamboo Chimes excite one tube (of seven) per collision;
 * the sound level decays by the sound decay; the resonances' sum goes through STK's three-tap equalizer. Guiro and
 * Wrench are STK's ratchet: each sweep the energy falls from 1 by a fixed step plus 0.2 %, collisions add energy^2,
 * and the excitation is noise x level x energy. Water Drops: a drop (at a collision, chance nObjects / 32767) takes
 * one of three resonances, at a random frequency around the second base frequency, which then sweeps upwards
 * (x 1.0001 a sample) while its gain decays.
 *
 * Where the port departs from STK on purpose (docs/SPEC.md "Sound"): the resonators ring out instead of stopping
 * when the energy falls below 0.001 (no click); FREQ also moves the varied and the sweeping frequencies (STK resets
 * them to the base frequencies, undoing the control); every Water hit starts with a drop (in STK two hits in three
 * are silent); Guiro and Wrench: DECAY sets the scrape (1..8 sweeps a hit) and the hit's amplitude scales it.
 *
 * Formats: energy and sound level Q24; the resonator states Q20 (gen_shakers.py checks the headroom in float);
 * coefficients Q29 (a1 = -2 r cos w from SK_COS, interpolated; computed when FREQ changes, at a varying resonance's
 * collision, every sample for a sweeping drop), the feedback rounded towards zero (a floor leaves a DC limit cycle in
 * the low resonances); frequencies Hz in Q16; out Q15 through SK_OUTG. The four parameters index the generated tables
 * directly. The step clock counts samples: phase += BPM x 2 per sample, a 16th every FS x 30.
 *
 * Main loop -> ISR: volatile requests (run_req, stop_req, silence_req, pad_mask, rec, sel, next_req) taken at the start
 * of a block. ISR -> main loop: the published playhead, hit counters, the running flag, the swapped pattern. The ISR
 * reads the kit and the pattern directly (single bytes: a torn read is a value either way) and, while recording,
 * sets trigs in the pattern. */

#define E_HIT_Q24 1677722        /* 0.1 in Q24: STK's energy per hit at amplitude 1 */
#define E_MAX_Q24 (1 << 24)
#define STEP_UNITS ((uint32_t)FS * 30u)          /* phase units per 16th (phase += BPM x 2 per sample) */
#define NOTE_OFF_BLOCKS 64u      /* notes out: the note-off ~46 ms after the note-on */

#define RATCHET_MAX 32           /* sweeps queued at most (hits pile up while a scrape runs) */
#define FQ_NONE 0xFFu            /* voice_t.fq: the coefficients are not computed yet */
typedef struct {
    int32_t e, lvl;              /* energy, sound level (Q24); the ratchet's energy is e too */
    int32_t y1[SK_MAXRES], y2[SK_MAXRES];   /* the resonances' outputs (Q20) */
    int32_t a1[SK_MAXRES];       /* their -2 r cos w now (Q29) */
    int32_t s1, s2;              /* the sum's last two samples (the equalizer) */
    uint32_t wf[3];              /* Water: the drops' frequencies (Hz Q16) */
    int32_t wg[3];               /* Water: the drops' gains (Q30, 0 = none) */
    uint32_t seed;               /* the dice (an LCG per voice) */
    int16_t count;               /* the ratchet: sweeps left */
    uint16_t ramp;               /* the ratchet: the scrape's amplitude x 256 */
    uint8_t fq;                  /* the FREQ the coefficients were computed for (FQ_NONE: not yet) */
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

static void voice_silence(uint32_t t)            /* everything back to zero (the dice and the coefficients stay) */
{
    voice_t *v = &snd.v[t];
    uint32_t k;
    v->e = v->lvl = v->s1 = v->s2 = 0;
    for (k = 0; k < SK_MAXRES; k++)
        v->y1[k] = v->y2[k] = 0;
    for (k = 0; k < 3u; k++)
        v->wg[k] = 0;
    v->count = 0;
    snd.active &= ~(1u << t);
}

static void snd_init(void)
{
    uint32_t t;
    for (t = 0; t < NT; t++) {
        snd.v[t].seed = 0x9E3779B9u * (t + 1u);
        snd.v[t].fq = FQ_NONE;
        voice_silence(t);
    }
}

/* -2 r cos(2 pi f / FS), Q29, for f in Hz Q16 and r in Q30: SK_COS (cos over 0..pi in 2048 steps) interpolated */
static int32_t sk_a1(uint32_t f, int32_t r)
{
    uint32_t pos = (uint32_t)(((uint64_t)f * 398915783u) >> 32), i;   /* f 4096 / FS, Q16 (398915783 = 2^32 x 4096 / FS) */
    int32_t c;
    if (pos >= (2048u << 16) - 1u)
        pos = (2048u << 16) - 1u;
    i = pos >> 16;
    c = SK_COS[i] + (int32_t)(((int64_t)(SK_COS[i + 1u] - SK_COS[i]) * (int32_t)(pos & 0xFFFFu)) >> 16);
    return -(int32_t)(((int64_t)r * c) >> 30);
}

static uint32_t sk_freq(uint32_t t, uint32_t k, uint32_t fq)   /* resonance k's frequency under FREQ fq, Hz Q16 */
{
    uint64_t f = (uint64_t)SK_RES[t][k].f * (uint32_t)SK_FSCALE[fq] >> 16;
    return f > (uint64_t)(FS * 49 / 100) << 16 ? (uint32_t)(FS * 49 / 100) << 16 : (uint32_t)f;
}

static __attribute__((noinline)) void voice_coefs(uint32_t t, voice_t *v, uint32_t fq)   /* FREQ changed */
{
    uint32_t k, n = SK_NRES[t];
    for (k = 0; k < n; k++)
        v->a1[k] = sk_a1(sk_freq(t, k, fq), SK_RES[t][k].r);
    v->fq = (uint8_t)fq;
}

static int32_t dice_noise(uint32_t *seed)        /* -32768 .. 32767: STK's noise() in Q15 */
{
    *seed = *seed * 1664525u + 1013904223u;
    return (int32_t)(*seed >> 16) - 32768;
}

static __attribute__((noinline)) void voice_vary(uint32_t t, voice_t *v)   /* a collision: new random frequencies */
{
    uint32_t k, n = SK_NRES[t], fq = v->fq;
    for (k = 0; k < n; k++)
        if (SK_RES[t][k].vary) {
            int32_t m = 65536 + (int32_t)(((int64_t)SK_VARY[t] * dice_noise(&v->seed)) >> 15);   /* 1 + vary x noise */
            v->a1[k] = sk_a1((uint32_t)((uint64_t)sk_freq(t, k, fq) * (uint32_t)m >> 16), SK_RES[t][k].r);
        }
}

static __attribute__((noinline)) void water_drop(uint32_t t, voice_t *v)   /* STK waterDrop's event */
{
    uint32_t j = (uint32_t)(((v->seed = v->seed * 1664525u + 1013904223u) >> 16) * 3u >> 16), k = 2;
    uint32_t base = sk_freq(t, 1, v->fq);
    static const int32_t CENTRE[3] = {49152, 65536, 81920};   /* 0.75, 1.0, 1.25 (Q16) */
    v->lvl = v->e;
    if (j == 0u && !v->wg[0])
        k = 0;
    else if (j == 1u && !v->wg[1])
        k = 1;
    else if (v->wg[2])
        return;
    {
        int32_t m = CENTRE[k] + (dice_noise(&v->seed) >> 1);   /* + 0.25 x noise (Q16) */
        int32_t g = dice_noise(&v->seed);
        v->wf[k] = (uint32_t)((uint64_t)base * (uint32_t)m >> 16);
        v->wg[k] = (g < 0 ? -g : g) << 15;                     /* |noise|, Q30 */
    }
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
    if (v->fq == FQ_NONE || v->fq != kit.t[t].p[SP_FRQ])
        voice_coefs(t, v, kit.t[t].p[SP_FRQ]);
    if (SK_KIND[t] == SK_RATCHET) {              /* a scrape: DECAY sets its sweeps, the amplitude its level */
        int32_t c = v->count + 1 + kit.t[t].p[SP_DEC] * 7 / 127;
        v->count = (int16_t)(c > RATCHET_MAX ? RATCHET_MAX : c);
        v->ramp = (uint16_t)amp_q8;
    } else {
        int32_t e = v->e + (int32_t)((uint32_t)E_HIT_Q24 / 256u * amp_q8);
        v->e = e > E_MAX_Q24 ? E_MAX_Q24 : e;
        if (SK_KIND[t] == SK_WATER)
            water_drop(t, v);                    /* every hit starts with a drop */
    }
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
        if (d1 == 2u)                            /* STK's code (not its comment): 11 decay, 4 objects */
            kit.t[t].p[SP_EN] = (uint8_t)d2;
        else if (d1 == 11u)
            kit.t[t].p[SP_DEC] = (uint8_t)d2;
        else if (d1 == 4u)
            kit.t[t].p[SP_OBJ] = (uint8_t)d2;
        else if (d1 == 1u)
            kit.t[t].p[SP_FRQ] = (uint8_t)d2;
        else if (d1 == 120u || d1 == 123u)
            snd.silence_req = 1;
    }
}

/* the resonances and the equalizer for one sample: x[k] the excitations (Q20, before each resonance's gain), the
 * output in Q20. Rounded towards zero (a floor leaves a DC limit cycle) */
static inline int32_t voice_res(voice_t *v, const sk_res_t *res, uint32_t n, const int32_t *x, int32_t e0, int32_t e1,
                                int32_t e2)
{
    uint32_t k;
    int32_t sum = 0, o;
    for (k = 0; k < n; k++) {
        int64_t acc = (int64_t)v->a1[k] * v->y1[k] + (int64_t)res[k].r2 * v->y2[k];
        int32_t y = x[k] - (int32_t)((acc + (acc < 0 ? (1 << 29) - 1 : 0)) >> 29);
        v->y2[k] = v->y1[k];
        v->y1[k] = y;
        sum += y;
    }
    o = e0 * sum + e1 * v->s1 + e2 * v->s2;
    v->s2 = v->s1;
    v->s1 = sum;
    return o;
}

static int voice_quiet(const voice_t *v, uint32_t n)   /* every state near zero: the voice can go idle */
{
    uint32_t k;
    if (v->lvl >= 64 || v->s1 >= 64 || v->s1 <= -64 || v->wg[0] || v->wg[1] || v->wg[2])
        return 0;
    for (k = 0; k < n; k++)
        if (v->y1[k] >= 16 || v->y1[k] <= -16 || v->y2[k] >= 16 || v->y2[k] <= -16)
            return 0;
    return 1;
}

/* one voice into the mix: n samples, its gains Q15 */
static void voice_render(uint32_t t, int32_t *mix, uint32_t n, int32_t gl, int32_t gr)
{
    voice_t *v = &snd.v[t];
    const uint8_t *p = kit.t[t].p;
    const sk_res_t *res = SK_RES[t];
    uint32_t kind = SK_KIND[t], nres = SK_NRES[t], thr = SK_THR[t][p[SP_OBJ]], i, k;
    int32_t cg = (int32_t)SK_CG[t][p[SP_OBJ]], snd_d = SK_SND[t], outg = SK_OUTG[t];
    int32_t e0 = SK_EQ[t][0], e1 = SK_EQ[t][1], e2 = SK_EQ[t][2], cgk[SK_MAXRES];
    int live;
    if (v->fq != p[SP_FRQ])
        voice_coefs(t, v, p[SP_FRQ]);
    for (k = 0; k < nres; k++)                   /* each resonance's gain x currentGain, Q16 */
        cgk[k] = (int32_t)(((int64_t)cg * res[k].g) >> 16);
    if (kind == SK_WATER)
        thr >>= 5;                               /* rand(32767) < nObjects, not rand(1024) */
    live = kind == SK_RATCHET ? v->count > 0 : v->e >= SK_MIN_ENERGY_Q24;
    if (live && kind != SK_RATCHET)              /* the system decay, once per block (SK_SD32 = decay^32) */
        v->e = (int32_t)((int64_t)v->e * SK_SD32[t][p[SP_DEC]] >> 30);
    for (i = 0; i < n; i++) {
        int32_t x[SK_MAXRES], o, s;
        for (k = 0; k < nres; k++)
            x[k] = 0;
        if (kind == SK_RATCHET) {
            if (v->count > 0) {
                v->e -= SK_RDELTA[t] + (int32_t)(((int64_t)v->e * 131) >> 16);   /* - (delta + 0.002 e) */
                if (v->e < 0) {
                    v->e = 1 << 24;
                    v->count--;
                }
                v->seed = v->seed * 1664525u + 1013904223u;
                if ((v->seed >> 16) < thr)
                    v->lvl += (int32_t)(((int64_t)v->e * v->e) >> 24);
                {
                    int32_t a = (int32_t)(((int64_t)v->lvl * v->e) >> 24);          /* level x energy, Q24 */
                    a = (int32_t)(((int64_t)a * dice_noise(&v->seed) * v->ramp) >> 23);   /* x noise x amplitude */
                    for (k = 0; k < nres; k++)
                        x[k] = (int32_t)(((int64_t)a * cgk[k]) >> 20);
                }
            }
        } else if (kind == SK_WATER) {
            if (v->e >= SK_MIN_ENERGY_Q24) {
                v->seed = v->seed * 1664525u + 1013904223u;
                if ((v->seed >> 16) < thr)
                    water_drop(t, v);
            }
            for (k = 0; k < 3u; k++) {           /* the drops: gains decay, frequencies sweep up */
                if (!v->wg[k])
                    continue;
                v->wg[k] = (int32_t)(((int64_t)v->wg[k] * res[k].r) >> 30);
                if (v->wg[k] > SK_WATER_MIN_Q30) {
                    v->wf[k] += (uint32_t)(((uint64_t)v->wf[k] * SK_SWEEP_Q26) >> 26);
                    v->a1[k] = sk_a1(v->wf[k], res[k].r);
                    x[k] = (int32_t)(((((int64_t)v->lvl * cgk[k]) >> 20) * v->wg[k]) >> 30);
                } else
                    v->wg[k] = 0;
            }
        } else if (live) {
            v->seed = v->seed * 1664525u + 1013904223u;
            if ((v->seed >> 16) < thr) {         /* a collision */
                int32_t l = v->lvl += v->e;
                if (kind == SK_TUBES) {
                    k = (uint32_t)(((v->seed = v->seed * 1664525u + 1013904223u) >> 16) * nres >> 16);
                    x[k] = (int32_t)(((int64_t)l * cgk[k]) >> 20);
                } else {
                    for (k = 0; k < nres; k++)
                        x[k] = (int32_t)(((int64_t)l * cgk[k]) >> 20);
                    if (SK_VARY[t])
                        voice_vary(t, v);
                }
            }
        }
        v->lvl = (int32_t)(((int64_t)v->lvl * snd_d) >> 15);
        o = voice_res(v, res, nres, x, e0, e1, e2);
        s = (int32_t)(((int64_t)o * outg) >> 21);                  /* Q20 x Q16 -> Q15 */
        mix[2u * i] += s * gl >> 15;
        mix[2u * i + 1u] += s * gr >> 15;
    }
    if ((kind == SK_RATCHET ? v->count <= 0 : v->e < SK_MIN_ENERGY_Q24) && voice_quiet(v, nres))
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
