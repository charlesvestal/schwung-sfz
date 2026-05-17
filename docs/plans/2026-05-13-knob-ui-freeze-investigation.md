# 2026-05-13 — Knob UI exposure freeze: evidence + plan

## Symptom

When `xsynth_plugin.c`'s `get_param("chain_params")` and `get_param("ui_hierarchy")` return per-preset knob entries (key/name/min/max varying with the loaded `.dspreset`), the Move device freezes. Reverting to a static fixed JSON (4 fixed params, no knobs) avoids the freeze. SCHED_FIFO removal already fixed the device-wide freeze on preset switch — this is a separate freeze, triggered by the JSON shape itself.

## Evidence collected (host side)

All references are to the menu-style-v2 worktree of the schwung host.

### Buffer sizes are NOT the limit

- `shadow_constants.h:72` — `SHADOW_PARAM_VALUE_LEN = 65536`.
- `shadow_chain_mgmt.h:48` — `chain_params_cache[65536]` per master-FX slot.
- `chain_host.c:7425` — modulation-refresh `buf[32768]` for synth dlsym call.
- `chain_host.c:139` — `MAX_CHAIN_PARAMS = 256`.

The OLD sfizz output (4 fixed + 8 knob slots, ~600 bytes) and our worst-case xsynth output (4 + 16 knobs, ~1200 bytes) are both comfortably inside every buffer. **Buffer size is not the cause.**

### Synth chain_params is parsed ONCE from `module.json`

- `chain_host.c:5084` — `parse_chain_params(synth_path, inst->synth_params, ...)` at v2 synth load.
- The host's `inst->synth_params` cache is the STATIC module.json shape, never refreshed on preset switch.
- Dynamic `get_param("synth:chain_params")` is only consumed by:
  - `chain_mod_refresh_target_param_cache` (modulation), `chain_host.c:7422`
  - the shadow proxy forwarding the JSON to the JS UI

This means the host's modulation/param lookup logic is happy with the static `module.json` shape regardless of what the plugin returns dynamically. The dynamic shape only matters for the JS UI.

### UI re-fetches `chain_params` on every preset change

- `shadow_ui.js:7542-7548` — on hierarchy-editor preset change:
  ```js
  hierEditorChainParams = getComponentChainParams(hierEditorSlot, hierEditorComponent);
  invalidateKnobContextCache();
  ```
- `getComponentChainParams` (line 3128) does `JSON.parse(getSlotParam(slot, "synth:chain_params"))`.
- The cached `hierEditorChainParams` array drives the encoder-row knob bindings.
- `invalidateKnobContextCache` clears `cachedKnobContexts` so the next paint rebuilds them from the new array (line 7935-7945).

When the SHAPE of `chain_params` changes between preset switches (e.g. preset A exposes 4 knobs, preset B exposes 7), the UI's cached knob bindings tied to slot indices become inconsistent with the new array length.

### OLD sfizz plugin's working pattern

`sfz_plugin.c:2564-2599` — the working pattern that did NOT freeze:

- `chain_params` returns **fixed shape**: 4 built-in params + **exactly 8 knob slots** (always).
- Unused knob slots are filled with placeholder entries:
  ```c
  ",{\"key\":\"knob_%d\",\"name\":\"—\",\"type\":\"float\",\"min\":0,\"max\":1,\"default\":0,\"step\":0.02}"
  ```
- Used knob slots also use **normalized 0..1 range** with `step=0.02`:
  ```c
  ",{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"float\",\"min\":0,\"max\":1,\"default\":%g,\"step\":0.02,\"unit\":\"%%\"}"
  ```
  The plugin maps `0..1 → DS range` internally inside `set_param` before sending the CC.
- `ui_hierarchy.levels.root.knobs` is `["knob_0", "knob_1", ..., "knob_7"]` (also fixed).
- Only `ui_hierarchy.levels.root.params` grows per preset (extra knob entries with proper `min`/`max`/`label` for the params menu).

**Key insight**: the OLD plugin's `chain_params` SHAPE is invariant across preset switches. Only labels (`name`) and per-knob `default` change. The UI never sees `chain_params.length` change between presets.

## Hypotheses

### H1 (highest confidence) — Shape change confuses UI knob-binding cache

The JS knob-context cache (`shadow_ui.js:7935`) is indexed by knob slot position. When `chain_params.length` shrinks (e.g. preset A has 7 knobs → preset B has 4), the array index that used to map to a real knob now maps to undefined / off the end of the array. If any code path reads past the end without bounds-checking, it loops or throws; if the UI repeatedly retries the failing render, the device appears frozen.

The OLD plugin sidestepped this by always declaring 12 entries.

### H2 — Synchronous dlsym call from UI thread blocking on heavy plugin work

When the audio thread is mid-`rebuild_matrix` (20-40 ms), a synchronous `get_param("chain_params")` could be serialized behind it if there is any shared serialization in the shadow proxy. Less likely after SCHED_FIFO removal (kernel can preempt) but possible if the proxy queue is FIFO per slot.

### H3 — Modulation refresh thrashing on shape change

`chain_mod_refresh_target_param_cache` (`chain_host.c:7422`) is called only when modulation lookups happen. If shape changes invalidate something deeper, it could keep refreshing. Unlikely to be the primary cause but worth checking.

H1 is strongly supported by the OLD plugin's stable-shape pattern working and our dynamic-shape pattern failing. H2/H3 are weaker; remain as backup hypotheses if H1 fix doesn't resolve.

## Recommended plan

### Phase 1 — Adopt the OLD sfizz fixed-shape pattern (next session)

Low risk, follows proven working precedent.

1. **`xsynth_plugin.c` `get_param("chain_params")`**: emit fixed 4 + 8 knob slots, padding unused with `name:"—"` placeholders. Use normalized 0..1 / step=0.02 for all knob entries.
2. **`set_param("knob_N", "0.75")`**: existing code already routes to CC; verify it maps 0..1 → DS range correctly (or do the mapping in plugin if it currently expects raw DS units).
3. **`get_param("knob_N")`**: return current fraction in 0..1, not raw DS value.
4. **`get_param("ui_hierarchy")`**:
   - `knobs`: fixed `["knob_0"..."knob_7"]` (or `DS_KNOB_LIVE_COUNT-2 = 6` to match comment in CLAUDE.md, leaving room for octave + gain in encoder row).
   - `params`: fixed built-in entries + per-knob entries (this part is OK to be dynamic per OLD plugin precedent — it only affects params menu, not encoder row bindings).
5. **`module.json` chain_params**: declare matching 4 + 8 fixed slots so the static host cache (`inst->synth_params`) lines up with the dynamic shape.
6. **State save/restore**: knob values as 0..1 fractions (already done in OLD plugin pattern).

Test sequence:
1. Switch between presets with varying knob counts (0, 4, 7, 8). Confirm no freeze.
2. Confirm encoder-row knobs change their displayed label when switching presets.
3. Confirm a real knob (Cosmos has multiple) modulates audio when turned.

### Phase 2 — Confirm with instrumentation (only if Phase 1 still freezes)

Add timing logs in plugin's `get_param("chain_params")` and `get_param("ui_hierarchy")` writing to `/data/UserData/schwung/tmp/xsynth_debug.log`:
- Length of returned JSON
- Timestamp + preset name

Compare freeze occurrences against these logs. If the freeze happens when JSON shape changes (length differs from previous call), H1 confirmed at the boundary.

If freeze persists with stable shape: instrument host side in `chain_host.c:7905` (the synth `chain_params` handler) and in `shadow_ui.js` `getComponentChainParams` to find the actual point of stall.

### Phase 3 — Future enhancement (after Phase 1 stable)

- Live CC modulation for AMP_VOLUME, cutoff, etc. inside xsynth (currently knobs only fire at preset-load time, no per-CC re-eval of region opcodes).
- Move `rebuild_matrix` off the audio thread to a worker (perf, not freeze — currently a one-time 20-40ms hit on preset switch).

## Open questions to answer in next session

1. Does our current `set_param("knob_N", ...)` accept raw DS values or 0..1 fractions? (CLAUDE.md says raw — needs adjustment for OLD pattern.)
2. Is `DS_KNOB_LIVE_COUNT` (currently 8 per CLAUDE.md, minus 2 for octave/gain → 6 knobs in encoder row) the right number to expose to the encoder row, or should we match OLD plugin's 8?
3. Does module.json need a static 12-slot declaration even though the plugin returns dynamic JSON? (Modulation lookup uses module.json shape; if a future user wires modulation to a knob, the static metadata must include it.)

## Files of interest

- Plugin: `src/dsp/xsynth_plugin.c` — `v2_get_param` (line ~700), `v2_set_param` (knob_N routing)
- Plugin: `src/dsp/dspreset_to_xsynth_sfz.c` — `enumerate_ui_knobs` (CC assignment, knob metadata)
- Plugin: `src/module.json` — needs static 4 + 8 chain_params entries
- Host (reference only — do NOT edit): `chain_host.c:7905-7958`, `shadow_chain_mgmt.c:2481-2506`, `shadow_ui.js:3127-3144, 7542-7548, 7935-7945`
- OLD working code: `src/dsp/sfz_plugin.c:2564-2710` — copy this pattern verbatim
