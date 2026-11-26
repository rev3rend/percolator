/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 R. Luke DuBois */
/* Percolator's Shakers engine (firmware/app/sound.c, fixed point) against STK's own Shakers (build/host/stk_ref.txt
 * from tests/shakers_ref.cpp): every instrument, at its defaults and with DECAY, OBJECTS and FREQ moved, measured the
 * same way (tests/shk_metrics.h, the means over many hits): the spectrum's centroid, the decay time to -30 dB and
 * the level (the engine's output taken back to STK's units through SK_OUTG). Where the port departs from STK on
 * purpose (docs/SPEC.md), the case is shown but not compared: FREQ on the varying resonances and on Water (STK undoes
 * the control at the next collision), and Water's decay and centroid (STK cuts every sound when the energy falls
 * below 0.001, ~25 ms for Water, before a drop's upward sweep; here the drops ring out). Water's level is compared.
 * The same cut shortens STK's long resonances (the coins) when DECAY is low: where STK's decay reaches the time its
 * energy takes to fall to 0.001 (stk_cut), ours may be longer, never shorter.
 *   build/host/shakers_test build/host/stk_ref.txt */
#include "host_platform.h"
#include "shk_metrics.h"

#define HITS 32
#define CENT_TOL 0.15            /* the centroid within 15 % */
#define DECAY_TOL 0.25           /* the decay within 25 % (or a frame and a bit) */
#define LEVEL_DB 3.0             /* the level within 3 dB */

static int failed;
static void check(const char *what, int ok)
{
    printf("%-64s %s\n", what, ok ? "ok" : "FAIL");
    failed += !ok;
}

static double ref[23][SHK_NCASE][4];   /* centroid, decay, peak, hits */

static double stk_cut(uint32_t t, uint32_t dec)  /* s until STK's energy (0.1 x amplitude) falls below 0.001 */
{
    double sd = pow(SK_SD32[t][dec] / 1073741824.0, 1.0 / 32.0);
    return log(0.1 * SHK_AMP / 0.001) / -log(sd) / SHK_FS;
}

static int load_ref(const char *path)
{
    FILE *f = fopen(path, "r");
    int t, c, hits, silent, n = 0;
    double cen, dec, pk;
    if (!f)
        return 0;
    while (fscanf(f, "%d %d %d %d %lf %lf %lf", &t, &c, &hits, &silent, &cen, &dec, &pk) == 7)
        if (t >= 0 && t < 23 && c >= 0 && c < SHK_NCASE) {
            ref[t][c][0] = cen;
            ref[t][c][1] = dec;
            ref[t][c][2] = pk;
            ref[t][c][3] = hits;
            n++;
        }
    fclose(f);
    return n == 23 * SHK_NCASE;
}

static void render_case(uint32_t t, uint32_t c, shk_acc *acc)   /* our engine, one voice, HITS hits */
{
    static double x[SHK_LEN];
    static int32_t mix[2u * CTL];
    uint32_t h, i, b;
    double scale = 65536.0 / SK_OUTG[t] / 32768.0;             /* Q15 out -> STK's float units */
    if (SK_KIND[t] == SK_RATCHET)
        scale /= SHK_AMP;                                     /* (STK ignores the amplitude: ours scales the scrape) */
    kit.t[t].p[SP_EN] = 100;
    kit.t[t].p[SP_DEC] = (uint8_t)SHK_PARAM[c][0];
    kit.t[t].p[SP_OBJ] = (uint8_t)SHK_PARAM[c][1];
    kit.t[t].p[SP_FRQ] = (uint8_t)SHK_PARAM[c][2];
    for (h = 0; h < (uint32_t)HITS; h++) {
        voice_silence(t);
        snd.v[t].seed = 0x2545F491u * (1000u * t + 17u * c + h + 1u);
        snd_hit(t, 200);                                      /* ENERGY 100 */
        for (b = 0; b < SHK_LEN / CTL; b++) {
            for (i = 0; i < 2u * CTL; i++)
                mix[i] = 0;
            if ((snd.active >> t) & 1u)
                voice_render(t, mix, CTL, 32767, 32767);
            for (i = 0; i < CTL; i++)
                x[b * CTL + i] = mix[2u * i] * (32768.0 / 32767.0) * scale;
        }
        for (i = b * CTL; i < SHK_LEN; i++)
            x[i] = 0.0;
        shk_add(acc, x);
    }
}

int main(int argc, char **argv)
{
    uint32_t t, c;
    int all_cent = 1, all_dec = 1, all_lvl = 1, quiet_ok = 1, cut = 0;
    if (!load_ref(argc > 1 ? argv[1] : "build/host/stk_ref.txt")) {
        puts("shakers: no STK reference (run build/host/shakers_ref > build/host/stk_ref.txt)");
        return 1;
    }
    host_boot();
    printf("  %-20s %-11s %15s %13s %13s\n", "", "", "centroid Hz", "decay s", "level dB");
    printf("  %-20s %-11s %7s %7s %6s %6s %6s\n", "instrument", "case", "STK", "ours", "STK", "ours", "diff");
    for (t = 0; t < NT; t++)
        for (c = 0; c < SHK_NCASE; c++) {
            static shk_acc acc;
            double cen, dec, lvl, rc = ref[t][c][0], rd = ref[t][c][1], rl = ref[t][c][2], db;
            int compared = !(c == SHK_FREQ_HI && (SK_VARY[t] || SK_KIND[t] == SK_WATER));
            int water = SK_KIND[t] == SK_WATER;       /* (its decay and centroid differ on purpose) */
            int ok_c, ok_d, ok_l;
            memset(&acc, 0, sizeof acc);
            render_case(t, c, &acc);
            cen = shk_centroid(&acc);
            dec = acc.hits ? acc.decay_sum / acc.hits : 0.0;
            lvl = acc.hits ? acc.peak_sum / acc.hits : 0.0;
            db = rl > 0.0 && lvl > 0.0 ? 20.0 * log10(lvl / rl) : 99.0;
            ok_c = water || fabs(cen - rc) <= CENT_TOL * rc;
            ok_d = water || fabs(dec - rd) <= DECAY_TOL * rd + 0.015 ||
                   (SK_KIND[t] != SK_RATCHET && rd >= 0.8 * stk_cut(t, (uint32_t)SHK_PARAM[c][0]) && dec > rd);
            if (ok_d && !water && fabs(dec - rd) > DECAY_TOL * rd + 0.015)
                cut++;
            ok_l = fabs(db) <= LEVEL_DB;
            printf("  %-20s %-11s %7.0f %7.0f %6.3f %6.3f %+6.1f %s\n", SK_NAME[t], SHK_CASE[c], rc, cen, rd, dec, db,
                   !compared ? "(not compared: FREQ on varied frequencies, on purpose)"
                   : !(ok_c && ok_d && ok_l) ? "  <--" : water ? "(level only: STK cuts the drops)"
                   : fabs(dec - rd) > DECAY_TOL * rd + 0.015 ? "(longer: STK cuts the ringing)" : "");
            if (acc.silent)
                quiet_ok = 0;
            if (!compared)
                continue;
            all_cent &= ok_c;
            all_dec &= ok_d;
            all_lvl &= ok_l;
        }
    check("every instrument and case: the spectrum's centroid as STK's (15 %)", all_cent);
    check("every instrument and case: the decay time as STK's (25 %)", all_dec);
    check("every instrument and case: the level as STK's (3 dB)", all_lvl);
    check("every hit sounds (Water too: every hit starts with a drop)", quiet_ok);
    printf("  (%d cases ring longer than STK where STK cuts the sound at energy 0.001)\n", cut);
    {   /* the FREQ fix: moving FREQ moves the varying resonances too */
        static shk_acc lo, hi;
        memset(&lo, 0, sizeof lo);
        memset(&hi, 0, sizeof hi);
        render_case(4, SHK_DEFAULT, &lo);
        render_case(4, SHK_FREQ_HI, &hi);
        check("Sleigh Bells: FREQ 112 raises the varied bells (STK would not)", shk_centroid(&hi) > 1.4 * shk_centroid(&lo));
    }
    puts(failed ? "SHAKERS TEST FAILED" : "shakers test passed");
    return failed ? 1 : 0;
}
