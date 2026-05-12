/*
 * DecentSampler `.dspreset` → xsynth-compatible SFZ converter.
 *
 * xsynth doesn't honor `_oncc` opcodes, mod LFOs, or routed effect buses, so
 * the converter resolves every UI-knob `value=` statically into the target
 * opcode at conversion time and skips effect block emission entirely.
 *
 * Knobs that move filter cutoff, group amp, or ADSR all turn into fixed
 * opcodes baked into the SFZ. The user can still see and adjust them in the
 * params menu, but a change requires a preset reload to take effect.
 *
 * Returns a malloc'd path to a temp `.converted.sfz` file, or NULL on error.
 * Caller frees the path. The temp file is left on disk so xsynth can load it
 * normally; it's overwritten on the next preset switch.
 */
#ifndef DSPRESET_TO_XSYNTH_SFZ_H
#define DSPRESET_TO_XSYNTH_SFZ_H

#ifdef __cplusplus
extern "C" {
#endif

#define DS_MAX_KNOBS 16

/* One UI knob from the dspreset, exposed for the plugin to surface as a
 * parameter. The plugin maps set_param("knob_<idx>", t01) to
 * xshim_cc(synth, 0, cc_number, t01 * 127) so xsynth's per-region
 * `_oncc<N>` bindings (emitted by the converter) move the targeted
 * parameter. `current` holds the position the knob loaded with (0..1) so
 * the plugin can echo the dspreset's authored default. */
typedef struct {
    char key[16];           /* "knob_0".."knob_15" */
    char label[32];         /* DS label (with fallbacks) */
    double current_t01;     /* current live position 0..1 */
    double dspreset_t01;    /* dspreset-time default (frozen) */
    int cc_number;          /* synthetic MIDI CC (102..117) */
} ds_xsynth_knob_t;

/* Converts a .dspreset to an xsynth-compatible SFZ. Writes a list of UI
 * knobs that need to be exposed to the host. Returns a malloc'd path to
 * the temp .converted.sfz, or NULL on error. Caller frees the path. */
char *convert_dspreset_to_xsynth_sfz(
    const char *path,
    ds_xsynth_knob_t knobs_out[DS_MAX_KNOBS],
    int *knob_count_out
);

#ifdef __cplusplus
}
#endif

#endif
