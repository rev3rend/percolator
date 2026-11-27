// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 R. Luke DuBois
// The reference for tests/shakers_test.c: STK's own Shakers (tests/stk/, unmodified; Perry R. Cook and Gary P.
// Scavone, MIT-style licence) renders every instrument, HITS hits per case (each a fresh instrument and its own rand
// seed, as STK's collisions are dice), measured by tests/shk_metrics.h, written as one line per instrument and case:
//   type case hits silent centroid_hz decay_s mean_peak
// Guiro and Wrench: STK adds one ratchet sweep per noteOn (and spends the first at once: it expects CC 2 scraping);
// Percolator gives a hit 1 + DECAY x 7 / 127 sweeps, so STK gets that many noteOns.
//   c++ -O2 -Itests/stk -Itests tests/shakers_ref.cpp tests/stk/Shakers.cpp tests/stk/Stk.cpp -o build/host/shakers_ref
//   build/host/shakers_ref > build/host/stk_ref.txt
#include "Shakers.h"
#include <cstdio>
#include <cstdlib>
extern "C" {
#include "shk_metrics.h"
}
using namespace stk;

int main()
{
    static double buf[SHK_LEN];
    Stk::setSampleRate(SHK_FS);
    for (int t = 0; t < 23; t++)
        for (int c = 0; c < SHK_NCASE; c++) {
            static shk_acc acc;
            memset(&acc, 0, sizeof acc);
            int hits = t == 21 ? shk_hits(t) * 3 : shk_hits(t);   // Water: two hits in three are silent in STK
            for (int h = 0; h < hits; h++) {
                srand(1000u * t + 17u * c + h + 1u);
                Shakers s(t);
                int ons = t == 19 || t == 20 ? 1 + SHK_PARAM[c][0] * 7 / 127 : 1;
                s.controlChange(11, SHK_PARAM[c][0]);           // system decay: CC 11 in STK's code (its comment
                s.controlChange(4, SHK_PARAM[c][1]);            // says 4); number of objects: CC 4 (comment: 11)
                s.controlChange(1, SHK_PARAM[c][2]);
                for (int k = 0; k < ons; k++)                    // (noteOn picks the type from the note)
                    s.noteOn(220.0 * pow(2.0, (t - 57) / 12.0), SHK_AMP);
                for (int i = 0; i < SHK_LEN; i++)
                    buf[i] = s.tick();
                shk_add(&acc, buf);
            }
            printf("%d %d %d %d %.1f %.4f %.6g\n", t, c, acc.hits, acc.silent, shk_centroid(&acc),
                   acc.hits ? acc.decay_sum / acc.hits : 0.0, acc.hits ? acc.peak_sum / acc.hits : 0.0);
        }
    return 0;
}
