# Decent Sampler coverage roadmap

**Date:** 2026-05-14
**Status:** Phase 3 (live `volume_oncc`) complete. This roadmap supersedes the
old "downstream chain modules handle effects" position — we'll host SFZ
effects, LFOs, and mod envelopes inside the SFZ module instead.

## Why the position change

The xsynth-upstream stance ("no runtime modulation, no in-engine effects") is
a project-policy decision, not a technical one. We've already forked and
landed live `_oncc` on volume. Doing the same for the rest of the
modulation/effects surface keeps preset-as-authored playback intact without
forcing users to manually rebuild each preset's effect chain in Move.

## Audit baseline (144 installed dspresets, excluding Salamander)

Binding parameters by frequency:
1. `FX_FILTER_FREQUENCY` 151 — static today, **need live**.
2. `FX_MIX` 148 — needs in-module effect hosting.
3. `FX_REVERB_WET_LEVEL` 143 — needs in-module reverb.
4. `ENV_ATTACK` 140 — static (ARIA bake) today.
5. `FX_FEEDBACK` 136 — needs delay/chorus.
6. `FX_MOD_RATE` 131 — needs LFO.
7. `VALUE` 130 — button-state bindings (tab + ENABLED toggle).
8. `FX_MOD_DEPTH` 130 — needs LFO depth.
9. `AMP_VOLUME` 110 — live at `level="group"` (Phase 3); `instrument`/`tag` not yet.
10. `FX_WET_LEVEL` 93 — needs effects.
11. `ENV_RELEASE` 82 — static (ARIA).
12. `FX_REVERB_*` 78–143 — needs reverb.
13. `FX_DELAY_TIME` 78 — needs delay.
14. `FX_FILTER_RESONANCE` 70 — static; need live.
15. `FX_STEREO_OFFSET` 65 — needs in-module stereo widener (or treat as no-op).
16. `FX_CENTER_FREQUENCY` 65 — needs bandpass filter type.
17. `ENABLED` 32 — button toggle gating groups.
18. `FREQUENCY` / `MOD_AMOUNT` 25 — LFO sources.
19. `PAN` 4 — static; need live.
20. `GROUP_TUNING` 12, `LEVEL` 7, `FX_IR_FILE` 4 — converter fixes / convolution.

Effect types: reverb 143, delay 93, chorus 82, phaser 65, lowpass 83 (✓),
lowpass_4pl 67 (✓), gain 14 (✓), convolution 1.

Group attrs: tab presence in 79/144 files; `tags=` 22; `groupTuning=` 13;
`enabled=` 6; `loopCrossfade*` a handful.

## Phased plan

Each phase ends with on-device smoke test + commit. Diagnostic
instrumentation re-introduced only if a phase causes regressions.

### Phase 4 — converter quick wins (~1 day)

Pure converter changes. No xsynth-core touches.

- `AMP_VOLUME` `level="instrument"`: emit `volume=` on `<global>` + matching
  `volume_oncc<CC>` on `<global>`. 51 binding instances.
- `groupTuning=` attribute: per-region `tune=` cents inside the affected
  group. 13 files.
- `enabled="false"` on `<group>`: emit `volume=-144` (silent) so the group
  effectively disappears. 6 files.
- `level="tag"` AMP_VOLUME with `<group tags="x">`: emit
  `volume_oncc<CC>=<delta>` on EVERY group bearing that tag. 22 files.

### Phase 5 — live `cutoff_oncc`, `resonance_oncc`, `pan_oncc` (~2 days)

Mirror of Phase 3. Parser → region → spawner Arc → SIMD generator that
multiplies/adds into the cutoff filter / resonance / pan stage.

- xsynth-soundfonts: `SfzOpcode::CutoffOncc(u8, f32)`,
  `ResonanceOncc(u8, f32)`, `PanOncc(u8, f32)`. Region builder stores each.
- xsynth-core: 3 new SIMD generators (or one parameterized over target);
  same `RECOMPUTE_INTERVAL` cadence as `SIMDVoiceOnccAmp`.
- DS converter: emit `cutoff_oncc<CC>=` for `FX_FILTER_FREQUENCY` knob
  bindings, `resonance_oncc<CC>=` for `FX_FILTER_RESONANCE`,
  `pan_oncc<CC>=` for `PAN`. Static baseline = knob-min endpoint.
- Plugin: same CC resend after apply (already in place).

Verification: Wörlitzer's "Filter" / "Reso" knobs sweep live.

### Phase 6 — `_curvecc` lookup tables (~2-3 days)

Fixes the dB-linear ≠ amp-linear taper mismatch on every live `_oncc`.
DS knobs use linear-amp translations; `volume_oncc` is linear in dB.
Result: knob feel is steep / different from authored.

- xsynth-soundfonts: parse `<base>_curvecc<N>` opcodes referencing a curve
  table; parse `<curve>` table definitions; map.
- xsynth-core: SIMD generators look up curve[cc] instead of raw `cc/127`.
- DS converter: emit a curve table per knob with the binding's translation
  pre-baked into 128 sample points. Emit `volume_curvecc<CC>=<curve_id>`
  alongside `volume_oncc<CC>=<full_swing>`.

Verification: WörliTzer mid-knob position matches authored loudness.

### Phase 7 — bandpass filter + tab/button gating (~2-3 days)

- **Bandpass**: converter emits `fil_type=bpf_2p` for `<effect type="bandpass">`.
  `FX_CENTER_FREQUENCY` binding hits cutoff. Already supported in xsynth.
- **Tab/button gating**: 79/144 files use `<tab>`. Buttons toggle effect
  chains via `ENABLED` bindings. Two-tier approach:
  - **Phase 7a (immediate, converter)**: bake the CURRENT button state
    into the converted SFZ — disabled groups get `volume=-144`. No live
    button switching; user reloads preset to change selection.
  - **Phase 7b (optional)**: assign synthetic CCs to button states, surface
    them as plugin params, emit `enabled_oncc<CC>` or equivalent. Live
    switching without reload.

### Phase 8 — loop crossfade (~2 days)

xsynth-core voice/sampler change. Interpolate samples across the loop
boundary using `loopCrossfade=<frames>`. Eliminates the click at loop
points on long-sustain instruments.

### Phase 9 — Delay effect (~1-2 days)

First in-module effect. Per-channel ring buffer with feedback + mix +
optional stereo offset.

- xsynth-core: new `effects::Delay` struct, time/feedback/mix params.
- ChannelEffectsChain that runs after `apply_channel_effects`.
- DS converter: `<effect type="delay">` → emit a delay-config block the
  channel processes; `FX_DELAY_TIME` / `FX_FEEDBACK` / `FX_MIX` / `FX_WET_LEVEL`
  knob bindings route to per-effect oncc opcodes.

### Phase 10 — Reverb effect (~2-3 days)

Algorithmic reverb. Either roll our own Freeverb-style (Schroeder allpass
+ comb filter banks) or vendor an MIT-licensed crate. Per-channel state.

- DS converter: `<effect type="reverb">` → reverb-config block.
  `FX_REVERB_*` knob bindings route appropriately.

### Phase 11 — LFOs + mod envelopes (~3-4 days)

The xsynth-upstream "no modulation" line ends here. Per-voice or per-channel
LFO sources, per-voice mod envelopes (reuse existing envelope code with a
different target hookup).

- New SIMD generator `SIMDLfo` (sine / triangle / square / saw, frequency
  in Hz, depth, target).
- Mod envelope: existing `SIMDVoiceEnvelope` shape, but output goes to a
  modulation target rather than directly multiplying amp.
- DS converter: parse `<modulators>` block, `<lfo>` elements, route their
  outputs to the same family of targets the static knobs hit
  (`FX_FILTER_FREQUENCY`, `PAN`, `AMP_VOLUME`, pitch).
- 13 FREQUENCY + 12 MOD_AMOUNT bindings activate.

### Phase 12 — Chorus + Phaser (~3 days)

Depends on Phase 11. Both are LFO-modulated effects.

- Chorus: short variable delay with LFO-modulated delay time. ~1 day.
- Phaser: 4–6 cascaded all-pass filters with LFO-modulated center.
  ~1–2 days.

### Phase 13 — Stereo widener / misc (~1 day)

`FX_STEREO_OFFSET` (65 bindings) typically just widens stereo image.
Simple haas-effect or M/S widener.

## Out of scope (still)

- Upstream xsynth maintainer concerns — we're a fork.
- Generic CC / aftertouch / pitch-wheel as SF2 modulator sources — orthogonal.
- DS `<button>` UI styling, image themes — pure visuals.
- **Convolution effect** (`<effect type="convolution">`). Only 1 of 144
  installed presets uses it (`curly - electric piano`, likely a cabinet
  IR). The preset will sound drier than authored; rest of the library is
  unaffected. Revisit only if a higher-value preset shows up needing it.

## Cumulative coverage estimate

After Phases 4–12 land, ~93% of binding-parameter instances become live.
Remaining ~7% is mostly TEXT (pure UI labels — no DSP impact),
FX_IR_FILE/convolution (4 bindings, 1 preset — explicitly skipped), and a
long tail of niche opcodes.

## Use external libraries where they fit

The fork is LGPL-3.0. Compatible for static link: MIT, Apache 2.0, BSD,
ISC, LGPL. Incompatible: GPL-only (e.g., CloudSeed), AGPL.

### Promising candidates

**`fundsp`** — MIT/Apache 2.0. Rust audio-DSP library by Sami Perttu.
Provides reverb (Freeverb-style), delay, chorus, phaser, filters, LFOs,
envelopes via a composable signal graph. Per-channel post-mix effects map
well onto it. Could supply Phases 9, 10, 12 nearly turn-key.

Concerns to validate before depending on it:
- not SIMD-vectorized; effects are sample-by-sample. Acceptable for a
  per-channel post-mix stage (block size 128 × 1 channel), worse if we
  ever needed it in the per-voice hot path.
- pulls a moderate dependency tree (numeric-array, generic-array).
  Inspect transitively for binary-size impact on Move.
- API style is functional graphs, somewhat unlike our existing SIMD chain.

**`freeverb` crate** — MIT, minimal Freeverb implementation. Drop-in for
Phase 10 if `fundsp` is too heavy.

**`rustfft`** — already in our transitive deps (symphonia uses it).
Free for Phase 13 (convolution).

**`biquad`** — already in our deps (xsynth uses it for filters). Good
starting point for any extra EQ/filter stages we'd need.

### Evaluation plan

Before starting Phase 9 (Delay) we'll prototype:
1. `fundsp::reverb` integration: dependency size, block-render cost on
   Move (target: < 200 µs per render block for a 4-channel reverb send),
   API ergonomics inside our channel.read_samples post-stage.
2. If size or cost is unacceptable: fall back to a ~300-line hand-rolled
   Freeverb + delay + chorus stack. Phaser is small enough to hand-roll
   regardless (4–6 allpass filters).

### Roll-our-own zone

- **LFOs and mod envelopes** stay custom — they're per-voice in the SIMD
  chain, and our existing `SIMDVoiceEnvelope` can be reused with a
  different target hookup. External crates don't fit cleanly there.
- **Per-voice modulation routing** (which LFO/envelope drives which target)
  is matrix shape, not a library problem.
- **DS-specific opcode parsing and converter logic** — entirely our code.

## Open questions

1. **Effect hosting placement**: per-channel inside xsynth-core, or a new
   `effects/` crate in the schwung fork? Per-channel inside xsynth fits the
   existing render path but couples effects to xsynth's release cycle.
2. **LFO scope**: per-voice (each note's own LFO) vs per-channel (one shared
   LFO). DS specs typically per-channel. Per-voice would be more flexible
   but uses more CPU.
3. **Curve table cap**: SFZv2 allows up to 256 curves per file. Our converter
   would emit ~16 (one per knob); fine within any reasonable limit.
4. **Whether `fundsp` is the right anchor**: validate via the Phase 9
   prototype before depending on it.
