/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 R. Luke DuBois */
/* The effects (audio ISR, after the voices), in float on the FPU (FM1_FPU: fx_block and the helpers it calls; the
 * ISR hands them integers only). Adapted from Solinaesque's fx.c (the delay, the FDN reverb's form, the LFO table).
 * One chain on the whole mix (docs/SPEC.md "Sound", chosen 2026-11-26), in this order:
 *
 *   [filter -> drive, if POSITION PRE] -> chorus -> delay -> reverb -> [filter -> drive, if POST] -> (the master level
 *   and the soft limit: sound.c)
 *
 *   filter   one knob: below the middle a low-pass (18 kHz down to ~200 Hz), above a high-pass (20 Hz up to ~5 kHz),
 *            the middle off; RESONANCE; a resonant state-variable filter (Zavalishin's TPT form: stable anywhere).
 *   drive    a smooth saturation (tanh-like), gain 1..10, normalised so full scale stays full scale.
 *   chorus   each side a short line read at a swept delay (8 ms +- up to 3 ms, the sides 90 degrees apart); DEPTH sets
 *            depth and mix together (0: bypassed); RATE 0.1 .. 5 Hz.
 *   delay    an insert: ping-pong, the mix's mono sum into the left line, each repeat crossing to the other side,
 *            LEVEL the echoes added to the mix (the dry stays); TIME in note values of the tempo (the clock's when
 *            following one), at most DLY_LEN (~1 s: the longest values shorten at slow tempos); a change glides
 *            (~110 ms, like tape: no click); FEEDBACK 0..95 %; TONE a one-pole low-pass in the loop. int16 lines in
 *            the pool (176 KB): a store truncates towards 0, so every tail ends in exact silence; LEVEL 0: the lines
 *            are kept silent.
 *   reverb   an insert after the delay (its echoes go into the room): a short room, four lines (25..40 ms, mutually
 *            prime) behind two allpass diffusers, mixed by a 4 x 4 Hadamard matrix (lossless: the decay is the gain
 *            alone), each through a one-pole low-pass (DAMPING); SIZE sets the gain, 0.35 .. 0.85 (~0.2 s .. ~1.4 s
 *            to -60 dB); lines 1 + 3 left, 2 + 4 right; LEVEL the room added. It runs while LEVEL > 0 and for its
 *            tail after.
 * Big buffers in the pool (zeroed at boot: all-zero is a silent, valid start). */

#define DLY_LEN 45056u           /* the delay lines: 1.02 s */
#define CH_LEN 1024u             /* the chorus lines: 23 ms */
#define RV_N 4u
static const uint16_t RV_LEN[RV_N] = {1117, 1301, 1543, 1777};   /* 25 .. 40 ms */
static const uint16_t RV_AP[2] = {223, 79};
#define RV_TOTAL (1117u + 1301u + 1543u + 1777u)
#define RV_LONGEST 1777u

static int16_t dly_buf[2][DLY_LEN] __attribute__((section(".pool")));
static float ch_buf[2][CH_LEN] __attribute__((section(".pool")));
static float rv_buf[RV_TOTAL] __attribute__((section(".pool")));
static float rv_ap[223u + 79u] __attribute__((section(".pool")));

/* sin(2 pi i / 64), i = 0..64: the LFO and the filter's tan (interpolated) */
static const float FX_SIN[65] = {
    0.000000f, 0.098017f, 0.195090f, 0.290285f, 0.382683f, 0.471397f, 0.555570f, 0.634393f,
    0.707107f, 0.773010f, 0.831470f, 0.881921f, 0.923880f, 0.956940f, 0.980785f, 0.995185f,
    1.000000f, 0.995185f, 0.980785f, 0.956940f, 0.923880f, 0.881921f, 0.831470f, 0.773010f,
    0.707107f, 0.634393f, 0.555570f, 0.471397f, 0.382683f, 0.290285f, 0.195090f, 0.098017f,
    0.000000f, -0.098017f, -0.195090f, -0.290285f, -0.382683f, -0.471397f, -0.555570f, -0.634393f,
    -0.707107f, -0.773010f, -0.831470f, -0.881921f, -0.923880f, -0.956940f, -0.980785f, -0.995185f,
    -1.000000f, -0.995185f, -0.980785f, -0.956940f, -0.923880f, -0.881921f, -0.831470f, -0.773010f,
    -0.707107f, -0.634393f, -0.555570f, -0.471397f, -0.382683f, -0.290285f, -0.195090f, -0.098017f,
    0.000000f,
};

static struct {
    uint32_t dly_w;
    float dly_cur;               /* the delay time now (samples), gliding to the set time */
    float dly_lp[2];             /* the tone filter in each feedback loop */
    uint8_t dly_on;              /* the last block had LEVEL above 0 (turning it on starts at the set time) */
    uint32_t ch_w, ch_ph;        /* the chorus lines' write index, the LFO's phase */
    uint16_t rv_pos[RV_N], rv_appos[2];
    float rv_lp[RV_N];
    uint32_t rv_tail;            /* samples the reverb still runs after LEVEL went to 0 */
    float f_ic[2][2];            /* the master filter's two integrators, per side */
    uint32_t dly_quiet, rv_quiet;   /* after app_audio_stop: samples written as zeros (one pass, no clear) */
} fxs;

static FM1_FPU float fx_sin(uint32_t ph)         /* sin(2 pi ph / 2^32): the table, interpolated */
{
    uint32_t i = ph >> 26;
    float f = (float)((ph >> 10) & 0xFFFFu) * (1.0f / 65536.0f);
    return FX_SIN[i] + (FX_SIN[i + 1u] - FX_SIN[i]) * f;
}

static FM1_FPU float fx_exp2(float x)            /* 2^x for -10 < x < 10 (the filter's sweep): no libm */
{
    int32_t n = (int32_t)(x < 0.0f ? x - 1.0f : x);
    float f = x - (float)n, p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * 0.0096181f)));
    for (; n > 0; n--)
        p *= 2.0f;
    for (; n < 0; n++)
        p *= 0.5f;
    return p;
}

static FM1_FPU float fx_tanh(float z)            /* tanh, a rational fit (|error| < 2 % up to 3, 1 beyond) */
{
    if (z > 3.0f)
        return 1.0f;
    if (z < -3.0f)
        return -1.0f;
    return z * (27.0f + z * z) / (27.0f + 9.0f * z * z);
}

/* filter -> drive on the stereo block (POSITION picks where in the chain) */
static FM1_FPU void fx_filter_drive(float *l, float *r, uint32_t n)
{
    const uint8_t *fx = kit.fx;
    uint32_t i;
    if (fx[FX_FILT] != 50u) {
        int hp = fx[FX_FILT] > 50u;
        float u = (float)(hp ? fx[FX_FILT] - 50u : 50u - fx[FX_FILT]) * 0.02f;   /* 0..1 from the middle */
        float fc = hp ? 20.0f * fx_exp2(u * 8.0f) : 18000.0f * fx_exp2(-u * 6.5f);
        uint32_t ph = (uint32_t)(fc * (2147483648.0f / (float)FS));             /* pi fc / FS as a phase */
        float g = fx_sin(ph) / fx_sin(ph + 0x40000000u), k = 2.0f - 1.9f * (float)fx[FX_RES] * 0.01f;
        float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
        uint32_t c;
        for (c = 0; c < 2u; c++) {
            float *x = c ? r : l, ic1 = fxs.f_ic[c][0], ic2 = fxs.f_ic[c][1];
            for (i = 0; i < n; i++) {
                float v3 = x[i] - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
                ic1 = 2.0f * v1 - ic1;
                ic2 = 2.0f * v2 - ic2;
                x[i] = hp ? x[i] - k * v1 - v2 : v2;
            }
            fxs.f_ic[c][0] = ic1 > -1e-3f && ic1 < 1e-3f ? 0.0f : ic1;   /* (silence: exact 0) */
            fxs.f_ic[c][1] = ic2 > -1e-3f && ic2 < 1e-3f ? 0.0f : ic2;
        }
    } else
        fxs.f_ic[0][0] = fxs.f_ic[0][1] = fxs.f_ic[1][0] = fxs.f_ic[1][1] = 0.0f;
    if (fx[FX_DRIVE]) {
        const float dg = 1.0f + 9.0f * (float)fx[FX_DRIVE] * 0.01f, norm = 32768.0f / fx_tanh(dg);
        for (i = 0; i < n; i++) {
            l[i] = fx_tanh(dg * l[i] * (1.0f / 32768.0f)) * norm;
            r[i] = fx_tanh(dg * r[i] * (1.0f / 32768.0f)) * norm;
        }
    }
}

/* the chain on the stereo mix, in place (Q15 units, any size: the soft limit follows); s16: samples per 16th now */
static FM1_FPU __attribute__((noinline)) void fx_block(int32_t *mix, uint32_t n, uint32_t s16)
{
    static const uint8_t DLY_32[8] = {1, 2, 3, 4, 6, 8, 12, 16};   /* DLY_NAME in 32nds: 1/32 .. 1/2 */
    float l[CTL], r[CTL];
    uint32_t i;
    const uint8_t *fx = kit.fx;
    for (i = 0; i < n; i++) {
        l[i] = (float)mix[2u * i];
        r[i] = (float)mix[2u * i + 1u];
    }
    if (!fx[FX_FPOS])
        fx_filter_drive(l, r, n);                /* PRE: filter and drive first */

    /* ---- chorus */
    if (fx[FX_CHORUS]) {
        const float amt = (float)fx[FX_CHORUS] * 0.01f, ms = FS / 1000.0f, wet = 0.5f * amt, dry = 1.0f - 0.3f * amt;
        const float base = 8.0f * ms, dep = 3.0f * ms * amt;
        const float hz = 0.1f * fx_exp2((float)fx[FX_CRATE] * 0.0564f);         /* RATE: 0.1 .. 5 Hz */
        const uint32_t inc = (uint32_t)(hz / (float)FS * 4294967296.0f) * CTL;  /* per block */
        float dl0 = base + dep * fx_sin(fxs.ch_ph), dr0 = base + dep * fx_sin(fxs.ch_ph + 0x40000000u);
        float dl1 = base + dep * fx_sin(fxs.ch_ph + inc), dr1 = base + dep * fx_sin(fxs.ch_ph + inc + 0x40000000u);
        float sl = (dl1 - dl0) * (1.0f / CTL), sr = (dr1 - dr0) * (1.0f / CTL);
        uint32_t w = fxs.ch_w;
        for (i = 0; i < n; i++) {
            float d, fr, yl, yr;
            int32_t di;
            ch_buf[0][w & (CH_LEN - 1u)] = l[i];
            ch_buf[1][w & (CH_LEN - 1u)] = r[i];
            d = dl0 + sl * (float)i;
            di = (int32_t)d;
            fr = d - (float)di;
            yl = ch_buf[0][(w - (uint32_t)di) & (CH_LEN - 1u)] * (1.0f - fr) +
                 ch_buf[0][(w - (uint32_t)di - 1u) & (CH_LEN - 1u)] * fr;
            d = dr0 + sr * (float)i;
            di = (int32_t)d;
            fr = d - (float)di;
            yr = ch_buf[1][(w - (uint32_t)di) & (CH_LEN - 1u)] * (1.0f - fr) +
                 ch_buf[1][(w - (uint32_t)di - 1u) & (CH_LEN - 1u)] * fr;
            l[i] = dry * l[i] + wet * yl;
            r[i] = dry * r[i] + wet * yr;
            w++;
        }
        fxs.ch_w = w;
        fxs.ch_ph += inc;
    }


    /* ---- delay (an insert: ping-pong from the mix's mono sum) */
    {
        uint32_t w = fxs.dly_w, len = s16 * DLY_32[fx[FX_DTIME] < 8u ? fx[FX_DTIME] : 3u] / 2u;
        len = len < 16u ? 16u : len > DLY_LEN - 4u ? DLY_LEN - 4u : len;
        if (fxs.dly_quiet) {                     /* after a stop: silent, zeros written as it goes */
            for (i = 0; i < n; i++) {
                dly_buf[0][w] = dly_buf[1][w] = 0;
                if (++w >= DLY_LEN)
                    w = 0;
            }
            fxs.dly_quiet = fxs.dly_quiet > n ? fxs.dly_quiet - n : 0u;
            fxs.dly_lp[0] = fxs.dly_lp[1] = 0.0f;
            fxs.dly_on = 0;
        } else if (fx[FX_DLVL]) {
            const float fb = (float)(fx[FX_DFB] > 95u ? 95u : fx[FX_DFB]) * 0.01f, mix_g = (float)fx[FX_DLVL] * 0.01f;
            const float t = (float)fx[FX_DTONE] * 0.01f, c = 0.05f + 0.95f * t * t, target = (float)len;
            float cur = fxs.dly_on ? fxs.dly_cur : target, lpl = fxs.dly_lp[0], lpr = fxs.dly_lp[1];
            for (i = 0; i < n; i++) {
                float fr, yl, yr, v;
                int32_t di;
                uint32_t a, b;
                cur += (target - cur) * 0.0002f;         /* the glide */
                di = (int32_t)cur;
                fr = cur - (float)di;
                a = w >= (uint32_t)di ? w - (uint32_t)di : w + DLY_LEN - (uint32_t)di;
                b = a ? a - 1u : DLY_LEN - 1u;
                yl = (float)dly_buf[0][a] * (1.0f - fr) + (float)dly_buf[0][b] * fr;
                yr = (float)dly_buf[1][a] * (1.0f - fr) + (float)dly_buf[1][b] * fr;
                lpl += (yl - lpl) * c;
                lpr += (yr - lpr) * c;
                v = 0.5f * (l[i] + r[i]) + fb * lpr;   /* the mix in on the left; the right's repeat crosses back */
                dly_buf[0][w] = (int16_t)(v > 32767.0f ? 32767.0f : v < -32767.0f ? -32767.0f : v);
                v = fb * lpl;                    /* the left's repeat crosses to the right */
                dly_buf[1][w] = (int16_t)(v > 32767.0f ? 32767.0f : v < -32767.0f ? -32767.0f : v);
                l[i] += mix_g * lpl;
                r[i] += mix_g * lpr;
                if (++w >= DLY_LEN)
                    w = 0;
            }
            fxs.dly_cur = cur;
            fxs.dly_on = 1;
            fxs.dly_lp[0] = lpl > -0.5f && lpl < 0.5f ? 0.0f : lpl;   /* (far below one LSB: exact 0) */
            fxs.dly_lp[1] = lpr > -0.5f && lpr < 0.5f ? 0.0f : lpr;
        } else {                                 /* off: the lines kept silent, so turning it on starts clean */
            for (i = 0; i < n; i++) {
                dly_buf[0][w] = dly_buf[1][w] = 0;
                if (++w >= DLY_LEN)
                    w = 0;
            }
            fxs.dly_lp[0] = fxs.dly_lp[1] = 0.0f;
            fxs.dly_on = 0;
        }
        fxs.dly_w = w;
    }

    /* ---- reverb (an insert after the delay: the echoes go in too) */
    if (fx[FX_RLVL])
        fxs.rv_tail = 3u * FS;                   /* (the longest room's tail to exact silence: < 3 s) */
    if (fxs.rv_quiet || fxs.rv_tail) {
        const float s = (float)fx[FX_RSIZE] * 0.01f, g = 0.5f * (0.35f + 0.5f * s);   /* (Hadamard / 2) */
        const float c = 0.9f - 0.7f * (float)fx[FX_RDAMP] * 0.01f;
        const float mix_g = fxs.rv_quiet ? 0.0f : (float)fx[FX_RLVL] * 0.006f, in_g = fxs.rv_quiet ? 0.0f : 0.25f;
        static const uint16_t BASE[RV_N] = {0, 1117u, 1117u + 1301u, 1117u + 1301u + 1543u};
        uint32_t k;
        for (i = 0; i < n; i++) {
            float x = in_g * (l[i] + r[i]), d[RV_N], h[RV_N], v;
            uint32_t a = fxs.rv_appos[0];
            v = rv_ap[a] - 0.6f * x;             /* two allpasses: y = buf - g x; buf = x + g y */
            rv_ap[a] = x + 0.6f * v;
            fxs.rv_appos[0] = (uint16_t)(a + 1u >= RV_AP[0] ? 0u : a + 1u);
            x = v;
            a = fxs.rv_appos[1];
            v = rv_ap[223u + a] - 0.6f * x;
            rv_ap[223u + a] = x + 0.6f * v;
            fxs.rv_appos[1] = (uint16_t)(a + 1u >= RV_AP[1] ? 0u : a + 1u);
            x = v;
            for (k = 0; k < RV_N; k++)
                d[k] = rv_buf[BASE[k] + fxs.rv_pos[k]];
            h[0] = d[0] + d[1] + d[2] + d[3];
            h[1] = d[0] - d[1] + d[2] - d[3];
            h[2] = d[0] + d[1] - d[2] - d[3];
            h[3] = d[0] - d[1] - d[2] + d[3];
            for (k = 0; k < RV_N; k++) {
                float lp = fxs.rv_lp[k] + (h[k] * g - fxs.rv_lp[k]) * c, wv = (k & 1u ? -x : x) + lp;
                fxs.rv_lp[k] = lp > -0.5f && lp < 0.5f ? 0.0f : lp;
                rv_buf[BASE[k] + fxs.rv_pos[k]] = wv > -0.5f && wv < 0.5f ? 0.0f : wv;   /* tails end at 0 */
                fxs.rv_pos[k] = (uint16_t)(fxs.rv_pos[k] + 1u >= RV_LEN[k] ? 0u : fxs.rv_pos[k] + 1u);
            }
            l[i] += mix_g * (d[0] + d[2]);
            r[i] += mix_g * (d[1] + d[3]);
        }
        if (fxs.rv_quiet)
            fxs.rv_quiet = fxs.rv_quiet > n ? fxs.rv_quiet - n : 0u;
        if (!fx[FX_RLVL] && fxs.rv_tail)
            fxs.rv_tail = fxs.rv_tail > n ? fxs.rv_tail - n : 0u;
    }

    if (fx[FX_FPOS])
        fx_filter_drive(l, r, n);                /* POST: filter and drive after the room */

    for (i = 0; i < n; i++) {                    /* back to integers (truncation: towards 0) */
        float a = l[i], b = r[i];
        mix[2u * i] = (int32_t)(a > 1e6f ? 1e6f : a < -1e6f ? -1e6f : a);
        mix[2u * i + 1u] = (int32_t)(b > 1e6f ? 1e6f : b < -1e6f ? -1e6f : b);
    }
}

static int fx_any(void)                          /* anything to do (else fx_block is skipped: no cost) */
{
    const uint8_t *fx = kit.fx;
    return fx[FX_DLVL] || fx[FX_RLVL] || fxs.rv_tail || fxs.rv_quiet || fxs.dly_quiet || fxs.dly_on ||
           fx[FX_CHORUS] || fx[FX_FILT] != 50u || fx[FX_DRIVE];
}

static __attribute__((noinline)) void fx_reset(void)   /* the ISR, on app_audio_stop (rare): every tail silenced */
{
    memset(ch_buf, 0, sizeof ch_buf);
    fxs.dly_quiet = DLY_LEN;                     /* (one pass of zeros through the delay lines, then off) */
    fxs.rv_quiet = RV_LONGEST + 512u;
    fxs.rv_tail = 0;
    fxs.rv_lp[0] = fxs.rv_lp[1] = fxs.rv_lp[2] = fxs.rv_lp[3] = 0.0f;
    fxs.f_ic[0][0] = fxs.f_ic[0][1] = fxs.f_ic[1][0] = fxs.f_ic[1][1] = 0.0f;
}
