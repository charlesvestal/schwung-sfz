# Phase 3 retry — live `volume_oncc` for WörliTzer's silent layers

**Date:** 2026-05-13
**Status:** Phase 3 WIP is stashed in both repos (`stash@{0}`).
**Prerequisite:** pre-existing audio-thread freeze on preset switch must be
  understood. Instrumentation is deployed; waiting on a captured repro.

## Why we're retrying

Phase 3 (commit goal: WörliTzer's silent layers respond to encoder knobs
live, no preset reload) was attempted as one large change touching:

- xsynth-soundfonts parser (`volume_oncc<N>` recognition)
- xsynth-core `VoiceSpawner` trait signature (added `cc_state` arg)
- xsynth-core `VoiceChannel.cc_state: Arc<[AtomicU8; 128]>`
- new `SIMDVoiceOnccAmp` SIMD generator
- DS converter (`volume_oncc<CC>=<dB-delta>` per group, silent-endpoint
  baseline)
- plugin CC resend after `xshim_load_apply`

The single-step landing was followed by repeated audio-thread freezes on
preset switching. **A diagnostic bisect (removing only the SIMD generator
insertion) showed the freeze pre-existed** the Phase 3 work — but trying
to debug Phase 3 simultaneously with the freeze made it impossible to
attribute. We reverted everything and went back to known-good, then added
heartbeat instrumentation.

## What we keep, what we change

Phase 3 design itself is sound. The retry changes only the **delivery**:

- Small, separately-verifiable landings.
- Each landing gets its own commit + on-device smoke test before the next.
- "Smoke test" = load WörliTzer + switch presets at least 5 times without
  freeze (the freeze is non-deterministic but typically appears within a
  handful of switches on the WIP build).

## Landing sequence

### Step 0 — Wait for freeze instrumentation to catch a repro

The pre-existing freeze is the highest-priority risk. If it triggers
during a Phase 3 step, attribution becomes muddy again. The deployed
instrumentation will tell us:

- Which post-apply block hangs (16-block force-log window after each apply)
- Voice count at the freezing block (was a voice just spawned?)
- `MemAvailable` at the freezing block (memory pressure?)
- Whether `render_before` fires without `render_after` (freeze inside
  `xshim_render`) or neither fires (host stopped calling us)

When the freeze is understood and fixed, proceed.

### Step 1 — Plugin-only: CC resend after apply

**Files:** `src/dsp/xsynth_plugin.c` only.

After `xshim_load_apply` succeeds, iterate `inst->knobs[]` and call
`xshim_cc(inst->synth, 0, k->cc_number, cc_val)` for each, using
`inst->knob_current[i]` mapped to 0..127.

**Effect on existing builds:** none — these CC events get stored in
xsynth-core's channel control state (CC7 lerps, CC10 lerps, etc.); for
unknown CCs (102..117 we allocate) they hit the `_ => {}` arm and do
nothing. Zero runtime impact.

**Why first:** if Phase 3 ever lands properly, this is the piece that
brings the silent layers to their **authored** start position on fresh
load (not just on state restore). Independently useful even without
live modulation.

**Verify:** switch presets repeatedly with WörliTzer. No regression.
Sound should be identical to today's known-good behavior (the inserted
CC events are accepted by xsynth but have no consumers yet).

### Step 2 — Parser + region: recognize `volume_oncc<N>`

**Files:**
- `src/dsp/third_party/xsynth/soundfonts/src/sfz/parse.rs`
- `src/dsp/third_party/xsynth/soundfonts/src/sfz/region.rs`

Add `SfzOpcode::VolumeOncc(u8, f32)`. Route `<base>_oncc<N>` parser so
`base_name == "volume"` returns `VolumeOncc(cc, db)`. Add
`RegionParams.volume_oncc: Vec<(u8, f32)>` populated by
`RegionParamsBuilder.update_from_flag`.

**Effect on existing builds:** none — `RegionParams.volume_oncc` is a
new field that no code reads. Compiler may warn about unused field —
add `#[allow(dead_code)]` on the field if needed.

**Verify:** load WörliTzer, verify it sounds identical to Step 1.
Switch presets to confirm no new freeze cause.

### Step 3 — DS converter: emit `volume_oncc<CC>` (alongside static volume)

**Files:** `src/dsp/dspreset_to_xsynth_sfz.c`.

Update `binding_param_is_live` to include `AMP_VOLUME` / `TAG_VOLUME`.
Document-order iteration in `apply_ui_overrides` so `knob_idx` aligns
with `out_knobs[]`. For `AMP_VOLUME` / `TAG_VOLUME` at group level,
emit `volume_oncc<knob_cc>=<dB-delta>` per group AND keep the static
`volume=<dB-at-knob's-current-position>` baseline (NOT silent endpoint
— that's Step 5).

**Effect on existing builds:** SFZ files now include `volume_oncc<N>`
opcodes. xsynth's parser sees them as `VolumeOncc(cc, db)` (from
Step 2), stores on `RegionParams`. Nothing consumes the field — runtime
behavior identical to before.

**Verify:** inspect a converted SFZ, confirm `volume_oncc` lines
present. Audio should be identical to Step 2 (static knob position
still drives the static `volume=` line). Switch presets.

### Step 4 — Core: `CcState` + atomic store on Raw CC (no consumer yet)

**Files:**
- `src/dsp/third_party/xsynth/core/src/voice.rs` (add `CcState` type,
  `new_cc_state()`)
- `src/dsp/third_party/xsynth/core/src/channel/mod.rs` (add
  `VoiceChannel.cc_state: CcState`, initialize in `new`)
- `src/dsp/third_party/xsynth/core/src/channel/control.rs` (store
  `cc_state[cc].store(val, Relaxed)` on every `ControlEvent::Raw`)

**Do NOT add yet:** `VoiceSpawner` trait signature change. No voices
read `cc_state`. The atomic store is the only new instruction in the
event-drain path; it should add ~1 ns per CC.

**Verify:** switch presets repeatedly. Confirm no new freeze cause.
Confirm CC7 / CC10 / etc. still work normally.

### Step 5 — Core: `SIMDVoiceOnccAmp` generator + spawn-time wiring

**Files:**
- `src/dsp/third_party/xsynth/core/src/voice/oncc_amp.rs` (new — the
  SIMD generator that polls atomic CC state with `RECOMPUTE_INTERVAL=8`
  counter; static_amp=true when bindings empty short-circuits all atomic
  loads)
- `src/dsp/third_party/xsynth/core/src/voice.rs` (mod decl + export)
- `src/dsp/third_party/xsynth/core/src/soundfont/mod.rs`
  (`SampleVoiceSpawnerParams.volume_oncc: Arc<[(u8, f32)]>`, populate
  from `region.volume_oncc`)
- `src/dsp/third_party/xsynth/core/src/soundfont/voice_spawners/{stereo,mono}.rs`
  (capture `volume_oncc` into spawner, insert `apply_volume_oncc` stage
  in chain — always present, `static_amp` short-circuits when empty)
- Both spawner impls of `VoiceSpawner::spawn_voice` and the calling
  chain (`channel/key.rs`, `channel/channel_sf.rs`, `channel/voice_spawner.rs`,
  `channel/mod.rs::drain_events_with_budget`) take `cc_state: &CcState`.

This is the **largest** step. It's the one we can't break further.

**Key invariant:** when `volume_oncc` bindings are empty (which is the
case for ALL non-DS-converted SFZ files, plus SF2), `SIMDVoiceOnccAmp`
runs `static_amp=true` and never touches an atomic on the hot path. Cost
is one extra SIMD multiply per sample-width — same order as the existing
velocity stage.

**Verify:**
- Switch presets repeatedly. No freeze (assuming Step 0 is resolved).
- Native SFZ (Stereo Rhodes, Clean Fender): identical sound to Step 4.
- WörliTzer: still silent layers (Step 3 still emits static volume
  baseline at knob's authored position — no live modulation yet because
  baseline doesn't bracket the full range).
- Render perf: heartbeat `render_us` should be within 10% of Step 4
  numbers for the same voice count.

### Step 6 — DS converter: switch to silent-endpoint baseline

**Files:** `src/dsp/dspreset_to_xsynth_sfz.c`.

Change static `volume=` emission for `AMP_VOLUME`/`TAG_VOLUME` groups
from "dB at knob's current position" to "dB at knob's MIN position".
Emit `volume_oncc<CC>=<dB-at-max - dB-at-min>` (full sweep delta).

The plugin's Step 1 CC resend pushes the knob's current-position CC,
which via `SIMDVoiceOnccAmp` adds the right delta to recover the
authored amp. Voices sound correct on load.

**Verify:**
- WörliTzer: layers audible at fresh load (knob CC resend brings them
  up from the silent baseline).
- Move a layer knob: amp changes in real time, no preset reload.
- Switch presets repeatedly: no freeze.

### Step 7 — Cleanup

- Remove the diagnostic instrumentation added during the freeze hunt
  (the post-apply 16-block force-log, `MemAvailable` snapshots).
- Document the live-CC support surface in `CLAUDE.md`:
  - "`volume_oncc<N>` honored, sampled per voice every ~46 samples"
  - "Other `_oncc` targets remain static-at-load via ARIA baker"
- Bump xsynth-fork submodule pointer in parent repo to the new fork commit.
- Release notes for the parent repo.

## Risk register

| Risk | Mitigation |
|---|---|
| Pre-existing freeze recurs and gets attributed to Phase 3 | Step 0 — fix the pre-existing freeze first. |
| `Arc<[AtomicU8; 128]>` allocation per channel adds memory overhead | One Arc per channel, ~128 bytes. Negligible. |
| Trait signature change breaks downstream xsynth crates | We don't use realtime/render/kdmapi crates from xsynth. Schwung-only. |
| Per-voice extra SIMD stage measurably slows render | Bench at Step 5. If >10% regression, profile and look for inlining hints. |
| Atomic poll cadence too coarse → audible stepping on knob sweep | `RECOMPUTE_INTERVAL=8` × `S::Vf32::WIDTH=4` × `1/44100` ≈ 0.7 ms granularity. Imperceptible. If audible, reduce to 4. |
| Knob's CC value at preset load doesn't match its `value=` attribute | Step 1's resend converts knob fraction → CC linearly. Matches converter's static `volume=` math. |

## Open questions for future work (not in this plan)

- `cutoff_oncc<N>` for live filter sweeps (mirror of `volume_oncc` but
  multiplies into the voice's cutoff parameter).
- `pan_oncc<N>` similarly.
- Knobs that bind to multiple parameters across different generators
  (currently only one `_oncc` target per knob is honored).
- DS `<button>` toggle states (separate effort; not knob-shaped).
