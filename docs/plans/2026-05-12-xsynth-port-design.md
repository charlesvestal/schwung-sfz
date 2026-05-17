# SFZ Module — sfizz → xsynth Port Design

**Date:** 2026-05-12
**Branch:** `xsynth-eval` (rename `xsynth-port` once Phase 1 lands)
**Status:** Phase 1 in progress.

## Why

Sustain-pedal-stacked chords on layered patches (WörliTzer, Stereo Rhodes,
BaltiWurli) glitch under sfizz on Move. Per-voice render is ~22 µs/voice; at
60+ active voices we exceed the 2900 µs SPI block budget.

Benchmarks on Move (Stereo Rhodes, 1-region patch):

| Voices | sfizz mean | xsynth mean | Speedup |
|--------|-----------|-------------|---------|
| 12 | 137 µs | 157 µs | 0.9× |
| 24 | 386 µs | 233 µs | 1.7× |
| 48 | 897 µs | 376 µs | 2.4× |
| 64 | 1173 µs | 392 µs | 3.0× |
| 96 | 1212 µs | 419 µs | 2.9× |

p99 at n=64: sfizz **1629 µs** vs xsynth **691 µs**. That's the difference
between glitching and not glitching during dense play.

xsynth uses aggressive SIMD and per-key parallelism. sfizz is single-voice,
biquad-per-sample. Their performance gap is structural, not tunable.

## Trade-off: feature loss

xsynth's SFZ subset is intentionally narrow (per maintainer design):

| Feature | sfizz | xsynth |
|---------|-------|--------|
| Sample mapping (key/vel ranges, RR) | full | yes |
| ADSR amp envelope | full | yes |
| Filter (cutoff, resonance, type) | full | yes |
| **`_oncc<N>` opcodes** | yes | **no** |
| LFOs, mod envelopes | yes | no |
| Effects (fverb, lpf_4p) | yes | no |
| `directtomain`/`fx1tomain` routing | yes | no |
| `lpf_2p`/`lpf_4p` filter types | yes | only generic `cutoff` |

Our DS converter relies heavily on `_oncc` for live knob → filter / amp / pan
modulation. Without it, knobs become **static at load time** — moving a knob
requires a preset reload (~2s).

## Upstream or fork?

**Schwung-only fork.** xsynth's stated design philosophy explicitly excludes
runtime modulation, mod LFOs, and generic CC-driven modulators ("would add
significant hot-path and binary-size cost"). Our DS-knob workflow is the kind
of feature they'd reject. We'd waste weeks negotiating.

LGPL-3.0 static-link is legal with source publication. schwung-sfz is open.
One LICENSE note in the module README covers compliance.

## Repo layout (post-port)

```
src/dsp/third_party/
  xsynth/           ← submodule, our fork if Phase 3 needs core changes
  xsynth_shim/      ← schwung-owned Rust crate, C ABI over xsynth-core
  sfizz/            ← removed on this branch
```

## Phased implementation

### Phase 1 — Engine swap, native `.sfz` only (1-2 days)

**Goal:** prove the swap works in the full plugin context, not just bench.

- Vendor `xsynth` submodule (done)
- Write `xsynth_shim` Rust staticlib exposing minimal C ABI (done)
- Update Dockerfile with Rust nightly + aarch64 cross (done)
- Rewrite `sfz_plugin.c`:
  - Replace `sfizz_*` calls with `xshim_*`
  - Strip DS conversion path (skip `.dspreset` files in scan)
  - Keep instrument-tree scan, preset list, L/R navigation
  - Keep `gain`, `octave_transpose`, `voices` params
  - Drop `knob_0..knob_15`, `sample_quality` (Phase 2)
- Update `scripts/build.sh` to build xsynth_shim then link into `dsp.so`
- Deploy, test with `StereoRhodes` and `Clean Fender`

**Decision gate:** if real-plugin CPU at n=64 isn't ≥2× better than sfizz, abort.

### Phase 2 — DS converter for xsynth subset (1-2 days)

- Rewrite `convert_dspreset_to_sfz` to emit xsynth-compatible SFZ:
  - Strip `_oncc<N>` opcodes
  - Strip `<effect type="reverb">` (use chain reverb instead)
  - Strip `<effect type="lowpass*">` opcodes for `directtomain`/`fx1tomain`
  - Convert `lpf_2p`/`lpf_4p` → generic `cutoff` (xsynth's only filter)
  - Bake knob `value=` defaults statically into per-region opcodes
  - Multi-group / round-robin: keep, xsynth handles basic SFZ structure
- WörliTzer-class patches load and play; knobs static
- Tests: `Cosmos`, `Raw Violin`, `WörliTzer`, `K4Coll-1.01`, `DS - The Synths`

### Phase 3 — Live CC modulation (3-7 days, risky)

Fork xsynth-core. Add a per-channel CC → parameter mapping mechanism:

```rust
// In xsynth-core channel state
struct CCBinding {
    cc: u8,
    target: CCTarget, // Cutoff, Amp, Pan
    delta_per_unit: f32,
    base_value: f32,
}

// At voice render: each voice samples channel's current CC values, applies
// to its cutoff/amp/pan inputs. Adds ~one float-mul per voice per CC binding.
```

DS converter (Phase 2) emits these bindings via shim API on load, replacing
the static bake-in.

**Risk fallback:** if Phase 3 stalls, knobs stay static. Acceptable — user
gets a one-time knob position per preset load, no live sweep.

**Alternative architecture (if per-region mutation is too invasive):** route
each DS group to a distinct MIDI channel; assign `cutoff` to that channel at
load; mutate channel's cutoff via existing channel-event surface. Smaller
xsynth-core change.

### Phase 4 — Polish (1-2 days)

- Voice limit: surface as plugin param, configure on channel group
- Preset reload memory mgmt: ensure no leak, no stale samples
- Sustain pedal CC64: confirm xsynth honors it; patch if not
- Crash safety: shim already wraps in `catch_unwind`, audit for missing cases
- 2s load time: investigate (likely sample decode, possibly parallelizable)

## Build pipeline changes

- Dockerfile: + rustup nightly + aarch64-unknown-linux-gnu (done)
- `scripts/build.sh`:
  - `cargo +nightly build --release --target aarch64-unknown-linux-gnu` in `xsynth_shim/`
  - Link `libxsynth_shim.a` into `dsp.so`
  - Skip sfizz CMake (Phase 1+)
- Deploy unchanged (`install.sh`)

## Distribution / licensing

- `LICENSE` of the module clarifies: xsynth is LGPL-3.0, statically linked;
  full source is at the schwung-sfz repo; user can rebuild `dsp.so` from
  source with modified xsynth per LGPL §4(d)
- xsynth-shim crate is BSD-2 (matches our module's existing license posture)

## Open questions

1. **xsynth voice-cap default ≈ 53** (visible as `active=53` in benchmark when
   asking for 64). Need to find the polyphony knob and surface it.
2. **WörliTzer.converted.sfz failed to load via xsynth** — confirms Phase 2
   is mandatory. Need to enumerate every opcode/effect we emit and audit.
3. **Sample format support** — xsynth uses symphonia for decoding. Does it
   handle 24-bit WAVs from DecentSampler libraries? Test before committing.
4. **Multi-output buses** — if any DS patches route to non-main bus, xsynth
   doesn't. Falls back to summing into main.

## Success criteria

- WörliTzer at 60+ voices sustains without audible glitch
- All converted native `.sfz` patches still load and play correctly
- Preset switch time ≤ 3s (vs sfizz's <1s; accept regression)
- No crashes on rapid preset changes or note bursts
- CPU at n=24 (steady-state) leaves headroom for other modules in the chain
