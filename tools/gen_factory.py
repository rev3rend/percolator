#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 R. Luke DuBois
"""Percolator's factory content (run by tools/build.py generate()).

  gen_factory.py OUT.h

16 patterns (P01..P16), 8 kits (K1..K8) and 4 songs (S1..S4) in four styles, two kits and four patterns each:
hand percussion, ambient, metal and industrial, generative. Written here as readable step strings, expanded by the
firmware into its pattern_t / kit_t / song_t (firmware/app/data.c factory_*) when a slot holds no saved record.

Step strings (one character a step; the string's length is the track's LENGTH):
  .  off            x  trig           X  accented trig
  o  trig, 50 %     q  trig, 25 %     p  trig, 75 %
  r  ratchet x2     R  ratchet x3 DOWN      u  ratchet x4 UP
  c  condition 1:2  C  condition 2:2  h  condition 1:4  f  FILL   n  NOT FILL   1  FIRST
  e  nudged early (-2/12)             l  nudged late (+2/12)
A track's options: div (DIV_NAME index: 0 1/1, 1 1/2, 2 1/4, 3 1/8, 4 2/1, 5 4/1) and locks {step: (E, D, O, F)}
(None: no lock for that parameter).
"""
import sys

# the tracks (STK Shakers types)
MAR, CAB, SEK, TAM, SLB, BMB, SND, COK, STX, CRN, BRK, LRK, MUG, PNY, NKL, DIM, QTR, FRC, PES, GUI, WRN, WTR, ANG = range(23)
NFX = 14
FX = dict(DTIME=0, DFB=1, DTONE=2, DLVL=3, RSIZE=4, RDAMP=5, RLVL=6, CHORUS=7, FILT=8, RES=9, DRIVE=10, MASTER=11,
          FPOS=12, CRATE=13)
FX_DEFAULT = dict(DTIME=3, DFB=35, DTONE=60, DLVL=0, RSIZE=50, RDAMP=50, RLVL=0, CHORUS=0, FILT=50, RES=0, DRIVE=0,
                  MASTER=80, FPOS=0, CRATE=40)
DLY = {"1/32": 0, "1/16": 1, "1/16D": 2, "1/8": 3, "1/8D": 4, "1/4": 5, "1/4D": 6, "1/2": 7}

# ------------------------------------------------------------------ kits
# tracks: {track: (ENERGY, DECAY, OBJECTS, FREQ, LEVEL, PAN)}; the rest at (100, 64, 64, 64, 80, 50)
KITS = [
    ("STUDIO", {MAR: (100, 64, 64, 70, 70, 40), CAB: (95, 64, 64, 64, 62, 62), SEK: (90, 64, 64, 64, 60, 30),
                TAM: (100, 64, 64, 64, 70, 58), SLB: (90, 64, 64, 64, 50, 66), STX: (110, 64, 64, 70, 75, 50),
                CRN: (120, 50, 64, 40, 82, 50), DIM: (100, 64, 64, 64, 55, 72), GUI: (100, 40, 64, 64, 65, 35)},
     dict(RSIZE=35, RDAMP=60, RLVL=25)),
    ("CLUB", {MAR: (100, 64, 64, 82, 70, 38), CAB: (100, 64, 70, 70, 60, 64), SEK: (95, 64, 64, 70, 62, 28),
              TAM: (110, 60, 64, 70, 72, 60), SLB: (90, 64, 64, 70, 48, 70), STX: (115, 64, 64, 76, 74, 50),
              CRN: (127, 44, 64, 36, 86, 50), DIM: (105, 64, 64, 70, 58, 74), GUI: (100, 32, 64, 70, 62, 34)},
     dict(DTIME=DLY["1/8D"], DFB=30, DTONE=50, DLVL=25, RSIZE=30, RLVL=20, CHORUS=15, CRATE=40, DRIVE=15, FPOS=1)),
    ("RAIN ROOM", {WTR: (100, 70, 64, 64, 85, 50), SND: (70, 90, 64, 56, 55, 36), LRK: (60, 64, 64, 64, 50, 64),
                   BRK: (80, 64, 64, 40, 50, 46), BMB: (90, 64, 64, 64, 60, 70), SEK: (70, 64, 64, 56, 40, 30)},
     dict(DTIME=DLY["1/4"], DFB=45, DTONE=35, DLVL=30, RSIZE=80, RDAMP=40, RLVL=55, FILT=40, RES=10, MASTER=95)),
    ("GLASS GARDEN", {ANG: (115, 64, 64, 64, 95, 50), BMB: (110, 64, 64, 70, 90, 34), SLB: (100, 64, 64, 64, 80, 66),
                      DIM: (105, 70, 64, 90, 75, 24), PES: (105, 70, 64, 80, 75, 76), MAR: (90, 64, 64, 90, 85, 50),
                      CAB: (100, 64, 64, 80, 80, 60), SEK: (95, 64, 64, 80, 75, 40)},
     dict(DTIME=DLY["1/8D"], DFB=50, DTONE=70, DLVL=35, RSIZE=70, RLVL=50, CHORUS=45, CRATE=30, MASTER=100)),
    ("WORKSHOP", {COK: (115, 64, 64, 40, 85, 50), WRN: (100, 48, 64, 64, 70, 66), PNY: (100, 64, 64, 64, 60, 30),
                  NKL: (90, 56, 64, 72, 60, 70), QTR: (100, 64, 64, 64, 55, 40), MUG: (100, 64, 64, 64, 65, 58),
                  CRN: (120, 50, 64, 44, 72, 50), DIM: (100, 64, 64, 64, 58, 60), PES: (95, 64, 64, 64, 52, 44)},
     dict(DRIVE=25, FPOS=0, RSIZE=20, RLVL=20)),
    ("FOUNDRY", {COK: (127, 70, 64, 28, 88, 50), MUG: (110, 90, 64, 30, 70, 40), QTR: (110, 70, 64, 30, 65, 62),
                 FRC: (100, 70, 64, 34, 60, 30), WRN: (100, 64, 64, 40, 72, 70), NKL: (100, 60, 64, 36, 60, 56),
                 PES: (100, 64, 64, 30, 58, 44), CRN: (127, 60, 64, 30, 76, 50)},
     dict(FILT=30, RES=60, DRIVE=60, FPOS=1, DTIME=DLY["1/16"], DFB=20, DLVL=20, RSIZE=40, RLVL=25)),
    ("CLOCKWORK", {STX: (115, 64, 64, 80, 85, 50), CRN: (115, 56, 64, 56, 85, 40), BRK: (105, 64, 64, 64, 75, 62),
                   LRK: (95, 64, 64, 64, 75, 30), DIM: (105, 64, 64, 76, 70, 72), ANG: (100, 64, 64, 64, 70, 56)},
     dict(DTIME=DLY["1/16D"], DFB=35, DTONE=60, DLVL=25, RSIZE=30, RLVL=20, MASTER=100)),
    ("DRIFT", {WTR: (100, 70, 64, 56, 70, 40), ANG: (90, 70, 64, 56, 65, 60), BMB: (90, 64, 64, 56, 60, 30),
               SLB: (80, 64, 64, 56, 45, 70), CRN: (100, 56, 64, 40, 60, 50), SND: (70, 80, 64, 56, 45, 60)},
     dict(CHORUS=35, CRATE=15, DTIME=DLY["1/4D"], DFB=65, DTONE=40, DLVL=40, RSIZE=85, RLVL=55, FILT=45)),
]
assert len(KITS) == 8

# --------------------------------------------------------------- patterns
# (name, tempo BPM, swing %, kit 1..8, [(track, steps, options)])
def T(track, steps, **opt):
    return (track, steps, opt)

PATTERNS = [
    # hand percussion (K1 STUDIO, K2 CLUB)
    ("SAMBA", 104, 52, 1, [T(TAM, "x.xXx.xXx.xXx.xX"), T(MAR, "XxxxXxxxXxxxXxxx"), T(STX, "x.x.xx.x.x.xx.x."),
                           T(CRN, "....X.......X..."), T(GUI, "x.......x......."), T(CAB, "..x...x...x...x.")]),
    ("SON CLAVE", 96, 56, 1, [T(STX, "x..x..x...x.x..."), T(MAR, "x.X.x.X.x.X.x.X."), T(GUI, "X..xX..xX..xX..x"),
                              T(CAB, "....x.......x..x"), T(CRN, "...X..x....X..x."), T(DIM, "x...x...x...x.x.")]),
    ("WEST AFRICAN BELL", 112, 50, 2, [T(DIM, "X.X.XX.X.X.X"), T(SEK, "XxxXxxXxxXxx"), T(CRN, "X.....x....."),
                                       T(STX, "..x..x..x..x"), T(MAR, "xoxxoxxoxxox"), T(TAM, ".....X.....X")]),
    ("TAMBOURINE SOUL", 92, 60, 2, [T(CRN, "X.....x.X......."), T(TAM, "....X.......X..."),
                                    T(MAR, "xoxoxoxoxoxoxoxo"), T(STX, "..........x...x."), T(GUI, "x..............."),
                                    T(SLB, "c..............."), T(CAB, "..............f.")]),
    # ambient (K3 RAIN ROOM, K4 GLASS GARDEN)
    ("RAIN", 76, 50, 3, [T(SND, "xoxoxoxo", div=1), T(LRK, "qqqqqqqqqqqqqqqq"), T(WTR, "x...c....o..c..."),
                         T(BRK, "............q..."), T(SEK, "o.o.o.o.o.o.o.o."), T(BMB, "c.......h.......")]),
    ("GLASS GARDEN", 84, 50, 4, [T(ANG, "x..o..x..o..o..."), T(BMB, "x...o...", div=1), T(SLB, "........u......."),
                                 T(DIM, "c.....c.....c..."), T(PES, "x..o...")]),
    ("TIDE", 70, 50, 3, [T(WTR, "x.o.."), T(BRK, "x...o.."), T(SND, "x..o..x..o.", div=1),
                         T(LRK, "q.q.q.q.q.q.q"), T(BMB, "c........")]),
    ("NIGHT INSECTS", 96, 50, 4, [T(MAR, "pqpqpqpqpqpqpqpq", div=4, locks={0: (80, None, None, 110)}),
                                  T(CAB, "..o...o...o...o."), T(SEK, "o.o.o.o.o.o.o.o."), T(ANG, "...c.......c...."),
                                  T(SLB, "q...............")]),
    # metal and industrial (K5 WORKSHOP, K6 FOUNDRY)
    ("WORKSHOP", 110, 50, 5, [T(COK, "X.....x...x.X..."), T(WRN, "....x.......x..."), T(NKL, "xxrxxxrxxxrxxxrx"),
                              T(PNY, "..x...x...x...x."), T(CRN, "....X.......X..."), T(MUG, ".......c.......f")]),
    ("COINS", 100, 50, 5, [T(QTR, "x..x..x.x.x.x.x.", locks={0: (None, None, None, 40), 3: (None, None, None, 64),
                                                            6: (None, None, None, 80), 8: (None, None, None, 64),
                                                            10: (None, None, None, 90), 12: (None, None, None, 72),
                                                            14: (None, None, None, 56)}),
                           T(DIM, "....x.....x.....", locks={4: (None, None, None, 96), 10: (None, None, None, 84)}),
                           T(PNY, "x.......x......."), T(MUG, "....x.......x..."), T(COK, "X.......X.......")]),
    ("FACTORY FLOOR", 120, 54, 6, [T(COK, "X..x..X...x.X..."), T(QTR, "..R...R...R...R."), T(WRN, "........x......."),
                                   T(MUG, "x.x.x.x.x.x.x.x."), T(CRN, "....X.......X..."), T(FRC, "x....o....o.")]),
    ("CLANK", 128, 50, 6, [T(NKL, "oooooooooooooooo", div=4), T(WRN, "............ffff"), T(COK, "X...X...X...X..."),
                           T(PES, "..x...x...x...x.", locks={2: (None, None, None, 30), 6: (None, None, None, 50),
                                                         10: (None, None, None, 30), 14: (None, None, None, 70)}),
                           T(QTR, "x.....x.....")]),
    # generative (K7 CLOCKWORK, K8 DRIFT)
    ("POLYMETER", 112, 50, 7, [T(STX, "X.x.."), T(CRN, "X..x..."), T(BRK, "x...x..x..."), T(DIM, "x.....x....x."),
                               T(LRK, "o.o.o.o.o.o.o.o.")]),
    ("PROBABILITY FIELD", 104, 50, 7, [T(STX, "oooooooooooooooo"), T(CRN, "X...o...X...o..."),
                                       T(LRK, "qqqqqqqqqqqqqqqq"), T(ANG, "q...q...q...q..."), T(BRK, "..o...o...o...o."),
                                       T(DIM, "q.q.q.q.q.q.q.q.")]),
    ("CONDITIONS", 90, 50, 8, [T(WTR, "x...c...C...h..."), T(ANG, "c..C..c..C..h..."), T(BMB, "....x.......h..."),
                               T(SLB, "f.......f......."), T(CRN, "X.......X......."), T(SND, "xoxoxoxoxoxoxoxo")]),
    ("CLOCKWORK DRIFT", 100, 50, 8, [T(STX, "x.e.x.l.x.e.x.l.", div=4), T(CRN, "X..x..x.", div=1),
                                     T(ANG, "x..o.."), T(DIM, "x...l...e."), T(WTR, "....c.......c...")]),
]
assert len(PATTERNS) == 16

SONGS = [  # (name, [(pattern 1.., repeats)], loop)
    ("HAND TOUR", [(1, 2), (2, 2), (3, 2), (4, 2)], 1),
    ("AMBIENT TOUR", [(5, 2), (6, 2), (7, 2), (8, 2)], 1),
    ("METAL TOUR", [(9, 2), (10, 2), (11, 2), (12, 2)], 1),
    ("GENERATIVE TOUR", [(13, 2), (14, 2), (15, 2), (16, 2)], 1),
]

COND = {"c": 1, "C": 2, "h": 6, "f": 10, "n": 11, "1": 12}


def step_bytes(ch):
    """one step character -> (f, prob, rat) as data.c's step_t (prob: the chance taken away, 5 % a unit)"""
    f, prob, rat = 1, 0, 0
    if ch == ".":
        return None
    if ch == "X":
        f |= 2
    elif ch == "o":
        prob = 10
    elif ch == "q":
        prob = 15
    elif ch == "p":
        prob = 5
    elif ch == "r":
        rat = 1
    elif ch == "R":
        rat = 2 | 2 << 2
    elif ch == "u":
        rat = 3 | 1 << 2
    elif ch in COND:
        f |= COND[ch] << 2
    elif ch == "e":
        rat = (-2 & 15) << 4
    elif ch == "l":
        rat = 2 << 4
    elif ch != "x":
        raise SystemExit(f"gen_factory: unknown step character {ch!r}")
    return f, prob, rat


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    hdr, trk, steps, idx = [], [], [], [0]
    for name, bpm, swing, kit, tracks in PATTERNS:
        if not 40 <= bpm <= 240 or not 50 <= swing <= 75 or not 1 <= kit <= 8:
            raise SystemExit(f"gen_factory: {name}: tempo, swing or kit out of range")
        hdr.append((bpm * 2, swing, kit))
        tl = [(16, 0)] * 23
        seen = set()
        for t, s, opt in tracks:
            if t in seen or not 1 <= len(s) <= 16:
                raise SystemExit(f"gen_factory: {name}: track {t} twice or a length outside 1..16")
            seen.add(t)
            tl[t] = (len(s), opt.get("div", 0))
            locks = opt.get("locks", {})
            for k, ch in enumerate(s):
                b = step_bytes(ch)
                lk = locks.get(k)
                if b is None and lk is None:
                    continue
                f, prob, rat = b if b else (0, 0, 0)
                l4 = [0, 0, 0, 0] if lk is None else [0 if v is None else v + 1 for v in lk]
                steps.append((t, k, f, prob, rat, *l4))
        trk.append(tl)
        idx.append(len(steps))
    lines = ["/* generated by tools/gen_factory.py: Percolator's factory patterns, kits and songs. Do not edit. */",
             "#define FP_N %d" % len(PATTERNS), "#define FK_N %d" % len(KITS), "#define FS_N %d" % len(SONGS),
             "static const char *const FP_NAME[FP_N] = {%s};" % ", ".join('"%s"' % p[0] for p in PATTERNS),
             "static const char *const FK_NAME[FK_N] = {%s};" % ", ".join('"%s"' % k[0] for k in KITS),
             "static const char *const FS_NAME[FS_N] = {%s};" % ", ".join('"%s"' % s[0] for s in SONGS),
             "static const uint16_t FP_HDR[FP_N][3] = {%s};  /* tempo x 2, swing, kit 1.. */" %
             ", ".join("{%d, %d, %d}" % h for h in hdr),
             "static const uint8_t FP_TRK[FP_N][23][2] = {  /* length, divide */"]
    for tl in trk:
        lines.append("    {%s}," % ", ".join("{%d, %d}" % x for x in tl))
    lines += ["};", "static const uint16_t FP_IDX[FP_N + 1] = {%s};" % ", ".join(map(str, idx)),
              "static const uint8_t FP_STEP[][9] = {  /* track, step, f, prob, rat, locks x 4 */"]
    for s in steps:
        lines.append("    {%s}," % ", ".join(map(str, s)))
    lines += ["};", "static const uint8_t FK_TRK[FK_N][23][6] = {  /* E, D, O, F, level, pan */"]
    fx_rows = []
    for name, tracks, fx in KITS:
        row = [tracks.get(t, (100, 64, 64, 64, 80, 50)) for t in range(23)]
        lines.append("    {%s}," % ", ".join("{%d, %d, %d, %d, %d, %d}" % r for r in row))
        f = dict(FX_DEFAULT, **fx)
        fx_rows.append([f[k] for k, _ in sorted(FX.items(), key=lambda kv: kv[1])])
    lines += ["};", "static const uint8_t FK_FX[FK_N][%d] = {%s};" % (NFX, ", ".join(
        "{%s}" % ", ".join(map(str, r)) for r in fx_rows)),
              "static const uint8_t FS_LOOP[FS_N] = {%s};" % ", ".join(str(s[2]) for s in SONGS),
              "static const uint8_t FS_ENT[FS_N][8][2] = {%s};  /* pattern 1.. (0: end), repeats */" % ", ".join(
                  "{%s}" % ", ".join("{%d, %d}" % e for e in (s[1] + [(0, 0)] * (8 - len(s[1])))) for s in SONGS)]
    with open(sys.argv[1], "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"factory: {len(PATTERNS)} patterns ({len(steps)} steps), {len(KITS)} kits, {len(SONGS)} songs")


if __name__ == "__main__":
    main()
