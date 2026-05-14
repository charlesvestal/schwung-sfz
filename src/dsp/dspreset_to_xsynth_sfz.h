/*
 * DecentSampler `.dspreset` → xsynth-compatible SFZ converter.
 *
 * For knob handling: the converter both bakes each knob's default `value=`
 * statically into the target opcode AND emits ARIA `_oncc<N>` opcodes for
 * the subset xsynth honors (currently `ampeg_*`). Each knob is also
 * exported as a `ds_knob_t` so the plugin can surface it as a Move param
 * and send live CC changes via `xshim_cc` — knobs that only target
 * ampeg-style opcodes respond live; other targets stay at their load-time
 * defaults.
 *
 * Returns a malloc'd path to a temp `.converted.sfz` file, or NULL on
 * error. Caller frees the path. The temp file is left on disk so xsynth
 * can load it normally; it's overwritten on the next preset switch.
 */
#ifndef DSPRESET_TO_XSYNTH_SFZ_H
#define DSPRESET_TO_XSYNTH_SFZ_H

#ifdef __cplusplus
extern "C" {
#endif

#define DS_MAX_KNOBS 16
#define DS_MAX_TABS 8
#define DS_MAX_TAB_NAME_LEN 24

/* One UI knob (`<labeled-knob>` or `<control>`) extracted from the
 * dspreset and surfaced to the plugin. */
typedef struct {
    char  key[16];          /* "knob_0"..."knob_15" */
    char  label[32];        /* DS `label=` (auto-derived if empty) */
    double min_value;       /* DS `minValue=` (default 0) */
    double max_value;       /* DS `maxValue=` (default 1) */
    double default_value;   /* DS `value=` */
    int   cc_number;        /* Synthetic MIDI CC (102..117) routed to xsynth */
    int   live;             /* 1 if this knob's bindings can respond to live
                             * CC (currently: ampeg_* targets only). 0 if it
                             * only affects load-time defaults. */
    int   tab_idx;          /* Phase 6.5: which <tab> the knob lives in
                             * (0 when the dspreset has no tabs). */
} ds_knob_t;

/* Phase 6.5: tab metadata — one entry per `<tab>` element in the
 * dspreset's `<ui>` block. */
typedef struct {
    char name[DS_MAX_TAB_NAME_LEN];  /* `<tab name="...">`; "main" if absent */
} ds_tab_t;

/* Convert + populate knob metadata.
 *
 * On success, `out_knobs` is filled with `*out_knob_count` entries (at
 * most DS_MAX_KNOBS). Pass NULL for `out_knobs` / `out_knob_count` to
 * skip knob extraction.
 */
char *convert_dspreset_to_xsynth_sfz(const char *path,
                                      ds_knob_t *out_knobs,
                                      int *out_knob_count,
                                      ds_tab_t  *out_tabs,
                                      int *out_tab_count);

#ifdef __cplusplus
}
#endif

#endif
