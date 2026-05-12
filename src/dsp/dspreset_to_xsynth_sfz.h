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

char *convert_dspreset_to_xsynth_sfz(const char *path);

#ifdef __cplusplus
}
#endif

#endif
