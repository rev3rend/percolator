/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 R. Luke DuBois */
/* The MIDItoSpeech sidecar (IDMNYU/synthaccess, MIDItoSpeech/devices, a JSON file) for what Percolator SENDS over
 * USB-MIDI with MIDI OUT on (GLOBAL 2), generated from the firmware's tables so it never drifts from them:
 *   CC 2 / 11 / 4 / 1   the selected track's energy, decay, objects, frequency (sound.c SP_CC), 0..127 as the screen
 *                       shows them ("value")
 *   Program Change      the pattern starting, 0..31, by its factory name in the reader's words (speak.c spk());
 *                       a pattern renamed on the unit is still read by its slot's factory name
 * Notes (each track's hit) and the clock are left out: MIDItoSpeech reads controls.
 *   cc -O1 -Ibuild/gen -Ifirmware/src -Ifirmware/app -Itests -o build/host/gen_synthaccess tools/gen_synthaccess.c -lm
 *   build/host/gen_synthaccess > docs/synthaccess/Percolator.json   (tests/synthaccess_check.py checks it) */
#include "host_platform.h"

static void put_str(const char *s)               /* a JSON string (the words are ASCII, no quotes or backslashes) */
{
    putchar('"');
    for (; *s; s++)
        putchar(*s == '"' || *s == '\\' ? '\'' : *s);
    putchar('"');
}

int main(void)
{
    static const uint8_t ORDER[4] = {SP_FRQ, SP_EN, SP_OBJ, SP_DEC};   /* by CC number: 1, 2, 4, 11 */
    char d[64], key[8];
    uint32_t i;
    host_boot();
    printf("{\n  \"device\": {\n");
    printf("    \"name\": \"Percolator\",\n    \"manufacturer\": \"R. Luke DuBois (custom firmware for the M-VAVE FM-1)\",\n");
    printf("    \"language\": \"en-US\",\n    \"version\": 1,\n");
    printf("    \"program_change\": {\n      \"0\": {\n        \"label\": \"pattern\",\n        \"data\": \"enum\",\n"
           "        \"enum\": \"patterns\"\n      }\n    },\n");
    printf("    \"CC\": {\n");
    for (i = 0; i < 4u; i++) {
        uint32_t p = ORDER[i];
        d[0] = 0;
        spk(d, sizeof d, SP_NAME[p]);
        fmt_int(key, SP_CC[p]);
        printf("      ");
        put_str(key);
        printf(": {\n        \"label\": ");
        put_str(d);
        printf(",\n        \"data\": \"value\"\n      }%s\n", i + 1u < 4u ? "," : "");
    }
    printf("    },\n    \"patterns\": [");
    for (i = 0; i < NPAT; i++) {                 /* Program Change 0..31: "1, samba" .. "25" (blank) */
        fmt_int(d, (int32_t)i + 1);
        if (i < FP_N) {
            cat(d, ", ", sizeof d);
            spk(d, sizeof d, FP_NAME[i]);
        }
        printf("%s", i ? ", " : "");
        put_str(d);
    }
    printf("]\n  }\n}\n");
    return 0;
}
