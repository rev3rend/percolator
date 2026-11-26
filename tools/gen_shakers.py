#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 R. Luke DuBois
#
# The instrument data below is transcribed from the Synthesis ToolKit in C++ (STK), src/Shakers.cpp:
#   Copyright (c) 1995-2023 Perry R. Cook and Gary P. Scavone, MIT-style licence (LICENSES/STK.txt).
"""Percolator's Shakers tables (run by tools/build.py generate()).

  gen_shakers.py OUT.h

STK's Shakers class (Perry Cook's PhISEM) for the 23 instruments, as data, in the fixed point of the firmware's
engine (firmware/app/sound.c):

  SK_RES[t][k]   every resonance: frequency (Hz, Q16), radius (Q30), r^2 (Q29), gain (Q16), varies (STK
                 doVaryFrequency); SK_NRES[t] how many
  SK_KIND[t]     the model: SK_GENERIC, SK_RATCHET (Guiro, Wrench), SK_WATER (Water Drops), SK_TUBES (Tuned Bamboo
                 Chimes: one tube per collision)
  SK_SD32[t][v]  the system decay per 32-sample block (systemDecay^32), Q30, for DECAY v = 0..127 (STK CC 11, in
                 its code; its comment says 4:
                 base + 2 (v/128 - 0.5) decayScale (1 - base))
  SK_THR[t][v]   a collision's chance per sample in 1/65536 (STK: rand(1024) < nObjects; Water: rand(32767), so
                 the engine divides by 32), for OBJECTS v (STK CC 4 in its code, 11 in its comment:
                 nObjects = 2 v/128 baseObjects + 1.1)
  SK_CG[t][v]    currentGain, Q16 (log(nObjects) baseGain / nObjects)
  SK_FSCALE[v]   the FREQ factor 4^(v/128 - 0.5), Q16 (CC 1)
  SK_COS[i]      cos(pi i / 2048), i = 0..2048, Q30 (the resonances' coefficients, computed in the ISR)
  SK_SND[t]      the sound decay, Q15;  SK_EQ[t] the equalizer's taps;  SK_VARY[t] the frequency variation, Q16;
  SK_RDELTA[t]   the ratchet's energy step per sample, Q24
  SK_OUTG[t]     the output gain, Q16 from the engine's Q20 units to Q15: each instrument's mean peak at its
                 defaults, ENERGY 100, lands near SK_TARGET (simulate() below: the engine's model in float)

Where the firmware departs from STK, on purpose (docs/SPEC.md "Sound"):
  - the resonators ring out instead of stopping when the energy falls below 0.001 (no cut-off click);
  - FREQ also moves the varied and the sweeping frequencies (STK recomputes them from the base frequencies: the
    control is undone at the next collision);
  - Water Drops: every hit starts with a drop (STK's drop chance, 10 in 32767 per sample while the energy lasts
    ~25 ms, leaves two hits in three silent);
  - Guiro, Wrench: DECAY sets the scrape, 1..8 ratchet sweeps per hit (64: 4; STK adds one per noteOn), and the
    hit's amplitude scales the scrape (STK ignores it). STK sets no system decay for them; none is used.
The output is cached: the header carries a hash of this file and is rewritten only when it changes.
"""
import hashlib
import math
import os
import random
import sys

FS = 44100
SK_TARGET = 9000                 # Q15 mean peak per instrument at its defaults (~ -11 dBFS)
ENERGY_DEFAULT = 100             # the kit's default ENERGY (0..127): amplitude 200 / 256
MIN_ENERGY = 0.001
SWEEP = 1.0001                   # STK WATER_FREQ_SWEEP
GENERIC, RATCHET, WATER, TUBES = 0, 1, 2, 3

# name, short, system decay, sound decay, gain, objects, decay scale, vary factor, equalizer, resonances
# [(freq, radius, gain, varies)]
def R(fs, rs, gs, vary):
    return [(f, r, g, vary(i)) for i, (f, r, g) in enumerate(zip(fs, rs, gs))]
ALL, NONE, NOT0 = (lambda i: 1), (lambda i: 0), (lambda i: int(i > 0))
MUG = ([2123, 4518, 8856, 10753], [0.997] * 4, [1.0, 0.8, 0.6, 0.4])
def mug(cf, cr, cg):
    return R(MUG[0] + cf, MUG[1] + cr, MUG[2] + cg, NONE)
INSTR = [
    ("Maraca", "MAR", 0.999, 0.95, 4.0, 25, 0.97, 0.0, (1, -1, 0), R([3200], [0.96], [1.0], NONE)),
    ("Cabasa", "CAB", 0.997, 0.96, 8.0, 512, 0.97, 0.0, (1, -1, 0), R([3000], [0.7], [1.0], NONE)),
    ("Sekere", "SEK", 0.999, 0.96, 4.0, 64, 0.94, 0.0, (1, 0, -1), R([5500], [0.6], [1.0], NONE)),
    ("Tambourine", "TAM", 0.9985, 0.95, 1.0, 32, 0.95, 0.05, (1, 0, -1),
     R([2300, 5600, 8100], [0.96, 0.99, 0.99], [0.1, 0.8, 1.0], NOT0)),
    ("Sleigh Bells", "SLB", 0.9994, 0.97, 1.0, 32, 0.9, 0.03, (1, 0, -1),
     R([2500, 5300, 6500, 8300, 9800], [0.99] * 5, [1.0, 1.0, 1.0, 0.5, 0.3], ALL)),
    ("Bamboo Chimes", "BMB", 0.9999, 0.9, 0.4, 1.2, 0.7, 0.2, (1, 0, 0),
     R([2800, 0.8 * 2800, 1.2 * 2800], [0.995] * 3, [1.0] * 3, ALL)),
    ("Sand Paper", "SND", 0.999, 0.999, 0.5, 128, 0.97, 0.0, (1, 0, -1), R([4500], [0.6], [1.0], NONE)),
    ("Coke Can", "COK", 0.999, 0.97, 0.5, 48, 0.95, 0.0, (1, 0, -1),
     R([370, 1025, 1424, 2149, 3596], [0.99, 0.992, 0.992, 0.992, 0.992], [1.0, 1.8, 1.8, 1.8, 1.8], NONE)),
    ("Sticks", "STX", 0.998, 0.96, 6.0, 2, 0.96, 0.0, (1, 0, -1), R([5500], [0.6], [1.0], NONE)),
    ("Crunch", "CRN", 0.99806, 0.95, 4.0, 7, 0.96, 0.0, (1, -1, 0), R([800], [0.95], [1.0], NONE)),
    ("Big Rocks", "BRK", 0.9965, 0.98, 4.0, 23, 0.95, 0.11, (1, 0, -1), R([6460], [0.932], [1.0], ALL)),
    ("Little Rocks", "LRK", 0.99586, 0.98, 4.0, 1600, 0.95, 0.18, (1, 0, -1), R([9000], [0.843], [1.0], ALL)),
    ("Next Mug", "MUG", 0.9995, 0.97, 0.8, 3, 0.95, 0.0, (1, 0, -1), mug([], [], [])),
    ("Penny + Mug", "PNY", 0.9995, 0.97, 0.8, 3, 0.95, 0.0, (1, 0, -1),
     mug([11000, 5200, 3835], [0.999] * 3, [1.0, 0.8, 0.5])),
    ("Nickel + Mug", "NKL", 0.9995, 0.97, 0.8, 3, 0.95, 0.0, (1, 0, -1),
     mug([5583, 9255, 9805], [0.9992] * 3, [1.0, 0.8, 0.5])),
    ("Dime + Mug", "DIM", 0.9995, 0.97, 0.8, 3, 0.95, 0.0, (1, 0, -1),
     mug([4450, 4974, 9945], [0.9993] * 3, [1.0, 0.8, 0.5])),
    ("Quarter + Mug", "QTR", 0.9995, 0.97, 0.8, 3, 0.95, 0.0, (1, 0, -1),
     mug([1708, 8863, 9045], [0.9995] * 3, [1.0, 0.8, 0.5])),
    ("Franc + Mug", "FRC", 0.9995, 0.97, 0.8, 3, 0.95, 0.0, (1, 0, -1),
     mug([5583, 11010, 1917], [0.9995] * 3, [0.7, 0.4, 0.3])),
    ("Peso + Mug", "PES", 0.9995, 0.97, 0.8, 3, 0.95, 0.0, (1, 0, -1),
     mug([7250, 8150, 10060], [0.9996] * 3, [1.0, 1.2, 0.7])),
    ("Guiro", "GUI", 0.999, 0.95, 0.4, 128, 0.95, 0.0, (1, 0, -1), R([2500, 4000], [0.97, 0.97], [1.0, 1.0], NONE)),
    ("Wrench", "WRN", 0.999, 0.95, 0.4, 128, 0.95, 0.0, (1, 0, -1), R([3200, 8000], [0.99, 0.992], [1.0, 1.0], NONE)),
    ("Water Drops", "WTR", 0.996, 0.95, 1.0, 10, 0.8, 0.0, (-1, 0, 1),
     R([450, 600, 750], [0.9985] * 3, [1.0] * 3, NONE)),
    ("Tuned Bamboo Chimes", "ANG", 0.9999, 0.95, 0.5, 1.2, 0.7, 0.0, (1, 0, -1),
     R([1046.6, 1174.8, 1397.0, 1568, 1760, 2093.3, 2350], [0.996] * 7, [1.0] * 7, NONE)),
]
assert len(INSTR) == 23
KIND = [GENERIC] * 23
KIND[19] = KIND[20] = RATCHET
KIND[21] = WATER
KIND[22] = TUBES
RDELTA = {19: 0.0001, 20: 0.00015}    # STK baseRatchetDelta


def sysdecay(t, v):
    base, scale = INSTR[t][2], INSTR[t][6]
    return min(base + 2.0 * (v / 128.0 - 0.5) * scale * (1.0 - base), 0.999999)


def nobjects(t, v):
    return 2.0 * v / 128.0 * INSTR[t][5] + 1.1


def cgain(t, v):
    n = nobjects(t, v)
    return math.log(n) * INSTR[t][4] / n


def fscale(v):
    return 4.0 ** (v / 128.0 - 0.5)


def a1_of(f, r):
    return -2.0 * r * math.cos(2.0 * math.pi * min(f, 0.49 * FS) / FS)


def ratchet_count(dec):
    return 1 + dec * 7 // 127


def simulate(t, secs=0.8, seed=1, dec=64, obj=64, frq=64, amp=ENERGY_DEFAULT * 2 / 256.0):
    """the engine's model in float (sound.c), one hit: (peak, largest resonator |y|)"""
    rnd = random.Random(seed)
    noise = lambda: rnd.random() * 2.0 - 1.0
    _, _, _, snd, _, _, _, vary, (b0, b1, b2), res = INSTR[t]
    kind, n = KIND[t], len(res)
    sd32, thr, cg = sysdecay(t, dec) ** 32, nobjects(t, obj) / 1024.0, cgain(t, obj)
    fs = fscale(frq)
    a1 = [a1_of(f * fs, r) for f, r, _, _ in res]
    a2 = [r * r for _, r, _, _ in res]
    y1, y2 = [0.0] * n, [0.0] * n
    s1 = s2 = 0.0
    e = lvl = 0.0
    count = 0
    wf, wg = [res[k][0] * fs for k in range(n)], [0.0] * n
    pk = ymax = 0.0

    def drop():
        nonlocal lvl
        lvl = e
        j = rnd.randrange(3)
        base = res[1][0] * fs
        if j == 0 and wg[0] == 0.0:
            wf[0], wg[0] = base * (0.75 + 0.25 * noise()), abs(noise())
        elif j == 1 and wg[1] == 0.0:
            wf[1], wg[1] = base * (1.0 + 0.25 * noise()), abs(noise())
        elif wg[2] == 0.0:
            wf[2], wg[2] = base * (1.25 + 0.25 * noise()), abs(noise())

    if kind == RATCHET:
        count = ratchet_count(dec)
    else:
        e = min(e + 0.1 * amp, 1.0)
        if kind == WATER:
            drop()
    for i in range(int(secs * FS)):
        xs = [0.0] * n
        if kind == RATCHET:
            if count > 0:
                e -= RDELTA[t] + 0.002 * e
                if e < 0.0:
                    e = 1.0
                    count -= 1
                if rnd.random() < thr:
                    lvl += e * e
                x = lvl * noise() * e * amp
                xs = [x] * n
        else:
            live = e >= MIN_ENERGY
            if live and i % 32 == 0:
                e *= sd32
            if kind == WATER:
                if live and rnd.random() < nobjects(t, obj) / 32767.0:
                    drop()
                for k in range(3):
                    wg[k] *= res[k][1]
                    if wg[k] > 0.001:
                        wf[k] *= SWEEP
                        a1[k] = a1_of(wf[k], res[k][1])
                    else:
                        wg[k] = 0.0
                xs = [lvl * wg[k] for k in range(3)]
            elif live and rnd.random() < thr:
                lvl += e
                if kind == TUBES:
                    xs[rnd.randrange(7)] = lvl
                else:
                    xs = [lvl] * n
                for k in range(n):
                    if res[k][3]:
                        a1[k] = a1_of(res[k][0] * fs * (1.0 + vary * noise()), res[k][1])
        lvl *= snd
        s = 0.0
        for k in range(n):
            y = xs[k] * res[k][2] * cg - a1[k] * y1[k] - a2[k] * y2[k]
            y2[k], y1[k] = y1[k], y
            s += y
            ymax = max(ymax, abs(y))
        o = b0 * s + b1 * s1 + b2 * s2
        s2, s1 = s1, s
        pk = max(pk, abs(o))
    return pk, ymax


def table(name, ctype, rows, per_line=8):
    out = [f"static const {ctype} {name}[SK_N][128] = {{"]
    for t, row in enumerate(rows):
        out.append(f"    {{ /* {INSTR[t][0]} */")
        for i in range(0, 128, per_line):
            out.append("        " + ", ".join(str(x) for x in row[i:i + per_line]) + ",")
        out.append("    },")
    out.append("};")
    return "\n".join(out)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    out = sys.argv[1]
    with open(__file__, "rb") as f:
        digest = hashlib.sha256(f.read()).hexdigest()[:16]
    stamp = f"/* gen_shakers {digest} */"
    if os.path.exists(out):
        with open(out) as f:
            if f.readline().strip() == stamp:
                print("shakers: up to date")
                return
    q = lambda x, b: int(round(x * (1 << b)))
    sd32 = [[min(q(sysdecay(t, v) ** 32, 30), (1 << 30) - 1) for v in range(128)] for t in range(23)]
    thr = [[min(int(round(nobjects(t, v) * 64.0)), 65536) for v in range(128)] for t in range(23)]
    cg = [[q(cgain(t, v), 16) for v in range(128)] for t in range(23)]
    outg, report = [], []
    for t in range(23):
        peaks, ymax = [], 0.0
        for seed in range(1, 7):                 # the mean peak of six hits: the dice differ per hit
            p, y = simulate(t, seed=seed)
            peaks.append(p)
            ymax = max(ymax, y)
        if ymax >= 1000.0:                       # the engine's Q20 resonator state holds +-2048
            raise SystemExit(f"gen_shakers: {INSTR[t][0]}: |y| {ymax:.0f} exceeds the Q20 headroom")
        pk = sum(peaks) / len(peaks)
        g = SK_TARGET / 32768.0 / pk if pk > 0 else 1.0
        outg.append(q(min(g, 32767.0), 16))
        report.append(f"{INSTR[t][1]} {20 * math.log10(pk + 1e-12):+.0f}dB")
    lines = [
        stamp,
        "/* generated by tools/gen_shakers.py: STK Shakers data (Perry R. Cook, Gary P. Scavone; MIT-style licence,",
        " * LICENSES/STK.txt) as Percolator's fixed-point tables. Do not edit. */",
        "#define SK_N 23",
        "#define SK_MAXRES 7",
        "#define SK_TARGET %d" % SK_TARGET,
        "#define SK_MIN_ENERGY_Q24 %d" % q(MIN_ENERGY, 24),
        "#define SK_SWEEP_Q26 %d          /* WATER_FREQ_SWEEP - 1 = 0.0001, Q26 */" % q(SWEEP - 1.0, 26),
        "#define SK_WATER_MIN_Q30 %d   /* a drop below 0.001 has ended */" % q(0.001, 30),
        "enum { SK_GENERIC, SK_RATCHET, SK_WATER, SK_TUBES };",
        "static const char *const SK_NAME[SK_N] = {%s};" % ", ".join('"%s"' % i[0] for i in INSTR),
        "static const char *const SK_SHORT[SK_N] = {%s};" % ", ".join('"%s"' % i[1] for i in INSTR),
        "static const uint8_t SK_NRES[SK_N] = {%s};" % ", ".join(str(len(i[9])) for i in INSTR),
        "static const uint8_t SK_KIND[SK_N] = {%s};" % ", ".join(str(k) for k in KIND),
        "static const int32_t SK_SND[SK_N] = {%s};" % ", ".join(str(q(i[3], 15)) for i in INSTR),
        "static const int8_t SK_EQ[SK_N][3] = {%s};" % ", ".join("{%d, %d, %d}" % i[8] for i in INSTR),
        "static const int32_t SK_VARY[SK_N] = {%s};" % ", ".join(str(q(i[7], 16)) for i in INSTR),
        "static const int32_t SK_RDELTA[SK_N] = {%s};" % ", ".join(str(q(RDELTA.get(t, 0.0), 24)) for t in range(23)),
        "static const int32_t SK_OUTG[SK_N] = {%s};" % ", ".join(str(g) for g in outg),
        "typedef struct { uint32_t f; int32_t r, r2; int32_t g; uint8_t vary; } sk_res_t;",
        "static const sk_res_t SK_RES[SK_N][SK_MAXRES] = {",
    ]
    for i in INSTR:
        lines.append("    {%s}," % ", ".join("{%d, %d, %d, %d, %d}" % (q(f, 16), q(r, 30), q(r * r, 29), q(g, 16), v)
                                             for f, r, g, v in i[9]))
    lines += ["};",
              "static const int32_t SK_FSCALE[128] = {%s};" % ", ".join(str(q(fscale(v), 16)) for v in range(128)),
              "static const int32_t SK_COS[2049] = {",
              "\n".join("    " + ", ".join(str(q(math.cos(math.pi * i / 2048.0), 30)) for i in range(j, min(j + 8, 2049)))
                        + "," for j in range(0, 2049, 8)),
              "};",
              table("SK_SD32", "uint32_t", sd32),
              table("SK_THR", "uint32_t", thr),
              table("SK_CG", "uint32_t", cg)]
    with open(out, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("shakers: 23 instruments, mean peaks at defaults: " + " ".join(report))


if __name__ == "__main__":
    main()
