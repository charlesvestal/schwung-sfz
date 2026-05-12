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

/* --- helpers -------------------------------------------------------------- */

static double lin_to_db(double x) {
    if (x <= 1e-5) return -80.0;
    return 20.0 * log10(x);
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
} ds_effect_t;

static int parse_effects(const char *src, ds_effect_t fx[DS_MAX_FX]) {
    int count = 0;
    const char *p = strstr(src, "<effects");
    if (!p) return 0;
    const char *end = strstr(p, "</effects>");
    if (!end) end = src + strlen(src);

    p = strstr(p, "<effect ");
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

/* Walk every <control>/<labeled-knob>, resolve each <binding> to a static
 * SFZ-target value (knob's `value=` baked in). Writes into ENV_* /
 * filter-effect / group-amp_db buckets. CC-driven bindings (no fixed value)
 * are also resolved with the knob at its load-time position — that's the
 * static snapshot we want for xsynth. */
static void apply_ui_overrides(const char *src,
                               ds_effect_t fx[DS_MAX_FX], int fx_count,
                               char env_attack[32], char env_decay[32],
                               char env_sustain[32], char env_release[32],
                               double group_amp_db[DS_MAX_GROUPS]) {
    static const char *patterns[2] = { "<control", "<labeled-knob" };
    for (int pi = 0; pi < 2; pi++) {
        const char *p = src;
        size_t pat_len = strlen(patterns[pi]);
        while ((p = strstr(p, patterns[pi])) != NULL) {
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
                const char *c1 = strstr(bind_search_start, "</control>");
                const char *c2 = strstr(bind_search_start, "</labeled-knob>");
                if (c1 && c2)       bind_search_end = (c1 < c2) ? c1 : c2;
                else if (c1)        bind_search_end = c1;
                else if (c2)        bind_search_end = c2;
                else { p = tag_end + 1; continue; }
                if (strncmp(bind_search_end, "</control>", 10) == 0)       closer_skip = 10;
                else if (strncmp(bind_search_end, "</labeled-knob>", 15) == 0) closer_skip = 15;
            }

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
                               strcmp(level, "group") == 0 &&
                               position >= 0 && position < DS_MAX_GROUPS) {
                        /* 0..1 linear amp factor → dB, composes additively
                         * with the group's <group volume=...> attr. */
                        group_amp_db[position] = lin_to_db(atof(effective));
                    } else if (position >= 0 && position < fx_count) {
                        ds_effect_t *f = &fx[position];
                        if (strcmp(param, "FX_FILTER_FREQUENCY") == 0) {
                            strncpy(f->freq, effective, 31); f->freq[31] = '\0';
                        } else if (strcmp(param, "FX_FILTER_RESONANCE") == 0) {
                            strncpy(f->resonance, effective, 31); f->resonance[31] = '\0';
                        }
                        /* Reverb knobs: skip — xsynth has no reverb anyway. */
                    }
                }
                bp = bind_end + 1;
            }
            p = self_closed ? (tag_end + 1) : (bind_search_end + closer_skip);
        }
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

/* DS UI-knob binding info. Used both to bake static values and to emit
 * `_oncc<N>` opcodes so xsynth's per-voice modulation hears CC moves. */
typedef enum {
    DS_TARGET_NONE = 0,
    DS_TARGET_CUTOFF,         /* cutoff_oncc<N>, delta in Hz */
    DS_TARGET_GROUP_VOLUME,   /* per-group volume_oncc<N>, delta in dB */
    DS_TARGET_PAN,            /* pan_oncc<N>, delta -100..100 */
} ds_target_t;

typedef struct {
    int cc_number;           /* synthetic CC 102..117 */
    ds_target_t target;
    int target_group;        /* group index for GROUP_VOLUME, else -1 */
    double v_at_0;           /* value when knob is at min (CC=0) */
    double v_at_127;         /* value when knob is at max (CC=127) */
    int cc_at_load;          /* initial CC value derived from knob.value= */
} ds_cc_emit_t;

#define DS_MAX_CC_EMIT 64

static double clamp01(double v) {
    if (v < 0) return 0;
    if (v > 1) return 1;
    return v;
}

/* Walk every <control>/<labeled-knob>. For each, examine its <binding>s.
 * For each supported binding, allocate a synthetic CC (102..117), emit
 * an entry into `emits` describing what _oncc opcode to write, and append
 * a ds_xsynth_knob_t entry the host can surface as a parameter. */
static void enumerate_ui_knobs(const char *src, int lp_idx,
                               ds_cc_emit_t emits[DS_MAX_CC_EMIT],
                               int *emit_count,
                               ds_xsynth_knob_t knobs_out[DS_MAX_KNOBS],
                               int *knob_count_out,
                               int group_cc_driven[DS_MAX_GROUPS]) {
    *emit_count = 0;
    *knob_count_out = 0;
    int next_cc = 102;
    static const char *patterns[2] = { "<control", "<labeled-knob" };
    /* Single doc-order walk so knob indices line up with the UI. */
    const char *p = src;
    int kc = 0;
    while (kc < DS_MAX_KNOBS) {
        const char *c1 = strstr(p, "<control");
        const char *c2 = strstr(p, "<labeled-knob");
        const char *next = NULL; size_t pat_len = 0;
        if (c1 && c2) {
            if (c1 < c2) { next = c1; pat_len = 8; }
            else         { next = c2; pat_len = 13; }
        } else if (c1) { next = c1; pat_len = 8; }
        else if (c2)   { next = c2; pat_len = 13; }
        else break;
        (void)patterns;
        p = next;
        char nxt = p[pat_len];
        if (nxt != ' ' && nxt != '\t' && nxt != '\n' &&
            nxt != '\r' && nxt != '>' && nxt != '/') { p += pat_len; continue; }
        const char *tag_end = strchr(p, '>');
        if (!tag_end) break;
        char ctrl_tag[1024];
        int tlen = (int)(tag_end - p);
        if (tlen > 1023) tlen = 1023;
        memcpy(ctrl_tag, p, tlen);
        ctrl_tag[tlen] = '\0';

        const char *bind_start = tag_end + 1;
        const char *bind_end_search;
        int self_closed = (tag_end > p && *(tag_end - 1) == '/');
        int closer_skip = 0;
        if (self_closed) { bind_end_search = tag_end; }
        else {
            const char *e1 = strstr(bind_start, "</control>");
            const char *e2 = strstr(bind_start, "</labeled-knob>");
            if (e1 && e2)       bind_end_search = (e1 < e2) ? e1 : e2;
            else if (e1)        bind_end_search = e1;
            else if (e2)        bind_end_search = e2;
            else { p = tag_end + 1; continue; }
            closer_skip = (strncmp(bind_end_search, "</control>", 10) == 0) ? 10 : 15;
        }

        char cmin[64], cmax[64], cval[64], clabel[64], pname[64];
        xml_get_attr(ctrl_tag, "minValue",      cmin,   sizeof(cmin));
        xml_get_attr(ctrl_tag, "maxValue",      cmax,   sizeof(cmax));
        xml_get_attr(ctrl_tag, "value",         cval,   sizeof(cval));
        xml_get_attr(ctrl_tag, "label",         clabel, sizeof(clabel));
        xml_get_attr(ctrl_tag, "parameterName", pname,  sizeof(pname));
        double minv = cmin[0] ? atof(cmin) : 0.0;
        double maxv = cmax[0] ? atof(cmax) : 1.0;
        double cur  = cval[0] ? atof(cval) : minv;

        int cc_for_this = -1;
        int produced_any = 0;
        const char *bp = bind_start;
        while (bp && bp < bind_end_search) {
            const char *bind = strstr(bp, "<binding");
            if (!bind || bind >= bind_end_search) break;
            const char *bend = strchr(bind, '>');
            if (!bend || bend > bind_end_search) break;
            char btag[512];
            int blen = (int)(bend - bind);
            if (blen > 511) blen = 511;
            memcpy(btag, bind, blen);
            btag[blen] = '\0';

            char param[64] = "", position_str[16] = "", level[32] = "";
            xml_get_attr(btag, "parameter", param, sizeof(param));
            xml_get_attr(btag, "position",  position_str, sizeof(position_str));
            xml_get_attr(btag, "level",     level, sizeof(level));
            int position = position_str[0] ? atoi(position_str) : 0;

            ds_target_t target = DS_TARGET_NONE;
            int target_group = -1;
            if (strcmp(param, "FX_FILTER_FREQUENCY") == 0 &&
                lp_idx >= 0 && position == lp_idx) {
                target = DS_TARGET_CUTOFF;
            } else if ((strcmp(param, "AMP_VOLUME") == 0 ||
                        strcmp(param, "TAG_VOLUME") == 0) &&
                       strcmp(level, "group") == 0 &&
                       position >= 0 && position < DS_MAX_GROUPS) {
                target = DS_TARGET_GROUP_VOLUME;
                target_group = position;
            } else if (strcmp(param, "PAN") == 0) {
                target = DS_TARGET_PAN;
            }

            if (target != DS_TARGET_NONE && *emit_count < DS_MAX_CC_EMIT) {
                if (cc_for_this < 0) {
                    if (next_cc > 117) break;
                    cc_for_this = next_cc++;
                }
                double v0 = apply_binding_xform(btag, minv, maxv, minv);
                double v127 = apply_binding_xform(btag, minv, maxv, maxv);

                /* For GROUP_VOLUME the input v is a 0..1 linear amp;
                 * xsynth volume_oncc is in dB. Map endpoints to dB. */
                if (target == DS_TARGET_GROUP_VOLUME) {
                    v0   = lin_to_db(v0);
                    v127 = lin_to_db(v127);
                }
                /* For PAN, DS gives -1..1; xsynth pan range is -100..100. */
                if (target == DS_TARGET_PAN) {
                    v0   *= 100.0;
                    v127 *= 100.0;
                }

                ds_cc_emit_t *e = &emits[(*emit_count)++];
                e->cc_number = cc_for_this;
                e->target = target;
                e->target_group = target_group;
                e->v_at_0 = v0;
                e->v_at_127 = v127;
                double cur_t = (maxv != minv) ? (cur - minv) / (maxv - minv) : 0.0;
                e->cc_at_load = (int)(clamp01(cur_t) * 127.0 + 0.5);

                /* GROUP_VOLUME claims the group's amp — disable static
                 * volume so we don't double-apply. */
                if (target == DS_TARGET_GROUP_VOLUME &&
                    target_group >= 0 && target_group < DS_MAX_GROUPS) {
                    group_cc_driven[target_group] = 1;
                }
                produced_any = 1;
            }
            bp = bend + 1;
        }

        if (produced_any) {
            ds_xsynth_knob_t *k = &knobs_out[kc];
            snprintf(k->key, sizeof(k->key), "knob_%d", kc);
            if (clabel[0])      snprintf(k->label, sizeof(k->label), "%s", clabel);
            else if (pname[0])  snprintf(k->label, sizeof(k->label), "%s", pname);
            else                snprintf(k->label, sizeof(k->label), "Knob %d", kc + 1);
            double t01 = (maxv != minv) ? clamp01((cur - minv) / (maxv - minv)) : 0.0;
            k->current_t01 = t01;
            k->dspreset_t01 = t01;
            k->cc_number = cc_for_this;
            kc++;
        }

        p = self_closed ? (tag_end + 1) : (bind_end_search + closer_skip);
    }
    *knob_count_out = kc;
}

static int count_rr_groups(const char *src) {
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
        if (strcmp(mode, "round_robin") == 0) count++;
        p = tag_end + 1;
    }
    return count;
}

/* --- converter ----------------------------------------------------------- */

char *convert_dspreset_to_xsynth_sfz(
    const char *path,
    ds_xsynth_knob_t knobs_out[DS_MAX_KNOBS],
    int *knob_count_out
) {
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

    long out_cap = size * 2;
    char *sfz = malloc(out_cap);
    if (!sfz) { free(src); return NULL; }
    int pos = 0;
    char val[512];

    ds_effect_t fx[DS_MAX_FX] = {0};
    int fx_count = parse_effects(src, fx);

    char env_attack[32] = "", env_decay[32] = "";
    char env_sustain[32] = "", env_release[32] = "";
    double group_amp_db[DS_MAX_GROUPS];
    for (int i = 0; i < DS_MAX_GROUPS; i++) group_amp_db[i] = NAN;
    apply_ui_overrides(src, fx, fx_count, env_attack, env_decay,
                       env_sustain, env_release, group_amp_db);

    int rr_total = count_rr_groups(src);

    int lp_idx = -1, gain_idx = -1;
    for (int i = 0; i < fx_count; i++) {
        if (lp_idx < 0 && (strcmp(fx[i].type, "lowpass") == 0 ||
                            strcmp(fx[i].type, "lowpass_4pl") == 0)) lp_idx = i;
        if (gain_idx < 0 && strcmp(fx[i].type, "gain") == 0) gain_idx = i;
    }

    /* MOVE FORK Phase 3: enumerate UI knobs and emit `<param>_oncc<N>`
     * bindings so xsynth's per-voice modulation responds to live CC moves.
     * Knobs targeting AMP_VOLUME at group level mark that group as
     * CC-driven, suppressing the static volume= emit below to avoid
     * double-attenuation. */
    ds_cc_emit_t cc_emits[DS_MAX_CC_EMIT];
    int cc_emit_count = 0;
    int group_cc_driven[DS_MAX_GROUPS] = {0};
    int knob_count_local = 0;
    ds_xsynth_knob_t knobs_local[DS_MAX_KNOBS];
    enumerate_ui_knobs(src, lp_idx, cc_emits, &cc_emit_count,
                       knobs_local, &knob_count_local, group_cc_driven);
    if (knobs_out && knob_count_local > 0)
        memcpy(knobs_out, knobs_local, knob_count_local * sizeof(ds_xsynth_knob_t));
    if (knob_count_out) *knob_count_out = knob_count_local;

    /* === <global>: combined static defaults === */
    char *groups_tag = strstr(src, "<groups");
    char wrap_attack[64] = "", wrap_decay[64] = "";
    char wrap_sustain[64] = "", wrap_release[64] = "";
    char wrap_volume[64] = "", wrap_loop[64] = "";
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
        }
    }

    pos += snprintf(sfz + pos, out_cap - pos, "<global>\n");

    /* Volume: wrapper attr + gain effect (both in dB). xsynth caps volume
     * at +6 dB and clamps to -144 dB, so we add and pass through. */
    double global_vol_db = 0.0;
    if (wrap_volume[0]) global_vol_db += atof(wrap_volume);
    if (gain_idx >= 0 && fx[gain_idx].level[0]) global_vol_db += atof(fx[gain_idx].level);
    if (global_vol_db != 0.0)
        pos += snprintf(sfz + pos, out_cap - pos, "volume=%.2f\n", global_vol_db);

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
    if (wrap_loop[0])
        pos += snprintf(sfz + pos, out_cap - pos, "loop_mode=%s\n",
                        strcmp(wrap_loop, "true") == 0 ? "loop_continuous" : "no_loop");

    /* Lowpass: bake the knob-resolved frequency + resonance. xsynth supports
     * fil_type=lpf_1p/lpf_2p/lpf_4p/lpf_6p; we use lpf_2p as a generic
     * (DS's `lowpass` is documented as a 2-pole) and lpf_4p for `lowpass_4pl`. */
    if (lp_idx >= 0) {
        const char *fil_type = strcmp(fx[lp_idx].type, "lowpass_4pl") == 0
                                 ? "lpf_4p" : "lpf_2p";
        const char *cutoff = fx[lp_idx].freq[0]      ? fx[lp_idx].freq      : "22000";
        const char *q      = fx[lp_idx].resonance[0] ? fx[lp_idx].resonance : "0.7";
        /* Base cutoff is the bound knob's `value=` (applied statically by
         * apply_ui_overrides above). The _oncc<N>=delta below stacks live
         * modulation on top: effective = base + (cc/127) * delta. To make
         * a Tone knob actually sweep the full range we set base = v_at_0
         * (the value at cc=0) and delta = v_at_127 - v_at_0. */
        double base_cut = atof(cutoff);
        double delta_cut = 0.0;
        int cutoff_cc = -1;
        for (int i = 0; i < cc_emit_count; i++) {
            if (cc_emits[i].target == DS_TARGET_CUTOFF) {
                base_cut = cc_emits[i].v_at_0;
                delta_cut = cc_emits[i].v_at_127 - cc_emits[i].v_at_0;
                cutoff_cc = cc_emits[i].cc_number;
                break;
            }
        }
        pos += snprintf(sfz + pos, out_cap - pos,
                        "fil_type=%s\ncutoff=%g\nresonance=%s\n",
                        fil_type, base_cut, q);
        if (cutoff_cc >= 0 && (delta_cut > 1e-6 || delta_cut < -1e-6)) {
            pos += snprintf(sfz + pos, out_cap - pos,
                            "cutoff_oncc%d=%g\n", cutoff_cc, delta_cut);
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

        /* Group volume: <group volume="dB"> + knob-AMP_VOLUME dB + modVolume
         * (linear → dB). Without honoring these, multi-group patches like
         * WörliTzer play ~6× too loud. When a CC knob is bound to this
         * group's amp, skip the static contribution from group_amp_db /
         * modVolume — the volume_oncc<N> below stacks on top of base_db,
         * and double-applying would attenuate twice. */
        int gcc = (group_idx < DS_MAX_GROUPS) ? group_cc_driven[group_idx] : 0;
        double base_db = 0.0; int has_base = 0;
        xml_get_attr(tag_buf, "volume", val, sizeof(val));
        if (val[0]) { base_db = atof(val); has_base = 1; }

        double extra_db = 0.0; int has_extra = 0;
        if (!gcc && group_idx < DS_MAX_GROUPS && !isnan(group_amp_db[group_idx])) {
            extra_db = group_amp_db[group_idx]; has_extra = 1;
        } else if (!gcc) {
            xml_get_attr(tag_buf, "modVolume", val, sizeof(val));
            if (val[0]) { extra_db = lin_to_db(atof(val)); has_extra = 1; }
        }
        /* When CC-driven we still want a base; emit just the group's
         * authored volume=, falling back to the v_at_0 of the binding so
         * the load-time CC actually sweeps from the right starting point. */
        if (gcc) {
            for (int i = 0; i < cc_emit_count; i++) {
                if (cc_emits[i].target == DS_TARGET_GROUP_VOLUME &&
                    cc_emits[i].target_group == group_idx) {
                    base_db = cc_emits[i].v_at_0;
                    has_base = 1;
                    break;
                }
            }
        }
        if (has_base || has_extra) {
            double v = base_db + extra_db;
            if (v < -144) v = -144;
            if (v >  6.0) v =  6.0;
            pos += snprintf(sfz + pos, out_cap - pos, "volume=%.2f\n", v);
        }

        /* Emit any volume_oncc<N> bindings for this group. */
        for (int i = 0; i < cc_emit_count; i++) {
            if (cc_emits[i].target == DS_TARGET_GROUP_VOLUME &&
                cc_emits[i].target_group == group_idx) {
                double delta = cc_emits[i].v_at_127 - cc_emits[i].v_at_0;
                if (delta > 1e-6 || delta < -1e-6) {
                    pos += snprintf(sfz + pos, out_cap - pos,
                                    "volume_oncc%d=%g\n",
                                    cc_emits[i].cc_number, delta);
                }
            }
        }

        xml_get_attr(tag_buf, "ampVelTrack", val, sizeof(val));
        if (val[0]) {
            /* DS 0..1 → xsynth amp_veltrack -100..100. */
            float t = atof(val) * 100.0f;
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

        /* Round-robin: xsynth's parser silently ignores seq_position /
         * seq_length, so the regions in different RR groups will all sound
         * simultaneously on each note (same as no-RR). Acceptable
         * degradation — most layered DS patches don't depend on RR; the
         * ones that do (Frozen Glock, etc.) need explicit Phase 3 work. */
        xml_get_attr(tag_buf, "seqMode", val, sizeof(val));
        if (strcmp(val, "round_robin") == 0 && rr_total > 0) {
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

            xml_get_attr(stag, "rootNote", val, sizeof(val));
            if (val[0]) pos += snprintf(sfz + pos, out_cap - pos, "pitch_keycenter=%s\n", val);
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
            /* `end` and `transpose` are not in xsynth's opcode list; skip. */
            xml_get_attr(stag, "tuning", val, sizeof(val));
            if (val[0]) {
                /* DS tuning is in semitones (typically -12..+12). xsynth's
                 * `tune` is i16 cents, range -2400..2400. Convert. */
                int cents = (int)(atof(val) * 100.0);
                if (cents < -2400) cents = -2400;
                if (cents >  2400) cents =  2400;
                if (cents != 0)
                    pos += snprintf(sfz + pos, out_cap - pos, "tune=%d\n", cents);
            }
            xml_get_attr(stag, "pan", val, sizeof(val));
            if (val[0]) {
                int p = atoi(val);
                if (p < -100) p = -100; if (p > 100) p = 100;
                pos += snprintf(sfz + pos, out_cap - pos, "pan=%d\n", p);
            }
            xml_get_attr(stag, "loopEnabled", val, sizeof(val));
            if (val[0])
                pos += snprintf(sfz + pos, out_cap - pos, "loop_mode=%s\n",
                                strcmp(val, "true") == 0 ? "loop_continuous" : "no_loop");
            xml_get_attr(stag, "loopStart", val, sizeof(val));
            if (val[0]) pos += snprintf(sfz + pos, out_cap - pos, "loop_start=%s\n", val);
            xml_get_attr(stag, "loopEnd", val, sizeof(val));
            if (val[0]) pos += snprintf(sfz + pos, out_cap - pos, "loop_end=%s\n", val);

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
