/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 R. Luke DuBois */
/* What the Shakers reference test measures of one rendered hit, the same code for STK (tests/shakers_ref.cpp, C++)
 * and for Percolator's engine (tests/shakers_test.c, C): the power spectrum of the first 8192 samples (Hann window,
 * radix-2 FFT, summed over hits), its centroid; the decay time from the loudest 10 ms frame to the last frame within
 * 30 dB of it (the last: bursts with gaps between them, a ratchet's sweeps, sparse collisions, do not cut it short);
 * the peak. Hits are random (the collisions are dice), so the test compares means over several hits. */
#ifndef SHK_METRICS_H
#define SHK_METRICS_H
#include <math.h>
#include <string.h>

#define SHK_FS 44100
#define SHK_N 8192                   /* FFT size: the first ~186 ms of a hit */
#define SHK_LEN (SHK_FS * 2)         /* samples rendered per hit */
#define SHK_FRAME 441                /* 10 ms */

typedef struct {
    double spec[SHK_N / 2];          /* power, summed over hits */
    double decay_sum, peak_sum;
    int hits, silent;
} shk_acc;

static void shk_fft(double *re, double *im, int n)   /* in place, radix 2 */
{
    int i, j, k, m;
    for (i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (m = 2; m <= n; m <<= 1) {
        double a = -2.0 * M_PI / m;
        for (k = 0; k < n; k += m)
            for (j = 0; j < m / 2; j++) {
                double wr = cos(a * j), wi = sin(a * j);
                double xr = re[k + j + m / 2] * wr - im[k + j + m / 2] * wi;
                double xi = re[k + j + m / 2] * wi + im[k + j + m / 2] * wr;
                re[k + j + m / 2] = re[k + j] - xr;
                im[k + j + m / 2] = im[k + j] - xi;
                re[k + j] += xr;
                im[k + j] += xi;
            }
    }
}

static void shk_add(shk_acc *a, const double *x)    /* one hit of SHK_LEN samples, full scale 1.0 */
{
    static double re[SHK_N], im[SHK_N];
    double peak = 0.0, fmax = 0.0, lim;
    int i, f, nf = SHK_LEN / SHK_FRAME, fpk = 0, fend;
    for (i = 0; i < SHK_LEN; i++)
        peak = fabs(x[i]) > peak ? fabs(x[i]) : peak;
    if (peak < 1e-6) {                               /* (Water in STK: a hit without a drop) */
        a->silent++;
        return;
    }
    for (i = 0; i < SHK_N; i++) {
        re[i] = x[i] * (0.5 - 0.5 * cos(2.0 * M_PI * i / (SHK_N - 1)));
        im[i] = 0.0;
    }
    shk_fft(re, im, SHK_N);
    for (i = 0; i < SHK_N / 2; i++)
        a->spec[i] += re[i] * re[i] + im[i] * im[i];
    for (f = 0; f < nf; f++) {                       /* the loudest 10 ms frame */
        double e = 0.0;
        for (i = 0; i < SHK_FRAME; i++)
            e += x[f * SHK_FRAME + i] * x[f * SHK_FRAME + i];
        if (e > fmax) {
            fmax = e;
            fpk = f;
        }
    }
    lim = fmax * 1e-3;                               /* -30 dB in power */
    for (fend = nf - 1; fend > fpk; fend--) {        /* the last frame above it */
        double e = 0.0;
        for (i = 0; i < SHK_FRAME; i++)
            e += x[fend * SHK_FRAME + i] * x[fend * SHK_FRAME + i];
        if (e >= lim)
            break;
    }
    a->decay_sum += (double)(fend + 1 - fpk) * SHK_FRAME / SHK_FS;
    a->peak_sum += peak;
    a->hits++;
}

static double shk_centroid(const shk_acc *a)       /* Hz, 50 Hz .. 20 kHz */
{
    double num = 0.0, den = 0.0;
    int i;
    for (i = 0; i < SHK_N / 2; i++) {
        double hz = (double)i * SHK_FS / SHK_N;
        if (hz < 50.0 || hz > 20000.0)
            continue;
        num += hz * a->spec[i];
        den += a->spec[i];
    }
    return den > 0.0 ? num / den : 0.0;
}

/* the cases compared: the defaults, and one parameter moved */
enum { SHK_DEFAULT, SHK_FREQ_HI, SHK_DECAY_LO, SHK_OBJ_HI, SHK_NCASE };
static const char *const SHK_CASE[SHK_NCASE] = {"defaults", "FREQ 112", "DECAY 32", "OBJECTS 100"};
static const int SHK_PARAM[SHK_NCASE][3] = {{64, 64, 64}, {64, 64, 112}, {32, 64, 64}, {64, 100, 64}};  /* D O F */
#define SHK_AMP (200.0 / 256.0)                       /* ENERGY 100 */
/* hits per case: more for the instruments whose hits differ most (sparse collisions, ratchets, drops); STK's dice
 * are the C library's rand(), different on every platform, so the means must hold on any of them */
static int shk_hits(int t) { return t == 5 || t == 8 || t == 19 || t == 20 || t == 21 || t == 22 ? 128 : 48; }
#endif
