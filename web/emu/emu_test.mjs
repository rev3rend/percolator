// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 M-Vave-OP1 contributors
// The browser emulator's WebAssembly without a browser: boot, the screen drawn, a key sounds and lights its LED, a
// button lights its LED, MIDI IN sounds, the render keeps up with real time by a wide margin. Percolator: a 1.2 s
// splash first; the pads are percussion (a hit decays on its own, no note-off); the pad pressed becomes the selected
// track, whose LED stays lit; the speed check hits all 23 pads every ~23 ms (the engine's worst case).
//   node web/emu/emu_test.mjs build/emu/emu.wasm
import { readFileSync } from "node:fs";

let failed = 0;
const ok = (c, what) => { console.log(`${what.padEnd(60)} ${c ? "ok" : "FAIL"}`); if (!c) failed++; };

const { instance } = await WebAssembly.instantiate(readFileSync(process.argv[2] || "build/emu/emu.wasm"), {});
const ex = instance.exports;
ex._initialize();
ex.web_boot();
const N = 128;
const render = (quanta) => {
  let peak = 0;
  for (let q = 0; q < quanta; q++) {
    ex.web_render(N);
    const l = new Float32Array(ex.memory.buffer, ex.web_out_l(), N);
    for (const v of l) peak = Math.max(peak, Math.abs(v));
  }
  return peak;
};
const screen = () => new Uint16Array(ex.memory.buffer, ex.web_screen(), 240 * 240);
const leds = () => new Uint8Array(ex.memory.buffer, ex.web_leds(), 41);

render(20);
const fb = screen(), bg = fb[0];
ok(fb.some((c) => c !== bg), "boot: the splash drawn");
ok(ex.web_sample_rate() === 44100, "sample rate 44100");
ok(render(50) === 0, "silent with nothing held");
render(460);                                 // past the splash (1.2 s)

ex.web_keys(1 << 2);                         // G3: Sekere
const p = render(40);
ok(p > 0.02 && p <= 0.9, `a pad sounds (peak ${p.toFixed(3)})`);
ok(leds()[16] === 1, "a pad lights its LED");
ex.web_keys(0);
render(400);                                 // (~1.2 s: rung out)
ok(render(20) === 0, "released: decays to silence");
ok(leds()[16] === 1 && leds()[14] === 0, "the pad played is the selected track: its LED stays lit");

ex.web_buttons(1 << 10);                     // PLAY (an empty pattern: silent)
render(20);
ok(leds()[ex.web_button_id(10)] === 1, "PLAY held lights its LED");
ex.web_buttons(0);
render(120);                                 // (past the 300 ms double-tap window)
ex.web_buttons(1 << 10);                     // and again: stop
render(20);
ex.web_buttons(0);
render(20);

const draws = ex.web_screen_draws();
ex.web_enc(3, 2);                            // KNOB 1
render(20);
ok(ex.web_screen_draws() > draws, "a knob turned redraws");

ex.web_midi(0x90, 53, 100);                  // track 1, Maraca
ok(render(50) > 0.02, "MIDI note-on sounds");
render(400);
ok(render(20) === 0, "MIDI: decays to silence");

const t0 = performance.now();
for (let q = 0; q < 44100 / N * 5; q++) {    // 5 s with the main loop, all 23 pads hit every 8 quanta
  if (q % 8 === 0) ex.web_keys(0x7fffff);
  if (q % 8 === 4) ex.web_keys(0);
  render(1);
}
const rt = 5000 / (performance.now() - t0);
ok(rt > 20, `5 s rendered at ${rt.toFixed(0)}x real time (> 20x)`);

console.log(failed ? `EMU TEST FAILED (${failed})` : "emu test passed");
process.exit(failed ? 1 : 0);
