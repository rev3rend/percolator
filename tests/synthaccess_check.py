#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 R. Luke DuBois
"""docs/synthaccess/Percolator.json (the MIDItoSpeech sidecar) against the firmware: the file is the one
tools/gen_synthaccess.c makes now; the four CCs MIDI OUT sends (sound.c SP_CC) are there by name, read as the value
the screen shows; Program Change names the 32 pattern slots, the factory ones by name.
  tests/synthaccess_check.py build/host/gen_synthaccess docs/synthaccess/Percolator.json firmware/app/sound.c"""
import json
import re
import subprocess
import sys

gen, path, sound = sys.argv[1], sys.argv[2], sys.argv[3]
fresh = subprocess.run([gen], capture_output=True, text=True, check=True).stdout
committed = open(path).read()
failed = 0


def check(what, ok):
    global failed
    print(f"{what:<62} {'ok' if ok else 'FAIL'}")
    failed += not ok


check("the sidecar is current (as gen_synthaccess makes it now)", fresh == committed)
dev = json.loads(committed)["device"]
cc = [int(x) for x in re.search(r"SP_CC\[4\] = \{([^}]*)\}", open(sound).read()).group(1).split(",")]
check("the four CCs MIDI OUT sends, by name (2 11 4 1)",
      [dev["CC"].get(str(n), {}).get("label") for n in cc] == ["energy", "decay", "objects", "frequency"])
check("each read as the value the screen shows (0..127)", all(e["data"] == "value" for e in dev["CC"].values()))
check("Program Change: the 32 patterns, the factory ones by name",
      dev["program_change"]["0"]["enum"] == "patterns" and len(dev["patterns"]) == 32 and
      dev["patterns"][0] == "1, samba" and dev["patterns"][3] == "4, tambourine soul" and dev["patterns"][24] == "25")
check("words only (no symbols the reader's rules bar)",
      not re.search(r"[/|><*_=#\[\]{}~^@&$+]", " ".join(dev["patterns"] + [e["label"] for e in dev["CC"].values()])))
sys.exit(1 if failed else 0)
