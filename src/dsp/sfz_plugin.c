/*
 * SFZ Player DSP Plugin
 *
 * Uses sfizz to render SFZ and DecentSampler (.dspreset) instruments.
 * The instruments/ directory is scanned recursively (up to 5 levels deep).
 * Each .sfz/.dspreset file becomes its own instrument:
 *   - If a folder contains exactly one preset file, the folder name is used
 *     as the display name (e.g. instruments/Cosmos/COSMOS.dspreset → "Cosmos").
 *   - If a folder contains multiple preset files, each is its own instrument
 *     named after the filename (libraries like DS_The Synths flatten this way).
 *   - "Library wrapper" folders that only contain other folders are descended
 *     into (e.g. K4Coll-1.01/K4-Acoustic/K4-Acoustic.dspreset → "K4-Acoustic").
 *
 * V2 API only - instance-based for multi-instance support.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

/* Include plugin API - inline definitions to avoid path issues */
#include <stdint.h>

#define MOVE_PLUGIN_API_VERSION_2 2
#define MOVE_SAMPLE_RATE 44100
#define MOVE_FRAMES_PER_BLOCK 128

typedef struct host_api_v1 {
    uint32_t api_version;
    int sample_rate;
    int frames_per_block;
    uint8_t *mapped_memory;
    int audio_out_offset;
    int audio_in_offset;
    void (*log)(const char *msg);
    int (*midi_send_internal)(const uint8_t *msg, int len);
    int (*midi_send_external)(const uint8_t *msg, int len);
} host_api_v1_t;

typedef struct plugin_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *json_defaults);
    void (*destroy_instance)(void *instance);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    int (*get_error)(void *instance, char *buf, int buf_len);
    void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;

/* sfizz C API */
#include <sfizz.h>
#include <sfizz/import/sfizz_import.h>

/* Shared host API */
static const host_api_v1_t *g_host = NULL;

/* Constants */
#define MAX_INSTRUMENTS 512
#define MAX_PRESETS 1024
#define MAX_PATH_LEN 512
#define MAX_NAME_LEN 128
/* ~150ms debounce at 44.1kHz/128 frames per block */
#define DEBOUNCE_BLOCKS 56

typedef struct {
    char name[MAX_NAME_LEN];        /* Display name (top-level folder/file name) */
    char root[MAX_PATH_LEN];        /* Top-level folder path (for sample-resolution fallback) */
    int first_preset;               /* First index in presets[] (-1 if empty) */
    int preset_count;               /* How many presets belong to this instrument */
} instrument_entry_t;

typedef struct {
    char path[MAX_PATH_LEN];        /* Full path to .sfz/.dspreset file */
    char name[MAX_NAME_LEN];        /* Display name (filename without ext) */
    int instrument_idx;             /* Index into instruments[] */
} preset_entry_t;

/* Per-Instance State */
typedef struct {
    sfizz_synth_t *synth;
    int current_preset;             /* Flat index into presets[] */
    int instrument_count;
    int preset_count;
    int octave_transpose;
    float gain;
    instrument_entry_t instruments[MAX_INSTRUMENTS];
    preset_entry_t presets[MAX_PRESETS];
    char preset_name[MAX_NAME_LEN];
    char instrument_name[MAX_NAME_LEN];     /* Cached: current preset's parent */
    char module_dir[MAX_PATH_LEN];
    char load_error[256];
    int debounce_remaining;         /* Blocks remaining before loading */
    int pending_load;               /* 1 if a deferred load is pending */
    int suppress_next_preset_set;   /* Workaround for shared UI's setBank
                                     * which calls set_param('preset', 0)
                                     * immediately after set_param('bank', N).
                                     * We set the flag in the bank handler
                                     * and consume it in the preset handler. */
    float *left_buf;
    float *right_buf;
} sfz_instance_t;

/* Helper: log via host */
static void plugin_log(const char *msg) {
    if (g_host && g_host->log) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[sfz] %s", msg);
        g_host->log(buf);
    }
}

/* Helper: extract number from JSON */
static int json_get_number(const char *json, const char *key, float *out) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *pos = strstr(json, search);
    if (!pos) return -1;
    pos += strlen(search);
    while (*pos == ' ') pos++;
    *out = (float)atof(pos);
    return 0;
}

/* Helper: extract string from JSON */
static int json_get_string(const char *json, const char *key, char *out, int out_len) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *pos = strstr(json, search);
    if (!pos) return -1;
    pos += strlen(search);
    while (*pos == ' ') pos++;
    if (*pos != '"') return -1;
    pos++;
    const char *end = strchr(pos, '"');
    if (!end) return -1;
    int len = end - pos;
    if (len >= out_len) len = out_len - 1;
    strncpy(out, pos, len);
    out[len] = '\0';
    return len;
}

/* Check if file extension is a supported instrument format */
static int is_supported_instrument(const char *ext) {
    return (strcasecmp(ext, ".sfz") == 0 ||
            strcasecmp(ext, ".dspreset") == 0);
}

/* Skip our internal `.converted.sfz` temp files so they don't appear as
 * variants when load_sfz_file's unlink races with a variant scan. */
static int is_temp_converted_sfz(const char *filename) {
    const char *suffix = ".converted.sfz";
    size_t flen = strlen(filename);
    size_t slen = strlen(suffix);
    return flen >= slen && strcasecmp(filename + flen - slen, suffix) == 0;
}

#define SCAN_MAX_DEPTH 5

/* Sort helper for the per-instrument slice of presets[]. */
static int preset_entry_cmp(const void *a, const void *b) {
    const preset_entry_t *pa = (const preset_entry_t *)a;
    const preset_entry_t *pb = (const preset_entry_t *)b;
    return strcasecmp(pa->name, pb->name);
}

/* Append one preset_entry. */
static void add_preset(sfz_instance_t *inst, const char *dir_path,
                       const char *filename, int instrument_idx) {
    if (inst->preset_count >= MAX_PRESETS) return;
    const char *ext = strrchr(filename, '.');
    if (!ext) return;
    preset_entry_t *p = &inst->presets[inst->preset_count++];
    snprintf(p->path, sizeof(p->path), "%s/%s", dir_path, filename);
    int nlen = (int)(ext - filename);
    if (nlen >= MAX_NAME_LEN) nlen = MAX_NAME_LEN - 1;
    memcpy(p->name, filename, nlen);
    p->name[nlen] = '\0';
    p->instrument_idx = instrument_idx;
}

/* Walk a folder recursively, appending all .sfz/.dspreset files as presets
 * belonging to the given instrument index. */
static void collect_presets(sfz_instance_t *inst, const char *path,
                            int instrument_idx, int depth) {
    if (depth > SCAN_MAX_DEPTH) return;
    if (inst->preset_count >= MAX_PRESETS) return;

    DIR *dir = opendir(path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        if (inst->preset_count >= MAX_PRESETS) break;

        char child[MAX_PATH_LEN];
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);

        struct stat st;
        if (stat(child, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            collect_presets(inst, child, instrument_idx, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            const char *ext = strrchr(entry->d_name, '.');
            if (ext && is_supported_instrument(ext) &&
                !is_temp_converted_sfz(entry->d_name))
                add_preset(inst, path, entry->d_name, instrument_idx);
        }
    }

    closedir(dir);
}

/* Scan instruments/ and build a flat preset list across all instruments.
 * Each top-level entry (folder or loose file) becomes one instrument; every
 * .sfz/.dspreset inside is a preset belonging to that instrument. */
static void scan_instruments(sfz_instance_t *inst, const char *module_dir) {
    char dir_path[MAX_PATH_LEN];
    snprintf(dir_path, sizeof(dir_path), "%s/instruments", module_dir);

    inst->instrument_count = 0;
    inst->preset_count = 0;

    DIR *dir = opendir(dir_path);
    if (!dir) {
        plugin_log("No instruments/ directory found");
        return;
    }

    /* Pass 1: collect top-level instrument names, sort alphabetically. */
    typedef struct {
        char name[MAX_NAME_LEN];
        char full_path[MAX_PATH_LEN];
        int is_dir;       /* 1 = folder instrument, 0 = loose file */
        char filename[MAX_NAME_LEN];   /* for loose files */
    } scan_entry_t;
    static scan_entry_t entries[MAX_INSTRUMENTS];
    int entry_count = 0;

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (entry_count >= MAX_INSTRUMENTS) {
            plugin_log("Instrument list full, skipping extras");
            break;
        }
        char full_path[MAX_PATH_LEN];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, de->d_name);
        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        scan_entry_t *e = &entries[entry_count];
        if (S_ISDIR(st.st_mode)) {
            e->is_dir = 1;
            strncpy(e->name, de->d_name, MAX_NAME_LEN - 1);
            e->name[MAX_NAME_LEN - 1] = '\0';
            strncpy(e->full_path, full_path, MAX_PATH_LEN - 1);
            e->full_path[MAX_PATH_LEN - 1] = '\0';
            entry_count++;
        } else if (S_ISREG(st.st_mode)) {
            const char *ext = strrchr(de->d_name, '.');
            if (!ext || !is_supported_instrument(ext)) continue;
            if (is_temp_converted_sfz(de->d_name)) continue;
            e->is_dir = 0;
            int nlen = (int)(ext - de->d_name);
            if (nlen >= MAX_NAME_LEN) nlen = MAX_NAME_LEN - 1;
            memcpy(e->name, de->d_name, nlen);
            e->name[nlen] = '\0';
            strncpy(e->full_path, dir_path, MAX_PATH_LEN - 1);
            e->full_path[MAX_PATH_LEN - 1] = '\0';
            strncpy(e->filename, de->d_name, MAX_NAME_LEN - 1);
            e->filename[MAX_NAME_LEN - 1] = '\0';
            entry_count++;
        }
    }
    closedir(dir);

    /* Sort top-level entries alphabetically. */
    for (int i = 0; i < entry_count - 1; i++) {
        for (int j = 0; j < entry_count - 1 - i; j++) {
            if (strcasecmp(entries[j].name, entries[j + 1].name) > 0) {
                scan_entry_t t = entries[j];
                entries[j] = entries[j + 1];
                entries[j + 1] = t;
            }
        }
    }

    /* Pass 2: for each entry, register the instrument and collect its presets. */
    for (int i = 0; i < entry_count; i++) {
        if (inst->instrument_count >= MAX_INSTRUMENTS) break;
        scan_entry_t *e = &entries[i];

        int idx = inst->instrument_count;
        instrument_entry_t *instr = &inst->instruments[idx];
        strncpy(instr->name, e->name, MAX_NAME_LEN - 1);
        instr->name[MAX_NAME_LEN - 1] = '\0';
        strncpy(instr->root, e->full_path, MAX_PATH_LEN - 1);
        instr->root[MAX_PATH_LEN - 1] = '\0';
        instr->first_preset = inst->preset_count;
        int presets_before = inst->preset_count;

        if (e->is_dir) {
            collect_presets(inst, e->full_path, idx, 0);
            /* Sort just this instrument's slice of the flat list. */
            int n = inst->preset_count - presets_before;
            if (n > 1) {
                qsort(&inst->presets[presets_before], n,
                      sizeof(preset_entry_t), preset_entry_cmp);
            }
        } else {
            add_preset(inst, e->full_path, e->filename, idx);
        }

        instr->preset_count = inst->preset_count - presets_before;
        if (instr->preset_count == 0) {
            instr->first_preset = -1;
            /* Drop empty instruments — happens for folders with no .sfz/.dspreset. */
            continue;
        }
        inst->instrument_count++;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "Found %d instruments, %d presets total",
             inst->instrument_count, inst->preset_count);
    plugin_log(msg);
}

/* Try re-loading an SFZ with sample paths resolved from instrument root.
 * Used when .sfz is in a subdirectory (e.g. presets/) but samples are
 * at the instrument root (e.g. Samples/). */
static int try_load_with_root(sfz_instance_t *inst, const char *path,
                              const char *root_path) {
    char msg[512];

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 1024 * 1024) {
        fclose(f);
        return -1;
    }

    char *content = malloc(fsize + 1);
    if (!content) {
        fclose(f);
        return -1;
    }

    size_t read_len = fread(content, 1, fsize, f);
    fclose(f);
    content[read_len] = '\0';

    /* Virtual path at instrument root so sample paths resolve from there */
    char virtual_path[MAX_PATH_LEN];
    snprintf(virtual_path, sizeof(virtual_path), "%s/virtual.sfz", root_path);

    snprintf(msg, sizeof(msg), "Retrying with root: %s", root_path);
    plugin_log(msg);

    bool ok = sfizz_load_string(inst->synth, virtual_path, content);
    free(content);

    if (!ok) return -1;

    size_t preloaded = sfizz_get_num_preloaded_samples(inst->synth);
    snprintf(msg, sizeof(msg), "Root fallback: %zu preloaded samples", preloaded);
    plugin_log(msg);

    return (preloaded > 0) ? 0 : -1;
}

/* Simple XML attribute parser helper.
 * Finds attr="value" in a tag string. Tolerates whitespace around the `=` and
 * matches the attribute name as a token (preceded by whitespace or `<`) so
 * `tuning=` doesn't accidentally hit `groupTuning=`.
 * Returns value in out_val (null-terminated), or empty string if not found. */
static void xml_get_attr(const char *tag, const char *attr_name, char *out_val, int max_len) {
    out_val[0] = '\0';
    int name_len = (int)strlen(attr_name);
    const char *p = tag;
    while ((p = strstr(p, attr_name)) != NULL) {
        /* Must be at a token boundary: preceded by whitespace or '<'. */
        if (p > tag) {
            char prev = *(p - 1);
            if (prev != ' ' && prev != '\t' && prev != '\n' && prev != '\r' && prev != '<') {
                p += name_len;
                continue;
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

/* DecentSampler effect descriptor, indexed by `position` from the dspreset. */
#define DS_MAX_FX 8
typedef struct {
    char type[32];
    char freq[32];        /* lowpass: frequency */
    char resonance[32];   /* lowpass: resonance */
    char wet_level[32];   /* reverb/delay: wetLevel */
    char room_size[32];   /* reverb: roomSize */
    char damping[32];     /* reverb: damping */
    char level[32];       /* gain: level (dB) */
} ds_effect_t;

/* Pre-scan <effects> block. Returns number of effects parsed. */
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
        xml_get_attr(tag, "type", f->type, sizeof(f->type));
        xml_get_attr(tag, "frequency", f->freq, sizeof(f->freq));
        xml_get_attr(tag, "resonance", f->resonance, sizeof(f->resonance));
        xml_get_attr(tag, "wetLevel", f->wet_level, sizeof(f->wet_level));
        xml_get_attr(tag, "roomSize", f->room_size, sizeof(f->room_size));
        xml_get_attr(tag, "damping", f->damping, sizeof(f->damping));
        xml_get_attr(tag, "level", f->level, sizeof(f->level));

        p = tag_end + 1;
        p = strstr(p, "<effect ");
    }
    return count;
}

/* Apply a <binding>'s transformation (factor + linear/table translation) to
 * an arbitrary input value `v`. `in_min`/`in_max` define the input domain
 * (typically a UI control's [minValue,maxValue], but can also be CC range
 * 0..127 when computing values from a <midi><cc> binding). */
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

/* Compute the effective parameter value from a UI control + its <binding>.
 *
 * DS computes:
 *   1. clamp ctrl `value` to [ctrl.minValue, ctrl.maxValue]
 *   2. if binding has translation="linear" with translationOutputMin/Max,
 *      remap the clamped value linearly to the output range.
 *   3. multiply by binding's `factor` if present.
 *
 * Returns 1 on success and writes the result into `out` as a string. */
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

/* Walk <ui> controls and apply their initial `value=` to the effect that
 * their <binding parameter="..."> targets. Also captures ENV_* overrides. */
static void apply_ui_overrides(const char *src,
                               ds_effect_t fx[DS_MAX_FX], int fx_count,
                               char env_attack[32], char env_decay[32],
                               char env_sustain[32], char env_release[32]) {
    /* Some dspresets are sloppy about pairing tags (e.g. ASIMOV opens
     * `<labeled-knob>` and closes with `</control>`), so when looking for the
     * end of a control we accept whichever closing tag comes first. */
    static const char *patterns[2] = { "<control", "<labeled-knob" };

    for (int pi = 0; pi < 2; pi++) {
        const char *p = src;
        size_t pat_len = strlen(patterns[pi]);
        while ((p = strstr(p, patterns[pi])) != NULL) {
            /* Make sure next char is whitespace or '>' so we don't match
             * `<controls>` or similar. */
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

            /* Range to look for nested <binding>: from after this opening tag
             * to the matching closing tag (or end of self-closed control). */
            const char *bind_search_start = tag_end + 1;
            const char *bind_search_end;
            int self_closed = (tag_end > p && *(tag_end - 1) == '/');
            if (self_closed) {
                bind_search_end = tag_end;
            } else {
                /* Accept either `</control>` or `</labeled-knob>` as the
                 * closer — see ASIMOV malformed XML note above. */
                const char *c1 = strstr(bind_search_start, "</control>");
                const char *c2 = strstr(bind_search_start, "</labeled-knob>");
                if (c1 && c2)       bind_search_end = (c1 < c2) ? c1 : c2;
                else if (c1)        bind_search_end = c1;
                else if (c2)        bind_search_end = c2;
                else {
                    p = tag_end + 1;
                    continue;
                }
            }

            /* Advance past whichever closer matched (10 chars for `</control>`,
             * 15 for `</labeled-knob>`). Detect by reading the closer ahead. */
            int closer_skip = 0;
            if (!self_closed) {
                if (strncmp(bind_search_end, "</control>", 10) == 0)
                    closer_skip = 10;
                else if (strncmp(bind_search_end, "</labeled-knob>", 15) == 0)
                    closer_skip = 15;
            }

            const char *binding = strstr(bind_search_start, "<binding");
            if (!binding || binding > bind_search_end) {
                p = self_closed ? (tag_end + 1) : (bind_search_end + closer_skip);
                continue;
            }

            const char *bind_end = strchr(binding, '>');
            if (!bind_end) {
                p = bind_search_end;
                continue;
            }

            char bind_tag[512];
            int blen = (int)(bind_end - binding);
            if (blen > 511) blen = 511;
            memcpy(bind_tag, binding, blen);
            bind_tag[blen] = '\0';

            char param[64] = "", position_str[16] = "";
            xml_get_attr(bind_tag, "parameter", param, sizeof(param));
            xml_get_attr(bind_tag, "position",  position_str, sizeof(position_str));

            char effective[64];
            if (param[0] && compute_binding_value(ctrl_tag, bind_tag,
                                                   effective, sizeof(effective))) {
                int position = position_str[0] ? atoi(position_str) : 0;

                if (strcmp(param, "ENV_ATTACK") == 0) {
                    strncpy(env_attack, effective, 31);  env_attack[31] = '\0';
                } else if (strcmp(param, "ENV_DECAY") == 0) {
                    strncpy(env_decay, effective, 31);   env_decay[31]  = '\0';
                } else if (strcmp(param, "ENV_SUSTAIN") == 0) {
                    strncpy(env_sustain, effective, 31); env_sustain[31] = '\0';
                } else if (strcmp(param, "ENV_RELEASE") == 0) {
                    strncpy(env_release, effective, 31); env_release[31] = '\0';
                } else if (position >= 0 && position < fx_count) {
                    ds_effect_t *f = &fx[position];
                    if (strcmp(param, "FX_FILTER_FREQUENCY") == 0) {
                        strncpy(f->freq, effective, 31); f->freq[31] = '\0';
                    } else if (strcmp(param, "FX_FILTER_RESONANCE") == 0) {
                        strncpy(f->resonance, effective, 31); f->resonance[31] = '\0';
                    } else if (strcmp(param, "FX_REVERB_WET_LEVEL") == 0) {
                        strncpy(f->wet_level, effective, 31); f->wet_level[31] = '\0';
                    } else if (strcmp(param, "FX_REVERB_ROOM_SIZE") == 0) {
                        strncpy(f->room_size, effective, 31); f->room_size[31] = '\0';
                    } else if (strcmp(param, "FX_REVERB_DAMPING") == 0) {
                        strncpy(f->damping, effective, 31); f->damping[31] = '\0';
                    }
                }
            }

            p = self_closed ? (tag_end + 1) : (bind_search_end + closer_skip);
        }
    }
}

/* Count <group> tags whose seqMode="round_robin" so we can emit seq_length. */
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

/* Find the Nth `<control>` or `<labeled-knob>` element in src (in document
 * order). Returns the position of the opening `<` or NULL if not found. */
static const char *find_nth_ui_control(const char *src, int n) {
    const char *p = src;
    int idx = 0;
    while (*p) {
        const char *c1 = strstr(p, "<control");
        const char *c2 = strstr(p, "<labeled-knob");
        const char *next = NULL;
        if (c1 && c2)       next = (c1 < c2) ? c1 : c2;
        else if (c1)        next = c1;
        else                next = c2;
        if (!next) return NULL;
        size_t len = (next == c1) ? 8 : 13;
        char nx = next[len];
        if (nx == ' ' || nx == '\t' || nx == '\n' || nx == '\r' ||
            nx == '>' || nx == '/') {
            if (idx == n) return next;
            idx++;
        }
        p = next + len;
    }
    return NULL;
}

/* Resolve a DS parameter name to its sfizz `_oncc<N>` opcode + scaling.
 * Returns 0 if unsupported.
 *   is_reverb_bus      → opcode goes in the reverb <effect> block.
 *   sustain_pct        → DS 0..1 ratio → sfizz 0..100 percent.
 *   multiplicative_mod → sfizz applies the mod multiplicatively to the
 *                        base (true for `amplitude`, where mod=0 silences
 *                        the output regardless of base). For these we
 *                        skip the base override and rely on set_cc<N> +
 *                        the source depth to produce the right load value. */
typedef struct {
    char opcode[32];
    double scale;
    int is_reverb_bus;
    int sustain_pct;
    int multiplicative_mod;
} cc_target_t;

static int resolve_cc_target(const char *ds_param, cc_target_t *out) {
    memset(out, 0, sizeof(*out));
    out->scale = 1.0;
    if (strcmp(ds_param, "ENV_ATTACK") == 0)
        snprintf(out->opcode, sizeof(out->opcode), "ampeg_attack");
    else if (strcmp(ds_param, "ENV_DECAY") == 0)
        snprintf(out->opcode, sizeof(out->opcode), "ampeg_decay");
    else if (strcmp(ds_param, "ENV_SUSTAIN") == 0) {
        snprintf(out->opcode, sizeof(out->opcode), "ampeg_sustain");
        out->sustain_pct = 1;
    } else if (strcmp(ds_param, "ENV_RELEASE") == 0)
        snprintf(out->opcode, sizeof(out->opcode), "ampeg_release");
    else if (strcmp(ds_param, "FX_FILTER_FREQUENCY") == 0)
        snprintf(out->opcode, sizeof(out->opcode), "cutoff");
    else if (strcmp(ds_param, "FX_FILTER_RESONANCE") == 0)
        snprintf(out->opcode, sizeof(out->opcode), "resonance");
    else if (strcmp(ds_param, "AMP_VOLUME") == 0 ||
             strcmp(ds_param, "TAG_VOLUME") == 0) {
        snprintf(out->opcode, sizeof(out->opcode), "amplitude");
        out->scale = 100.0;          /* DS 0..1 → sfizz 0..100% */
        out->multiplicative_mod = 1; /* sfizz amplitude mod is *= */
    } else if (strcmp(ds_param, "FX_REVERB_WET_LEVEL") == 0) {
        snprintf(out->opcode, sizeof(out->opcode), "reverb_wet");
        out->scale = 100.0;
        out->is_reverb_bus = 1;
    } else if (strcmp(ds_param, "FX_REVERB_ROOM_SIZE") == 0) {
        snprintf(out->opcode, sizeof(out->opcode), "reverb_size");
        out->scale = 100.0;
        out->is_reverb_bus = 1;
    } else if (strcmp(ds_param, "FX_REVERB_DAMPING") == 0) {
        snprintf(out->opcode, sizeof(out->opcode), "reverb_damp");
        out->scale = 100.0;
        out->is_reverb_bus = 1;
    } else {
        return 0;
    }
    return 1;
}

/* Collected <midi><cc> bindings, ready to be emitted into the SFZ output. */
typedef struct {
    int cc_number;          /* CC number (1..127) */
    cc_target_t target;
    double v_at_0;          /* parameter value when CC=0 */
    double v_at_127;        /* parameter value when CC=127 */
    int cc_at_load;         /* CC value that reproduces the UI's load-time
                             * default — for level=ui this is derived from
                             * the target knob's `value=` attribute, so the
                             * patch loads at the position the dspreset
                             * intended. Defaults to 127 (full) when the
                             * binding is level=instrument with no UI ctrl. */
} ds_cc_binding_t;

#define DS_MAX_CC 16

/* Parse <midi><cc> blocks and resolve each binding to a sfizz opcode + range.
 * Returns the number of bindings collected. */
static int parse_midi_bindings(const char *src, ds_cc_binding_t bindings[DS_MAX_CC]) {
    int n = 0;
    const char *midi = strstr(src, "<midi");
    if (!midi) return 0;
    const char *midi_end = strstr(midi, "</midi>");
    if (!midi_end) midi_end = src + strlen(src);

    const char *p = midi;
    while (n < DS_MAX_CC) {
        const char *cc = strstr(p, "<cc ");
        if (!cc || cc >= midi_end) break;
        const char *cc_tag_end = strchr(cc, '>');
        if (!cc_tag_end) break;

        char cc_tag[256];
        int tlen = (int)(cc_tag_end - cc);
        if (tlen > 255) tlen = 255;
        memcpy(cc_tag, cc, tlen);
        cc_tag[tlen] = '\0';

        char num_str[16];
        xml_get_attr(cc_tag, "number", num_str, sizeof(num_str));
        int cc_num = num_str[0] ? atoi(num_str) : 0;
        if (cc_num <= 0 || cc_num > 127) {
            p = cc_tag_end + 1;
            continue;
        }

        const char *cc_close = strstr(cc_tag_end, "</cc>");
        if (!cc_close || cc_close > midi_end) cc_close = midi_end;

        const char *bind = strstr(cc_tag_end, "<binding");
        if (!bind || bind > cc_close) {
            p = cc_close;
            continue;
        }
        const char *bind_end = strchr(bind, '>');
        if (!bind_end) break;

        char bind_tag[512];
        int blen = (int)(bind_end - bind);
        if (blen > 511) blen = 511;
        memcpy(bind_tag, bind, blen);
        bind_tag[blen] = '\0';

        char level[32], param[64], pos_str[16];
        xml_get_attr(bind_tag, "level",     level,   sizeof(level));
        xml_get_attr(bind_tag, "parameter", param,   sizeof(param));
        xml_get_attr(bind_tag, "position",  pos_str, sizeof(pos_str));

        const char *target_param = NULL;
        double v_at_0 = 0, v_at_127 = 0;
        int cc_at_load = 127;       /* default for level=instrument */

        /* CC bindings have their own translation that maps CC 0..127 to
         * an output range. Apply it first to get values at the CC endpoints. */
        double cc_v0 = apply_binding_xform(bind_tag, 0, 127, 0);
        double cc_v127 = apply_binding_xform(bind_tag, 0, 127, 127);

        if (strcmp(level, "instrument") == 0) {
            target_param = param;
            v_at_0 = cc_v0;
            v_at_127 = cc_v127;
        } else if (strcmp(level, "ui") == 0 && strcmp(param, "VALUE") == 0 &&
                   pos_str[0]) {
            int target_pos = atoi(pos_str);
            const char *ctrl = find_nth_ui_control(src, target_pos);
            if (!ctrl) {
                p = cc_close;
                continue;
            }
            const char *ctrl_tag_end = strchr(ctrl, '>');
            if (!ctrl_tag_end) break;
            char ctrl_tag[1024];
            int clen = (int)(ctrl_tag_end - ctrl);
            if (clen > 1023) clen = 1023;
            memcpy(ctrl_tag, ctrl, clen);
            ctrl_tag[clen] = '\0';

            /* Find the target control's first <binding>. */
            const char *t_bind = strstr(ctrl_tag_end, "<binding");
            if (!t_bind) {
                p = cc_close;
                continue;
            }
            const char *t_bind_end = strchr(t_bind, '>');
            if (!t_bind_end) break;
            char t_bind_tag[512];
            int tblen = (int)(t_bind_end - t_bind);
            if (tblen > 511) tblen = 511;
            memcpy(t_bind_tag, t_bind, tblen);
            t_bind_tag[tblen] = '\0';

            char t_param[64];
            xml_get_attr(t_bind_tag, "parameter", t_param, sizeof(t_param));

            char cmin[64], cmax[64], cval[64];
            xml_get_attr(ctrl_tag, "minValue", cmin, sizeof(cmin));
            xml_get_attr(ctrl_tag, "maxValue", cmax, sizeof(cmax));
            xml_get_attr(ctrl_tag, "value",    cval, sizeof(cval));
            double minv = cmin[0] ? atof(cmin) : 0.0;
            double maxv = cmax[0] ? atof(cmax) : 1.0;

            target_param = t_param;
            v_at_0   = apply_binding_xform(t_bind_tag, minv, maxv, cc_v0);
            v_at_127 = apply_binding_xform(t_bind_tag, minv, maxv, cc_v127);

            /* Compute the CC value that reproduces the knob's load-time
             * `value=` attribute. The knob's value sits in the CC binding's
             * output range [OutMin, OutMax]; map it back to [0, 127]. */
            char omin[64], omax[64], trans[32];
            xml_get_attr(bind_tag, "translationOutputMin", omin,  sizeof(omin));
            xml_get_attr(bind_tag, "translationOutputMax", omax,  sizeof(omax));
            xml_get_attr(bind_tag, "translation",          trans, sizeof(trans));
            if (cval[0] && strcmp(trans, "linear") == 0 && omin[0] && omax[0]) {
                double k = atof(cval);
                double oM = atof(omin), oX = atof(omax);
                if (oX != oM) {
                    double t = (k - oM) / (oX - oM);
                    if (t < 0) t = 0; if (t > 1) t = 1;
                    cc_at_load = (int)(t * 127.0 + 0.5);
                }
            }
        } else {
            p = cc_close;
            continue;
        }

        cc_target_t t;
        if (target_param && resolve_cc_target(target_param, &t)) {
            ds_cc_binding_t *b = &bindings[n++];
            b->cc_number = cc_num;
            b->target = t;
            b->v_at_0 = v_at_0;
            b->v_at_127 = v_at_127;
            b->cc_at_load = cc_at_load;
        }

        p = cc_close;
    }
    return n;
}

/* Convert a .dspreset file to SFZ text and write as a temp .sfz file.
 * Returns path to the temp .sfz file, or NULL on failure.
 * Caller must free the returned string. */
static char *convert_dspreset_to_sfz(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 1024 * 1024) {
        fclose(f);
        return NULL;
    }

    char *src = (char *)malloc(size + 1);
    if (!src) { fclose(f); return NULL; }
    fread(src, 1, size, f);
    src[size] = '\0';
    fclose(f);

    /* Fix malformed XML: insert spaces between attributes where missing.
     * Track quote parity: odd = opening, even = closing. */
    {
        int fixes = 0, in_tag = 0, quote_count = 0;
        for (long i = 0; i < size; i++) {
            if (src[i] == '<') { in_tag = 1; quote_count = 0; }
            else if (src[i] == '>') { in_tag = 0; }
            if (in_tag && src[i] == '"') {
                quote_count++;
                if ((quote_count % 2 == 0) && i + 1 < size &&
                    ((src[i+1] >= 'a' && src[i+1] <= 'z') ||
                     (src[i+1] >= 'A' && src[i+1] <= 'Z')))
                    fixes++;
            }
        }
        if (fixes > 0) {
            char *dst = (char *)malloc(size + fixes + 1);
            if (!dst) { free(src); return NULL; }
            long j = 0;
            in_tag = 0; quote_count = 0;
            for (long i = 0; i < size; i++) {
                if (src[i] == '<') { in_tag = 1; quote_count = 0; }
                else if (src[i] == '>') { in_tag = 0; }
                dst[j++] = src[i];
                if (in_tag && src[i] == '"') {
                    quote_count++;
                    if ((quote_count % 2 == 0) && i + 1 < size &&
                        ((src[i+1] >= 'a' && src[i+1] <= 'z') ||
                         (src[i+1] >= 'A' && src[i+1] <= 'Z')))
                        dst[j++] = ' ';
                }
            }
            dst[j] = '\0';
            free(src);
            src = dst;
            size = j;
            char log_msg[128];
            snprintf(log_msg, sizeof(log_msg),
                     "Fixed %d malformed XML attributes in dspreset", fixes);
            plugin_log(log_msg);
        }
    }

    /* Now convert XML to SFZ by scanning for tags */
    /* Allocate output buffer (SFZ is typically smaller than XML) */
    char *sfz = (char *)malloc(size * 2);
    if (!sfz) { free(src); return NULL; }
    int pos = 0;
    char val[512];

    /* === Pre-scan: effects, UI overrides, round-robin === */
    ds_effect_t fx[DS_MAX_FX] = {0};
    int fx_count = parse_effects(src, fx);

    char env_attack[32] = "", env_decay[32] = "";
    char env_sustain[32] = "", env_release[32] = "";
    apply_ui_overrides(src, fx, fx_count, env_attack, env_decay, env_sustain, env_release);

    int rr_total = count_rr_groups(src);

    /* Parse <midi><cc> bindings — these become sfizz `_oncc<N>` opcodes plus
     * a `set_cc<N>=127` initializer at <control> level so CC starts maxed. */
    ds_cc_binding_t ccs[DS_MAX_CC];
    int cc_count = parse_midi_bindings(src, ccs);

    /* Locate effects we care about (first match wins by DS convention). */
    int reverb_idx = -1, lp_idx = -1, gain_idx = -1;
    for (int i = 0; i < fx_count; i++) {
        if (reverb_idx < 0 && strcmp(fx[i].type, "reverb") == 0) reverb_idx = i;
        if (lp_idx < 0 && (strcmp(fx[i].type, "lowpass") == 0 ||
                            strcmp(fx[i].type, "lowpass_4pl") == 0)) lp_idx = i;
        if (gain_idx < 0 && strcmp(fx[i].type, "gain") == 0) gain_idx = i;
    }

    /* === Dedupe CC bindings by target opcode ===
     * Multiple <midi><cc> blocks can target the same SFZ opcode (e.g. Raw
     * Violin Pad has CC11 + CC22 both modulating amplitude — global vs.
     * per-tag in DS, but we collapse to global). sfizz mod sources sum,
     * so emitting two amplitude_oncc<N>=100 would give 2x output. Keep
     * only the first binding per opcode. */
    int kept = 0;
    for (int i = 0; i < cc_count; i++) {
        int dup = 0;
        for (int j = 0; j < kept; j++) {
            if (strcmp(ccs[i].target.opcode, ccs[j].target.opcode) == 0) {
                dup = 1;
                break;
            }
        }
        if (!dup) {
            if (kept != i) ccs[kept] = ccs[i];
            kept++;
        }
    }
    cc_count = kept;

    /* === Emit <control> set_cc<N> initializers ===
     * Each CC binding's load-time CC value comes from the target UI knob's
     * `value=` attribute (or 127 default for level=instrument). */
    if (cc_count > 0) {
        int any = 0;
        for (int i = 0; i < cc_count; i++) {
            double delta;
            if (ccs[i].target.sustain_pct) {
                delta = (ccs[i].v_at_127 - ccs[i].v_at_0) * 100.0;
            } else {
                delta = (ccs[i].v_at_127 - ccs[i].v_at_0) * ccs[i].target.scale;
            }
            if (delta > -1e-6 && delta < 1e-6) continue;
            if (!any) {
                pos += snprintf(sfz + pos, size * 2 - pos, "<control>\n");
                any = 1;
            }
            pos += snprintf(sfz + pos, size * 2 - pos,
                            "set_cc%d=%d\n", ccs[i].cc_number,
                            ccs[i].cc_at_load);
        }
    }

    /* === Emit reverb effect block (sfizz fverb) ===
     * Implement DS's wet/dry MIX (not additive) by attenuating the main bus
     * (directtomain) by (1 - wet) and the fx1 bus (fx1tomain) by wet. The
     * fverb itself runs fully wet — the bus gains do the mix.
     *
     * Without this, fx1 output (full wet) was being added on top of the
     * full-strength main signal, ~doubling amplitude on heavy-reverb chords
     * (e.g. ASIMOV's Brave New World with wet=0.8). */
    if (reverb_idx >= 0) {
        /* DS-documented defaults: roomSize=0.7, damping=0.3, wetLevel=0. */
        double wet  = fx[reverb_idx].wet_level[0] ? atof(fx[reverb_idx].wet_level) : 0.0;
        double room = fx[reverb_idx].room_size[0] ? atof(fx[reverb_idx].room_size) : 0.7;
        double damp = fx[reverb_idx].damping[0]   ? atof(fx[reverb_idx].damping)   : 0.3;
        if (wet  < 0) wet  = 0; if (wet  > 1) wet  = 1;
        if (room < 0) room = 0; if (room > 1) room = 1;
        if (damp < 0) damp = 0; if (damp > 1) damp = 1;

        pos += snprintf(sfz + pos, size * 2 - pos,
                        "<effect>\ntype=fverb\nbus=fx1\n"
                        "reverb_dry=0\nreverb_wet=100\n");
        pos += snprintf(sfz + pos, size * 2 - pos, "reverb_size=%.1f\n", room * 100.0);
        pos += snprintf(sfz + pos, size * 2 - pos, "reverb_damp=%.1f\n", damp * 100.0);

        /* Wet/dry crossfade via bus gain so total amplitude stays ~unity. */
        pos += snprintf(sfz + pos, size * 2 - pos,
                        "directtomain=%.1f\nfx1tomain=%.1f\n",
                        (1.0 - wet) * 100.0, wet * 100.0);

        /* CC modulation for reverb_* opcodes (room/damp/wet). Override base
         * + emit oncc range, mirroring the global non-reverb path. */
        for (int i = 0; i < cc_count; i++) {
            if (!ccs[i].target.is_reverb_bus) continue;
            double base  = ccs[i].v_at_0   * ccs[i].target.scale;
            double delta = (ccs[i].v_at_127 - ccs[i].v_at_0) * ccs[i].target.scale;
            if (delta > -1e-6 && delta < 1e-6) continue;
            pos += snprintf(sfz + pos, size * 2 - pos, "%s=%g\n",
                            ccs[i].target.opcode, base);
            pos += snprintf(sfz + pos, size * 2 - pos, "%s_oncc%d=%g\n",
                            ccs[i].target.opcode, ccs[i].cc_number, delta);
        }
    }

    /* === Emit <global> with combined defaults === */
    char *groups_tag = strstr(src, "<groups");
    char wrapper_attack[64] = "", wrapper_decay[64] = "";
    char wrapper_sustain[64] = "", wrapper_release[64] = "";
    char wrapper_volume[64] = "", wrapper_loop[64] = "";
    if (groups_tag) {
        char *groups_end = strchr(groups_tag, '>');
        if (groups_end) {
            char tag_buf[1024];
            int tlen = groups_end - groups_tag;
            if (tlen >= (int)sizeof(tag_buf)) tlen = sizeof(tag_buf) - 1;
            memcpy(tag_buf, groups_tag, tlen);
            tag_buf[tlen] = '\0';
            xml_get_attr(tag_buf, "volume",      wrapper_volume,  sizeof(wrapper_volume));
            xml_get_attr(tag_buf, "attack",      wrapper_attack,  sizeof(wrapper_attack));
            xml_get_attr(tag_buf, "decay",       wrapper_decay,   sizeof(wrapper_decay));
            xml_get_attr(tag_buf, "sustain",     wrapper_sustain, sizeof(wrapper_sustain));
            xml_get_attr(tag_buf, "release",     wrapper_release, sizeof(wrapper_release));
            xml_get_attr(tag_buf, "loopEnabled", wrapper_loop,    sizeof(wrapper_loop));
        }
    }

    pos += snprintf(sfz + pos, size * 2 - pos, "<global>\n");

    /* Volume: <groups volume=...> first, then add any gain effect (dB). */
    if (wrapper_volume[0])
        pos += snprintf(sfz + pos, size * 2 - pos, "volume=%s\n", wrapper_volume);
    if (gain_idx >= 0 && fx[gain_idx].level[0])
        pos += snprintf(sfz + pos, size * 2 - pos, "global_volume=%s\n", fx[gain_idx].level);

    /* ADSR: prefer the explicit <groups> wrapper value (author's stated
     * default), fall back to the UI knob's `value=` when no wrapper attr.
     * Without runtime knob control on Move, the wrapper is usually closer
     * to what the author intended the patch to sound like. */
    const char *use_attack  = wrapper_attack[0]  ? wrapper_attack  : env_attack;
    const char *use_decay   = wrapper_decay[0]   ? wrapper_decay   : env_decay;
    const char *use_sustain = wrapper_sustain[0] ? wrapper_sustain : env_sustain;
    const char *use_release = wrapper_release[0] ? wrapper_release : env_release;
    if (use_attack[0])
        pos += snprintf(sfz + pos, size * 2 - pos, "ampeg_attack=%s\n", use_attack);
    if (use_decay[0])
        pos += snprintf(sfz + pos, size * 2 - pos, "ampeg_decay=%s\n", use_decay);
    if (use_sustain[0]) {
        float s = atof(use_sustain) * 100.0f;
        if (s < 0) s = 0; if (s > 100) s = 100;
        pos += snprintf(sfz + pos, size * 2 - pos, "ampeg_sustain=%.1f\n", s);
    }
    if (use_release[0])
        pos += snprintf(sfz + pos, size * 2 - pos, "ampeg_release=%s\n", use_release);
    else {
        /* Many DS presets (e.g. K4Coll pianos) specify no release attr at
         * all. DS's docs don't state a default, but its own boilerplate
         * dspreset uses release="0.430". sfizz's default is ~1ms which
         * cuts notes abruptly, so fall back to 0.5s. */
        pos += snprintf(sfz + pos, size * 2 - pos, "ampeg_release=0.5\n");
    }
    if (wrapper_loop[0])
        pos += snprintf(sfz + pos, size * 2 - pos, "loop_mode=%s\n",
                        strcmp(wrapper_loop, "true") == 0 ? "loop_continuous" : "no_loop");

    /* Lowpass filter at <global> level. DS defaults: frequency=22000,
     * resonance=0.7. sfizz `resonance` is in dB (a Q→dB conversion would
     * be more faithful, but the values DS authors set tend to land in
     * sane sfizz dB ranges anyway, so pass-through for now). */
    if (lp_idx >= 0) {
        const char *fil_type = strcmp(fx[lp_idx].type, "lowpass_4pl") == 0
                                 ? "lpf_4p" : "lpf_2p";
        const char *cutoff = fx[lp_idx].freq[0]      ? fx[lp_idx].freq      : "22000";
        const char *q      = fx[lp_idx].resonance[0] ? fx[lp_idx].resonance : "0.7";
        pos += snprintf(sfz + pos, size * 2 - pos,
                        "fil_type=%s\ncutoff=%s\nresonance=%s\n",
                        fil_type, cutoff, q);
    }

    /* Reverb send: route 100% to fx1 — wet level is controlled by reverb_wet. */
    if (reverb_idx >= 0)
        pos += snprintf(sfz + pos, size * 2 - pos, "effect1=100\n");

    /* === CC modulation opcodes ===
     * For most parameters we use absolute control: emit `base = v_at_0` and
     * `_oncc<N> = delta` so the patch sweeps fully across the CC range.
     *
     * For multiplicative-mod targets (sfizz's `amplitude`) we DON'T override
     * the base — sfizz applies the mod as `output *= mod` so a base override
     * either silences (mod=0 at CC=0) or doubles up. Instead we leave the
     * base at sfizz's default (unity) and let the source depth + set_cc do
     * the work. */
    for (int i = 0; i < cc_count; i++) {
        if (ccs[i].target.is_reverb_bus) continue;
        double base, delta;
        if (ccs[i].target.sustain_pct) {
            base  = ccs[i].v_at_0   * 100.0;
            delta = (ccs[i].v_at_127 - ccs[i].v_at_0) * 100.0;
        } else {
            base  = ccs[i].v_at_0   * ccs[i].target.scale;
            delta = (ccs[i].v_at_127 - ccs[i].v_at_0) * ccs[i].target.scale;
        }
        if (delta > -1e-6 && delta < 1e-6) continue;
        if (!ccs[i].target.multiplicative_mod) {
            pos += snprintf(sfz + pos, size * 2 - pos, "%s=%g\n",
                            ccs[i].target.opcode, base);
        }
        pos += snprintf(sfz + pos, size * 2 - pos, "%s_oncc%d=%g\n",
                        ccs[i].target.opcode, ccs[i].cc_number, delta);
    }

    /* Find each <group> */
    char *scan = src;
    while ((scan = strstr(scan, "<group")) != NULL) {
        /* Skip <groups> tag */
        if (scan[6] == 's' || scan[6] == 'S') { scan += 7; continue; }
        char *tag_end = strchr(scan, '>');
        if (!tag_end) break;

        char tag_buf[2048];
        int tlen = tag_end - scan;
        if (tlen >= (int)sizeof(tag_buf)) tlen = sizeof(tag_buf) - 1;
        memcpy(tag_buf, scan, tlen);
        tag_buf[tlen] = '\0';

        pos += snprintf(sfz + pos, size * 2 - pos, "<group>\n");
        xml_get_attr(tag_buf, "volume", val, sizeof(val));
        if (val[0]) pos += snprintf(sfz + pos, size * 2 - pos, "volume=%s\n", val);
        xml_get_attr(tag_buf, "ampVelTrack", val, sizeof(val));
        if (val[0]) pos += snprintf(sfz + pos, size * 2 - pos, "amp_veltrack=%s\n", val);
        xml_get_attr(tag_buf, "attack", val, sizeof(val));
        if (val[0]) pos += snprintf(sfz + pos, size * 2 - pos, "ampeg_attack=%s\n", val);
        xml_get_attr(tag_buf, "decay", val, sizeof(val));
        if (val[0]) pos += snprintf(sfz + pos, size * 2 - pos, "ampeg_decay=%s\n", val);
        xml_get_attr(tag_buf, "sustain", val, sizeof(val));
        if (val[0]) {
            float s = atof(val) * 100.0f;
            pos += snprintf(sfz + pos, size * 2 - pos, "ampeg_sustain=%.1f\n", s);
        }
        xml_get_attr(tag_buf, "release", val, sizeof(val));
        if (val[0]) pos += snprintf(sfz + pos, size * 2 - pos, "ampeg_release=%s\n", val);

        /* Round-robin: emit seq_position/seq_length so sfizz cycles between
         * sibling RR groups (Frozen Glock and similar). */
        xml_get_attr(tag_buf, "seqMode", val, sizeof(val));
        if (strcmp(val, "round_robin") == 0 && rr_total > 0) {
            char rr_pos[16];
            xml_get_attr(tag_buf, "seqPosition", rr_pos, sizeof(rr_pos));
            if (rr_pos[0]) {
                pos += snprintf(sfz + pos, size * 2 - pos,
                                "seq_length=%d\nseq_position=%s\n",
                                rr_total, rr_pos);
            }
        }

        /* Find <sample> tags within this group */
        char *sample_scan = tag_end;
        char *group_end = strstr(tag_end, "</group>");
        if (!group_end) group_end = src + size;

        while (sample_scan < group_end &&
               (sample_scan = strstr(sample_scan, "<sample")) != NULL &&
               sample_scan < group_end) {
            char *stag_end = strchr(sample_scan, '>');
            if (!stag_end || stag_end > group_end) break;

            /* Skip commented-out samples */
            char *comment_start = NULL;
            for (char *c = sample_scan - 1; c >= src && c >= sample_scan - 20; c--) {
                if (*c == '-' && c > src && *(c-1) == '-' && c > src+1 && *(c-2) == '!') {
                    comment_start = c - 2;
                    break;
                }
                if (*c != ' ' && *c != '\t' && *c != '\n' && *c != '\r') break;
            }
            if (comment_start) {
                sample_scan = stag_end + 1;
                continue;
            }

            char stag_buf[1024];
            int stlen = stag_end - sample_scan;
            if (stlen >= (int)sizeof(stag_buf)) stlen = sizeof(stag_buf) - 1;
            memcpy(stag_buf, sample_scan, stlen);
            stag_buf[stlen] = '\0';

            pos += snprintf(sfz + pos, size * 2 - pos, "<region>\n");

            /* rootNote -> key (must come before lokey/hikey) */
            xml_get_attr(stag_buf, "rootNote", val, sizeof(val));
            if (val[0]) pos += snprintf(sfz + pos, size * 2 - pos, "pitch_keycenter=%s\n", val);

            xml_get_attr(stag_buf, "path", val, sizeof(val));
            if (val[0]) {
                /* Normalize Windows backslashes (Raw Violin uses Samples\foo.wav). */
                for (char *p = val; *p; p++) if (*p == '\\') *p = '/';
                pos += snprintf(sfz + pos, size * 2 - pos, "sample=%s\n", val);
            }
            xml_get_attr(stag_buf, "loNote", val, sizeof(val));
            if (val[0]) pos += snprintf(sfz + pos, size * 2 - pos, "lokey=%s\n", val);
            xml_get_attr(stag_buf, "hiNote", val, sizeof(val));
            if (val[0]) pos += snprintf(sfz + pos, size * 2 - pos, "hikey=%s\n", val);
            xml_get_attr(stag_buf, "loVel", val, sizeof(val));
            if (val[0]) pos += snprintf(sfz + pos, size * 2 - pos, "lovel=%s\n", val);
            xml_get_attr(stag_buf, "hiVel", val, sizeof(val));
            if (val[0]) pos += snprintf(sfz + pos, size * 2 - pos, "hivel=%s\n", val);
            xml_get_attr(stag_buf, "start", val, sizeof(val));
            if (val[0]) pos += snprintf(sfz + pos, size * 2 - pos, "offset=%s\n", val);
            xml_get_attr(stag_buf, "end", val, sizeof(val));
            if (val[0]) pos += snprintf(sfz + pos, size * 2 - pos, "end=%s\n", val);
            xml_get_attr(stag_buf, "tuning", val, sizeof(val));
            if (val[0]) pos += snprintf(sfz + pos, size * 2 - pos, "transpose=%s\n", val);
            xml_get_attr(stag_buf, "pan", val, sizeof(val));
            if (val[0]) pos += snprintf(sfz + pos, size * 2 - pos, "pan=%s\n", val);
            xml_get_attr(stag_buf, "loopEnabled", val, sizeof(val));
            if (val[0]) pos += snprintf(sfz + pos, size * 2 - pos, "loop_mode=%s\n",
                                         strcmp(val, "true") == 0 ? "loop_continuous" : "no_loop");
            xml_get_attr(stag_buf, "loopStart", val, sizeof(val));
            if (val[0]) pos += snprintf(sfz + pos, size * 2 - pos, "loop_start=%s\n", val);
            xml_get_attr(stag_buf, "loopEnd", val, sizeof(val));
            if (val[0]) pos += snprintf(sfz + pos, size * 2 - pos, "loop_end=%s\n", val);

            sample_scan = stag_end + 1;
        }

        scan = group_end;
    }

    free(src);

    /* Write SFZ to temp file in same directory */
    char *tmp_path = (char *)malloc(strlen(path) + 16);
    if (!tmp_path) { free(sfz); return NULL; }
    /* Replace .dspreset with .converted.sfz */
    const char *dsp_ext = strrchr(path, '.');
    int base_len = dsp_ext ? (int)(dsp_ext - path) : (int)strlen(path);
    snprintf(tmp_path, strlen(path) + 16, "%.*s.converted.sfz", base_len, path);

    f = fopen(tmp_path, "wb");
    if (!f) { free(sfz); free(tmp_path); return NULL; }
    fwrite(sfz, 1, pos, f);
    fclose(f);
    free(sfz);

    char log_msg[128];
    snprintf(log_msg, sizeof(log_msg), "Converted dspreset to SFZ (%d bytes)", pos);
    plugin_log(log_msg);

    return tmp_path;
}

/* Load a .sfz or .dspreset file into the synth.
 * root_path is the instrument root folder, used as fallback for
 * sample resolution when the .sfz is in a subdirectory. */
static int load_sfz_file(sfz_instance_t *inst, const char *path,
                         const char *root_path) {
    char msg[512];

    snprintf(msg, sizeof(msg), "Loading: %s", path);
    plugin_log(msg);

    /* Check file exists and is readable */
    struct stat st;
    if (stat(path, &st) != 0) {
        snprintf(msg, sizeof(msg), "File not found: %s", path);
        plugin_log(msg);
        snprintf(inst->load_error, sizeof(inst->load_error),
                 "Instrument file not found");
        return -1;
    }
    snprintf(msg, sizeof(msg), "File size: %ld bytes", (long)st.st_size);
    plugin_log(msg);

    /* For .dspreset files, convert to SFZ and load directly.
     * This bypasses sfizz's built-in importer which has issues with
     * sfizz_load_string sample path resolution. */
    const char *ext = strrchr(path, '.');
    const char *format = NULL;

    if (ext && strcasecmp(ext, ".dspreset") == 0) {
        char *converted_path = convert_dspreset_to_sfz(path);
        if (converted_path) {
            if (!sfizz_load_file(inst->synth, converted_path)) {
                snprintf(msg, sizeof(msg), "Failed to load converted SFZ: %s",
                         converted_path);
                plugin_log(msg);
                snprintf(inst->load_error, sizeof(inst->load_error),
                         "DecentSampler conversion failed");
                unlink(converted_path);
                free(converted_path);
                return -1;
            }
            format = "DecentSampler instrument";
            /* unlink(converted_path); */
            free(converted_path);
        } else {
            /* Conversion failed, try sfizz's built-in importer as fallback */
            plugin_log("dspreset conversion failed, trying sfizz importer");
            if (!sfizz_load_or_import_file(inst->synth, path, &format)) {
                snprintf(inst->load_error, sizeof(inst->load_error),
                         "DecentSampler import failed - check XML format");
                return -1;
            }
        }
    } else {
        if (!sfizz_load_or_import_file(inst->synth, path, &format)) {
            snprintf(msg, sizeof(msg), "Failed to load: %s", path);
            plugin_log(msg);
            snprintf(inst->load_error, sizeof(inst->load_error),
                     "Failed to load instrument file");
            return -1;
        }
    }

    int num_regions = sfizz_get_num_regions(inst->synth);

    if (format) {
        snprintf(msg, sizeof(msg), "Imported %s: %d regions", format, num_regions);
    } else {
        snprintf(msg, sizeof(msg), "SFZ loaded: %d regions", num_regions);
    }
    plugin_log(msg);

    if (num_regions == 0) {
        snprintf(inst->load_error, sizeof(inst->load_error),
                 "Instrument loaded but has 0 regions (no samples mapped)");
        plugin_log("WARNING: 0 regions - instrument will produce no sound");
    } else {
        /* Check if samples were actually found on disk */
        size_t preloaded = sfizz_get_num_preloaded_samples(inst->synth);
        snprintf(msg, sizeof(msg), "Preloaded samples: %zu", preloaded);
        plugin_log(msg);
        if (preloaded == 0) {
            /* If SFZ is in a subdirectory, try resolving samples from
             * the instrument root. Many packs (e.g. drolez/SHLD) put
             * .sfz files in presets/ but reference Samples/ at the root. */
            const char *instrument_root = NULL;
            if (inst->current_preset >= 0 && inst->current_preset < inst->preset_count) {
                int ii = inst->presets[inst->current_preset].instrument_idx;
                if (ii >= 0 && ii < inst->instrument_count)
                    instrument_root = inst->instruments[ii].root;
            }
            if (root_path && root_path[0] &&
                ext && strcasecmp(ext, ".sfz") == 0 &&
                instrument_root && strcmp(root_path, instrument_root) != 0 &&
                try_load_with_root(inst, path, instrument_root) == 0) {
                plugin_log("Loaded with instrument root path fallback");
                inst->load_error[0] = '\0';
            } else {
                snprintf(inst->load_error, sizeof(inst->load_error),
                         "Sample files not found - upload complete instrument with audio files");
                plugin_log("WARNING: 0 preloaded samples - files missing from disk");
            }
        } else {
            inst->load_error[0] = '\0';
        }
    }

    return 0;
}

/* Find preset index by name, returns -1 if not found. */
static int find_preset_by_name(sfz_instance_t *inst, const char *name) {
    for (int i = 0; i < inst->preset_count; i++) {
        if (strcmp(inst->presets[i].name, name) == 0) return i;
    }
    return -1;
}

/* Find instrument index by name, returns -1 if not found. */
static int find_instrument_by_name(sfz_instance_t *inst, const char *name) {
    for (int i = 0; i < inst->instrument_count; i++) {
        if (strcmp(inst->instruments[i].name, name) == 0) return i;
    }
    return -1;
}

/* Update cached preset_name + instrument_name from the current preset index. */
static void sync_preset_display(sfz_instance_t *inst) {
    if (inst->preset_count <= 0 || inst->current_preset < 0 ||
        inst->current_preset >= inst->preset_count) {
        strcpy(inst->preset_name, "No presets");
        inst->instrument_name[0] = '\0';
        return;
    }
    preset_entry_t *p = &inst->presets[inst->current_preset];
    strncpy(inst->preset_name, p->name, sizeof(inst->preset_name) - 1);
    inst->preset_name[sizeof(inst->preset_name) - 1] = '\0';
    if (p->instrument_idx >= 0 && p->instrument_idx < inst->instrument_count) {
        strncpy(inst->instrument_name, inst->instruments[p->instrument_idx].name,
                sizeof(inst->instrument_name) - 1);
        inst->instrument_name[sizeof(inst->instrument_name) - 1] = '\0';
    } else {
        inst->instrument_name[0] = '\0';
    }
}

/* Load presets[current_preset] (called after debounce). */
static void do_load_preset(sfz_instance_t *inst) {
    int idx = inst->current_preset;
    if (idx < 0 || idx >= inst->preset_count) return;
    preset_entry_t *p = &inst->presets[idx];

    /* Sample-resolution root = directory containing this preset. */
    char root[MAX_PATH_LEN];
    strncpy(root, p->path, sizeof(root) - 1);
    root[sizeof(root) - 1] = '\0';
    char *slash = strrchr(root, '/');
    if (slash) *slash = '\0';

    load_sfz_file(inst, p->path, root);

    char msg[128];
    snprintf(msg, sizeof(msg), "Preset %d: %s [%s]",
             idx, inst->preset_name, inst->instrument_name);
    plugin_log(msg);
}

/* Switch to a preset (deferred load with debounce). */
static void set_preset_index(sfz_instance_t *inst, int index) {
    if (inst->preset_count <= 0) return;
    if (index < 0) index = inst->preset_count - 1;
    if (index >= inst->preset_count) index = 0;

    inst->current_preset = index;
    sync_preset_display(inst);

    sfizz_all_sound_off(inst->synth);

    inst->pending_load = 1;
    inst->debounce_remaining = DEBOUNCE_BLOCKS;
}

/* Switch and load immediately (for startup / state restore). */
static void set_preset_index_immediate(sfz_instance_t *inst, int index) {
    if (inst->preset_count <= 0) return;
    if (index < 0) index = inst->preset_count - 1;
    if (index >= inst->preset_count) index = 0;

    inst->current_preset = index;
    sync_preset_display(inst);

    sfizz_all_sound_off(inst->synth);
    inst->pending_load = 0;
    inst->debounce_remaining = 0;
    do_load_preset(inst);
}

/* Jump to the first preset of an instrument (used by the bank/jump menu). */
static void jump_to_instrument(sfz_instance_t *inst, int instrument_idx) {
    if (instrument_idx < 0 || instrument_idx >= inst->instrument_count) return;
    int first = inst->instruments[instrument_idx].first_preset;
    if (first < 0) return;
    set_preset_index(inst, first);
}

/* V2 API Implementation */

static void* v2_create_instance(const char *module_dir, const char *json_defaults) {
    char msg[256];
    snprintf(msg, sizeof(msg), "Creating instance from: %s", module_dir);
    plugin_log(msg);

    sfz_instance_t *inst = calloc(1, sizeof(sfz_instance_t));
    if (!inst) return NULL;

    strncpy(inst->module_dir, module_dir, sizeof(inst->module_dir) - 1);
    strcpy(inst->preset_name, "No preset");
    inst->instrument_name[0] = '\0';
    inst->load_error[0] = '\0';
    inst->gain = 1.0f;

    /* Allocate render buffers */
    inst->left_buf = calloc(MOVE_FRAMES_PER_BLOCK, sizeof(float));
    inst->right_buf = calloc(MOVE_FRAMES_PER_BLOCK, sizeof(float));
    if (!inst->left_buf || !inst->right_buf) {
        plugin_log("Failed to allocate render buffers");
        free(inst->left_buf);
        free(inst->right_buf);
        free(inst);
        return NULL;
    }

    /* Create sfizz synth */
    inst->synth = sfizz_create_synth();
    if (!inst->synth) {
        plugin_log("Failed to create sfizz synth");
        free(inst->left_buf);
        free(inst->right_buf);
        free(inst);
        return NULL;
    }

    /* Configure synth */
    int sample_rate = g_host ? g_host->sample_rate : MOVE_SAMPLE_RATE;
    sfizz_set_sample_rate(inst->synth, (float)sample_rate);
    sfizz_set_samples_per_block(inst->synth, MOVE_FRAMES_PER_BLOCK);
    sfizz_set_num_voices(inst->synth, 24);
    sfizz_set_preload_size(inst->synth, 131072); /* 128K samples - covers large offsets */
    sfizz_set_volume(inst->synth, inst->gain);

    snprintf(msg, sizeof(msg), "sfizz initialized: sample_rate=%d, block=%d",
             sample_rate, MOVE_FRAMES_PER_BLOCK);
    plugin_log(msg);

    /* Scan instruments */
    scan_instruments(inst, module_dir);

    /* Restore preset selection from defaults. Prefer name-based lookup so the
     * same patch survives reorderings; fall back to index. */
    int start_preset = 0;
    if (json_defaults) {
        float f;
        char name[MAX_NAME_LEN];
        if (json_get_string(json_defaults, "preset_name", name, sizeof(name)) > 0) {
            int idx = find_preset_by_name(inst, name);
            if (idx >= 0) start_preset = idx;
        } else if (json_get_string(json_defaults, "instrument_name", name,
                                   sizeof(name)) > 0) {
            int ii = find_instrument_by_name(inst, name);
            if (ii >= 0 && inst->instruments[ii].first_preset >= 0)
                start_preset = inst->instruments[ii].first_preset;
        }
        if (json_get_number(json_defaults, "preset", &f) == 0) {
            int idx = (int)f;
            if (idx >= 0 && idx < inst->preset_count) start_preset = idx;
        }
    }

    if (inst->preset_count > 0) {
        set_preset_index_immediate(inst, start_preset);
    }

    plugin_log("Instance created");
    return inst;
}

static void v2_destroy_instance(void *instance) {
    sfz_instance_t *inst = (sfz_instance_t *)instance;
    if (!inst) return;

    plugin_log("Instance destroying");

    if (inst->synth) {
        sfizz_free(inst->synth);
        inst->synth = NULL;
    }

    free(inst->left_buf);
    free(inst->right_buf);
    free(inst);
}

static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    sfz_instance_t *inst = (sfz_instance_t *)instance;
    if (!inst || !inst->synth || len < 2) return;
    (void)source;

    uint8_t status = msg[0] & 0xF0;
    uint8_t data1 = msg[1];
    uint8_t data2 = (len > 2) ? msg[2] : 0;

    int is_note = (status == 0x90 || status == 0x80);
    int note = data1;
    if (is_note) {
        note += inst->octave_transpose * 12;
        if (note < 0) note = 0;
        if (note > 127) note = 127;
    }

    switch (status) {
        case 0x90:  /* Note on */
            if (data2 > 0) {
                sfizz_send_note_on(inst->synth, 0, note, data2);
            } else {
                sfizz_send_note_off(inst->synth, 0, note, 0);
            }
            break;
        case 0x80:  /* Note off */
            sfizz_send_note_off(inst->synth, 0, note, data2);
            break;
        case 0xB0:  /* Control change */
            if (data1 == 123) {  /* All notes off */
                sfizz_all_sound_off(inst->synth);
            } else {
                sfizz_send_cc(inst->synth, 0, data1, data2);
                if (data1 == 64 || data1 == 1) {  /* Log sustain/mod */
                    char cc_msg[64];
                    snprintf(cc_msg, sizeof(cc_msg), "CC%d = %d", data1, data2);
                    plugin_log(cc_msg);
                }
            }
            break;
        case 0xE0:  /* Pitch bend */
            {
                int bend = (((int)data2 << 7) | data1) - 8192;
                sfizz_send_pitch_wheel(inst->synth, 0, bend);
            }
            break;
        case 0xC0:  /* Program change - map to flat preset list */
            if (data1 < inst->preset_count) {
                set_preset_index(inst, data1);
            }
            break;
        case 0xD0:  /* Channel pressure (aftertouch) */
            sfizz_send_channel_aftertouch(inst->synth, 0, data1);
            break;
    }
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    sfz_instance_t *inst = (sfz_instance_t *)instance;
    if (!inst) return;

    if (strcmp(key, "preset") == 0) {
        /* Shared sound_generator UI calls set_param('preset', '0') right after
         * a bank change. Suppress that specific follow-up so the bank jump
         * isn't immediately overwritten by preset 0. */
        if (inst->suppress_next_preset_set) {
            inst->suppress_next_preset_set = 0;
            return;
        }
        int idx = atoi(val);
        if (idx == inst->current_preset) return;
        set_preset_index(inst, idx);
    } else if (strcmp(key, "bank_index") == 0 || strcmp(key, "bank") == 0 ||
               strcmp(key, "instrument_index") == 0) {
        int idx = atoi(val);
        int current_bank = -1;
        if (inst->current_preset >= 0 && inst->current_preset < inst->preset_count)
            current_bank = inst->presets[inst->current_preset].instrument_idx;
        if (idx == current_bank) {
            /* Already on this bank — but the shared UI will still send the
             * follow-up preset=0 reset, so suppress it. */
            inst->suppress_next_preset_set = 1;
            return;
        }
        jump_to_instrument(inst, idx);
        inst->suppress_next_preset_set = 1;
    } else if (strcmp(key, "octave_transpose") == 0) {
        inst->octave_transpose = atoi(val);
        if (inst->octave_transpose < -4) inst->octave_transpose = -4;
        if (inst->octave_transpose > 4) inst->octave_transpose = 4;
    } else if (strcmp(key, "gain") == 0) {
        inst->gain = atof(val);
        if (inst->gain < 0.0f) inst->gain = 0.0f;
        if (inst->gain > 2.0f) inst->gain = 2.0f;
        if (inst->synth) {
            sfizz_set_volume(inst->synth, inst->gain);
        }
    } else if (strcmp(key, "all_notes_off") == 0 || strcmp(key, "panic") == 0) {
        if (inst->synth) sfizz_all_sound_off(inst->synth);
    } else if (strcmp(key, "state") == 0) {
        /* Restore state by preset name first, then fall back to index. */
        float f;
        char name[MAX_NAME_LEN];
        int idx = -1;
        if (json_get_string(val, "preset_name", name, sizeof(name)) > 0) {
            idx = find_preset_by_name(inst, name);
        }
        if (idx < 0 && json_get_string(val, "instrument_name", name,
                                        sizeof(name)) > 0) {
            int ii = find_instrument_by_name(inst, name);
            if (ii >= 0) idx = inst->instruments[ii].first_preset;
        }
        if (idx < 0 && json_get_number(val, "preset", &f) == 0) {
            int p = (int)f;
            if (p >= 0 && p < inst->preset_count) idx = p;
        }
        if (idx >= 0) set_preset_index_immediate(inst, idx);

        if (json_get_number(val, "octave_transpose", &f) == 0) {
            inst->octave_transpose = (int)f;
            if (inst->octave_transpose < -4) inst->octave_transpose = -4;
            if (inst->octave_transpose > 4) inst->octave_transpose = 4;
        }
        if (json_get_number(val, "gain", &f) == 0) {
            inst->gain = f;
            if (inst->gain < 0.0f) inst->gain = 0.0f;
            if (inst->gain > 2.0f) inst->gain = 2.0f;
            if (inst->synth) sfizz_set_volume(inst->synth, inst->gain);
        }
    }
}

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    sfz_instance_t *inst = (sfz_instance_t *)instance;
    if (!inst) return -1;

    if (strcmp(key, "load_error") == 0) {
        if (inst->load_error[0]) {
            return snprintf(buf, buf_len, "%s", inst->load_error);
        }
        return 0;
    }
    /* Flat preset list — scrolling crosses every preset in alphabetical
     * (instrument, then preset) order. */
    else if (strcmp(key, "preset") == 0 || strcmp(key, "current_patch") == 0) {
        return snprintf(buf, buf_len, "%d", inst->current_preset);
    } else if (strcmp(key, "preset_count") == 0 || strcmp(key, "total_patches") == 0) {
        return snprintf(buf, buf_len, "%d", inst->preset_count);
    } else if (strcmp(key, "preset_name") == 0 || strcmp(key, "patch_name") == 0 ||
               strcmp(key, "name") == 0) {
        strncpy(buf, inst->preset_name, buf_len - 1);
        buf[buf_len - 1] = '\0';
        return strlen(buf);
    }
    /* Bank = instrument (jump menu). bank_count > 1 enables Shift+L/R to
     * jump between instruments via set_param('bank_index', N). */
    else if (strcmp(key, "bank_name") == 0 || strcmp(key, "instrument_name") == 0) {
        strncpy(buf, inst->instrument_name, buf_len - 1);
        buf[buf_len - 1] = '\0';
        return strlen(buf);
    } else if (strcmp(key, "bank_count") == 0 || strcmp(key, "instrument_count") == 0) {
        return snprintf(buf, buf_len, "%d", inst->instrument_count);
    } else if (strcmp(key, "bank_index") == 0 || strcmp(key, "instrument_index") == 0) {
        int ii = 0;
        if (inst->current_preset >= 0 && inst->current_preset < inst->preset_count)
            ii = inst->presets[inst->current_preset].instrument_idx;
        return snprintf(buf, buf_len, "%d", ii);
    } else if (strcmp(key, "patch_in_bank") == 0) {
        int ii = 0, first = 0;
        if (inst->current_preset >= 0 && inst->current_preset < inst->preset_count) {
            ii = inst->presets[inst->current_preset].instrument_idx;
            first = inst->instruments[ii].first_preset;
        }
        return snprintf(buf, buf_len, "%d", inst->current_preset - first + 1);
    }
    /* Knob params */
    else if (strcmp(key, "octave_transpose") == 0) {
        return snprintf(buf, buf_len, "%d", inst->octave_transpose);
    } else if (strcmp(key, "gain") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->gain);
    }
    /* Flat preset list — every preset across all instruments. Each entry's
     * label is "Instrument / Preset" so the menu reads in context. */
    else if (strcmp(key, "preset_list") == 0) {
        int written = 0;
        written += snprintf(buf + written, buf_len - written, "[");
        for (int i = 0; i < inst->preset_count && written < buf_len - 80; i++) {
            if (i > 0) written += snprintf(buf + written, buf_len - written, ",");
            preset_entry_t *p = &inst->presets[i];
            const char *iname = (p->instrument_idx >= 0 &&
                                 p->instrument_idx < inst->instrument_count)
                                ? inst->instruments[p->instrument_idx].name : "";
            written += snprintf(buf + written, buf_len - written,
                "{\"label\":\"%s / %s\",\"index\":%d}", iname, p->name, i);
        }
        written += snprintf(buf + written, buf_len - written, "]");
        return written;
    }
    /* Instrument list (jump menu) — rescan each time so newly uploaded
     * libraries appear without a restart. */
    else if (strcmp(key, "instrument_list") == 0 || strcmp(key, "bank_list") == 0 ||
             strcmp(key, "soundfont_list") == 0) {
        char saved_preset_name[MAX_NAME_LEN];
        strncpy(saved_preset_name, inst->preset_name, MAX_NAME_LEN - 1);
        saved_preset_name[MAX_NAME_LEN - 1] = '\0';

        scan_instruments(inst, inst->module_dir);

        /* Restore current_preset by name if possible. */
        int restored = find_preset_by_name(inst, saved_preset_name);
        if (restored >= 0) inst->current_preset = restored;
        else if (inst->current_preset >= inst->preset_count)
            inst->current_preset = inst->preset_count > 0 ? 0 : -1;
        sync_preset_display(inst);

        int written = 0;
        written += snprintf(buf + written, buf_len - written, "[");
        for (int i = 0; i < inst->instrument_count && written < buf_len - 50; i++) {
            if (i > 0) written += snprintf(buf + written, buf_len - written, ",");
            written += snprintf(buf + written, buf_len - written,
                "{\"label\":\"%s\",\"index\":%d}",
                inst->instruments[i].name, i);
        }
        written += snprintf(buf + written, buf_len - written, "]");
        return written;
    }
    /* State serialization */
    else if (strcmp(key, "state") == 0) {
        return snprintf(buf, buf_len,
            "{\"preset_name\":\"%s\",\"instrument_name\":\"%s\","
            "\"preset\":%d,\"octave_transpose\":%d,\"gain\":%.2f}",
            inst->preset_name, inst->instrument_name,
            inst->current_preset, inst->octave_transpose, inst->gain);
    }
    /* UI hierarchy for shadow parameter editor */
    else if (strcmp(key, "ui_hierarchy") == 0) {
        const char *hierarchy = "{"
            "\"modes\":null,"
            "\"levels\":{"
                "\"root\":{"
                    "\"label\":\"SFZ\","
                    "\"list_param\":\"preset\","
                    "\"count_param\":\"preset_count\","
                    "\"name_param\":\"preset_name\","
                    "\"children\":null,"
                    "\"knobs\":[\"octave_transpose\",\"gain\"],"
                    "\"params\":["
                        "{\"key\":\"octave_transpose\",\"label\":\"Octave\"},"
                        "{\"key\":\"gain\",\"label\":\"Gain\"},"
                        "{\"level\":\"jump\",\"label\":\"Jump To Library\"}"
                    "]"
                "},"
                "\"jump\":{"
                    "\"label\":\"Library\","
                    "\"items_param\":\"instrument_list\","
                    "\"select_param\":\"bank_index\","
                    "\"children\":null,"
                    "\"knobs\":[],"
                    "\"params\":[]"
                "}"
            "}"
        "}";
        int len = strlen(hierarchy);
        if (len < buf_len) {
            strcpy(buf, hierarchy);
            return len;
        }
        return -1;
    }

    return -1;
}

static int v2_get_error(void *instance, char *buf, int buf_len) {
    sfz_instance_t *inst = (sfz_instance_t *)instance;
    if (!inst || !inst->load_error[0]) return 0;

    int len = strlen(inst->load_error);
    if (len >= buf_len) len = buf_len - 1;
    memcpy(buf, inst->load_error, len);
    buf[len] = '\0';
    return len;
}

static void v2_render_block(void *instance, int16_t *out_interleaved_lr, int frames) {
    sfz_instance_t *inst = (sfz_instance_t *)instance;
    if (!inst || !inst->synth) {
        memset(out_interleaved_lr, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    /* Handle debounced preset loading */
    if (inst->pending_load) {
        if (inst->debounce_remaining > 0) {
            inst->debounce_remaining--;
            /* Output silence while waiting */
            memset(out_interleaved_lr, 0, frames * 2 * sizeof(int16_t));
            return;
        }
        inst->pending_load = 0;
        do_load_preset(inst);
    }

    /* sfizz renders to separate float channel buffers */
    float *channels[2] = { inst->left_buf, inst->right_buf };
    sfizz_render_block(inst->synth, channels, 2, frames);

    /* Interleave and convert to int16 */
    for (int i = 0; i < frames; i++) {
        float left = inst->left_buf[i];
        float right = inst->right_buf[i];

        if (left > 1.0f) left = 1.0f;
        if (left < -1.0f) left = -1.0f;
        if (right > 1.0f) right = 1.0f;
        if (right < -1.0f) right = -1.0f;

        out_interleaved_lr[i * 2] = (int16_t)(left * 32767.0f);
        out_interleaved_lr[i * 2 + 1] = (int16_t)(right * 32767.0f);
    }
}

/* V2 API struct */
static plugin_api_v2_t g_plugin_api_v2 = {
    .api_version = MOVE_PLUGIN_API_VERSION_2,
    .create_instance = v2_create_instance,
    .destroy_instance = v2_destroy_instance,
    .on_midi = v2_on_midi,
    .set_param = v2_set_param,
    .get_param = v2_get_param,
    .get_error = v2_get_error,
    .render_block = v2_render_block
};

/* V2 Entry Point */
plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    plugin_log("V2 API initialized (sfizz)");
    return &g_plugin_api_v2;
}
