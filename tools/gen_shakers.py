#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 R. Luke DuBois
#
# The instrument data below is transcribed from the Synthesis ToolKit in C++ (STK), src/Shakers.cpp:
#   Copyright (c) 1995-2023 Perry R. Cook and Gary P. Scavone, MIT-style licence (LICENSES/STK.txt).
"""Percolator's Shakers tables (run by tools/build.py generate()).

  gen_shakers.py OUT.h

STK's Shakers class (Perry Cook's PhISEM) for the 23 instruments, as data: per instrument the system decay, sound
decay, gain, number of objects, decay scale, frequency variation, equalizer and every resonance (frequency, radius,
gain). The firmware's engine reads these in fixed point:

  SK_SD32[t][v]  the system decay per 32-sample block (systemDecay^32), Q30, for DECAY v = 0..127 (STK CC 4:
                 base + 2 (v/128 - 0.5) decayScale (1 - base))
  SK_THR[t][v]   the chance of a collision per sample, in 1/65536 (STK: rand(1024) < nObjects), for OBJECTS v (CC 11:
                 nObjects = 2 v/128 baseObjects + 1.1)
  SK_CG[t][v]    currentGain * the first resonance's gain, Q16 (CC 11: log(nObjects) baseGain / nObjects)
  SK_A1[t][v]    the first resonance's -2 r cos(2 pi f / FS), Q29, for FREQ v (CC 1: f = base * 4^(v/128 - 0.5))
  SK_A2[t]       its r^2, Q29;  SK_SND[t] the sound decay, Q15;  SK_EQ[t] the equalizer's three taps (-1, 0, 1)
  SK_OUTG[t]     the output gain, Q16 from the engine's Q20 units to Q15: each instrument's peak at its default
                 parameters and ENERGY 100 lands near SK_TARGET, measured by simulating the engine here (fixed seed)

The skeleton's engine (firmware/app/sound.c) uses the first resonance only and treats Guiro, Wrench and Water Drops
as the generic model; milestone 2 (docs/SPEC.md) ports the full model from the same data (SK_RES).
"""
import math
import random
import sys

FS = 44100
SK_TARGET = 9000                 # Q15 peak per instrument at its defaults (~ -11 dBFS; 23 can sound at once)
ENERGY_DEFAULT = 100             # the kit's default ENERGY (0..127): STK amplitude 100 / 127

# name, short, system decay, sound decay, gain, objects, decay scale, vary factor, vary resonances?, equalizer,
# resonances [(freq, radius, gain)].  Guiro and Wrench have no system decay in STK (the ratchet replaces it; STK
# keeps the previous type's value): 0.999 here, and decay scale 0.95, for the skeleton's generic model.
MUG = [(2123, 0.997, 1.0), (4518, 0.997, 0.8), (8856, 0.997, 0.6), (10753, 0.997, 0.4)]
INSTR = [
    ("Maraca", "MAR", 0.999, 0.95, 4.0, 25, 0.97, 0.0, 0, (1, -1, 0), [(3200, 0.96, 1.0)]),
    ("Cabasa", "CAB", 0.997, 0.96, 8.0, 512, 0.97, 0.0, 0, (1, -1, 0), [(3000, 0.7, 1.0)]),
    ("Sekere", "SEK", 0.999, 0.96, 4.0, 64, 0.94, 0.0, 0, (1, 0, -1), [(5500, 0.6, 1.0)]),
    ("Tambourine", "TAM", 0.9985, 0.95, 1.0, 32, 0.95, 0.05, 1, (1, 0, -1),
     [(2300, 0.96, 0.1), (5600, 0.99, 0.8), (8100, 0.99, 1.0)]),
    ("Sleigh Bells", "SLB", 0.9994, 0.97, 1.0, 32, 0.9, 0.03, 1, (1, 0, -1),
     [(2500, 0.99, 1.0), (5300, 0.99, 1.0), (6500, 0.99, 1.0), (8300, 0.99, 0.5), (9800, 0.99, 0.3)]),
    ("Bamboo Chimes", "BMB", 0.9999, 0.9, 0.4, 1.2, 0.7, 0.2, 1, (1, 0, 0),
     [(2800, 0.995, 1.0), (0.8 * 2800, 0.995, 1.0), (1.2 * 2800, 0.995, 1.0)]),
    ("Sand Paper", "SND", 0.999, 0.999, 0.5, 128, 0.97, 0.0, 0, (1, 0, -1), [(4500, 0.6, 1.0)]),
    ("Coke Can", "COK", 0.999, 0.97, 0.5, 48, 0.95, 0.0, 0, (1, 0, -1),
     [(370, 0.99, 1.0), (1025, 0.992, 1.8), (1424, 0.992, 1.8), (2149, 0.992, 1.8), (3596, 0.992, 1.8)]),
    ("Sticks", "STX", 0.998, 0.96, 6.0, 2, 0.96, 0.0, 0, (1, 0, -1), [(5500, 0.6, 1.0)]),
    ("Crunch", "CRN", 0.99806, 0.95, 4.0, 7, 0.96, 0.0, 0, (1, -1, 0), [(800, 0.95, 1.0)]),
    ("Big Rocks", "BRK", 0.9965, 0.98, 4.0, 23, 0.95, 0.11, 1, (1, 0, -1), [(6460, 0.932, 1.0)]),
    ("Little Rocks", "LRK", 0.99586, 0.98, 4.0, 1600, 0.95, 0.18, 1, (1, 0, -1), [(9000, 0.843, 1.0)]),
    ("Next Mug", "MUG", 0.9995, 0.97, 0.8, 3, 0.95, 0.0, 0, (1, 0, -1), MUG),
    ("Penny + Mug", "PNY", 0.9995, 0.97, 0.8, 3, 0.95, 0.0, 0, (1, 0, -1),
     MUG + [(11000, 0.999, 1.0), (5200, 0.999, 0.8), (3835, 0.999, 0.5)]),
    ("Nickel + Mug", "NKL", 0.9995, 0.97, 0.8, 3, 0.95, 0.0, 0, (1, 0, -1),
     MUG + [(5583, 0.9992, 1.0), (9255, 0.9992, 0.8), (9805, 0.9992, 0.5)]),
    ("Dime + Mug", "DIM", 0.9995, 0.97, 0.8, 3, 0.95, 0.0, 0, (1, 0, -1),
     MUG + [(4450, 0.9993, 1.0), (4974, 0.9993, 0.8), (9945, 0.9993, 0.5)]),
    ("Quarter + Mug", "QTR", 0.9995, 0.97, 0.8, 3, 0.95, 0.0, 0, (1, 0, -1),
     MUG + [(1708, 0.9995, 1.0), (8863, 0.9995, 0.8), (9045, 0.9995, 0.5)]),
    ("Franc + Mug", "FRC", 0.9995, 0.97, 0.8, 3, 0.95, 0.0, 0, (1, 0, -1),
     MUG + [(5583, 0.9995, 0.7), (11010, 0.9995, 0.4), (1917, 0.9995, 0.3)]),
    ("Peso + Mug", "PES", 0.9995, 0.97, 0.8, 3, 0.95, 0.0, 0, (1, 0, -1),
     MUG + [(7250, 0.9996, 1.0), (8150, 0.9996, 1.2), (10060, 0.9996, 0.7)]),
    ("Guiro", "GUI", 0.999, 0.95, 0.4, 128, 0.95, 0.0, 0, (1, 0, -1), [(2500, 0.97, 1.0), (4000, 0.97, 1.0)]),
    ("Wrench", "WRN", 0.999, 0.95, 0.4, 128, 0.95, 0.0, 0, (1, 0, -1), [(3200, 0.99, 1.0), (8000, 0.992, 1.0)]),
    ("Water Drops", "WTR", 0.996, 0.95, 1.0, 10, 0.8, 0.0, 0, (-1, 0, 1),
     [(450, 0.9985, 1.0), (600, 0.9985, 1.0), (750, 0.9985, 1.0)]),
    ("Tuned Bamboo Chimes", "ANG", 0.9999, 0.95, 0.5, 1.2, 0.7, 0.0, 0, (1, 0, -1),
     [(f, 0.996, 1.0) for f in (1046.6, 1174.8, 1397.0, 1568, 1760, 2093.3, 2350)]),
]
assert len(INSTR) == 23
RATCHET = {19: 0.0001, 20: 0.00015}   # Guiro, Wrench: STK's baseRatchetDelta (milestone 2)
WATER = 21                            # sweeping resonators (milestone 2)
ANGKLUNG = 22                         # one tube per collision (milestone 2)
MIN_ENERGY = 0.001


def sysdecay(t, v):
    base, scale = INSTR[t][2], INSTR[t][6]
    return min(base + 2.0 * (v / 128.0 - 0.5) * scale * (1.0 - base), 0.999999)


def nobjects(t, v):
    return 2.0 * v / 128.0 * INSTR[t][5] + 1.1


def cgain(t, v):
    n = nobjects(t, v)
    return math.log(n) * INSTR[t][4] / n * INSTR[t][10][0][2]


def a1(t, v):
    f, r, _ = INSTR[t][10][0]
    f = min(f * 4.0 ** (v / 128.0 - 0.5), 0.49 * FS)
    return -2.0 * r * math.cos(2.0 * math.pi * f / FS)


def simulate(t, secs=0.6, seed=1):
    """the skeleton engine's model in float (sound.c, first resonance): the peak and the largest |y| of one hit"""
    rnd = random.Random(seed)
    e = 0.1 * ENERGY_DEFAULT / 127.0
    lvl = y1 = y2 = 0.0
    sd, snd, n, g = sysdecay(t, 64), INSTR[t][3], nobjects(t, 64), cgain(t, 64)
    aa1, aa2 = a1(t, 64), INSTR[t][10][0][1] ** 2
    b0, b1, b2 = INSTR[t][9]
    pk = ymax = 0.0
    for i in range(int(secs * FS)):
        x = 0.0
        if e >= MIN_ENERGY:
            if i % 32 == 0:
                e *= sd ** 32
            if rnd.random() * 1024.0 < n:
                lvl += e
                x = lvl
        lvl *= snd
        y = x * g - aa1 * y1 - aa2 * y2
        o = b0 * y + b1 * y1 + b2 * y2
        y2, y1 = y1, y
        pk, ymax = max(pk, abs(o)), max(ymax, abs(y))
    return pk, ymax


def table(name, ctype, rows, per_line=8):
    out = [f"static const {ctype} {name}[23][128] = {{"]
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
    q = lambda x, b: int(round(x * (1 << b)))
    sd32 = [[min(q(sysdecay(t, v) ** 32, 30), (1 << 30) - 1) for v in range(128)] for t in range(23)]
    thr = [[min(int(round(nobjects(t, v) * 64.0)), 65536) for v in range(128)] for t in range(23)]
    cg = [[q(cgain(t, v), 16) for v in range(128)] for t in range(23)]
    a1t = [[q(a1(t, v), 29) for v in range(128)] for t in range(23)]
    outg, report = [], []
    for t in range(23):
        pk, ymax = simulate(t)
        if ymax >= 1000.0:          # the engine's Q20 resonator state holds +-2048
            raise SystemExit(f"gen_shakers: {INSTR[t][0]}: |y| {ymax:.0f} exceeds the Q20 headroom")
        g = SK_TARGET / 32768.0 / pk if pk > 0 else 1.0
        outg.append(q(min(g, 32767.0), 16))
        report.append(f"{INSTR[t][1]} {20 * math.log10(pk + 1e-12):+.0f}dB")
    lines = [
        "/* generated by tools/gen_shakers.py: STK Shakers data (Perry R. Cook, Gary P. Scavone; MIT-style licence,",
        " * LICENSES/STK.txt) as Percolator's fixed-point tables. Do not edit. */",
        "#define SK_N 23",
        "#define SK_MAXRES 7",
        "#define SK_TARGET %d" % SK_TARGET,
        "#define SK_MIN_ENERGY_Q24 %d" % q(MIN_ENERGY, 24),
        "static const char *const SK_NAME[SK_N] = {%s};" % ", ".join('"%s"' % i[0] for i in INSTR),
        "static const char *const SK_SHORT[SK_N] = {%s};" % ", ".join('"%s"' % i[1] for i in INSTR),
        "static const uint8_t SK_NRES[SK_N] = {%s};" % ", ".join(str(len(i[10])) for i in INSTR),
        "static const int32_t SK_A2[SK_N] = {%s};" % ", ".join(str(q(i[10][0][1] ** 2, 29)) for i in INSTR),
        "static const int32_t SK_SND[SK_N] = {%s};" % ", ".join(str(q(i[3], 15)) for i in INSTR),
        "static const int8_t SK_EQ[SK_N][3] = {%s};" % ", ".join("{%d, %d, %d}" % i[9] for i in INSTR),
        "static const int32_t SK_OUTG[SK_N] = {%s};" % ", ".join(str(g) for g in outg),
        "/* every resonance (milestone 2): frequency Hz, radius and gain in Q16 */",
        "static const struct { uint16_t f; uint32_t r, g; } SK_RES[SK_N][SK_MAXRES] = {",
    ]
    for i in INSTR:
        lines.append("    {%s}," % ", ".join("{%d, %d, %d}" % (round(f), q(r, 16), q(g, 16)) for f, r, g in i[10]))
    lines += ["};",
              table("SK_SD32", "uint32_t", sd32),
              table("SK_THR", "uint32_t", thr),
              table("SK_CG", "uint32_t", cg),
              table("SK_A1", "int32_t", a1t)]
    with open(sys.argv[1], "w") as f:
        f.write("\n".join(lines) + "\n")
    print("shakers: 23 instruments, peaks at defaults: " + " ".join(report))


if __name__ == "__main__":
    main()
