/*
 * DecentSampler .dspreset → xsynth-compatible SFZ.
 *
 * Differs from the sfizz converter (sfz_plugin.c) in three ways:
 *  1. No `_oncc<N>` opcodes emitted — xsynth ignores them. Knob values are
 *     resolved to static numbers at conversion time.
 *  2. No `<effect>` / `<control>` blocks emitted — xsynth has no reverb /
 *     bus routing / `set_cc` support. Reverb is the user's job downstream
 *     (chain reverb module).
 *  3. Only opcodes xsynth's SFZ parser handles are emitted; everything else
 *     (transpose, end, effect1, etc.) is dropped. xsynth would silently
 *     ignore them anyway, but emitting cruft makes the converted SFZ harder
 *     to inspect.
 *
 * Parser helpers (xml_get_attr, apply_binding_xform, etc.) are local copies
 * of the sfz_plugin.c versions. We're on a feature branch so the duplication
 * is intentional — the sfizz converter stays untouched until the engine
 * swap is final.
 */
#include "dspreset_to_xsynth_sfz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#define DS_MAX_FX     8
#define DS_MAX_GROUPS 32
/* Worst-case oncc bindings: every knob × every group, since one knob's
 * binding list can target multiple groups (e.g. WörliTzer "Line" knob
 * driving group 1 AND group 5 volumes). */
#define DS_MAX_GROUP_ONCC (DS_MAX_KNOBS * DS_MAX_GROUPS)

/* One live `volume_oncc<N>=<dB>` binding the converter discovered.
 * Emitted by the main loop as an SFZ opcode inside the matching
 * group. The voice spawner reads it from RegionParams.volume_oncc;
 * the SIMDVoiceOnccAmp generator samples CC<N> at render time.
 *
 * db_min and db_delta together define the binding: at cc=0 the voice
 * amp contribution is db_min (silent endpoint), at cc=127 it's
 * db_min + db_delta (full volume from this knob). The static SFZ
 * `volume=` baseline gets db_min added; the emitted `volume_oncc=` is
 * db_delta. When multiple bindings stack on one group and the sum of
 * db_min would clamp below xsynth's SFZ `volume` floor (-144) or its
 * spawner clamp (-96), all bindings on that group are scaled
 * proportionally so the static lands exactly at the floor and the
 * knob-at-max sum still reproduces the authored amp. */
typedef struct {
    int    group_position;  /* DS group index (0-based, matches `position=`) */
    int    cc_number;       /* Knob's synthetic MIDI CC (102..117) */
    double db_delta;        /* dB at knob_max minus dB at knob_min */
    double db_min;          /* dB at knob_min (silent endpoint, ≤ 0) */
    int    curve_id;        /* Phase 6: SFZ curve index; -1 = no curve */
} ds_group_oncc_t;

/* MOVE FORK / Phase 4: instrument-level AMP_VOLUME binding. Knob acts
 * as master volume across the whole instrument. Emitted on the SFZ
 * `<global>` block so every region picks it up. */
typedef struct {
    int    cc_number;
    double db_delta;
    double db_min;          /* silent-endpoint baseline (dB at knob_min) */
    int    curve_id;        /* Phase 6 */
} ds_global_oncc_t;

/* MOVE FORK / Phase 4: tag-level AMP_VOLUME binding. Knob drives the
 * volume of every group whose `tags="X"` attribute matches `tag`. The
 * main emit loop resolves these to per-group volume_oncc opcodes by
 * comparing the group's tags= against this list. */
#define DS_MAX_TAG_NAME 32
#define DS_MAX_TAG_ONCC (DS_MAX_KNOBS * 4)
typedef struct {
    char   tag[DS_MAX_TAG_NAME];
    int    cc_number;
    double db_delta;
    double db_min;          /* silent-endpoint baseline */
    int    curve_id;        /* Phase 6 */
} ds_tag_oncc_t;

/* MOVE FORK / Phase 5: filter live bindings (FX_FILTER_FREQUENCY,
 * FX_FILTER_RESONANCE). Emitted on `<global>` so every region's
 * cutoff/resonance is driven by the knob CC via xsynth-core's
 * SIMD*VoiceCutoffLive. Baseline = knob-min endpoint; the *_oncc<CC>
 * sweep covers knob_min → knob_max. */
typedef struct {
    int    cc_number;
    double base_hz;         /* freq at knob_min (silent endpoint, low cutoff) */
    double cents_delta;     /* 1200 * log2(max_hz / base_hz) */
    int    curve_id;        /* Phase 6 */
} ds_filter_freq_oncc_t;
typedef struct {
    int    cc_number;
    double base_db;         /* resonance dB at knob_min */
    double db_delta;        /* dB at knob_max minus dB at knob_min */
    int    curve_id;        /* Phase 6 */
} ds_filter_res_oncc_t;
/* MOVE FORK / Phase 5: PAN live binding (instrument-level — applied to
 * every region's pan via pan_oncc on <global>). */
typedef struct {
    int    cc_number;
    double base_pct;        /* pan (-100..100) at knob_min */
    double pct_delta;
    int    curve_id;        /* Phase 6 */
} ds_pan_oncc_t;

/* MOVE FORK / Phase 6: SFZv2 curve table. The converter generates one
 * curve per live `_oncc` binding, pre-baking the DS knob's translation
 * (apply_binding_xform) into 128 sample points. xsynth-core's SIMD
 * generators consult curve[cc] instead of cc/127 when a `_curvecc<N>`
 * opcode references this curve. */
#define DS_MAX_CURVES 64
typedef struct {
    int    id;
    double v[128];          /* normalized in [0, 1] */
} ds_curve_t;

/* --- helpers -------------------------------------------------------------- */

static double lin_to_db(double x) {
    if (x <= 1e-5) return -80.0;
    return 20.0 * log10(x);
}

static double apply_binding_xform(const char *bind_tag, double in_min,
                                  double in_max, double v);

/* MOVE FORK / Phase 6: build a 128-point curve table that maps SFZ
 * `cc/127` to the *fraction* of (target_max - target_min) we want
 * applied at that CC. Used by xsynth-core's SIMD generators when
 * `_curvecc<N>` references this table.
 *
 * Strategy: at each CC step, the user's knob is at position
 *   knob_pos = knob_min + (knob_max - knob_min) * cc/127
 * (i.e. linear in DS knob space — the on-device encoder is linear).
 * `apply_binding_xform` gives the parameter value at that position;
 * `target_at` then converts to the parameter's SFZ-native unit (dB for
 * volume, cents for cutoff, etc.). The curve point is normalized
 * relative to the endpoints so the SFZ side just multiplies by
 * full_swing.
 *
 * `kind` selects the target conversion:
 *   0 = volume (DS amp-linear → SFZ dB-linear)
 *   1 = cutoff (DS Hz-linear → SFZ cents-linear)
 *   2 = pass-through (caller already in target unit)
 */
static void build_curve(ds_curve_t *c, int id,
                        const char *bind_tag,
                        double knob_min, double knob_max,
                        double v_at_min, double v_at_max,
                        int kind) {
    c->id = id;
    /* Convert endpoints to the SFZ target unit so we can normalize. */
    double t_min, t_max;
    if (kind == 0) {
        t_min = lin_to_db(v_at_min);
        t_max = lin_to_db(v_at_max);
    } else if (kind == 1) {
        if (v_at_min < 1.0) v_at_min = 1.0;
        if (v_at_max <= v_at_min) v_at_max = v_at_min + 1.0;
        t_min = 0.0;  /* base */
        t_max = 1200.0 * log2(v_at_max / v_at_min);
    } else {
        t_min = v_at_min;
        t_max = v_at_max;
    }
    double range = t_max - t_min;
    if (range == 0.0) {
        for (int i = 0; i < 128; i++) c->v[i] = (double)i / 127.0;
        return;
    }
    for (int i = 0; i < 128; i++) {
        double frac = (double)i / 127.0;
        double knob_pos = knob_min + (knob_max - knob_min) * frac;
        double v = apply_binding_xform(bind_tag, knob_min, knob_max, knob_pos);
        double t;
        if (kind == 0) {
            t = lin_to_db(v);
        } else if (kind == 1) {
            if (v < 1.0) v = 1.0;
            t = 1200.0 * log2(v / v_at_min);
        } else {
            t = v;
        }
        double f = (t - t_min) / range;
        if (f < 0.0) f = 0.0;
        if (f > 1.0) f = 1.0;
        c->v[i] = f;
    }
}

static void xml_get_attr(const char *tag, const char *attr_name,
                         char *out_val, int max_len) {
    out_val[0] = '\0';
    int name_len = (int)strlen(attr_name);
    const char *p = tag;
    while ((p = strstr(p, attr_name)) != NULL) {
        if (p > tag) {
            char prev = *(p - 1);
            if (prev != ' ' && prev != '\t' && prev != '\n' && prev != '\r' && prev != '<') {
                p += name_len; continue;
            }
        }
        const char *q = p + name_len;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        if (*q != '=') { p += name_len; continue; }
        q++;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        if (*q != '"') { p += name_len; continue; }
        q++;
        const char *end = strchr(q, '"');
        if (!end) return;
        int len = (int)(end - q);
        if (len >= max_len) len = max_len - 1;
        memcpy(out_val, q, len);
        out_val[len] = '\0';
        return;
    }
}

typedef struct {
    char type[32];
    char freq[32];
    char resonance[32];
    char level[32];
    /* Phase 9/10: effect-specific attributes captured when present. */
    char wet_level[32];
    char room_size[32];
    char damping[32];
    char delay_time[32];
    char feedback[32];
    char mix[32];
} ds_effect_t;

static int parse_effects(const char *src, ds_effect_t fx[DS_MAX_FX]) {
    int count = 0;
    /* Walk every `<effect ` tag in the document (note the trailing
     * space — distinguishes from `<effects`). dspresets commonly
     * have multiple `<effects/>` blocks (per-group, often self-closed
     * + empty) plus a global `<effects>...</effects>` block. We need
     * effects from all of them; just iterate every individual
     * `<effect ` tag everywhere. */
    const char *end = src + strlen(src);
    const char *p = strstr(src, "<effect ");
    while (p && p < end && count < DS_MAX_FX) {
        const char *tag_end = strchr(p, '>');
        if (!tag_end || tag_end > end) break;
        char tag[512];
        int tlen = (int)(tag_end - p);
        if (tlen > 511) tlen = 511;
        memcpy(tag, p, tlen);
        tag[tlen] = '\0';

        ds_effect_t *f = &fx[count++];
        memset(f, 0, sizeof(*f));
        xml_get_attr(tag, "type",      f->type,      sizeof(f->type));
        xml_get_attr(tag, "frequency", f->freq,      sizeof(f->freq));
        xml_get_attr(tag, "resonance", f->resonance, sizeof(f->resonance));
        xml_get_attr(tag, "level",     f->level,     sizeof(f->level));
        xml_get_attr(tag, "wetLevel",  f->wet_level, sizeof(f->wet_level));
        xml_get_attr(tag, "roomSize",  f->room_size, sizeof(f->room_size));
        xml_get_attr(tag, "damping",   f->damping,   sizeof(f->damping));
        xml_get_attr(tag, "delayTime", f->delay_time, sizeof(f->delay_time));
        xml_get_attr(tag, "feedback",  f->feedback,   sizeof(f->feedback));
        xml_get_attr(tag, "mix",       f->mix,        sizeof(f->mix));

        p = tag_end + 1;
        p = strstr(p, "<effect ");
    }
    return count;
}

static double apply_binding_xform(const char *bind_tag, double in_min,
                                  double in_max, double v) {
    double minv = in_min, maxv = in_max;
    if (maxv < minv) { double t = maxv; maxv = minv; minv = t; }
    if (v < minv) v = minv;
    if (v > maxv) v = maxv;

    char trans[32], omin[64], omax[64], factor[64];
    xml_get_attr(bind_tag, "translation",          trans,  sizeof(trans));
    xml_get_attr(bind_tag, "translationOutputMin", omin,   sizeof(omin));
    xml_get_attr(bind_tag, "translationOutputMax", omax,   sizeof(omax));
    xml_get_attr(bind_tag, "factor",               factor, sizeof(factor));

    if (strcmp(trans, "linear") == 0 && omin[0] && omax[0]) {
        double range = (maxv - minv);
        double t = range != 0.0 ? (v - minv) / range : 0.0;
        double oMin = atof(omin), oMax = atof(omax);
        v = oMin + t * (oMax - oMin);
    } else if (strcmp(trans, "table") == 0) {
        char tbl[1024];
        xml_get_attr(bind_tag, "translationTable", tbl, sizeof(tbl));
        if (tbl[0]) {
            double keys[64], vals[64];
            int n = 0;
            const char *q = tbl;
            while (*q && n < 64) {
                while (*q == ' ' || *q == '\t') q++;
                keys[n] = atof(q);
                const char *comma = strchr(q, ',');
                if (!comma) break;
                vals[n] = atof(comma + 1);
                n++;
                const char *semi = strchr(comma, ';');
                if (!semi) break;
                q = semi + 1;
            }
            if (n >= 2) {
                double max_key = keys[n - 1];
                double t = (maxv != minv) ? (v - minv) / (maxv - minv) : 0.0;
                if (t < 0) t = 0; if (t > 1) t = 1;
                double st = t * max_key;
                if (st <= keys[0]) {
                    v = vals[0];
                } else if (st >= keys[n - 1]) {
                    v = vals[n - 1];
                } else {
                    int i = 0;
                    while (i < n - 1 && st > keys[i + 1]) i++;
                    double span = keys[i + 1] - keys[i];
                    double seg_t = span != 0.0 ? (st - keys[i]) / span : 0.0;
                    v = vals[i] + seg_t * (vals[i + 1] - vals[i]);
                }
            }
        }
    }
    if (factor[0]) v *= atof(factor);
    return v;
}

static int compute_binding_value(const char *ctrl_tag, const char *bind_tag,
                                 char *out, int out_len) {
    char ctrl_value[64], cmin[64], cmax[64];
    xml_get_attr(ctrl_tag, "value",    ctrl_value, sizeof(ctrl_value));
    xml_get_attr(ctrl_tag, "minValue", cmin,       sizeof(cmin));
    xml_get_attr(ctrl_tag, "maxValue", cmax,       sizeof(cmax));
    if (!ctrl_value[0]) return 0;
    double minv = cmin[0] ? atof(cmin) : 0.0;
    double maxv = cmax[0] ? atof(cmax) : 1.0;
    double v = apply_binding_xform(bind_tag, minv, maxv, atof(ctrl_value));
    snprintf(out, out_len, "%g", v);
    return 1;
}

/* Walk every <control>/<labeled-knob> IN DOCUMENT ORDER (same as
 * enumerate_ui_knobs), resolve each <binding> to a static SFZ-target
 * value (knob's `value=` baked in). Writes into ENV_* / filter-effect /
 * group-amp_db buckets. CC-driven bindings (no fixed value) are also
 * resolved with the knob at its load-time position — that's the static
 * snapshot.
 *
 * Document-order iteration aligns `knob_idx` with `knobs[]` (filled by
 * enumerate_ui_knobs), so we can look up each binding's owning knob and
 * its synthetic CC number. AMP_VOLUME / TAG_VOLUME group-level bindings
 * additionally record a `volume_oncc` entry the main emit loop writes
 * as `volume_oncc<CC>=<dB-delta>` opcodes per group. */
/* MOVE FORK / Phase 6: allocate a fresh curve_id, build the table,
 * push into curves_out. Returns the assigned id, or -1 if the bucket
 * is full / disabled. IDs start at 100 to avoid SFZv2's predefined
 * 0..7 curve slots. */
static int alloc_curve(ds_curve_t *curves_out, int *count_io,
                       const char *bind_tag,
                       double knob_min, double knob_max,
                       double v_at_min, double v_at_max,
                       int kind) {
    if (!curves_out || !count_io || *count_io >= DS_MAX_CURVES) return -1;
    int id = 100 + *count_io;
    build_curve(&curves_out[*count_io], id, bind_tag,
                knob_min, knob_max, v_at_min, v_at_max, kind);
    (*count_io)++;
    return id;
}

static void apply_ui_overrides(const char *src,
                               ds_effect_t fx[DS_MAX_FX], int fx_count,
                               char env_attack[32], char env_decay[32],
                               char env_sustain[32], char env_release[32],
                               double group_amp_db[DS_MAX_GROUPS],
                               const ds_knob_t *knobs, int knob_count,
                               ds_group_oncc_t oncc_out[DS_MAX_GROUP_ONCC],
                               int *oncc_count_io,
                               ds_global_oncc_t *global_oncc_out, int *global_oncc_count_io,
                               double *global_amp_db_out,
                               ds_tag_oncc_t *tag_oncc_out, int *tag_oncc_count_io,
                               ds_filter_freq_oncc_t *filter_freq_out, int *filter_freq_count_io,
                               ds_filter_res_oncc_t  *filter_res_out,  int *filter_res_count_io,
                               ds_pan_oncc_t         *pan_oncc_out,    int *pan_oncc_count_io,
                               ds_curve_t *curves_out, int *curves_count_io) {
    int knob_idx = 0;
    const char *p = src;
    while (1) {
        /* Pick the next <control or <labeled-knob, whichever comes
         * first — matches enumerate_ui_knobs's iteration so knob_idx
         * stays aligned with knobs[]. */
        const char *c1 = strstr(p, "<control");
        const char *c2 = strstr(p, "<labeled-knob");
        const char *next = NULL;
        size_t pat_len = 0;
        if (c1 && c2) {
            if (c1 < c2) { next = c1; pat_len = 8; }
            else         { next = c2; pat_len = 13; }
        } else if (c1) { next = c1; pat_len = 8; }
        else if (c2)   { next = c2; pat_len = 13; }
        else break;

        p = next;
        char nxt = p[pat_len];
        if (nxt != ' ' && nxt != '\t' && nxt != '\n' &&
            nxt != '\r' && nxt != '>' && nxt != '/') {
            p += pat_len; continue;
        }
        const char *tag_end = strchr(p, '>');
        if (!tag_end) break;
        char ctrl_tag[1024];
        int tlen = (int)(tag_end - p);
        if (tlen > 1023) tlen = 1023;
        memcpy(ctrl_tag, p, tlen);
        ctrl_tag[tlen] = '\0';

        const char *bind_search_start = tag_end + 1;
        const char *bind_search_end;
        int self_closed = (tag_end > p && *(tag_end - 1) == '/');
        int closer_skip = 0;
        if (self_closed) {
            bind_search_end = tag_end;
        } else {
            const char *e1 = strstr(bind_search_start, "</control>");
            const char *e2 = strstr(bind_search_start, "</labeled-knob>");
            if (e1 && e2)       bind_search_end = (e1 < e2) ? e1 : e2;
            else if (e1)        bind_search_end = e1;
            else if (e2)        bind_search_end = e2;
            else { p = tag_end + 1; knob_idx++; continue; }
            if (strncmp(bind_search_end, "</control>", 10) == 0)       closer_skip = 10;
            else if (strncmp(bind_search_end, "</labeled-knob>", 15) == 0) closer_skip = 15;
        }

        /* Knob endpoints for volume_oncc delta: dB at minValue vs dB at
         * maxValue, both via apply_binding_xform so any DS translation
         * table is honored. */
        char cmin[64], cmax[64];
        xml_get_attr(ctrl_tag, "minValue", cmin, sizeof(cmin));
        xml_get_attr(ctrl_tag, "maxValue", cmax, sizeof(cmax));
        double knob_min = cmin[0] ? atof(cmin) : 0.0;
        double knob_max = cmax[0] ? atof(cmax) : 1.0;
        int knob_cc = (knob_idx < knob_count) ? knobs[knob_idx].cc_number : -1;

        const char *bp = bind_search_start;
        while (bp && bp < bind_search_end) {
            const char *binding = strstr(bp, "<binding");
            if (!binding || binding >= bind_search_end) break;
            const char *bind_end = strchr(binding, '>');
            if (!bind_end || bind_end > bind_search_end) break;
            char bind_tag[512];
            int blen = (int)(bind_end - binding);
            if (blen > 511) blen = 511;
            memcpy(bind_tag, binding, blen);
            bind_tag[blen] = '\0';

            char param[64] = "", position_str[16] = "", level[32] = "";
            xml_get_attr(bind_tag, "parameter", param,        sizeof(param));
            xml_get_attr(bind_tag, "position",  position_str, sizeof(position_str));
            xml_get_attr(bind_tag, "level",     level,        sizeof(level));

            char effective[64];
            if (param[0] && compute_binding_value(ctrl_tag, bind_tag,
                                                 effective, sizeof(effective))) {
                int position = position_str[0] ? atoi(position_str) : 0;
                if (strcmp(param, "ENV_ATTACK") == 0) {
                    strncpy(env_attack, effective, 31); env_attack[31] = '\0';
                } else if (strcmp(param, "ENV_DECAY") == 0) {
                    strncpy(env_decay, effective, 31); env_decay[31] = '\0';
                } else if (strcmp(param, "ENV_SUSTAIN") == 0) {
                    strncpy(env_sustain, effective, 31); env_sustain[31] = '\0';
                } else if (strcmp(param, "ENV_RELEASE") == 0) {
                    strncpy(env_release, effective, 31); env_release[31] = '\0';
                } else if ((strcmp(param, "AMP_VOLUME") == 0 ||
                            strcmp(param, "TAG_VOLUME") == 0) &&
                           strcmp(level, "instrument") == 0 &&
                           knob_cc >= 0 &&
                           global_oncc_out && global_oncc_count_io &&
                           *global_oncc_count_io < DS_MAX_KNOBS) {
                    /* Phase 4: instrument-level AMP_VOLUME knob = master
                     * volume across all groups. Recorded for emission on
                     * the SFZ `<global>` block. Silent-endpoint baseline
                     * combines with whatever <groups volume="..."> the
                     * dspreset has, then volume_oncc adds the sweep. */
                    double v_at_min = apply_binding_xform(bind_tag, knob_min,
                                                          knob_max, knob_min);
                    double v_at_max = apply_binding_xform(bind_tag, knob_min,
                                                          knob_max, knob_max);
                    double db_min = lin_to_db(v_at_min);
                    double db_max = lin_to_db(v_at_max);
                    ds_global_oncc_t *e = &global_oncc_out[*global_oncc_count_io];
                    e->cc_number = knob_cc;
                    e->db_delta  = db_max - db_min;
                    e->db_min    = db_min;
                    e->curve_id  = alloc_curve(curves_out, curves_count_io,
                                                bind_tag, knob_min, knob_max,
                                                v_at_min, v_at_max, 0);
                    (*global_oncc_count_io)++;
                    if (global_amp_db_out) *global_amp_db_out += db_min;
                } else if ((strcmp(param, "AMP_VOLUME") == 0 ||
                            strcmp(param, "TAG_VOLUME") == 0) &&
                           strcmp(level, "tag") == 0 &&
                           knob_cc >= 0 &&
                           tag_oncc_out && tag_oncc_count_io &&
                           *tag_oncc_count_io < DS_MAX_TAG_ONCC) {
                    /* Phase 4: tag-level AMP_VOLUME knob = volume bus for
                     * every group with matching `tags=`. Stored with the
                     * binding's `identifier` attribute; the main emit
                     * loop resolves to per-group volume_oncc when it
                     * walks groups and reads each group's tags=. */
                    char tag_id[DS_MAX_TAG_NAME] = "";
                    xml_get_attr(bind_tag, "identifier", tag_id, sizeof(tag_id));
                    if (tag_id[0]) {
                        double v_at_min = apply_binding_xform(bind_tag, knob_min,
                                                              knob_max, knob_min);
                        double v_at_max = apply_binding_xform(bind_tag, knob_min,
                                                              knob_max, knob_max);
                        double db_min = lin_to_db(v_at_min);
                        double db_max = lin_to_db(v_at_max);
                        ds_tag_oncc_t *e = &tag_oncc_out[*tag_oncc_count_io];
                        strncpy(e->tag, tag_id, DS_MAX_TAG_NAME - 1);
                        e->tag[DS_MAX_TAG_NAME - 1] = '\0';
                        e->cc_number = knob_cc;
                        e->db_delta  = db_max - db_min;
                        e->db_min    = db_min;
                        e->curve_id  = alloc_curve(curves_out, curves_count_io,
                                                    bind_tag, knob_min, knob_max,
                                                    v_at_min, v_at_max, 0);
                        (*tag_oncc_count_io)++;
                    }
                } else if ((strcmp(param, "AMP_VOLUME") == 0 ||
                            strcmp(param, "TAG_VOLUME") == 0) &&
                           strcmp(level, "group") == 0 &&
                           position >= 0 && position < DS_MAX_GROUPS) {
                    if (knob_cc >= 0 &&
                        oncc_count_io && *oncc_count_io < DS_MAX_GROUP_ONCC) {
                        /* Phase 3 step 6: silent-endpoint baseline for
                         * live volume_oncc. The static `volume=` reflects
                         * the voice amp at cc=0 (knob at min position);
                         * the matching `volume_oncc<CC>=<delta>` brings
                         * it up to db_max at cc=127. At load the plugin
                         * sends CC = round(current_knob/range * 127) so
                         * the voice's amp at load equals
                         * `db_to_amp(db_min + delta * cc/127)`, which
                         * with the SIMD generator's render-time sampling
                         * makes knob movement audible without preset
                         * reload.
                         *
                         * Curve trade-off: SFZ `volume_oncc` scales
                         * linearly in dB; WörliTzer-style knobs use a
                         * linear-amp translation. The dB-linear path
                         * gives a steeper taper than authored (50% knob
                         * sounds much quieter than expected). Accepted
                         * trade-off until xsynth supports SFZ `_curvecc`
                         * lookup tables. */
                        double v_at_min = apply_binding_xform(bind_tag, knob_min,
                                                              knob_max, knob_min);
                        double v_at_max = apply_binding_xform(bind_tag, knob_min,
                                                              knob_max, knob_max);
                        double db_min = lin_to_db(v_at_min);
                        double db_max = lin_to_db(v_at_max);
                        /* group_amp_db left at NaN — the emit loop
                         * collects oncc db_min from oncc_out, scales
                         * if multiple bindings would underflow, and
                         * emits the final static volume there. */
                        ds_group_oncc_t *e = &oncc_out[*oncc_count_io];
                        e->group_position = position;
                        e->cc_number      = knob_cc;
                        e->db_delta       = db_max - db_min;
                        e->db_min         = db_min;
                        e->curve_id       = alloc_curve(curves_out, curves_count_io,
                                                         bind_tag, knob_min, knob_max,
                                                         v_at_min, v_at_max, 0);
                        (*oncc_count_io)++;
                    } else {
                        /* No live consumer (knob_idx misaligned or no
                         * synthetic CC assigned) — fall back to the
                         * static-at-current-knob baseline so the voice
                         * isn't silenced. */
                        group_amp_db[position] = lin_to_db(atof(effective));
                    }
                } else if (position >= 0 && position < fx_count) {
                    ds_effect_t *f = &fx[position];
                    /* Only effects that we convert into the global biquad
                     * (lowpass/highpass/bandpass/notch/peak) can host
                     * cutoff/resonance live bindings. K4-Acoustic's Phaser
                     * has its own FX_CENTER_FREQUENCY knob at position=1;
                     * routing it into cutoff_oncc accumulated with the real
                     * Filter knob at position=0 and pushed the voice biquad
                     * past Nyquist — bilinear-transform instability
                     * produced unbounded noise + NaN that silenced the
                     * voice chain. */
                    int is_filter_effect =
                        strcmp(f->type, "lowpass") == 0 ||
                        strcmp(f->type, "lowpass_1pl") == 0 ||
                        strcmp(f->type, "lowpass_4pl") == 0 ||
                        strcmp(f->type, "highpass") == 0 ||
                        strcmp(f->type, "bandpass") == 0 ||
                        strcmp(f->type, "notch") == 0 ||
                        strcmp(f->type, "peak") == 0;
                    /* Phase 7: FX_CENTER_FREQUENCY on a bandpass effect
                     * routes through the same cutoff_oncc path as
                     * FX_FILTER_FREQUENCY — xsynth's biquad doesn't
                     * distinguish (the static fil_type=bpf_2p does). */
                    if (is_filter_effect &&
                        (strcmp(param, "FX_FILTER_FREQUENCY") == 0 ||
                         strcmp(param, "FX_CENTER_FREQUENCY") == 0)) {
                        /* Phase 5: emit cutoff_oncc<CC>=<cents> when the
                         * knob has a synthetic CC. base_hz comes from the
                         * knob-min endpoint (silent-endpoint baseline);
                         * cents_delta covers knob_min → knob_max. The
                         * static `cutoff=` written elsewhere is overwritten
                         * by base_hz so they agree. */
                        if (knob_cc >= 0 &&
                            filter_freq_out && filter_freq_count_io &&
                            *filter_freq_count_io < DS_MAX_KNOBS) {
                            double v_at_min = apply_binding_xform(bind_tag, knob_min,
                                                                  knob_max, knob_min);
                            double v_at_max = apply_binding_xform(bind_tag, knob_min,
                                                                  knob_max, knob_max);
                            /* Guard against zero / negative freq values. */
                            if (v_at_min < 1.0)  v_at_min = 1.0;
                            if (v_at_max <= v_at_min) v_at_max = v_at_min + 1.0;
                            ds_filter_freq_oncc_t *e =
                                &filter_freq_out[*filter_freq_count_io];
                            e->cc_number = knob_cc;
                            e->base_hz   = v_at_min;
                            e->cents_delta = 1200.0 * log2(v_at_max / v_at_min);
                            e->curve_id = alloc_curve(curves_out, curves_count_io,
                                                       bind_tag, knob_min, knob_max,
                                                       v_at_min, v_at_max, 1);
                            (*filter_freq_count_io)++;
                            /* Overwrite static `freq` so the global emit
                             * uses the silent-endpoint baseline. */
                            snprintf(f->freq, sizeof(f->freq), "%.2f", v_at_min);
                        } else {
                            strncpy(f->freq, effective, 31); f->freq[31] = '\0';
                        }
                    } else if (is_filter_effect &&
                               strcmp(param, "FX_FILTER_RESONANCE") == 0) {
                        if (knob_cc >= 0 &&
                            filter_res_out && filter_res_count_io &&
                            *filter_res_count_io < DS_MAX_KNOBS) {
                            double v_at_min = apply_binding_xform(bind_tag, knob_min,
                                                                  knob_max, knob_min);
                            double v_at_max = apply_binding_xform(bind_tag, knob_min,
                                                                  knob_max, knob_max);
                            /* DS resonance ranges 0..1 (Q-like). xsynth's
                             * SFZ `resonance` is dB. Pass through directly
                             * (matches existing static behavior, ~0.7 ≈ 0
                             * dB). Future: scale to a fuller dB range. */
                            ds_filter_res_oncc_t *e =
                                &filter_res_out[*filter_res_count_io];
                            e->cc_number = knob_cc;
                            e->base_db   = v_at_min;
                            e->db_delta  = v_at_max - v_at_min;
                            e->curve_id  = -1; /* linear ok for resonance */
                            (*filter_res_count_io)++;
                            snprintf(f->resonance, sizeof(f->resonance), "%.4f", v_at_min);
                        } else {
                            strncpy(f->resonance, effective, 31); f->resonance[31] = '\0';
                        }
                    } else if (strcmp(param, "PAN") == 0) {
                        if (knob_cc >= 0 &&
                            pan_oncc_out && pan_oncc_count_io &&
                            *pan_oncc_count_io < DS_MAX_KNOBS) {
                            double v_at_min = apply_binding_xform(bind_tag, knob_min,
                                                                  knob_max, knob_min);
                            double v_at_max = apply_binding_xform(bind_tag, knob_min,
                                                                  knob_max, knob_max);
                            ds_pan_oncc_t *e = &pan_oncc_out[*pan_oncc_count_io];
                            e->cc_number = knob_cc;
                            e->base_pct  = v_at_min;
                            e->pct_delta = v_at_max - v_at_min;
                            e->curve_id  = -1; /* linear ok for pan */
                            (*pan_oncc_count_io)++;
                        }
                    }
                    /* Reverb knobs: skip — xsynth has no reverb anyway. */
                }
            }
            bp = bind_end + 1;
        }
        p = self_closed ? (tag_end + 1) : (bind_search_end + closer_skip);
        knob_idx++;
    }
}

/* Case-insensitive directory lookup. If `<base>/<rel>` exists exactly, copy
 * it through. Otherwise walk each path segment of `rel` against the on-disk
 * directory listing case-insensitively and emit the actual filenames. xsynth
 * is strict about case; many DS libraries mix `Samples/foo.aif` with
 * `samples/bar.wav` in the same .dspreset, which works on macOS / case-
 * insensitive HFS+ but breaks on Move's case-sensitive ext4. */
static int resolve_sample_path_ci(const char *base, const char *rel,
                                  char *out, int out_len) {
    /* Fast path: exact match exists. */
    char trial[1024];
    snprintf(trial, sizeof(trial), "%s/%s", base, rel);
    struct stat st;
    if (stat(trial, &st) == 0) {
        snprintf(out, out_len, "%s", rel);
        return 1;
    }

    /* Walk segments, fixing case at each step. */
    char working_dir[1024];
    snprintf(working_dir, sizeof(working_dir), "%s", base);

    /* Tokenize rel by '/'. */
    char relbuf[512];
    snprintf(relbuf, sizeof(relbuf), "%s", rel);
    char fixed_rel[512];
    int fr_pos = 0;
    fixed_rel[0] = '\0';

    char *save = NULL;
    char *seg = strtok_r(relbuf, "/", &save);
    while (seg) {
        /* Look in working_dir for a case-insensitive match for seg. */
        DIR *d = opendir(working_dir);
        if (!d) return 0;
        struct dirent *de;
        char matched[256] = "";
        while ((de = readdir(d)) != NULL) {
            if (strcasecmp(de->d_name, seg) == 0) {
                snprintf(matched, sizeof(matched), "%s", de->d_name);
                break;
            }
        }
        closedir(d);
        if (!matched[0]) return 0;

        /* Advance working_dir + accumulate fixed segments. */
        int wlen = (int)strlen(working_dir);
        snprintf(working_dir + wlen, sizeof(working_dir) - wlen, "/%s", matched);
        if (fr_pos > 0)
            fr_pos += snprintf(fixed_rel + fr_pos, sizeof(fixed_rel) - fr_pos,
                               "/%s", matched);
        else
            fr_pos += snprintf(fixed_rel, sizeof(fixed_rel), "%s", matched);

        seg = strtok_r(NULL, "/", &save);
    }

    /* Confirm final path exists. */
    if (stat(working_dir, &st) != 0) return 0;
    snprintf(out, out_len, "%s", fixed_rel);
    return 1;
}

static int count_rr_groups(const char *src) {
    /* MOVE FORK / 2026-05-16: also honor `seqMode="round_robin"` on the
     * `<groups>` parent — each child `<group>` then inherits RR mode and
     * uses its own seqPosition for slot membership. */
    int groups_rr = 0;
    const char *gp = strstr(src, "<groups");
    if (gp) {
        const char *ge = strchr(gp, '>');
        if (ge) {
            char gbuf[1024];
            int gl = (int)(ge - gp); if (gl > 1023) gl = 1023;
            memcpy(gbuf, gp, gl); gbuf[gl] = '\0';
            char gm[32];
            xml_get_attr(gbuf, "seqMode", gm, sizeof(gm));
            if (strcmp(gm, "round_robin") == 0) groups_rr = 1;
        }
    }
    int count = 0;
    const char *p = src;
    while ((p = strstr(p, "<group ")) != NULL) {
        const char *tag_end = strchr(p, '>');
        if (!tag_end) break;
        char tag[1024];
        int tlen = (int)(tag_end - p);
        if (tlen > 1023) tlen = 1023;
        memcpy(tag, p, tlen);
        tag[tlen] = '\0';
        char mode[32];
        xml_get_attr(tag, "seqMode", mode, sizeof(mode));
        /* A group counts toward the RR rotation when either it
         * declares seqMode itself or it inherits one from <groups>. */
        if (strcmp(mode, "round_robin") == 0 || groups_rr) count++;
        p = tag_end + 1;
    }
    return count;
}

/* --- knob enumeration ---------------------------------------------------- */

/* Short fallback label derived from a binding's DS `parameter=` name.
 * Returns NULL when the target has no obvious short alias (caller falls
 * back to "Knob" / group-name lookup). */
/* Effect-type prefix for ambiguous params (Rate/Depth/Mix/FB shared
 * across delay/chorus/phaser/reverb). Returns NULL if no disambiguation
 * is needed at this position. */
static const char *fx_short_prefix(const char *fx_type) {
    if (!fx_type || !fx_type[0]) return NULL;
    if (strcmp(fx_type, "delay")   == 0) return "Dly";
    if (strcmp(fx_type, "chorus")  == 0) return "Cho";
    if (strcmp(fx_type, "phaser")  == 0) return "Phs";
    if (strcmp(fx_type, "reverb")  == 0) return "Rev";
    return NULL;
}

/* True for DS params that are generic enough that multiple effects use
 * them — we'll prefix with the effect short name when found. */
static int param_is_ambiguous(const char *p) {
    return strcmp(p, "FX_FEEDBACK")  == 0 ||
           strcmp(p, "FX_MIX")       == 0 ||
           strcmp(p, "FX_MOD_RATE")  == 0 ||
           strcmp(p, "FX_MOD_DEPTH") == 0 ||
           strcmp(p, "FX_WET_LEVEL") == 0;
}

static const char *target_alias(const char *ds_param) {
    if (!ds_param || !ds_param[0]) return NULL;
    if (strcmp(ds_param, "FX_FILTER_FREQUENCY") == 0) return "Filter";
    if (strcmp(ds_param, "FX_FILTER_RESONANCE") == 0) return "Reso";
    if (strcmp(ds_param, "FX_CENTER_FREQUENCY") == 0) return "Center";
    if (strcmp(ds_param, "FX_REVERB_WET_LEVEL")  == 0) return "Reverb";
    if (strcmp(ds_param, "FX_REVERB_ROOM_SIZE")  == 0) return "Room";
    if (strcmp(ds_param, "FX_REVERB_DAMPING")    == 0) return "Damp";
    if (strcmp(ds_param, "FX_DELAY_TIME")        == 0) return "DlyTime";
    if (strcmp(ds_param, "FX_DELAY_FEEDBACK")    == 0) return "DlyFb";
    if (strcmp(ds_param, "FX_FEEDBACK")          == 0) return "Feedback";
    if (strcmp(ds_param, "FX_WET_LEVEL")         == 0) return "Wet";
    if (strcmp(ds_param, "FX_MIX")               == 0) return "Mix";
    if (strcmp(ds_param, "FX_MOD_RATE")          == 0) return "Rate";
    if (strcmp(ds_param, "FX_MOD_DEPTH")         == 0) return "Depth";
    if (strcmp(ds_param, "FX_STEREO_OFFSET")     == 0) return "Width";
    if (strcmp(ds_param, "FX_CHORUS_DEPTH")      == 0) return "ChrDepth";
    if (strcmp(ds_param, "FX_CHORUS_RATE")       == 0) return "ChrRate";
    if (strcmp(ds_param, "ENV_ATTACK")  == 0) return "Attack";
    if (strcmp(ds_param, "ENV_DECAY")   == 0) return "Decay";
    if (strcmp(ds_param, "ENV_SUSTAIN") == 0) return "Sustain";
    if (strcmp(ds_param, "ENV_RELEASE") == 0) return "Release";
    if (strcmp(ds_param, "AMP_VOLUME")  == 0) return "Volume";
    if (strcmp(ds_param, "TAG_VOLUME")  == 0) return "Volume";
    if (strcmp(ds_param, "PAN")         == 0) return "Pan";
    if (strcmp(ds_param, "ENABLED")     == 0) return "Enable";
    if (strcmp(ds_param, "GROUP_TUNING") == 0) return "Tune";
    if (strcmp(ds_param, "LEVEL")       == 0) return "Level";
    if (strcmp(ds_param, "MOD_AMOUNT")  == 0) return "Mod";
    if (strcmp(ds_param, "FREQUENCY")   == 0) return "Freq";
    return NULL;
}

/* Read the `name=` attribute of the Nth <group> in document order.
 * Used as a label fallback for AMP_VOLUME / TAG_VOLUME knobs when the
 * `<labeled-knob>` has an empty `label=` (common in image-themed
 * patches like WörliTzer where the knob graphic carries the label). */
static int get_group_name_for_position(const char *src, int position,
                                        char *out, int out_len) {
    out[0] = '\0';
    const char *p = src;
    int idx = 0;
    while ((p = strstr(p, "<group")) != NULL) {
        if (p[6] == 's' || p[6] == 'S') { p += 7; continue; }
        const char *tag_end = strchr(p, '>');
        if (!tag_end) break;
        if (idx == position) {
            char tag[1024];
            int tlen = (int)(tag_end - p);
            if (tlen > 1023) tlen = 1023;
            memcpy(tag, p, tlen);
            tag[tlen] = '\0';
            xml_get_attr(tag, "name", out, out_len);
            return out[0] ? 1 : 0;
        }
        idx++;
        p = tag_end + 1;
    }
    return 0;
}

/* Test if a `<binding parameter="X">` target is one that xsynth honors
 * for live CC modulation today.
 *  - ENV_*: routed through the ARIA `_oncc` baker which folds CC state
 *    into static values at load time (knob position frozen at load).
 *  - AMP_VOLUME / TAG_VOLUME: converter emits `volume_oncc<CC>=<dB>`
 *    alongside the static `volume=`. RegionParams collects them; a
 *    future voice-side generator (Phase 3 step 5) will sample CC live.
 *    Already useful as a knob-surface signal even before the
 *    generator lands. */
static int binding_param_is_live(const char *parameter) {
    if (!parameter || !parameter[0]) return 0;
    return strcmp(parameter, "ENV_ATTACK") == 0 ||
           strcmp(parameter, "ENV_DECAY") == 0 ||
           strcmp(parameter, "ENV_SUSTAIN") == 0 ||
           strcmp(parameter, "ENV_RELEASE") == 0 ||
           strcmp(parameter, "ENV_HOLD") == 0 ||
           strcmp(parameter, "AMP_VOLUME") == 0 ||
           strcmp(parameter, "TAG_VOLUME") == 0 ||
           /* Phase 5: filter + pan live oncc. */
           strcmp(parameter, "FX_FILTER_FREQUENCY") == 0 ||
           strcmp(parameter, "FX_CENTER_FREQUENCY") == 0 ||
           strcmp(parameter, "FX_FILTER_RESONANCE") == 0 ||
           strcmp(parameter, "PAN") == 0 ||
           /* Phase 10: reverb wet routes to the channel reverb via the
            * plugin's set_param handler (not through xsynth). */
           strcmp(parameter, "FX_REVERB_WET_LEVEL") == 0 ||
           /* Phase 9: delay parameters routed via plugin set_param.
            * FX_WET_LEVEL is the delay's wet control per DS
            * convention (reverb has its own FX_REVERB_WET_LEVEL). */
           strcmp(parameter, "FX_DELAY_TIME") == 0 ||
           strcmp(parameter, "FX_FEEDBACK") == 0 ||
           strcmp(parameter, "FX_WET_LEVEL") == 0;
}

/* Walk every `<labeled-knob>` / `<control>` element in document order and
 * populate `knobs_out` with one entry per UI knob. Each knob is assigned
 * a synthetic CC number (102..117) so the plugin can route knob movement
 * to `xshim_cc`. Returns the populated knob count. */
/* Phase 6.5: enumerate `<tab name="...">` elements in document order.
 * Returns the count (0 when the dspreset has no <ui><tab>s). Tabs with
 * no name attribute fall back to "Tab N". */
static int enumerate_ui_tabs(const char *src, ds_tab_t tabs_out[DS_MAX_TABS]) {
    int tc = 0;
    const char *p = src;
    while (tc < DS_MAX_TABS) {
        const char *t = strstr(p, "<tab");
        if (!t) break;
        char nxt = t[4];
        if (nxt != ' ' && nxt != '\t' && nxt != '\n' &&
            nxt != '\r' && nxt != '>' && nxt != '/') {
            p = t + 4; continue;
        }
        const char *te = strchr(t, '>');
        if (!te) break;
        char tag[512];
        int tlen = (int)(te - t);
        if (tlen > 511) tlen = 511;
        memcpy(tag, t, tlen); tag[tlen] = '\0';
        char name[DS_MAX_TAB_NAME_LEN];
        xml_get_attr(tag, "name", name, sizeof(name));
        if (!name[0]) snprintf(name, sizeof(name), "Tab %d", tc + 1);
        snprintf(tabs_out[tc].name, sizeof(tabs_out[tc].name), "%s", name);
        tc++;
        p = te + 1;
    }
    return tc;
}

/* Phase 6.5: given a position in src and the tabs array, find which
 * tab index encloses that position. Returns 0 when there are no tabs
 * (or the position is outside any tab — also tab 0). */
static int tab_index_at_position(const char *src, const char *pos,
                                  int tab_count) {
    if (tab_count <= 0) return 0;
    int current = 0;
    int seen = -1;
    const char *p = src;
    while (p < pos) {
        const char *to = strstr(p, "<tab");
        const char *tc = strstr(p, "</tab>");
        const char *next = NULL;
        if (to && tc) next = (to < tc) ? to : tc;
        else if (to)  next = to;
        else if (tc)  next = tc;
        if (!next || next >= pos) break;
        if (next == to) {
            char nxt = to[4];
            if (nxt == ' ' || nxt == '\t' || nxt == '\n' ||
                nxt == '\r' || nxt == '>' || nxt == '/') {
                seen++;
                current = seen;
            }
            const char *e = strchr(to, '>');
            p = e ? e + 1 : to + 4;
        } else {
            p = tc + 6;
        }
    }
    if (current < 0) current = 0;
    if (current >= tab_count) current = tab_count - 1;
    return current;
}

static int enumerate_ui_knobs(const char *src,
                              ds_knob_t knobs_out[DS_MAX_KNOBS],
                              const ds_tab_t *tabs, int tab_count,
                              const ds_effect_t *fx, int fx_count) {
    int kc = 0;
    int next_cc = 102;
    const char *p = src;

    while (kc < DS_MAX_KNOBS) {
        const char *c1 = strstr(p, "<control");
        const char *c2 = strstr(p, "<labeled-knob");
        const char *next = NULL;
        size_t pat_len = 0;
        if (c1 && c2) {
            if (c1 < c2) { next = c1; pat_len = 8; }
            else         { next = c2; pat_len = 13; }
        } else if (c1) { next = c1; pat_len = 8; }
        else if (c2)   { next = c2; pat_len = 13; }
        else break;

        p = next;
        char nxt = p[pat_len];
        if (nxt != ' ' && nxt != '\t' && nxt != '\n' &&
            nxt != '\r' && nxt != '>' && nxt != '/') {
            p += pat_len;
            continue;
        }
        const char *tag_end = strchr(p, '>');
        if (!tag_end) break;

        char ctrl_tag[1024];
        int tlen = (int)(tag_end - p);
        if (tlen > 1023) tlen = 1023;
        memcpy(ctrl_tag, p, tlen);
        ctrl_tag[tlen] = '\0';

        char cmin[64], cmax[64], cval[64], clabel[64], clabelp[64];
        xml_get_attr(ctrl_tag, "minValue",      cmin,    sizeof(cmin));
        xml_get_attr(ctrl_tag, "maxValue",      cmax,    sizeof(cmax));
        xml_get_attr(ctrl_tag, "value",         cval,    sizeof(cval));
        xml_get_attr(ctrl_tag, "label",         clabel,  sizeof(clabel));
        xml_get_attr(ctrl_tag, "parameterName", clabelp, sizeof(clabelp));

        /* Scan bindings inside this control to determine if any of them
         * have a live-capable target. Even if no bindings parse, we still
         * surface the knob (it'll just save/restore state without live
         * audio response). */
        int self_closed = (tag_end > p && *(tag_end - 1) == '/');
        const char *bind_start = tag_end + 1;
        const char *bind_end;
        if (self_closed) {
            bind_end = tag_end;
        } else {
            const char *e1 = strstr(bind_start, "</control>");
            const char *e2 = strstr(bind_start, "</labeled-knob>");
            if (e1 && e2)       bind_end = (e1 < e2) ? e1 : e2;
            else if (e1)        bind_end = e1;
            else if (e2)        bind_end = e2;
            else                bind_end = bind_start;
        }
        int any_live = 0;
        int any_reverb_wet = 0;
        int any_delay_time = 0, any_delay_fb = 0, any_delay_mix = 0;
        int any_chorus_rate = 0, any_chorus_depth = 0, any_chorus_mix = 0;
        int any_phaser_rate = 0, any_phaser_depth = 0, any_phaser_mix = 0,
            any_phaser_feedback = 0;
        int any_widener = 0;
        char first_param[64] = "";
        int first_position = -1;
        const char *bp = bind_start;
        while (bp < bind_end) {
            const char *bind = strstr(bp, "<binding");
            if (!bind || bind >= bind_end) break;
            const char *bend = strchr(bind, '>');
            if (!bend || bend > bind_end) break;
            char btag[512];
            int blen = (int)(bend - bind);
            if (blen > 511) blen = 511;
            memcpy(btag, bind, blen);
            btag[blen] = '\0';
            char param[64], pos_str[16];
            xml_get_attr(btag, "parameter", param, sizeof(param));
            xml_get_attr(btag, "position",  pos_str, sizeof(pos_str));
            int pos_idx = pos_str[0] ? atoi(pos_str) : -1;
            const char *fx_type = (pos_idx >= 0 && pos_idx < fx_count)
                                  ? fx[pos_idx].type : "";
            int is_chorus_target = (strcmp(fx_type, "chorus") == 0);
            int is_phaser_target = (strcmp(fx_type, "phaser") == 0);
            int is_delay_target  = (strcmp(fx_type, "delay") == 0);
            if (binding_param_is_live(param)) any_live = 1;
            if (strcmp(param, "FX_REVERB_WET_LEVEL") == 0) any_reverb_wet = 1;
            if (strcmp(param, "FX_DELAY_TIME")       == 0) any_delay_time = 1;
            if (strcmp(param, "FX_FEEDBACK") == 0 && is_delay_target) any_delay_fb = 1;
            if (strcmp(param, "FX_WET_LEVEL")        == 0) any_delay_mix  = 1;
            /* Phase 12: chorus knobs share FX_MOD_RATE/FX_MOD_DEPTH/
             * FX_MIX with phaser. Disambiguate by the binding's
             * position pointing at a `<effect type="chorus">`. */
            if (strcmp(param, "FX_MOD_RATE") == 0  && is_chorus_target) any_chorus_rate  = 1;
            if (strcmp(param, "FX_MOD_DEPTH") == 0 && is_chorus_target) any_chorus_depth = 1;
            if (strcmp(param, "FX_MIX") == 0       && is_chorus_target) any_chorus_mix   = 1;
            /* Phase 12: phaser shares the same FX_MOD_* params with
             * chorus — disambiguated by the binding's position pointing
             * at <effect type="phaser">. FX_FEEDBACK on a phaser routes
             * to the phaser, NOT the delay. */
            if (strcmp(param, "FX_MOD_RATE") == 0  && is_phaser_target) any_phaser_rate  = 1;
            if (strcmp(param, "FX_MOD_DEPTH") == 0 && is_phaser_target) any_phaser_depth = 1;
            if (strcmp(param, "FX_MIX") == 0       && is_phaser_target) any_phaser_mix   = 1;
            if (strcmp(param, "FX_FEEDBACK") == 0  && is_phaser_target) any_phaser_feedback = 1;
            /* Phase 13: FX_STEREO_OFFSET routes to channel widener. */
            if (strcmp(param, "FX_STEREO_OFFSET") == 0) any_widener = 1;
            if (first_param[0] == '\0' && param[0]) {
                strncpy(first_param, param, sizeof(first_param) - 1);
                first_param[sizeof(first_param) - 1] = '\0';
                first_position = pos_str[0] ? atoi(pos_str) : -1;
            }
            bp = bend + 1;
        }

        ds_knob_t *k = &knobs_out[kc];
        snprintf(k->key, sizeof(k->key), "knob_%d", kc);

        /* Label fallback chain: explicit DS `label=` > `parameterName=` >
         * group name (AMP_VOLUME/TAG_VOLUME) > short alias for the
         * binding's parameter > "Knob N". This handles image-themed
         * patches like WörliTzer where most `<labeled-knob>` elements
         * leave `label=` empty and rely on the graphic; the binding's
         * target identifies what the knob actually does. */
        const char *lbl = NULL;
        char group_name[64];
        if (clabel[0]) {
            lbl = clabel;
        } else if (clabelp[0]) {
            lbl = clabelp;
        } else if ((strcmp(first_param, "AMP_VOLUME") == 0 ||
                    strcmp(first_param, "TAG_VOLUME") == 0) &&
                   first_position >= 0 &&
                   get_group_name_for_position(src, first_position,
                                                group_name, sizeof(group_name))) {
            lbl = group_name;
        } else {
            lbl = target_alias(first_param);
        }
        /* Prefix ambiguous params (Rate/Depth/Mix/FB/Wet) with the
         * effect-type short name when we can identify it — otherwise the
         * params menu shows two indistinguishable "Feedback" entries when
         * a preset has both delay and phaser feedback knobs. */
        const char *prefix = NULL;
        if (lbl && param_is_ambiguous(first_param) &&
                first_position >= 0 && first_position < fx_count) {
            prefix = fx_short_prefix(fx[first_position].type);
        }
        if (!lbl || !lbl[0]) {
            snprintf(k->label, sizeof(k->label), "Knob %d", kc + 1);
        } else if (prefix) {
            snprintf(k->label, sizeof(k->label), "%s %s", prefix, lbl);
        } else {
            snprintf(k->label, sizeof(k->label), "%s", lbl);
        }
        k->min_value     = cmin[0] ? atof(cmin) : 0.0;
        k->max_value     = cmax[0] ? atof(cmax) : 1.0;
        k->default_value = cval[0] ? atof(cval) : k->min_value;
        /* CC range 102..119 is "undefined controllers" — safe to
         * commandeer for synthetic routing. Knobs past CC 119 get
         * cc_number=-1 (no live CC, but still listed in the menu
         * so they're discoverable). */
        k->cc_number     = (next_cc <= 119) ? next_cc++ : -1;
        /* Phase 9/10/12: knobs targeting fundsp post-mix effects are
         * surfaced as live — the plugin routes their values at
         * set_param time. */
        k->live          = any_live || any_reverb_wet ||
                           any_delay_time || any_delay_fb || any_delay_mix ||
                           any_chorus_rate || any_chorus_depth || any_chorus_mix ||
                           any_phaser_rate || any_phaser_depth || any_phaser_mix ||
                           any_phaser_feedback ||
                           any_widener;
        k->reverb_wet    = any_reverb_wet;
        k->delay_time    = any_delay_time;
        k->delay_feedback= any_delay_fb;
        k->delay_mix     = any_delay_mix;
        k->chorus_rate   = any_chorus_rate;
        k->chorus_depth  = any_chorus_depth;
        k->chorus_mix    = any_chorus_mix;
        k->phaser_rate   = any_phaser_rate;
        k->phaser_depth  = any_phaser_depth;
        k->phaser_feedback = any_phaser_feedback;
        k->phaser_mix    = any_phaser_mix;
        k->widener       = any_widener;
        k->tab_idx       = tab_index_at_position(src, p, tab_count);
        (void)tabs;

        kc++;
        /* Advance past the closing tag. bind_end points to the start
         * of `</control>` (10 chars) or `</labeled-knob>` (15 chars).
         * Previous code used a hardcoded +15 for both, which over-
         * shot for `</control>` and made strstr skip the NEXT
         * element on tightly-packed dspresets (K4-AirVoice et al.
         * lost 7 of 18 knobs to this). Scan to `>` instead. */
        if (self_closed) {
            p = tag_end + 1;
        } else {
            p = bind_end;
            while (*p && *p != '>') p++;
            if (*p == '>') p++;
        }
    }
    return kc;
}

/* --- converter ----------------------------------------------------------- */

char *convert_dspreset_to_xsynth_sfz(const char *path,
                                      ds_knob_t *out_knobs,
                                      int *out_knob_count,
                                      ds_tab_t  *out_tabs,
                                      int *out_tab_count,
                                      ds_reverb_cfg_t *out_reverb,
                                      ds_delay_cfg_t  *out_delay,
                                      ds_chorus_cfg_t *out_chorus,
                                      ds_phaser_cfg_t *out_phaser) {
    if (out_knob_count) *out_knob_count = 0;
    if (out_tab_count)  *out_tab_count  = 0;
    if (out_reverb)     { memset(out_reverb, 0, sizeof(*out_reverb)); }
    if (out_delay)      { memset(out_delay,  0, sizeof(*out_delay));  }
    if (out_chorus)     { memset(out_chorus, 0, sizeof(*out_chorus)); }
    if (out_phaser)     { memset(out_phaser, 0, sizeof(*out_phaser)); }
    /* Base directory for sample-path resolution (the dspreset's parent). */
    char base_dir[1024];
    snprintf(base_dir, sizeof(base_dir), "%s", path);
    char *last_slash = strrchr(base_dir, '/');
    if (last_slash) *last_slash = '\0';
    else base_dir[0] = '\0';

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 1024 * 1024) { fclose(f); return NULL; }
    char *src = malloc(size + 1);
    if (!src) { fclose(f); return NULL; }
    fread(src, 1, size, f);
    src[size] = '\0';
    fclose(f);

    /* Same malformed-XML fixup as the sfizz converter — some DS authors
     * forget spaces between attrs (e.g. ASIMOV). */
    {
        int fixes = 0, in_tag = 0, quote_count = 0;
        for (long i = 0; i < size; i++) {
            if (src[i] == '<') { in_tag = 1; quote_count = 0; }
            else if (src[i] == '>') { in_tag = 0; }
            if (in_tag && src[i] == '"') {
                quote_count++;
                if ((quote_count % 2 == 0) && i + 1 < size &&
                    ((src[i+1] >= 'a' && src[i+1] <= 'z') ||
                     (src[i+1] >= 'A' && src[i+1] <= 'Z'))) fixes++;
            }
        }
        if (fixes > 0) {
            char *dst = malloc(size + fixes + 1);
            if (!dst) { free(src); return NULL; }
            long j = 0; in_tag = 0; quote_count = 0;
            for (long i = 0; i < size; i++) {
                if (src[i] == '<') { in_tag = 1; quote_count = 0; }
                else if (src[i] == '>') { in_tag = 0; }
                dst[j++] = src[i];
                if (in_tag && src[i] == '"') {
                    quote_count++;
                    if ((quote_count % 2 == 0) && i + 1 < size &&
                        ((src[i+1] >= 'a' && src[i+1] <= 'z') ||
                         (src[i+1] >= 'A' && src[i+1] <= 'Z'))) dst[j++] = ' ';
                }
            }
            dst[j] = '\0';
            free(src); src = dst; size = j;
        }
    }

    /* Pre-Phase 6 the SFZ output was roughly the same size as the
     * dspreset XML — 2× was plenty. After Phase 6 each knob with a
     * non-linear translation emits a 128-row <curve> block (~1.8 KB
     * each). Small parity-test dspresets are only a few hundred bytes
     * but generate KB of curves; the old `size * 2` cap overflowed
     * and corrupted the heap. Use a generous floor so the cap always
     * exceeds the worst-case curve + per-region emit. */
    long out_cap = size * 4;
    if (out_cap < 1024 * 1024) out_cap = 1024 * 1024;
    char *sfz = malloc(out_cap);
    if (!sfz) { free(src); return NULL; }
    int pos = 0;
    char val[512];

    ds_effect_t fx[DS_MAX_FX] = {0};
    int fx_count = parse_effects(src, fx);

    /* MOVE FORK: enumerate UI knobs once src is loaded. Caller may pass
     * NULL out_knobs to skip extraction (e.g. converter CLI). We need
     * the knob list inside apply_ui_overrides for CC lookups on live
     * volume_oncc bindings; if the caller skipped extraction we
     * enumerate into a local buffer for use within this call. */
    ds_knob_t local_knobs[DS_MAX_KNOBS];
    ds_knob_t *use_knobs = out_knobs ? out_knobs : local_knobs;
    /* Phase 6.5: enumerate tabs first so each knob can record its tab_idx. */
    ds_tab_t local_tabs[DS_MAX_TABS];
    ds_tab_t *use_tabs = out_tabs ? out_tabs : local_tabs;
    int tab_count_local = enumerate_ui_tabs(src, use_tabs);
    if (out_tabs && out_tab_count) *out_tab_count = tab_count_local;
    int knob_count_local = enumerate_ui_knobs(src, use_knobs,
                                              use_tabs, tab_count_local,
                                              fx, fx_count);
    if (out_knobs && out_knob_count) {
        *out_knob_count = knob_count_local;
    }

    char env_attack[32] = "", env_decay[32] = "";
    char env_sustain[32] = "", env_release[32] = "";
    double group_amp_db[DS_MAX_GROUPS];
    for (int i = 0; i < DS_MAX_GROUPS; i++) group_amp_db[i] = NAN;
    ds_group_oncc_t group_oncc[DS_MAX_GROUP_ONCC];
    int group_oncc_count = 0;
    /* Phase 4: instrument-level + tag-level AMP_VOLUME buckets. */
    ds_global_oncc_t global_oncc[DS_MAX_KNOBS];
    int global_oncc_count = 0;
    double global_amp_db_extra = 0.0;     /* silent-baseline contributions */
    ds_tag_oncc_t tag_oncc[DS_MAX_TAG_ONCC];
    int tag_oncc_count = 0;
    /* Phase 5: filter freq/resonance + pan live bindings. */
    ds_filter_freq_oncc_t filter_freq_oncc[DS_MAX_KNOBS];
    int filter_freq_oncc_count = 0;
    ds_filter_res_oncc_t  filter_res_oncc[DS_MAX_KNOBS];
    int filter_res_oncc_count = 0;
    ds_pan_oncc_t         pan_oncc[DS_MAX_KNOBS];
    int pan_oncc_count = 0;
    /* Phase 6: curve tables pre-baked from DS knob translations. */
    ds_curve_t curves[DS_MAX_CURVES];
    int curves_count = 0;
    apply_ui_overrides(src, fx, fx_count, env_attack, env_decay,
                       env_sustain, env_release, group_amp_db,
                       use_knobs, knob_count_local,
                       group_oncc, &group_oncc_count,
                       global_oncc, &global_oncc_count, &global_amp_db_extra,
                       tag_oncc, &tag_oncc_count,
                       filter_freq_oncc, &filter_freq_oncc_count,
                       filter_res_oncc, &filter_res_oncc_count,
                       pan_oncc, &pan_oncc_count,
                       curves, &curves_count);

    int rr_total = count_rr_groups(src);

    /* Phase 9/10: extract reverb + delay config from the dspreset's
     * <effect> list so the plugin can install matching fundsp units. */
    for (int i = 0; i < fx_count; i++) {
        if (out_reverb && !out_reverb->enabled &&
            strcmp(fx[i].type, "reverb") == 0) {
            out_reverb->enabled = 1;
            out_reverb->wet_level = fx[i].wet_level[0] ? atof(fx[i].wet_level) : 0.5;
            out_reverb->room_size = fx[i].room_size[0] ? atof(fx[i].room_size) : 0.5;
            out_reverb->damping   = fx[i].damping[0]   ? atof(fx[i].damping)   : 0.3;
        }
        if (out_delay && !out_delay->enabled &&
            strcmp(fx[i].type, "delay") == 0) {
            out_delay->enabled = 1;
            out_delay->delay_seconds = fx[i].delay_time[0] ? atof(fx[i].delay_time) : 0.25;
            out_delay->feedback      = fx[i].feedback[0]   ? atof(fx[i].feedback)   : 0.4;
            /* Default mix 0 (dry). Authors who want delay audible
             * either set wetLevel/mix on the <effect> tag or bind a
             * UI knob to FX_WET_LEVEL; either way overrides this. */
            out_delay->mix           = fx[i].wet_level[0] ? atof(fx[i].wet_level)
                                       : (fx[i].mix[0]    ? atof(fx[i].mix) : 0.0);
        }
        if (out_chorus && !out_chorus->enabled &&
            strcmp(fx[i].type, "chorus") == 0) {
            out_chorus->enabled = 1;
            /* Initial mix at 0 (dry) — author opts in via the
             * `<effect mix=>` attr or a UI knob bound to FX_MIX. */
            out_chorus->mix   = fx[i].mix[0]      ? atof(fx[i].mix)      : 0.0;
            out_chorus->rate  = 0.7;  /* fundsp chorus default */
            out_chorus->depth = 0.5;
        }
        if (out_phaser && !out_phaser->enabled &&
            strcmp(fx[i].type, "phaser") == 0) {
            out_phaser->enabled = 1;
            /* Author opts in via UI knob bound to FX_MIX; install dry. */
            out_phaser->mix      = fx[i].mix[0] ? atof(fx[i].mix) : 0.0;
            out_phaser->rate     = 0.4;  /* DS default phaser rate */
            out_phaser->depth    = 0.5;
            out_phaser->feedback = fx[i].feedback[0] ? atof(fx[i].feedback) : 0.5;
        }
    }

    int lp_idx = -1, gain_idx = -1;
    for (int i = 0; i < fx_count; i++) {
        if (lp_idx < 0 && (strcmp(fx[i].type, "lowpass") == 0 ||
                            strcmp(fx[i].type, "lowpass_1pl") == 0 ||
                            strcmp(fx[i].type, "lowpass_4pl") == 0 ||
                            strcmp(fx[i].type, "highpass") == 0 ||
                            strcmp(fx[i].type, "bandpass") == 0 ||
                            strcmp(fx[i].type, "notch") == 0 ||
                            strcmp(fx[i].type, "peak") == 0)) lp_idx = i;
        if (gain_idx < 0 && strcmp(fx[i].type, "gain") == 0) gain_idx = i;
    }

    /* Phase 7b / 2026-05-16: per-group modulator state. `level="group"`
     * <lfo>/<envelope> bindings target a specific group via `groupIndex`
     * or `position`; we collect their parameters here and emit them on
     * the matching `<group>` SFZ block during the group walk below.
     * Groups with no group-scoped modulators leave these zero (treated
     * as inactive by xsynth). */
    typedef struct {
        double amp_lfo_freq;
        double amp_lfo_depth_db;
        double fil_lfo_freq;
        double fil_lfo_depth_cents;
        double pan_lfo_freq;
        double pan_lfo_depth_pct;
        double pitch_lfo_freq;
        double pitch_lfo_depth_cents;
        double fileg_attack_s;
        double fileg_decay_s;
        double fileg_sustain;
        double fileg_release_s;
        double fileg_depth_cents;
        /* When fileg targets this group, override the static cutoff
         * to the env's low endpoint so the envelope swing is audible
         * (mirrors the global case in apply_ui_overrides). */
        double fileg_cutoff_floor;
    } per_group_mod_t;
    per_group_mod_t pgm[DS_MAX_GROUPS];
    memset(pgm, 0, sizeof(pgm));
    for (int i = 0; i < DS_MAX_GROUPS; i++) pgm[i].fileg_sustain = 1.0;

    /* === Phase 11: parse <modulators><lfo> blocks ===
     * MVP: amp LFO (tremolo). Look for any <lfo> whose first <binding>
     * targets LEVEL or AMP_VOLUME and emit `amplfo_freq` /
     * `amplfo_depth` on <global>.
     *
     * Also detect knob bindings to type="modulator" position="i" with
     * parameter="MOD_AMOUNT" or "FREQUENCY" — these are the dspreset's
     * LFO speed / depth controls. When found, route them through
     * amplfo_freq_oncc / amplfo_depth_oncc so the knob drives the
     * LFO live. */
    double amp_lfo_freq = 0.0;
    double amp_lfo_depth_db = 0.0;
    int    amp_lfo_freq_cc = -1, amp_lfo_depth_cc = -1;
    double amp_lfo_freq_delta = 0.0, amp_lfo_depth_delta = 0.0;
    /* Phase 11: filter LFO (curly's wobble). Depth in cents. */
    double fil_lfo_freq = 0.0;
    double fil_lfo_depth_cents = 0.0;
    int    fil_lfo_freq_cc = -1, fil_lfo_depth_cc = -1;
    double fil_lfo_freq_delta = 0.0, fil_lfo_depth_delta = 0.0;
    /* Phase 11: pan LFO. Depth in percent (-100..100). */
    double pan_lfo_freq = 0.0;
    double pan_lfo_depth_pct = 0.0;
    int    pan_lfo_freq_cc = -1, pan_lfo_depth_cc = -1;
    double pan_lfo_freq_delta = 0.0, pan_lfo_depth_delta = 0.0;
    /* Phase 11: filter envelope (autowah). Cents at peak. */
    double fileg_attack_s = 0.0, fileg_decay_s = 0.0;
    double fileg_sustain  = 1.0, fileg_release_s = 0.0;
    double fileg_depth_cents = 0.0;
    /* MOVE FORK / 2026-05-16: when DS's envelope binding declares
     * translation="linear" with explicit min/max Hz, the captured sweep
     * is linear-in-Hz. SFZ-spec fileg adds level·depth cents, which is
     * exponential — endpoints match but the midpoint differs by ~one
     * octave. We work around it by emitting a `<curve>` table that maps
     * [0,1] envelope level → [0,1] fraction of total exp-cents depth
     * such that the resulting Hz traces the linear-Hz line. xsynth-core
     * picks up the curve via the `fileg_curve=<id>` opcode we emit
     * alongside fileg_depth. linear_hz_lo/hi are the binding's
     * translationOutputMin/Max; depth_cents stays
     * 1200*log2(hi/lo). */
    int    fileg_linear_hz = 0;
    double fileg_linear_hz_lo = 0.0;
    double fileg_linear_hz_hi = 0.0;
    int    fileg_curve_id    = -1;
    /* Phase 11: pitch LFO (vibrato). Cents at peak. */
    double pitch_lfo_freq = 0.0;
    double pitch_lfo_depth_cents = 0.0;
    {
        const char *mp = strstr(src, "<modulators");
        const char *me = mp ? strstr(mp, "</modulators>") : NULL;
        /* Walk modulators in document order so position indexes match
         * the dspreset's `<binding position="i">` references. */
        const char *cur = mp ? mp + 12 : NULL;
        int mod_idx = 0;
        while (cur && (!me || cur < me)) {
            const char *lfo_open = strstr(cur, "<lfo");
            const char *env_open = strstr(cur, "<envelope");
            const char *next = NULL;
            int is_lfo = 0;
            if (lfo_open && env_open) {
                if (lfo_open < env_open) { next = lfo_open; is_lfo = 1; }
                else                       next = env_open;
            } else if (lfo_open) { next = lfo_open; is_lfo = 1; }
            else if (env_open)   next = env_open;
            if (!next || (me && next >= me)) break;
            const char *tag_end = strchr(next, '>');
            if (!tag_end) break;
            const char *close = is_lfo ? strstr(tag_end, "</lfo>")
                                       : strstr(tag_end, "</envelope>");
            if (!close) break;

            if (is_lfo) {
                char lfo_tag[512];
                int ll = (int)(tag_end - next);
                if (ll > 511) ll = 511;
                memcpy(lfo_tag, next, ll);
                lfo_tag[ll] = '\0';
                char freq_str[32], mod_amount_str[32];
                xml_get_attr(lfo_tag, "frequency", freq_str, sizeof(freq_str));
                xml_get_attr(lfo_tag, "modAmount", mod_amount_str, sizeof(mod_amount_str));
                double freq = freq_str[0] ? atof(freq_str) : 1.0;
                double mod_amount = mod_amount_str[0] ? atof(mod_amount_str) : 1.0;

                /* Scan inner bindings for amp-targeting (LEVEL / AMP_VOLUME). */
                const char *bp = tag_end + 1;
                while (bp && bp < close) {
                    const char *bt = strstr(bp, "<binding");
                    if (!bt || bt >= close) break;
                    const char *bte = strchr(bt, '>');
                    if (!bte) break;
                    char btag[512];
                    int btl = (int)(bte - bt);
                    if (btl > 511) btl = 511;
                    memcpy(btag, bt, btl);
                    btag[btl] = '\0';
                    char param[64], omin[32], omax[32], blevel[16],
                         bgrp[16], bpos[16];
                    xml_get_attr(btag, "parameter", param, sizeof(param));
                    xml_get_attr(btag, "translationOutputMin", omin, sizeof(omin));
                    xml_get_attr(btag, "translationOutputMax", omax, sizeof(omax));
                    xml_get_attr(btag, "level", blevel, sizeof(blevel));
                    xml_get_attr(btag, "groupIndex", bgrp, sizeof(bgrp));
                    xml_get_attr(btag, "position", bpos, sizeof(bpos));
                    /* Phase 7b: route level="group" modulator bindings
                     * to per-group state. `groupIndex` (newer DS) and
                     * `position` (older DS) both name the target
                     * group's 0-based index in the <groups> list. */
                    int target_group = -1;
                    int is_group_scoped = (blevel[0] &&
                                           strcmp(blevel, "group") == 0);
                    if (is_group_scoped) {
                        if (bgrp[0]) target_group = atoi(bgrp);
                        else if (bpos[0]) target_group = atoi(bpos);
                        if (target_group < 0 || target_group >= DS_MAX_GROUPS) {
                            bp = bte + 1; continue;
                        }
                    } else if (blevel[0] && strcmp(blevel, "instrument") != 0) {
                        /* Unknown scope (e.g. tag) — skip. */
                        bp = bte + 1; continue;
                    }
                    if (strcmp(param, "FX_FILTER_FREQUENCY") == 0
                            && (is_group_scoped ||
                                (fil_lfo_freq == 0.0 && fil_lfo_freq_cc < 0))) {
                        double output_min = omin[0] ? atof(omin) : 0.0;
                        double output_max = omax[0] ? atof(omax) : 1.0;
                        double full_swing_hz = (output_max - output_min) * 0.5;
                        if (full_swing_hz < 0) full_swing_hz = -full_swing_hz;
                        /* Rough Hz→cents mapping: 1200 cents = one octave.
                         * We treat outputMax/2 as the Hz swing around a
                         * nominal 1 kHz center; depth_cents=log2(1+swing/1k).
                         * Works well at typical cutoff values (300-3kHz).
                         * Cap at 2400 cents (±2 octaves). */
                        double swing_ratio = full_swing_hz / 1000.0;
                        double max_depth_cents = 1200.0 * log2(1.0 + swing_ratio);
                        if (max_depth_cents > 2400.0) max_depth_cents = 2400.0;
                        if (is_group_scoped) {
                            pgm[target_group].fil_lfo_freq = freq;
                            pgm[target_group].fil_lfo_depth_cents = max_depth_cents * mod_amount;
                            bp = bte + 1; continue;
                        }
                        fil_lfo_freq = freq;
                        fil_lfo_depth_cents = max_depth_cents * mod_amount;

                        /* Knob routing for filter LFO (same pattern as
                         * amp LFO below). Find knobs whose binding is
                         * type="modulator" position="mod_idx"
                         * parameter="FREQUENCY"/"MOD_AMOUNT" and route
                         * via fillfo_freq_oncc / fillfo_depth_oncc. */
                        char fneedle[96];
                        snprintf(fneedle, sizeof(fneedle),
                                 "type=\"modulator\" position=\"%d\"", mod_idx);
                        const char *kp2 = src;
                        int kwi = 0;
                        while (kp2) {
                            const char *c1 = strstr(kp2, "<control");
                            const char *c2 = strstr(kp2, "<labeled-knob");
                            const char *ctrl_start = NULL;
                            size_t pat_len = 0;
                            if (c1 && c2) {
                                if (c1 < c2) { ctrl_start = c1; pat_len = 8; }
                                else         { ctrl_start = c2; pat_len = 13; }
                            } else if (c1) { ctrl_start = c1; pat_len = 8; }
                            else if (c2)   { ctrl_start = c2; pat_len = 13; }
                            else break;
                            char nxt = ctrl_start[pat_len];
                            if (nxt != ' ' && nxt != '\t' && nxt != '\n' &&
                                nxt != '\r' && nxt != '>' && nxt != '/') {
                                kp2 = ctrl_start + pat_len; continue;
                            }
                            const char *cte = strchr(ctrl_start, '>');
                            if (!cte) break;
                            int self_closed = (*(cte - 1) == '/');
                            const char *ctrl_close = NULL;
                            if (!self_closed) {
                                const char *e1 = strstr(cte, "</control>");
                                const char *e2 = strstr(cte, "</labeled-knob>");
                                if (e1 && e2)       ctrl_close = (e1 < e2) ? e1 : e2;
                                else if (e1)        ctrl_close = e1;
                                else if (e2)        ctrl_close = e2;
                            }
                            const char *bes = self_closed ? cte : ctrl_close;
                            if (bes) {
                                const char *bsc = cte + 1;
                                while (bsc < bes) {
                                    const char *bb = strstr(bsc, "<binding");
                                    if (!bb || bb >= bes) break;
                                    const char *bbe = strchr(bb, '>');
                                    if (!bbe) break;
                                    char bbtag[512];
                                    int bbl = (int)(bbe - bb);
                                    if (bbl > 511) bbl = 511;
                                    memcpy(bbtag, bb, bbl);
                                    bbtag[bbl] = '\0';
                                    if (strstr(bbtag, fneedle)) {
                                        char bparam[64];
                                        xml_get_attr(bbtag, "parameter", bparam, sizeof(bparam));
                                        if (kwi < knob_count_local) {
                                            const ds_knob_t *kn = &use_knobs[kwi];
                                            if (kn->cc_number >= 0) {
                                                if (strcmp(bparam, "FREQUENCY") == 0
                                                        && fil_lfo_freq_cc < 0) {
                                                    fil_lfo_freq_cc = kn->cc_number;
                                                    fil_lfo_freq = kn->min_value;
                                                    fil_lfo_freq_delta = kn->max_value - kn->min_value;
                                                }
                                                if (strcmp(bparam, "MOD_AMOUNT") == 0
                                                        && fil_lfo_depth_cc < 0) {
                                                    fil_lfo_depth_cc = kn->cc_number;
                                                    fil_lfo_depth_cents = 0.0;
                                                    fil_lfo_depth_delta = max_depth_cents;
                                                }
                                            }
                                        }
                                    }
                                    bsc = bbe + 1;
                                }
                            }
                            kwi++;
                            kp2 = self_closed ? cte + 1 : (ctrl_close ? ctrl_close + 1 : cte + 1);
                        }
                    }
                    if ((strcmp(param, "PITCH") == 0 ||
                         strcmp(param, "GROUP_TUNING") == 0)
                            && (is_group_scoped || pitch_lfo_freq == 0.0)) {
                        double output_min = omin[0] ? atof(omin) : -1.0;
                        double output_max = omax[0] ? atof(omax) :  1.0;
                        double full_swing = (output_max - output_min) * 0.5;
                        if (full_swing < 0) full_swing = -full_swing;
                        /* DS PITCH binding output unit = semitones for
                         * GROUP_TUNING, fractional cents for PITCH. Treat
                         * full_swing as semitones — depth_cents = swing * 100.
                         * Cap ±1200 cents (one octave). */
                        double depth_cents = full_swing * 100.0 * mod_amount;
                        if (depth_cents > 1200.0) depth_cents = 1200.0;
                        if (is_group_scoped) {
                            pgm[target_group].pitch_lfo_freq = freq;
                            pgm[target_group].pitch_lfo_depth_cents = depth_cents;
                            bp = bte + 1; continue;
                        }
                        pitch_lfo_freq = freq;
                        pitch_lfo_depth_cents = depth_cents;
                    }
                    if (strcmp(param, "PAN") == 0
                            && (is_group_scoped ||
                                (pan_lfo_freq == 0.0 && pan_lfo_freq_cc < 0))) {
                        double output_min = omin[0] ? atof(omin) : -1.0;
                        double output_max = omax[0] ? atof(omax) :  1.0;
                        double full_swing = (output_max - output_min) * 0.5;
                        if (full_swing < 0) full_swing = -full_swing;
                        /* DS pan is -1..1; SFZ pan_lfo depth is in
                         * percent (-100..100). Full swing of 1 → 100%. */
                        double max_depth_pct = full_swing * 100.0;
                        if (max_depth_pct > 100.0) max_depth_pct = 100.0;
                        if (is_group_scoped) {
                            pgm[target_group].pan_lfo_freq = freq;
                            pgm[target_group].pan_lfo_depth_pct = max_depth_pct * mod_amount;
                            bp = bte + 1; continue;
                        }
                        pan_lfo_freq = freq;
                        pan_lfo_depth_pct = max_depth_pct * mod_amount;
                    }
                    if ((strcmp(param, "LEVEL") == 0 || strcmp(param, "AMP_VOLUME") == 0)
                            && (is_group_scoped ||
                                (amp_lfo_freq == 0.0 && amp_lfo_freq_cc < 0))) {
                        double output_min = omin[0] ? atof(omin) : 0.0;
                        double output_max = omax[0] ? atof(omax) : 1.0;
                        double range = output_max - output_min;
                        if (range < 0) range = -range;
                        /* DS interprets AMP_VOLUME range (typically 0..1)
                         * as full-scale multiplicative modulation —
                         * audible swing from silence to peak. Empirically
                         * matched against DS at vel=100 with output 0..1:
                         * 25 dB depth lands the parity at gain Δ ≈ 0.7 dB
                         * vs the previous 20·log10(1+swing) ≈ 3.5 dB
                         * which was 11 dB too quiet. Cap at 36 dB so an
                         * out-of-spec 0..N range doesn't overflow. */
                        double max_depth = range * 25.0;
                        if (max_depth > 36.0) max_depth = 36.0;
                        if (is_group_scoped) {
                            pgm[target_group].amp_lfo_freq = freq;
                            pgm[target_group].amp_lfo_depth_db = max_depth * mod_amount;
                            bp = bte + 1; continue;
                        }
                        /* Default static: scale by XML modAmount. */
                        amp_lfo_freq = freq;
                        amp_lfo_depth_db = max_depth * mod_amount;

                        /* Search for knob bindings to this modulator (mod_idx). */
                        char needle[96];
                        snprintf(needle, sizeof(needle),
                                 "type=\"modulator\" position=\"%d\"", mod_idx);
                        for (int ki = 0; ki < knob_count_local; ki++) {
                            const ds_knob_t *k = &use_knobs[ki];
                            if (k->cc_number < 0) continue;
                            /* Look up this knob's tag in src to inspect bindings. */
                            char target_key[32];
                            snprintf(target_key, sizeof(target_key), "knob_%d", ki);
                            /* We have the knob's cc + min/max already. Just
                             * check if it has a modulator binding to mod_idx
                             * by re-scanning src for the matching binding
                             * substring near this knob. */
                            /* Simpler: scan all bindings in src for
                             *   type="modulator" position="mod_idx"
                             *   parameter="MOD_AMOUNT" or "FREQUENCY"
                             * tied to ANY knob — assume knob index in
                             * document order matches ki. Then test if the
                             * surrounding control has this knob's cc. */
                            (void)target_key;
                        }

                        /* Find first knob whose binding targets this LFO. */
                        const char *kp = src;
                        int knob_walk_idx = 0;
                        while (kp) {
                            const char *c1 = strstr(kp, "<control");
                            const char *c2 = strstr(kp, "<labeled-knob");
                            const char *ctrl_start = NULL;
                            size_t pat_len = 0;
                            if (c1 && c2) {
                                if (c1 < c2) { ctrl_start = c1; pat_len = 8; }
                                else         { ctrl_start = c2; pat_len = 13; }
                            } else if (c1) { ctrl_start = c1; pat_len = 8; }
                            else if (c2)   { ctrl_start = c2; pat_len = 13; }
                            else break;
                            char nxt = ctrl_start[pat_len];
                            if (nxt != ' ' && nxt != '\t' && nxt != '\n' &&
                                nxt != '\r' && nxt != '>' && nxt != '/') {
                                kp = ctrl_start + pat_len; continue;
                            }
                            const char *ctrl_tag_end = strchr(ctrl_start, '>');
                            if (!ctrl_tag_end) break;
                            int self_closed = (*(ctrl_tag_end - 1) == '/');
                            const char *ctrl_close = NULL;
                            if (!self_closed) {
                                const char *e1 = strstr(ctrl_tag_end, "</control>");
                                const char *e2 = strstr(ctrl_tag_end, "</labeled-knob>");
                                if (e1 && e2)       ctrl_close = (e1 < e2) ? e1 : e2;
                                else if (e1)        ctrl_close = e1;
                                else if (e2)        ctrl_close = e2;
                            }
                            /* Search bindings inside this control for the
                             * modulator targeting our mod_idx. */
                            const char *bend_search = self_closed ? ctrl_tag_end : ctrl_close;
                            if (bend_search && strstr(ctrl_tag_end + 1, needle) &&
                                strstr(ctrl_tag_end + 1, needle) < bend_search) {
                                const char *bind_scan = ctrl_tag_end + 1;
                                while (bind_scan < bend_search) {
                                    const char *bb = strstr(bind_scan, "<binding");
                                    if (!bb || bb >= bend_search) break;
                                    const char *bbe = strchr(bb, '>');
                                    if (!bbe) break;
                                    char bbtag[512];
                                    int bbl = (int)(bbe - bb);
                                    if (bbl > 511) bbl = 511;
                                    memcpy(bbtag, bb, bbl);
                                    bbtag[bbl] = '\0';
                                    if (strstr(bbtag, needle)) {
                                        char bparam[64];
                                        xml_get_attr(bbtag, "parameter", bparam, sizeof(bparam));
                                        if (knob_walk_idx < knob_count_local) {
                                            const ds_knob_t *kn = &use_knobs[knob_walk_idx];
                                            if (kn->cc_number >= 0) {
                                                if (strcmp(bparam, "FREQUENCY") == 0
                                                        && amp_lfo_freq_cc < 0) {
                                                    amp_lfo_freq_cc = kn->cc_number;
                                                    amp_lfo_freq = kn->min_value;
                                                    amp_lfo_freq_delta = kn->max_value - kn->min_value;
                                                }
                                                if (strcmp(bparam, "MOD_AMOUNT") == 0
                                                        && amp_lfo_depth_cc < 0) {
                                                    amp_lfo_depth_cc = kn->cc_number;
                                                    /* Base depth = 0 (knob at min); delta = max depth. */
                                                    amp_lfo_depth_db = 0.0;
                                                    amp_lfo_depth_delta = max_depth;
                                                }
                                            }
                                        }
                                    }
                                    bind_scan = bbe + 1;
                                }
                            }
                            knob_walk_idx++;
                            kp = self_closed ? ctrl_tag_end + 1
                                              : (ctrl_close ? ctrl_close + 1 : ctrl_tag_end + 1);
                        }
                    }
                    bp = bte + 1;
                }
            }
            if (!is_lfo) {
                /* Phase 11: envelope binding to FX_FILTER_FREQUENCY = autowah. */
                char env_tag[512];
                int el = (int)(tag_end - next);
                if (el > 511) el = 511;
                memcpy(env_tag, next, el);
                env_tag[el] = '\0';
                char a[32]={0}, d[32]={0}, s[32]={0}, rl[32]={0};
                xml_get_attr(env_tag, "attack",  a,  sizeof(a));
                xml_get_attr(env_tag, "decay",   d,  sizeof(d));
                xml_get_attr(env_tag, "sustain", s,  sizeof(s));
                xml_get_attr(env_tag, "release", rl, sizeof(rl));
                const char *bp = tag_end + 1;
                while (bp && bp < close) {
                    const char *bt = strstr(bp, "<binding");
                    if (!bt || bt >= close) break;
                    const char *bte = strchr(bt, '>');
                    if (!bte) break;
                    char btag[512];
                    int btl = (int)(bte - bt);
                    if (btl > 511) btl = 511;
                    memcpy(btag, bt, btl);
                    btag[btl] = '\0';
                    char param[64], omin[32], omax[32], ttable[256], txform[32],
                         blevel[16], bgrp[16], bpos[16];
                    xml_get_attr(btag, "parameter", param, sizeof(param));
                    xml_get_attr(btag, "translationOutputMin", omin, sizeof(omin));
                    xml_get_attr(btag, "translationOutputMax", omax, sizeof(omax));
                    xml_get_attr(btag, "translationTable", ttable, sizeof(ttable));
                    xml_get_attr(btag, "translation", txform, sizeof(txform));
                    xml_get_attr(btag, "level", blevel, sizeof(blevel));
                    xml_get_attr(btag, "groupIndex", bgrp, sizeof(bgrp));
                    xml_get_attr(btag, "position", bpos, sizeof(bpos));
                    /* Phase 7b: route level="group" envelope bindings to
                     * per-group fileg state. */
                    int target_group = -1;
                    int is_group_scoped = (blevel[0] &&
                                           strcmp(blevel, "group") == 0);
                    if (is_group_scoped) {
                        if (bgrp[0]) target_group = atoi(bgrp);
                        else if (bpos[0]) target_group = atoi(bpos);
                        if (target_group < 0 || target_group >= DS_MAX_GROUPS) {
                            bp = bte + 1; continue;
                        }
                    } else if (blevel[0] && strcmp(blevel, "instrument") != 0) {
                        bp = bte + 1; continue;
                    }
                    if (strcmp(param, "FX_FILTER_FREQUENCY") == 0
                            && (is_group_scoped || fileg_depth_cents == 0.0)) {
                        double output_min = omin[0] ? atof(omin) : 0.0;
                        double output_max = omax[0] ? atof(omax) : 1.0;
                        /* When the binding uses translation="table" with
                         * an explicit translationTable, the Hz endpoints
                         * are encoded as the table's first/last `y` values
                         * (`x,y;x,y;...`). Without this, swing collapses to
                         * 0..1 default and depth_cents is inaudible
                         * (WobbliTzer's autowah was emitting depth=1.7
                         * cents — basically no autowah). */
                        if (strcmp(txform, "table") == 0 && ttable[0]) {
                            double tmin = 1e9, tmax = -1e9;
                            const char *pp = ttable;
                            while (*pp) {
                                const char *comma = strchr(pp, ',');
                                const char *semi  = strchr(pp, ';');
                                if (!comma) break;
                                double y = atof(comma + 1);
                                if (y < tmin) tmin = y;
                                if (y > tmax) tmax = y;
                                if (!semi) break;
                                pp = semi + 1;
                            }
                            if (tmax > tmin) {
                                output_min = tmin;
                                output_max = tmax;
                            }
                        }
                        double swing_hz = output_max - output_min;
                        if (swing_hz < 0) swing_hz = -swing_hz;
                        /* Direct log2 ratio of the table endpoints — at
                         * env peak, cutoff = output_min * 2^(depth/1200)
                         * should land at output_max. Cap at 9600 cents
                         * (~8 octaves) which covers a 33 Hz → 22 kHz
                         * sweep. base cutoff is overridden to output_min
                         * below so the envelope swings the full range. */
                        double max_depth_cents = 1200.0 * log2(output_max / (output_min > 1.0 ? output_min : 1.0));
                        if (max_depth_cents > 9600.0) max_depth_cents = 9600.0;
                        if (max_depth_cents < 0.0)    max_depth_cents = 0.0;
                        if (is_group_scoped) {
                            pgm[target_group].fileg_depth_cents = max_depth_cents;
                            pgm[target_group].fileg_attack_s  = a[0]  ? atof(a)  : 0.0;
                            pgm[target_group].fileg_decay_s   = d[0]  ? atof(d)  : 0.0;
                            pgm[target_group].fileg_sustain   = s[0]  ? atof(s)  : 1.0;
                            pgm[target_group].fileg_release_s = rl[0] ? atof(rl) : 0.0;
                            /* Group-scoped fileg needs its OWN cutoff
                             * floor (per-region cutoff= on this group's
                             * regions). The global lowpass stays at its
                             * authored cutoff for other groups. */
                            if (output_min > 0.0 && output_min < output_max) {
                                pgm[target_group].fileg_cutoff_floor = output_min;
                            }
                            bp = bte + 1; continue;
                        }
                        fileg_depth_cents = max_depth_cents;
                        fileg_attack_s  = a[0]  ? atof(a)  : 0.0;
                        fileg_decay_s   = d[0]  ? atof(d)  : 0.0;
                        fileg_sustain   = s[0]  ? atof(s)  : 1.0;
                        fileg_release_s = rl[0] ? atof(rl) : 0.0;
                        /* MOVE FORK / 2026-05-16: linear-Hz sweep
                         * needs a curve table to match DS's
                         * midpoint. See the fileg_linear_hz_* decl
                         * above. Only meaningful with explicit
                         * Min/Max attrs (range > 0). */
                        if ((!txform[0] || strcmp(txform, "linear") == 0)
                                && output_min > 0.0 && output_max > output_min
                                && curves_count < DS_MAX_CURVES) {
                            fileg_linear_hz    = 1;
                            fileg_linear_hz_lo = output_min;
                            fileg_linear_hz_hi = output_max;
                            fileg_curve_id     = 100 + curves_count;
                            ds_curve_t *cv     = &curves[curves_count++];
                            cv->id = fileg_curve_id;
                            double denom = log2(output_max / output_min);
                            for (int ci = 0; ci < 128; ci++) {
                                double t = (double)ci / 127.0;
                                double f = output_min +
                                           (output_max - output_min) * t;
                                cv->v[ci] = log2(f / output_min) / denom;
                            }
                        }
                        /* Override the static cutoff to the envelope's
                         * low endpoint so the filter starts CLOSED and
                         * opens to the high endpoint at envelope peak.
                         * Without this, the lowpass sits at 22 kHz (wide
                         * open) and depth_cents adds on top — past
                         * Nyquist, sanitize clamps, autowah inaudible. */
                        if (lp_idx >= 0 && output_min > 0.0 &&
                                output_min < output_max) {
                            snprintf(fx[lp_idx].freq,
                                     sizeof(fx[lp_idx].freq),
                                     "%.2f", output_min);
                        }
                    }
                    bp = bte + 1;
                }
            }
            cur = close + (is_lfo ? 6 : 11);
            mod_idx++;
        }
    }

    /* === <global>: combined static defaults === */
    char *groups_tag = strstr(src, "<groups");
    char wrap_attack[64] = "", wrap_decay[64] = "";
    char wrap_sustain[64] = "", wrap_release[64] = "";
    char wrap_volume[64] = "", wrap_loop[64] = "";
    char wrap_seqmode[32] = "";
    if (groups_tag) {
        char *gt_end = strchr(groups_tag, '>');
        if (gt_end) {
            char tag_buf[1024];
            int tlen = gt_end - groups_tag;
            if (tlen >= (int)sizeof(tag_buf)) tlen = sizeof(tag_buf) - 1;
            memcpy(tag_buf, groups_tag, tlen);
            tag_buf[tlen] = '\0';
            xml_get_attr(tag_buf, "volume",      wrap_volume,  sizeof(wrap_volume));
            xml_get_attr(tag_buf, "attack",      wrap_attack,  sizeof(wrap_attack));
            xml_get_attr(tag_buf, "decay",       wrap_decay,   sizeof(wrap_decay));
            xml_get_attr(tag_buf, "sustain",     wrap_sustain, sizeof(wrap_sustain));
            xml_get_attr(tag_buf, "release",     wrap_release, sizeof(wrap_release));
            xml_get_attr(tag_buf, "loopEnabled", wrap_loop,    sizeof(wrap_loop));
            /* MOVE FORK / 2026-05-16: DS allows `seqMode` on the
             * `<groups>` parent — every child `<group>` inherits it.
             * Without this propagation, RR presets like 18_round_robin
             * (seqMode on <groups>, seqPosition on each <group>) fired
             * every group simultaneously. */
            xml_get_attr(tag_buf, "seqMode",     wrap_seqmode, sizeof(wrap_seqmode));
        }
    }

    /* Phase 6: emit `<curve>` blocks before any `<global>`. Each curve
     * pre-bakes the DS knob's translation into 128 sample points; xsynth
     * SIMD generators look up curve[cc] in place of cc/127 when a
     * matching `_curvecc<N>=<id>` opcode references it. */
    for (int ci = 0; ci < curves_count; ci++) {
        pos += snprintf(sfz + pos, out_cap - pos,
                        "<curve>\nindex=%d\n", curves[ci].id);
        for (int pi = 0; pi < 128; pi++) {
            pos += snprintf(sfz + pos, out_cap - pos,
                            "v%03d=%.6f\n", pi, curves[ci].v[pi]);
        }
    }

    pos += snprintf(sfz + pos, out_cap - pos, "<global>\n");

    /* Count `<group>` elements that will produce audible voices on
     * NoteOn (skip release-trigger groups and explicitly disabled
     * groups). DS auto-scales the bus to keep the sum from clipping
     * when multiple groups layer onto the same note; without an
     * equivalent we drove the int16 ceiling and clipped through the
     * tanhf soft-clip on multi-group presets like Cosmos or K4.
     * Power-preserving headroom (-3 dB per group doubling) keeps
     * single-group presets unchanged and N-group presets at the same
     * peak as DS. */
    int active_group_count = 0;
    {
        const char *gp = src;
        while ((gp = strstr(gp, "<group")) != NULL) {
            if (gp[6] == 's' || gp[6] == 'S') { gp += 7; continue; }
            const char *gt = strchr(gp, '>');
            if (!gt) break;
            char gtb[1024];
            int gtl = gt - gp; if (gtl >= (int)sizeof(gtb)) gtl = sizeof(gtb)-1;
            memcpy(gtb, gp, gtl); gtb[gtl] = '\0';
            char trig[16] = "", ena[16] = "";
            xml_get_attr(gtb, "trigger", trig, sizeof(trig));
            xml_get_attr(gtb, "enabled", ena,  sizeof(ena));
            int is_release_trig = (strcmp(trig, "release") == 0);
            int is_disabled     = (strcmp(ena,  "false")   == 0);
            if (!is_release_trig && !is_disabled) active_group_count++;
            gp = gt + 1;
        }
    }
    double mix_headroom_db = 0.0;
    if (active_group_count > 1) {
        /* -10·log10(N): power-preserving for correlated signals. */
        mix_headroom_db = -10.0 *
                          (log(active_group_count) / log(10.0));
    }

    /* Volume: wrapper attr + gain effect (both in dB). xsynth caps volume
     * at +6 dB and clamps to -144 dB, so we add and pass through.
     * xsynth's SFZ parser uses `parse_i16_in_range(-144..=6)` and rejects
     * decimals — emit as integer. */
    double global_vol_db = mix_headroom_db;
    if (wrap_volume[0]) global_vol_db += atof(wrap_volume);
    if (gain_idx >= 0 && fx[gain_idx].level[0]) global_vol_db += atof(fx[gain_idx].level);
    /* `<global> volume=` carries only the static-only pieces
     * (`<groups volume=>` wrapper + gain effect). Phase 4 oncc shifts
     * (instrument / tag / group level) are emitted per-group below
     * so they can be scaled to fit xsynth's -96 dB voice-amp floor
     * when multiple silent-endpoint shifts stack. */
    if (global_vol_db != 0.0) {
        double v = global_vol_db;
        if (v < -144) v = -144;
        if (v >  6.0) v =  6.0;
        int gvi = (int)(v >= 0 ? v + 0.5 : v - 0.5);
        pos += snprintf(sfz + pos, out_cap - pos, "volume=%d\n", gvi);
    }
    /* Phase 4: do NOT emit instrument-level volume_oncc on <global>.
     * Multiple stacked silent-endpoint shifts (instrument + tag +
     * group) can exceed xsynth's -96 dB spawner clamp; we need to
     * scale proportionally per-group. Emission of instrument-level
     * deltas is done per-group below, alongside tag-level and
     * group-level, so all three can be scaled together. */

    /* ADSR: knob `value=` from apply_ui_overrides wins over wrapper attr —
     * the knob represents what the user "sees", and on xsynth there's no
     * live CC so the knob is purely a static authored value. */
    const char *use_attack  = env_attack[0]  ? env_attack  : wrap_attack;
    const char *use_decay   = env_decay[0]   ? env_decay   : wrap_decay;
    const char *use_sustain = env_sustain[0] ? env_sustain : wrap_sustain;
    const char *use_release = env_release[0] ? env_release : wrap_release;
    if (use_attack[0])
        pos += snprintf(sfz + pos, out_cap - pos, "ampeg_attack=%s\n", use_attack);
    if (use_decay[0])
        pos += snprintf(sfz + pos, out_cap - pos, "ampeg_decay=%s\n", use_decay);
    if (use_sustain[0]) {
        float s = atof(use_sustain) * 100.0f;
        if (s < 0) s = 0; if (s > 100) s = 100;
        pos += snprintf(sfz + pos, out_cap - pos, "ampeg_sustain=%.1f\n", s);
    }
    if (use_release[0]) {
        pos += snprintf(sfz + pos, out_cap - pos, "ampeg_release=%s\n", use_release);
    } else {
        /* Same fallback as the sfizz converter — many DS presets omit release
         * but xsynth's default is too short; 0.5 s keeps piano tails alive. */
        pos += snprintf(sfz + pos, out_cap - pos, "ampeg_release=0.5\n");
    }
    /* Phase 11: amp LFO from <modulators><lfo binding=LEVEL/AMP_VOLUME>.
     * `amp_lfo_freq_cc` set when a knob binds to type="modulator"
     * parameter="FREQUENCY"; same for depth via MOD_AMOUNT. The base
     * + delta lets the host drive the LFO live from those knobs. */
    if (amp_lfo_freq > 0.0 || amp_lfo_freq_cc >= 0) {
        pos += snprintf(sfz + pos, out_cap - pos,
                        "amplfo_freq=%.4f\n", amp_lfo_freq);
        if (amp_lfo_freq_cc >= 0) {
            pos += snprintf(sfz + pos, out_cap - pos,
                            "amplfo_freq_oncc%d=%.4f\n",
                            amp_lfo_freq_cc, amp_lfo_freq_delta);
        }
    }
    if (amp_lfo_depth_db > 0.0 || amp_lfo_depth_cc >= 0) {
        pos += snprintf(sfz + pos, out_cap - pos,
                        "amplfo_depth=%.2f\n", amp_lfo_depth_db);
        if (amp_lfo_depth_cc >= 0) {
            pos += snprintf(sfz + pos, out_cap - pos,
                            "amplfo_depth_oncc%d=%.2f\n",
                            amp_lfo_depth_cc, amp_lfo_depth_delta);
        }
    }
    /* Phase 11: filter LFO. */
    if (fil_lfo_freq > 0.0 || fil_lfo_freq_cc >= 0) {
        pos += snprintf(sfz + pos, out_cap - pos,
                        "fillfo_freq=%.4f\n", fil_lfo_freq);
        if (fil_lfo_freq_cc >= 0) {
            pos += snprintf(sfz + pos, out_cap - pos,
                            "fillfo_freq_oncc%d=%.4f\n",
                            fil_lfo_freq_cc, fil_lfo_freq_delta);
        }
    }
    if (fil_lfo_depth_cents > 0.0 || fil_lfo_depth_cc >= 0) {
        pos += snprintf(sfz + pos, out_cap - pos,
                        "fillfo_depth=%.2f\n", fil_lfo_depth_cents);
        if (fil_lfo_depth_cc >= 0) {
            pos += snprintf(sfz + pos, out_cap - pos,
                            "fillfo_depth_oncc%d=%.2f\n",
                            fil_lfo_depth_cc, fil_lfo_depth_delta);
        }
    }
    /* Phase 11: pan LFO. */
    if (pan_lfo_freq > 0.0 || pan_lfo_freq_cc >= 0) {
        pos += snprintf(sfz + pos, out_cap - pos,
                        "panlfo_freq=%.4f\n", pan_lfo_freq);
        if (pan_lfo_freq_cc >= 0) {
            pos += snprintf(sfz + pos, out_cap - pos,
                            "panlfo_freq_oncc%d=%.4f\n",
                            pan_lfo_freq_cc, pan_lfo_freq_delta);
        }
    }
    if (pan_lfo_depth_pct > 0.0 || pan_lfo_depth_cc >= 0) {
        pos += snprintf(sfz + pos, out_cap - pos,
                        "panlfo_depth=%.2f\n", pan_lfo_depth_pct);
        if (pan_lfo_depth_cc >= 0) {
            pos += snprintf(sfz + pos, out_cap - pos,
                            "panlfo_depth_oncc%d=%.2f\n",
                            pan_lfo_depth_cc, pan_lfo_depth_delta);
        }
    }
    /* Phase 11: filter envelope (autowah). */
    if (fileg_depth_cents > 0.0) {
        pos += snprintf(sfz + pos, out_cap - pos,
                        "fileg_attack=%.4f\nfileg_decay=%.4f\nfileg_sustain=%.4f\n"
                        "fileg_release=%.4f\nfileg_depth=%.2f\n",
                        fileg_attack_s, fileg_decay_s, fileg_sustain,
                        fileg_release_s, fileg_depth_cents);
        /* MOVE FORK / 2026-05-16: bake DS's linear-Hz envelope sweep
         * into a curve table so xsynth (SFZ exp-cents domain) lands on
         * DS's midpoint. Without the curve our endpoints match (200 /
         * 8000 Hz) but the half-attack point sits an octave high. */
        if (fileg_linear_hz && fileg_curve_id >= 0) {
            pos += snprintf(sfz + pos, out_cap - pos,
                            "fileg_curve=%d\n", fileg_curve_id);
        }
    }
    /* Phase 11: pitch LFO (vibrato). */
    if (pitch_lfo_freq > 0.0 && pitch_lfo_depth_cents > 0.0) {
        pos += snprintf(sfz + pos, out_cap - pos,
                        "pitchlfo_freq=%.4f\npitchlfo_depth=%.2f\n",
                        pitch_lfo_freq, pitch_lfo_depth_cents);
    }
    /* MOVE FORK / 2026-05-16: do NOT emit loop_mode at <global>. Each
     * region's loop state is set per-<sample> below — when a sample
     * has no explicit `loopEnabled`, it defaults to one-shot rather
     * than inheriting from a sibling that opted into looping. */

    /* Filter: map DS filter types to xsynth's SFZ parser variants.
     * xsynth supports lpf_{1,2,4,6}p, hpf_{1,2,4,6}p, bpf_{1,2}p. DS
     * has more types; we approximate where necessary:
     *   lowpass        → lpf_2p   (DS docs: 2-pole)
     *   lowpass_1pl    → lpf_1p
     *   lowpass_4pl    → lpf_4p
     *   highpass       → hpf_2p
     *   bandpass       → bpf_2p
     *   notch / peak   → bpf_2p   (closest xsynth has; not strictly
     *                              correct but better than dropping
     *                              the filter and letting the dry
     *                              signal through unchanged) */
    if (lp_idx >= 0) {
        const char *src_type = fx[lp_idx].type;
        const char *fil_type = "lpf_2p";
        if (strcmp(src_type, "lowpass_1pl") == 0) fil_type = "lpf_1p";
        else if (strcmp(src_type, "lowpass_4pl") == 0) fil_type = "lpf_4p";
        else if (strcmp(src_type, "highpass") == 0) fil_type = "hpf_2p";
        else if (strcmp(src_type, "bandpass") == 0) fil_type = "bpf_2p";
        else if (strcmp(src_type, "notch") == 0) fil_type = "bpf_2p";
        else if (strcmp(src_type, "peak") == 0) fil_type = "bpf_2p";
        /* default lpf_2p covers DS "lowpass" */
        const char *cutoff = fx[lp_idx].freq[0]      ? fx[lp_idx].freq      : "22000";
        const char *q      = fx[lp_idx].resonance[0] ? fx[lp_idx].resonance : "0.7";
        pos += snprintf(sfz + pos, out_cap - pos,
                        "fil_type=%s\ncutoff=%s\nresonance=%s\n",
                        fil_type, cutoff, q);
        /* Phase 5: live filter sweep from FX_FILTER_FREQUENCY /
         * FX_FILTER_RESONANCE knob bindings. xsynth-core picks up these
         * opcodes via SIMD*VoiceCutoffLive and recomputes biquad
         * coefficients per render block. */
        /* When a filter envelope is present (autowah), suppress the
         * static Filter-knob cutoff_oncc emit — the envelope alone
         * drives cutoff so its full swing is audible. Otherwise the
         * knob's default position (e.g. WobbliTzer Filter=0.82) raises
         * the base cutoff to ~6 kHz before the envelope kicks in,
         * leaving the env's sweep happening above audible range and
         * the autowah inaudible. */
        if (fileg_depth_cents == 0.0) {
            for (int i = 0; i < filter_freq_oncc_count; i++) {
                pos += snprintf(sfz + pos, out_cap - pos,
                                "cutoff_oncc%d=%.2f\n",
                                filter_freq_oncc[i].cc_number,
                                filter_freq_oncc[i].cents_delta);
                if (filter_freq_oncc[i].curve_id >= 0) {
                    pos += snprintf(sfz + pos, out_cap - pos,
                                    "cutoff_curvecc%d=%d\n",
                                    filter_freq_oncc[i].cc_number,
                                    filter_freq_oncc[i].curve_id);
                }
            }
        }
        for (int i = 0; i < filter_res_oncc_count; i++) {
            pos += snprintf(sfz + pos, out_cap - pos,
                            "resonance_oncc%d=%.4f\n",
                            filter_res_oncc[i].cc_number,
                            filter_res_oncc[i].db_delta);
            if (filter_res_oncc[i].curve_id >= 0) {
                pos += snprintf(sfz + pos, out_cap - pos,
                                "resonance_curvecc%d=%d\n",
                                filter_res_oncc[i].cc_number,
                                filter_res_oncc[i].curve_id);
            }
        }
    }
    /* Phase 5: live pan_oncc from PAN knob bindings. Emitted on
     * <global> so every region's pan is driven by the knob CC. */
    for (int i = 0; i < pan_oncc_count; i++) {
        pos += snprintf(sfz + pos, out_cap - pos,
                        "pan_oncc%d=%.2f\n",
                        pan_oncc[i].cc_number,
                        pan_oncc[i].pct_delta);
        if (pan_oncc[i].curve_id >= 0) {
            pos += snprintf(sfz + pos, out_cap - pos,
                            "pan_curvecc%d=%d\n",
                            pan_oncc[i].cc_number,
                            pan_oncc[i].curve_id);
        }
    }

    /* === walk <group>s === */
    char *scan = src;
    int group_idx = 0;
    while ((scan = strstr(scan, "<group")) != NULL) {
        if (scan[6] == 's' || scan[6] == 'S') { scan += 7; continue; }
        char *tag_end = strchr(scan, '>');
        if (!tag_end) break;
        char tag_buf[2048];
        int tlen = tag_end - scan;
        if (tlen >= (int)sizeof(tag_buf)) tlen = sizeof(tag_buf) - 1;
        memcpy(tag_buf, scan, tlen);
        tag_buf[tlen] = '\0';

        pos += snprintf(sfz + pos, out_cap - pos, "<group>\n");

        /* Phase 7b: per-group modulators from level="group" <lfo> /
         * <envelope> bindings. SFZ inheritance carries these to every
         * region in the group, so groups the author left modulator-
         * free play unmodulated even when other groups have autowah
         * or vibrato active (the 01 WörliTzer vs WobbliTzer story). */
        if (group_idx >= 0 && group_idx < DS_MAX_GROUPS) {
            const per_group_mod_t *m = &pgm[group_idx];
            if (m->amp_lfo_freq > 0.0 || m->amp_lfo_depth_db > 0.0) {
                pos += snprintf(sfz + pos, out_cap - pos,
                                "amplfo_freq=%.4f\namplfo_depth=%.2f\n",
                                m->amp_lfo_freq, m->amp_lfo_depth_db);
            }
            if (m->fil_lfo_freq > 0.0 || m->fil_lfo_depth_cents > 0.0) {
                pos += snprintf(sfz + pos, out_cap - pos,
                                "fillfo_freq=%.4f\nfillfo_depth=%.2f\n",
                                m->fil_lfo_freq, m->fil_lfo_depth_cents);
            }
            if (m->pan_lfo_freq > 0.0 && m->pan_lfo_depth_pct > 0.0) {
                pos += snprintf(sfz + pos, out_cap - pos,
                                "panlfo_freq=%.4f\npanlfo_depth=%.2f\n",
                                m->pan_lfo_freq, m->pan_lfo_depth_pct);
            }
            if (m->pitch_lfo_freq > 0.0 && m->pitch_lfo_depth_cents > 0.0) {
                pos += snprintf(sfz + pos, out_cap - pos,
                                "pitchlfo_freq=%.4f\npitchlfo_depth=%.2f\n",
                                m->pitch_lfo_freq, m->pitch_lfo_depth_cents);
            }
            if (m->fileg_depth_cents > 0.0) {
                pos += snprintf(sfz + pos, out_cap - pos,
                                "fileg_attack=%.4f\nfileg_decay=%.4f\n"
                                "fileg_sustain=%.4f\nfileg_release=%.4f\n"
                                "fileg_depth=%.2f\n",
                                m->fileg_attack_s, m->fileg_decay_s,
                                m->fileg_sustain, m->fileg_release_s,
                                m->fileg_depth_cents);
                /* Per-group cutoff floor so the autowah swings the
                 * full table range on THIS group only. */
                if (m->fileg_cutoff_floor > 0.0) {
                    pos += snprintf(sfz + pos, out_cap - pos,
                                    "cutoff=%.2f\n",
                                    m->fileg_cutoff_floor);
                }
            }
        }

        /* trigger="release" → emit `trigger=release` so xsynth fires these
         * regions on NoteOff (release sample / key-up thump) and NOT on
         * NoteOn. Without this, key-up samples bleed into every note as
         * a granular/clicky background layer at the group's static
         * volume. xsynth's SFZ parser maps "attack"/"release"/"first"/
         * "legato" — anything else silently defaults to Attack. */
        xml_get_attr(tag_buf, "trigger", val, sizeof(val));
        if (strcmp(val, "release") == 0) {
            pos += snprintf(sfz + pos, out_cap - pos, "trigger=release\n");
        }

        /* Phase 4: collect this group's tag (if any) for tag-level
         * volume_oncc matching below. Single-tag form only (DS allows
         * multi via comma but no installed preset uses it). */
        char group_tag[DS_MAX_TAG_NAME] = "";
        xml_get_attr(tag_buf, "tags", group_tag, sizeof(group_tag));

        /* Group base static volume (NO oncc-related shifts yet). */
        double base_db = 0.0; int has_base = 0;
        xml_get_attr(tag_buf, "volume", val, sizeof(val));
        if (val[0]) { base_db = atof(val); has_base = 1; }

        /* modVolume contributes only when no group-level oncc binding
         * exists for this group (oncc bindings supersede the static
         * modVolume baseline). */
        double mod_extra_db = 0.0; int has_mod_extra = 0;
        {
            char tmp[64];
            xml_get_attr(tag_buf, "modVolume", tmp, sizeof(tmp));
            if (tmp[0]) { mod_extra_db = lin_to_db(atof(tmp)); has_mod_extra = 1; }
        }

        /* Collect every oncc binding that applies to this group:
         * group-level (matching position), tag-level (matching tags=),
         * instrument-level (always applies). */
        struct { int cc; double dmin; double delta; int curve_id; } group_bindings[DS_MAX_KNOBS * 3];
        int gb_count = 0;
        for (int oi = 0; oi < group_oncc_count && gb_count < (int)(sizeof(group_bindings)/sizeof(group_bindings[0])); oi++) {
            if (group_oncc[oi].group_position != group_idx) continue;
            group_bindings[gb_count].cc    = group_oncc[oi].cc_number;
            group_bindings[gb_count].dmin  = group_oncc[oi].db_min;
            group_bindings[gb_count].delta = group_oncc[oi].db_delta;
            group_bindings[gb_count].curve_id = group_oncc[oi].curve_id;
            gb_count++;
        }
        for (int ti = 0; ti < tag_oncc_count && gb_count < (int)(sizeof(group_bindings)/sizeof(group_bindings[0])); ti++) {
            if (!group_tag[0] || strcmp(group_tag, tag_oncc[ti].tag) != 0) continue;
            group_bindings[gb_count].cc    = tag_oncc[ti].cc_number;
            group_bindings[gb_count].dmin  = tag_oncc[ti].db_min;
            group_bindings[gb_count].delta = tag_oncc[ti].db_delta;
            group_bindings[gb_count].curve_id = tag_oncc[ti].curve_id;
            gb_count++;
        }
        for (int gi = 0; gi < global_oncc_count && gb_count < (int)(sizeof(group_bindings)/sizeof(group_bindings[0])); gi++) {
            group_bindings[gb_count].cc    = global_oncc[gi].cc_number;
            group_bindings[gb_count].dmin  = global_oncc[gi].db_min;
            group_bindings[gb_count].delta = global_oncc[gi].db_delta;
            group_bindings[gb_count].curve_id = global_oncc[gi].curve_id;
            gb_count++;
        }

        /* Compute the un-clamped static volume = base + Σ db_min. If
         * it would underflow xsynth's -96 dB spawner clamp (and SFZ's
         * -144 dB parse clamp), scale all bindings' db_min AND db_delta
         * proportionally so:
         *   - clamped static = -96 (the spawner floor)
         *   - knob-at-max sum (Σ scaled_delta) still reproduces base_db
         *
         * Without scaling, the un-clamped negative would get truncated
         * but the matching positive volume_oncc deltas wouldn't —
         * resulting in the voice playing tens of dB louder than
         * authored at preset load. Affects only multi-stack groups
         * (e.g. Legacy Knight: instrument + tag bindings on one group).
         * Single-binding groups (WörliTzer, DS Synths) need no scaling. */
        double sum_db_min = 0.0;
        for (int i = 0; i < gb_count; i++) sum_db_min += group_bindings[i].dmin;
        double scale = 1.0;
        const double SPAWNER_FLOOR_DB = -96.0;
        if (sum_db_min < 0.0 && base_db + sum_db_min < SPAWNER_FLOOR_DB) {
            double available = SPAWNER_FLOOR_DB - base_db;  /* negative */
            if (available < 0.0) scale = available / sum_db_min;
            if (scale < 0.0) scale = 0.0;
            if (scale > 1.0) scale = 1.0;
        }

        /* Emit static volume = base + scaled Σ db_min + modVolume
         * (when no oncc bindings absorb the per-group baseline). */
        int has_oncc_static = (gb_count > 0);
        double extra_db = has_oncc_static ? (sum_db_min * scale) : mod_extra_db;
        int has_extra = has_oncc_static || has_mod_extra;
        if (has_base || has_extra) {
            double v = base_db + extra_db;
            if (v < -144) v = -144;
            if (v >  6.0) v =  6.0;
            /* xsynth's SFZ parser uses `parse_i16_in_range(val, -144..=6)`
             * for `volume` — it REJECTS decimals and silently drops the
             * opcode on parse failure. Round to integer dB. */
            int vi = (int)(v >= 0 ? v + 0.5 : v - 0.5);
            pos += snprintf(sfz + pos, out_cap - pos, "volume=%d\n", vi);
        }

        /* Emit volume_oncc<CC>= for each applicable binding, scaled to
         * match the static. At cc=knob_current, voice amp =
         * `db_to_amp(base + Σ scaled_dmin + Σ scaled_delta · cc/127)`,
         * which equals the authored amp at knob's load position when
         * scale=1, and the floor-clamped equivalent when scale<1. */
        for (int i = 0; i < gb_count; i++) {
            double d = group_bindings[i].delta * scale;
            if (d < -144.0) d = -144.0;
            if (d >  144.0) d =  144.0;
            pos += snprintf(sfz + pos, out_cap - pos,
                            "volume_oncc%d=%.2f\n",
                            group_bindings[i].cc, d);
            if (group_bindings[i].curve_id >= 0) {
                pos += snprintf(sfz + pos, out_cap - pos,
                                "volume_curvecc%d=%d\n",
                                group_bindings[i].cc,
                                group_bindings[i].curve_id);
            }
        }

        /* Phase 4: groupTuning attribute. DS semitones → xsynth tune=
         * (i16 cents, clamped to -2400..2400). Applied at the group
         * level via inheritance — every region inside picks it up
         * unless it sets its own tune=. */
        char group_tuning[32] = "";
        xml_get_attr(tag_buf, "groupTuning", group_tuning, sizeof(group_tuning));
        double group_tune_cents = group_tuning[0] ? atof(group_tuning) * 100.0 : 0.0;

        xml_get_attr(tag_buf, "ampVelTrack", val, sizeof(val));
        {
            /* DS uses a flatter velocity-to-amp curve than SFZ's default
             * (40·log10 squared). Empirically calibrated against DS at
             * vel = 20/64/100/127: scaling DS's 0..1 to xsynth's
             * 0..80 brings the worst per-velocity gain Δ from ~14 dB
             * down to ~1 dB. Without explicit ampVelTrack, DS still
             * has its own default vel response — emit 80 so every
             * group inherits the same curve. */
            float ds_vt = val[0] ? atof(val) : 1.0;
            float t = ds_vt * 80.0f;
            if (t < -100) t = -100; if (t > 100) t = 100;
            pos += snprintf(sfz + pos, out_cap - pos, "amp_veltrack=%.1f\n", t);
        }
        xml_get_attr(tag_buf, "attack", val, sizeof(val));
        if (val[0]) pos += snprintf(sfz + pos, out_cap - pos, "ampeg_attack=%s\n", val);
        xml_get_attr(tag_buf, "decay", val, sizeof(val));
        if (val[0]) pos += snprintf(sfz + pos, out_cap - pos, "ampeg_decay=%s\n", val);
        xml_get_attr(tag_buf, "sustain", val, sizeof(val));
        if (val[0]) {
            float s = atof(val) * 100.0f;
            pos += snprintf(sfz + pos, out_cap - pos, "ampeg_sustain=%.1f\n", s);
        }
        xml_get_attr(tag_buf, "release", val, sizeof(val));
        if (val[0]) pos += snprintf(sfz + pos, out_cap - pos, "ampeg_release=%s\n", val);

        /* Round-robin: xsynth's parser honors seq_position / seq_length
         * (RrState in soundfont/mod.rs). Inherit `seqMode` from the
         * <groups> parent so presets that hoist it (18_round_robin and
         * most real-world DS RR libs) emit per-group seq opcodes
         * correctly instead of all firing simultaneously. */
        xml_get_attr(tag_buf, "seqMode", val, sizeof(val));
        int group_is_rr = (strcmp(val, "round_robin") == 0)
                       || (strcmp(wrap_seqmode, "round_robin") == 0);
        if (group_is_rr && rr_total > 0) {
            char rr_pos[16];
            xml_get_attr(tag_buf, "seqPosition", rr_pos, sizeof(rr_pos));
            if (rr_pos[0]) {
                pos += snprintf(sfz + pos, out_cap - pos,
                                "seq_length=%d\nseq_position=%s\n",
                                rr_total, rr_pos);
            }
        }

        /* === walk <sample>s in this group === */
        char *sample_scan = tag_end;
        char *group_end = strstr(tag_end, "</group>");
        if (!group_end) group_end = src + size;

        while (sample_scan < group_end &&
               (sample_scan = strstr(sample_scan, "<sample")) != NULL &&
               sample_scan < group_end) {
            char *stag_end = strchr(sample_scan, '>');
            if (!stag_end || stag_end > group_end) break;

            /* Skip XML-commented samples (cheap scan for `<!--`). */
            char *cmt = NULL;
            for (char *c = sample_scan - 1; c >= src && c >= sample_scan - 20; c--) {
                if (*c == '-' && c > src && *(c-1) == '-' && c > src+1 && *(c-2) == '!') {
                    cmt = c - 2; break;
                }
                if (*c != ' ' && *c != '\t' && *c != '\n' && *c != '\r') break;
            }
            if (cmt) { sample_scan = stag_end + 1; continue; }

            char stag[1024];
            int slen = stag_end - sample_scan;
            if (slen >= (int)sizeof(stag)) slen = sizeof(stag) - 1;
            memcpy(stag, sample_scan, slen);
            stag[slen] = '\0';

            pos += snprintf(sfz + pos, out_cap - pos, "<region>\n");

            /* Stash the rootNote for later — we may bump it by an
             * integer-semitone tuning offset since xsynth has no
             * `transpose=` opcode and `tune=` only covers ±100 cents.
             * Shifting pitch_keycenter up by N semitones makes the
             * sample render N semitones lower when played at the
             * same MIDI note. */
            int root_note = 60;
            char root_str[16] = "";
            xml_get_attr(stag, "rootNote", root_str, sizeof(root_str));
            if (root_str[0]) root_note = atoi(root_str);
            xml_get_attr(stag, "path", val, sizeof(val));
            if (val[0]) {
                /* Windows backslash → forward slash. */
                for (char *p = val; *p; p++) if (*p == '\\') *p = '/';
                /* Case-insensitive fixup so xsynth (strict on case) finds
                 * the sample even when the dspreset uses `samples/foo.wav`
                 * but the on-disk dir is `Samples/foo.wav`. */
                char fixed[512];
                if (base_dir[0] &&
                    resolve_sample_path_ci(base_dir, val, fixed, sizeof(fixed))) {
                    pos += snprintf(sfz + pos, out_cap - pos, "sample=%s\n", fixed);
                } else {
                    pos += snprintf(sfz + pos, out_cap - pos, "sample=%s\n", val);
                }
            }
            xml_get_attr(stag, "loNote", val, sizeof(val));
            if (val[0]) pos += snprintf(sfz + pos, out_cap - pos, "lokey=%s\n", val);
            xml_get_attr(stag, "hiNote", val, sizeof(val));
            if (val[0]) pos += snprintf(sfz + pos, out_cap - pos, "hikey=%s\n", val);
            xml_get_attr(stag, "loVel", val, sizeof(val));
            if (val[0]) pos += snprintf(sfz + pos, out_cap - pos, "lovel=%s\n", val);
            xml_get_attr(stag, "hiVel", val, sizeof(val));
            if (val[0]) pos += snprintf(sfz + pos, out_cap - pos, "hivel=%s\n", val);
            xml_get_attr(stag, "start", val, sizeof(val));
            if (val[0]) pos += snprintf(sfz + pos, out_cap - pos, "offset=%s\n", val);
            /* `end` and `transpose` are not in xsynth's opcode list; skip.
             * Phase 4: combine <sample tuning> (semitones) with the
             * containing group's <group groupTuning> (semitones,
             * pre-converted to cents above) for the final `tune=`. */
            xml_get_attr(stag, "tuning", val, sizeof(val));
            double total_cents = group_tune_cents;
            if (val[0]) total_cents += atof(val) * 100.0;
            int semis = 0;
            int cents = 0;
            if (total_cents != 0.0) {
                /* Split into integer-semitones (folded into
                 * pitch_keycenter) and the fractional remainder (tune).
                 * xsynth has no `transpose=` opcode and `tune=` is
                 * bounded to ±100 cents. DS-style tunings like -50
                 * semitones need to come from shifting pitch_keycenter:
                 * raising root_note by N st makes the sample render N
                 * st lower at the same MIDI note. */
                double rem = total_cents;
                if (rem >= 100.0 || rem <= -100.0) {
                    semis = (int)(rem / 100.0);
                    rem -= semis * 100.0;
                }
                cents = (int)(rem >= 0 ? rem + 0.5 : rem - 0.5);
                if (cents < -100) cents = -100;
                if (cents >  100) cents =  100;
            }
            /* Negative DS tuning lowers pitch → raise root_note.
             * Clamp to MIDI 0..127 (so a wildly large tuning falls
             * off the playable range, which is the DS behavior too). */
            int adjusted_root = root_note - semis;
            if (adjusted_root < 0)   adjusted_root = 0;
            if (adjusted_root > 127) adjusted_root = 127;
            pos += snprintf(sfz + pos, out_cap - pos,
                            "pitch_keycenter=%d\n", adjusted_root);
            if (cents != 0)
                pos += snprintf(sfz + pos, out_cap - pos, "tune=%d\n", cents);
            xml_get_attr(stag, "pan", val, sizeof(val));
            if (val[0]) {
                int p = atoi(val);
                if (p < -100) p = -100; if (p > 100) p = 100;
                pos += snprintf(sfz + pos, out_cap - pos, "pan=%d\n", p);
            }
            /* Per-sample <sample volume="..."> (DS supports it for
             * velocity-layer balancing). Accepts "-6dB" or a raw dB
             * value. SFZ region-level volume in dB. */
            xml_get_attr(stag, "volume", val, sizeof(val));
            if (val[0]) {
                double v_db = atof(val);
                /* Strip trailing "dB" / "DB" by truncation — atof already
                 * stopped at the suffix so we just pass the number. */
                int vi = (int)(v_db >= 0 ? v_db + 0.5 : v_db - 0.5);
                if (vi < -144) vi = -144;
                if (vi >    6) vi =    6;
                if (vi != 0)
                    pos += snprintf(sfz + pos, out_cap - pos, "volume=%d\n", vi);
            }
            /* MOVE FORK / 2026-05-16: each <sample> opts in to looping
             * via its own `loopEnabled`. DS accepts "true"/"1"/"yes"
             * as truthy. When the sample omits the attribute, default
             * to one-shot — DON'T inherit from the <groups> parent;
             * that was the old behavior and caused unexpected looping
             * for sibling samples after one author opted in. */
            xml_get_attr(stag, "loopEnabled", val, sizeof(val));
            int loop_on = (val[0] && (strcmp(val, "true") == 0
                                   || strcmp(val, "1")    == 0
                                   || strcmp(val, "yes")  == 0));
            pos += snprintf(sfz + pos, out_cap - pos, "loop_mode=%s\n",
                            loop_on ? "loop_continuous" : "no_loop");
            xml_get_attr(stag, "loopStart", val, sizeof(val));
            if (val[0]) pos += snprintf(sfz + pos, out_cap - pos, "loop_start=%s\n", val);
            xml_get_attr(stag, "loopEnd", val, sizeof(val));
            if (val[0]) pos += snprintf(sfz + pos, out_cap - pos, "loop_end=%s\n", val);
            /* Phase 8: loopCrossfade=<frames> in DS → loop_crossfade=
             * <seconds> in SFZ. Assume 44.1 kHz source rate when
             * converting frames to seconds — most DS sample libraries
             * ship at 44.1k. The xsynth side multiplies seconds by the
             * output rate to get the actual frame count, so any drift
             * from a non-44.1k source is bounded to a few frames of
             * crossfade. */
            xml_get_attr(stag, "loopCrossfade", val, sizeof(val));
            if (val[0]) {
                double frames = atof(val);
                if (frames > 0.0) {
                    pos += snprintf(sfz + pos, out_cap - pos,
                                    "loop_crossfade=%.4f\n",
                                    frames / 44100.0);
                }
            }

            sample_scan = stag_end + 1;
        }

        scan = group_end;
        group_idx++;
    }

    free(src);

    /* Write to temp file alongside the .dspreset. */
    char *tmp_path = malloc(strlen(path) + 16);
    if (!tmp_path) { free(sfz); return NULL; }
    const char *dot = strrchr(path, '.');
    int base_len = dot ? (int)(dot - path) : (int)strlen(path);
    snprintf(tmp_path, strlen(path) + 16, "%.*s.converted.sfz", base_len, path);

    f = fopen(tmp_path, "wb");
    if (!f) { free(sfz); free(tmp_path); return NULL; }
    fwrite(sfz, 1, pos, f);
    fclose(f);
    free(sfz);
    return tmp_path;
}
