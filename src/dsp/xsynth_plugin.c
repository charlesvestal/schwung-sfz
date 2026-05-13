/*
 * xsynth-backed SFZ player DSP plugin (Phase 1 baseline).
 *
 * Same V2 plugin API surface as sfz_plugin.c, but the engine underneath is
 * xsynth (per-key parallel, SIMD voice render) instead of sfizz. Trade-off:
 * SFZ subset is narrower — no `_oncc`, no LFOs, no `lpf_2p`/`fverb`/etc. —
 * so only native `.sfz` files are scanned in Phase 1. DS conversion will
 * land in Phase 2 with a converter rewrite that emits xsynth-compatible SFZ.
 *
 * Performance target: 60+ active voices on layered patches without glitching
 * on the Move SPI audio path. Benchmarks (Stereo Rhodes) show ~3x speedup
 * vs sfizz at n=64. See docs/plans/2026-05-12-xsynth-port-design.md.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <sched.h>
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

#include "dspreset_to_xsynth_sfz.h"

/* xsynth_shim C ABI */
typedef struct XSynthHandle XSynthHandle;
extern XSynthHandle* xshim_create(uint32_t sample_rate, uint32_t channels);
extern void          xshim_destroy(XSynthHandle*);
extern int           xshim_load_sfz(XSynthHandle*, const char *path);
extern void          xshim_note_on(XSynthHandle*, uint8_t ch, uint8_t key, uint8_t vel);
extern void          xshim_note_off(XSynthHandle*, uint8_t ch, uint8_t key);
extern void          xshim_cc(XSynthHandle*, uint8_t ch, uint8_t cc, uint8_t val);
extern void          xshim_pitch_bend(XSynthHandle*, uint8_t ch, float value);
extern void          xshim_all_notes_off(XSynthHandle*);
extern void          xshim_render(XSynthHandle*, float *out_interleaved, size_t num_samples);
extern uint64_t      xshim_voice_count(const XSynthHandle*);
extern void          xshim_set_layer_count(XSynthHandle*, uint32_t layers);
extern void          xshim_set_polyphony_cap(XSynthHandle*, uint32_t cap);
extern void          xshim_set_spawn_burst_limit(XSynthHandle*, uint32_t limit);
extern uint32_t      xshim_take_noteon_count(XSynthHandle*);
extern void          xshim_take_render_breakdown(XSynthHandle*, uint32_t *out4);
extern size_t        xshim_last_error(char *out_buf, size_t out_len);
extern int           xshim_load_sfz_async(XSynthHandle*, const char *path);
extern int           xshim_load_status(const XSynthHandle*);
extern void          xshim_load_cancel(XSynthHandle*);
extern int           xshim_load_apply(XSynthHandle*);
extern void          xshim_load_clear_status(XSynthHandle*);
#define XSHIM_LOAD_IDLE      0
#define XSHIM_LOAD_LOADING   1
#define XSHIM_LOAD_READY     2
#define XSHIM_LOAD_ERROR     3
#define XSHIM_LOAD_CANCELLED 4

static const host_api_v1_t *g_host = NULL;

#define MAX_INSTRUMENTS 512
#define MAX_PRESETS 1024
#define MAX_PATH_LEN 512
#define MAX_NAME_LEN 128
#define DEBOUNCE_BLOCKS 56

typedef struct {
    char name[MAX_NAME_LEN];
    char root[MAX_PATH_LEN];
    int first_preset;
    int preset_count;
} instrument_entry_t;

typedef struct {
    char path[MAX_PATH_LEN];
    char name[MAX_NAME_LEN];
    int instrument_idx;
} preset_entry_t;

typedef struct {
    XSynthHandle *synth;
    int current_preset;
    int instrument_count;
    int preset_count;
    int octave_transpose;
    int voices;                    /* Polyphony cap (one unit per note-on).
                                    * Wired to xshim_set_polyphony_cap. */
    int spawn_burst;               /* Max NoteOn events drained per render
                                    * block. Caps voice-spawn cost during
                                    * quantized chord bursts; surplus events
                                    * defer to next block (~3 ms each). */
    float gain;
    instrument_entry_t instruments[MAX_INSTRUMENTS];
    preset_entry_t presets[MAX_PRESETS];
    char preset_name[MAX_NAME_LEN];
    char instrument_name[MAX_NAME_LEN];
    char module_dir[MAX_PATH_LEN];
    char load_error[256];
    int debounce_remaining;
    int pending_load;
    int suppress_next_preset_set;
    float *render_buf;             /* interleaved L/R/L/R..., 2 * frames */
} xsynth_instance_t;

static void plugin_log(const char *msg) {
    if (g_host && g_host->log) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[sfz] %s", msg);
        g_host->log(buf);
    }
}

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

/* Phase 2: .sfz native + .dspreset via dspreset_to_xsynth_sfz converter. */
static int is_supported_instrument(const char *ext) {
    return strcasecmp(ext, ".sfz") == 0 || strcasecmp(ext, ".dspreset") == 0;
}

/* Skip the converter's own temp files (.converted.sfz) so they don't show up
 * as variants alongside the source .dspreset. */
static int is_temp_converted_sfz(const char *filename) {
    const char *suffix = ".converted.sfz";
    size_t flen = strlen(filename), slen = strlen(suffix);
    return flen >= slen && strcasecmp(filename + flen - slen, suffix) == 0;
}

#define SCAN_MAX_DEPTH 5

static int preset_entry_cmp(const void *a, const void *b) {
    const preset_entry_t *pa = a;
    const preset_entry_t *pb = b;
    return strcasecmp(pa->name, pb->name);
}

static void add_preset(xsynth_instance_t *inst, const char *dir_path,
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

static void collect_presets(xsynth_instance_t *inst, const char *path,
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

static void scan_instruments(xsynth_instance_t *inst, const char *module_dir) {
    char dir_path[MAX_PATH_LEN];
    snprintf(dir_path, sizeof(dir_path), "%s/instruments", module_dir);
    inst->instrument_count = 0;
    inst->preset_count = 0;
    DIR *dir = opendir(dir_path);
    if (!dir) { plugin_log("No instruments/ directory found"); return; }

    typedef struct {
        char name[MAX_NAME_LEN];
        char full_path[MAX_PATH_LEN];
        int is_dir;
        char filename[MAX_NAME_LEN];
    } scan_entry_t;
    static scan_entry_t entries[MAX_INSTRUMENTS];
    int entry_count = 0;

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (entry_count >= MAX_INSTRUMENTS) { plugin_log("Instrument list full"); break; }
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

    for (int i = 0; i < entry_count - 1; i++)
        for (int j = 0; j < entry_count - 1 - i; j++)
            if (strcasecmp(entries[j].name, entries[j + 1].name) > 0) {
                scan_entry_t t = entries[j]; entries[j] = entries[j + 1]; entries[j + 1] = t;
            }

    for (int i = 0; i < entry_count; i++) {
        if (inst->instrument_count >= MAX_INSTRUMENTS) break;
        scan_entry_t *e = &entries[i];
        int idx = inst->instrument_count;
        instrument_entry_t *ins = &inst->instruments[idx];
        strncpy(ins->name, e->name, MAX_NAME_LEN - 1);
        ins->name[MAX_NAME_LEN - 1] = '\0';
        strncpy(ins->root, e->full_path, MAX_PATH_LEN - 1);
        ins->root[MAX_PATH_LEN - 1] = '\0';
        ins->first_preset = inst->preset_count;
        int presets_before = inst->preset_count;
        if (e->is_dir) {
            collect_presets(inst, e->full_path, idx, 0);
            int n = inst->preset_count - presets_before;
            if (n > 1)
                qsort(&inst->presets[presets_before], n, sizeof(preset_entry_t), preset_entry_cmp);
        } else {
            add_preset(inst, e->full_path, e->filename, idx);
        }
        ins->preset_count = inst->preset_count - presets_before;
        if (ins->preset_count == 0) { ins->first_preset = -1; continue; }
        inst->instrument_count++;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "Found %d instruments, %d presets total",
             inst->instrument_count, inst->preset_count);
    plugin_log(msg);
}

static int find_preset_by_name(xsynth_instance_t *inst, const char *name) {
    for (int i = 0; i < inst->preset_count; i++)
        if (strcmp(inst->presets[i].name, name) == 0) return i;
    return -1;
}

static int find_instrument_by_name(xsynth_instance_t *inst, const char *name) {
    for (int i = 0; i < inst->instrument_count; i++)
        if (strcmp(inst->instruments[i].name, name) == 0) return i;
    return -1;
}

static void sync_preset_display(xsynth_instance_t *inst) {
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

static int load_sfz_file(xsynth_instance_t *inst, const char *path) {
    char msg[512];
    snprintf(msg, sizeof(msg), "Loading: %s", path);
    plugin_log(msg);
    struct stat st;
    if (stat(path, &st) != 0) {
        snprintf(inst->load_error, sizeof(inst->load_error), "Instrument file not found");
        return -1;
    }

    /* .dspreset → convert to .converted.sfz next to source, load that. */
    const char *ext = strrchr(path, '.');
    const char *load_path = path;
    char *converted = NULL;
    if (ext && strcasecmp(ext, ".dspreset") == 0) {
        converted = convert_dspreset_to_xsynth_sfz(path);
        if (!converted) {
            plugin_log("DS converter returned NULL");
            snprintf(inst->load_error, sizeof(inst->load_error),
                     "DecentSampler conversion failed");
            return -1;
        }
        snprintf(msg, sizeof(msg), "Converted dspreset → %s", converted);
        plugin_log(msg);
        load_path = converted;
    }

    int rc = xshim_load_sfz_async(inst->synth, load_path);
    free(converted);
    if (rc != 0) {
        char err[256] = {0};
        xshim_last_error(err, sizeof(err));
        snprintf(inst->load_error, sizeof(inst->load_error),
                 "xsynth async load failed: %s", err[0] ? err : "unknown");
        return -1;
    }
    inst->load_error[0] = '\0';
    return 0;
}

static void do_load_preset(xsynth_instance_t *inst) {
    int idx = inst->current_preset;
    if (idx < 0 || idx >= inst->preset_count) return;
    preset_entry_t *p = &inst->presets[idx];
    load_sfz_file(inst, p->path);
    char msg[128];
    snprintf(msg, sizeof(msg), "Preset %d: %s [%s]",
             idx, inst->preset_name, inst->instrument_name);
    plugin_log(msg);
}

static void set_preset_index(xsynth_instance_t *inst, int index) {
    if (inst->preset_count <= 0) return;
    if (index < 0) index = inst->preset_count - 1;
    if (index >= inst->preset_count) index = 0;
    inst->current_preset = index;
    sync_preset_display(inst);
    xshim_all_notes_off(inst->synth);
    xshim_load_cancel(inst->synth);
    inst->pending_load = 1;
    inst->debounce_remaining = DEBOUNCE_BLOCKS;
}

static void set_preset_index_immediate(xsynth_instance_t *inst, int index) {
    if (inst->preset_count <= 0) return;
    if (index < 0) index = inst->preset_count - 1;
    if (index >= inst->preset_count) index = 0;
    inst->current_preset = index;
    sync_preset_display(inst);
    xshim_all_notes_off(inst->synth);
    inst->pending_load = 0;
    inst->debounce_remaining = 0;
    do_load_preset(inst);
}

static void jump_to_instrument(xsynth_instance_t *inst, int instrument_idx) {
    if (instrument_idx < 0 || instrument_idx >= inst->instrument_count) return;
    int first = inst->instruments[instrument_idx].first_preset;
    if (first < 0) return;
    set_preset_index(inst, first);
}

/* --- V2 API --- */

static void* v2_create_instance(const char *module_dir, const char *json_defaults) {
    char msg[256];
    snprintf(msg, sizeof(msg), "Creating instance from: %s", module_dir);
    plugin_log(msg);

    xsynth_instance_t *inst = calloc(1, sizeof(xsynth_instance_t));
    if (!inst) return NULL;
    strncpy(inst->module_dir, module_dir, sizeof(inst->module_dir) - 1);
    strcpy(inst->preset_name, "No preset");
    inst->gain = 1.0f;
    inst->voices = 14;     /* polyphony cap (note-groups). 14 polyphony
                            * × WörliTzer's 5 voices/note ≈ 70 voices,
                            * which is the safe ceiling at current
                            * per-voice render cost. */
    inst->spawn_burst = 3;  /* hidden — defaults to 3 NoteOn/block.
                             * Quantized chord bursts spread spawn cost
                             * across blocks; 3 keeps burst-block render
                             * cost bounded. */

    inst->render_buf = calloc(MOVE_FRAMES_PER_BLOCK * 2, sizeof(float));
    if (!inst->render_buf) { free(inst); return NULL; }

    int sample_rate = g_host ? g_host->sample_rate : MOVE_SAMPLE_RATE;
    inst->synth = xshim_create((uint32_t)sample_rate, 2);
    if (!inst->synth) {
        plugin_log("xshim_create failed");
        free(inst->render_buf); free(inst); return NULL;
    }

    snprintf(msg, sizeof(msg), "xsynth initialized: sample_rate=%d, block=%d",
             sample_rate, MOVE_FRAMES_PER_BLOCK);
    plugin_log(msg);

    scan_instruments(inst, module_dir);

    int start_preset = 0, found = 0;
    if (json_defaults) {
        float f;
        char name[MAX_NAME_LEN];
        if (!found && json_get_string(json_defaults, "preset_name", name, sizeof(name)) > 0) {
            int idx = find_preset_by_name(inst, name);
            if (idx >= 0) { start_preset = idx; found = 1; }
        }
        if (!found && json_get_string(json_defaults, "instrument_name", name, sizeof(name)) > 0) {
            int ii = find_instrument_by_name(inst, name);
            if (ii >= 0 && inst->instruments[ii].first_preset >= 0) {
                start_preset = inst->instruments[ii].first_preset; found = 1;
            }
        }
        if (!found && json_get_number(json_defaults, "preset", &f) == 0) {
            int idx = (int)f;
            if (idx >= 0 && idx < inst->preset_count) { start_preset = idx; found = 1; }
        }
    }
    if (inst->preset_count > 0)
        set_preset_index_immediate(inst, start_preset);

    if (json_defaults) {
        float f;
        if (json_get_number(json_defaults, "octave_transpose", &f) == 0) {
            inst->octave_transpose = (int)f;
            if (inst->octave_transpose < -4) inst->octave_transpose = -4;
            if (inst->octave_transpose >  4) inst->octave_transpose =  4;
        }
        if (json_get_number(json_defaults, "gain", &f) == 0) {
            inst->gain = f;
            if (inst->gain < 0.0f) inst->gain = 0.0f;
            if (inst->gain > 2.0f) inst->gain = 2.0f;
        }
        if (json_get_number(json_defaults, "voices", &f) == 0) {
            int v = (int)f;
            if (v < 4)   v = 4;
            if (v > 128) v = 128;
            inst->voices = v;
        }
    }

    /* Polyphony cap applies at the channel level, dropping oldest releasing
     * groups when the user-set N polyphony budget is exceeded. We push the
     * default (typically 64 → ~13 notes for a 5-layer WörliTzer, plenty for
     * a Bass patch) so even tracks that never touch the knob get a bounded
     * worst-case render time. */
    if (inst->synth) {
        xshim_set_polyphony_cap(inst->synth, (uint32_t)inst->voices);
        xshim_set_spawn_burst_limit(inst->synth, (uint32_t)inst->spawn_burst);
    }

    plugin_log("Instance created");
    return inst;
}

static void v2_destroy_instance(void *instance) {
    xsynth_instance_t *inst = instance;
    if (!inst) return;
    plugin_log("Instance destroying");
    if (inst->synth) { xshim_destroy(inst->synth); inst->synth = NULL; }
    free(inst->render_buf);
    free(inst);
}

static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    xsynth_instance_t *inst = instance;
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
    case 0x90:
        if (data2 > 0) xshim_note_on(inst->synth, 0, (uint8_t)note, data2);
        else            xshim_note_off(inst->synth, 0, (uint8_t)note);
        break;
    case 0x80:
        xshim_note_off(inst->synth, 0, (uint8_t)note);
        break;
    case 0xB0:
        if (data1 == 123) xshim_all_notes_off(inst->synth);
        else              xshim_cc(inst->synth, 0, data1, data2);
        break;
    case 0xE0: {
        int bend = (((int)data2 << 7) | data1) - 8192;
        /* xsynth PitchBend takes -1..1, product of value*sensitivity. */
        xshim_pitch_bend(inst->synth, 0, (float)bend / 8192.0f);
        break;
    }
    case 0xC0:
        if (data1 < inst->preset_count) set_preset_index(inst, data1);
        break;
    /* 0xD0 channel pressure: xsynth doesn't expose aftertouch yet; ignore. */
    }
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    xsynth_instance_t *inst = instance;
    if (!inst) return;

    if (strcmp(key, "preset") == 0) {
        if (inst->suppress_next_preset_set) { inst->suppress_next_preset_set = 0; return; }
        int idx = atoi(val);
        if (idx == inst->current_preset) return;
        set_preset_index(inst, idx);
    } else if (strcmp(key, "bank_index") == 0 || strcmp(key, "bank") == 0 ||
               strcmp(key, "instrument_index") == 0) {
        int idx = atoi(val);
        int current_bank = -1;
        if (inst->current_preset >= 0 && inst->current_preset < inst->preset_count)
            current_bank = inst->presets[inst->current_preset].instrument_idx;
        if (idx == current_bank) { inst->suppress_next_preset_set = 1; return; }
        jump_to_instrument(inst, idx);
        inst->suppress_next_preset_set = 1;
    } else if (strcmp(key, "octave_transpose") == 0) {
        inst->octave_transpose = atoi(val);
        if (inst->octave_transpose < -4) inst->octave_transpose = -4;
        if (inst->octave_transpose >  4) inst->octave_transpose =  4;
    } else if (strcmp(key, "gain") == 0) {
        inst->gain = atof(val);
        if (inst->gain < 0.0f) inst->gain = 0.0f;
        if (inst->gain > 2.0f) inst->gain = 2.0f;
    } else if (strcmp(key, "voices") == 0) {
        int v = atoi(val);
        if (v < 4)   v = 4;
        if (v > 128) v = 128;
        inst->voices = v;
        /* User-facing "Voices" knob = polyphony cap (one unit per note),
         * not per-key layer cap. Multi-layer presets like WörliTzer spend
         * 5 internal voices per polyphony unit; the cap is musically
         * intuitive regardless. */
        if (inst->synth) xshim_set_polyphony_cap(inst->synth, (uint32_t)v);
    } else if (strcmp(key, "spawn_burst") == 0) {
        int v = atoi(val);
        if (v < 1)  v = 1;
        if (v > 64) v = 64;
        inst->spawn_burst = v;
        if (inst->synth) xshim_set_spawn_burst_limit(inst->synth, (uint32_t)v);
    } else if (strcmp(key, "all_notes_off") == 0 || strcmp(key, "panic") == 0) {
        if (inst->synth) xshim_all_notes_off(inst->synth);
    } else if (strcmp(key, "state") == 0) {
        float f;
        char name[MAX_NAME_LEN];
        int idx = -1;
        if (json_get_string(val, "preset_name", name, sizeof(name)) > 0)
            idx = find_preset_by_name(inst, name);
        if (idx < 0 && json_get_string(val, "instrument_name", name, sizeof(name)) > 0) {
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
            if (inst->octave_transpose >  4) inst->octave_transpose =  4;
        }
        if (json_get_number(val, "gain", &f) == 0) {
            inst->gain = f;
            if (inst->gain < 0.0f) inst->gain = 0.0f;
            if (inst->gain > 2.0f) inst->gain = 2.0f;
        }
        if (json_get_number(val, "voices", &f) == 0) {
            int v = (int)f;
            if (v < 4)   v = 4;
            if (v > 128) v = 128;
            inst->voices = v;
            if (inst->synth) xshim_set_polyphony_cap(inst->synth, (uint32_t)v);
        }
        if (json_get_number(val, "spawn_burst", &f) == 0) {
            int v = (int)f;
            if (v < 1)  v = 1;
            if (v > 64) v = 64;
            inst->spawn_burst = v;
            if (inst->synth) xshim_set_spawn_burst_limit(inst->synth, (uint32_t)v);
        }
    }
}

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    xsynth_instance_t *inst = instance;
    if (!inst) return -1;

    if (strcmp(key, "load_error") == 0) {
        if (inst->load_error[0]) return snprintf(buf, buf_len, "%s", inst->load_error);
        return 0;
    }
    else if (strcmp(key, "preset") == 0 || strcmp(key, "current_patch") == 0)
        return snprintf(buf, buf_len, "%d", inst->current_preset);
    else if (strcmp(key, "preset_count") == 0 || strcmp(key, "total_patches") == 0)
        return snprintf(buf, buf_len, "%d", inst->preset_count);
    else if (strcmp(key, "preset_name") == 0 || strcmp(key, "patch_name") == 0) {
        strncpy(buf, inst->preset_name, buf_len - 1);
        buf[buf_len - 1] = '\0';
        return strlen(buf);
    }
    else if (strcmp(key, "name") == 0)
        return snprintf(buf, buf_len, "SFZ");
    else if (strcmp(key, "bank_name") == 0 || strcmp(key, "instrument_name") == 0) {
        strncpy(buf, inst->instrument_name, buf_len - 1);
        buf[buf_len - 1] = '\0';
        return strlen(buf);
    }
    else if (strcmp(key, "bank_count") == 0 || strcmp(key, "instrument_count") == 0)
        return snprintf(buf, buf_len, "%d", inst->instrument_count);
    else if (strcmp(key, "bank_index") == 0 || strcmp(key, "instrument_index") == 0) {
        int ii = 0;
        if (inst->current_preset >= 0 && inst->current_preset < inst->preset_count)
            ii = inst->presets[inst->current_preset].instrument_idx;
        return snprintf(buf, buf_len, "%d", ii);
    }
    else if (strcmp(key, "patch_in_bank") == 0) {
        int ii = 0, first = 0;
        if (inst->current_preset >= 0 && inst->current_preset < inst->preset_count) {
            ii = inst->presets[inst->current_preset].instrument_idx;
            first = inst->instruments[ii].first_preset;
        }
        return snprintf(buf, buf_len, "%d", inst->current_preset - first + 1);
    }
    else if (strcmp(key, "octave_transpose") == 0)
        return snprintf(buf, buf_len, "%d", inst->octave_transpose);
    else if (strcmp(key, "gain") == 0)
        return snprintf(buf, buf_len, "%.2f", inst->gain);
    else if (strcmp(key, "voices") == 0)
        return snprintf(buf, buf_len, "%d", inst->voices);
    else if (strcmp(key, "spawn_burst") == 0)
        return snprintf(buf, buf_len, "%d", inst->spawn_burst);
    else if (strcmp(key, "chain_params") == 0) {
        /* Phase 1: no DS knobs. octave/gain/voices live in the params menu. */
        return snprintf(buf, buf_len,
            "[{\"key\":\"preset\",\"name\":\"Instrument\","
             "\"type\":\"int\",\"min\":0,\"max_param\":\"preset_count\","
             "\"default\":0},"
             "{\"key\":\"octave_transpose\",\"name\":\"Octave\","
             "\"type\":\"int\",\"min\":-4,\"max\":4,\"default\":0},"
             "{\"key\":\"gain\",\"name\":\"Gain\","
             "\"type\":\"float\",\"min\":0,\"max\":2,\"default\":1.0,\"step\":0.02},"
             "{\"key\":\"voices\",\"name\":\"Polyphony\","
             "\"type\":\"int\",\"min\":4,\"max\":128,\"default\":14}]");
    }
    else if (strcmp(key, "preset_list") == 0) {
        int written = snprintf(buf, buf_len, "[");
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
    else if (strcmp(key, "instrument_list") == 0 || strcmp(key, "bank_list") == 0 ||
             strcmp(key, "soundfont_list") == 0) {
        char saved_preset_name[MAX_NAME_LEN];
        strncpy(saved_preset_name, inst->preset_name, MAX_NAME_LEN - 1);
        saved_preset_name[MAX_NAME_LEN - 1] = '\0';
        scan_instruments(inst, inst->module_dir);
        int restored = find_preset_by_name(inst, saved_preset_name);
        if (restored >= 0) inst->current_preset = restored;
        else if (inst->current_preset >= inst->preset_count)
            inst->current_preset = inst->preset_count > 0 ? 0 : -1;
        sync_preset_display(inst);
        int written = snprintf(buf, buf_len, "[");
        for (int i = 0; i < inst->instrument_count && written < buf_len - 50; i++) {
            if (i > 0) written += snprintf(buf + written, buf_len - written, ",");
            written += snprintf(buf + written, buf_len - written,
                "{\"label\":\"%s\",\"index\":%d}", inst->instruments[i].name, i);
        }
        written += snprintf(buf + written, buf_len - written, "]");
        return written;
    }
    else if (strcmp(key, "state") == 0) {
        return snprintf(buf, buf_len,
            "{\"preset_name\":\"%s\",\"instrument_name\":\"%s\","
            "\"preset\":%d,\"octave_transpose\":%d,\"gain\":%.2f,"
            "\"voices\":%d,\"spawn_burst\":%d}",
            inst->preset_name, inst->instrument_name,
            inst->current_preset, inst->octave_transpose, inst->gain,
            inst->voices, inst->spawn_burst);
    }
    else if (strcmp(key, "ui_hierarchy") == 0) {
        return snprintf(buf, buf_len,
            "{\"modes\":null,\"levels\":{\"root\":{"
            "\"label\":\"SFZ\","
            "\"list_param\":\"preset\","
            "\"count_param\":\"preset_count\","
            "\"name_param\":\"preset_name\","
            "\"children\":null,"
            "\"knobs\":[],\"params\":["
                "{\"key\":\"octave_transpose\",\"label\":\"Octave\"},"
                "{\"key\":\"gain\",\"label\":\"Gain\"},"
                "{\"key\":\"voices\",\"label\":\"Polyphony\"},"
                "{\"level\":\"jump\",\"label\":\"Jump To Library\"}"
            "]},"
            "\"jump\":{"
                "\"label\":\"Library\","
                "\"items_param\":\"instrument_list\","
                "\"select_param\":\"bank_index\","
                "\"children\":null,"
                "\"knobs\":[],\"params\":[]"
            "}}}");
    }

    return -1;
}

static int v2_get_error(void *instance, char *buf, int buf_len) {
    xsynth_instance_t *inst = instance;
    if (!inst || !inst->load_error[0]) return 0;
    int n = snprintf(buf, buf_len, "%s", inst->load_error);
    return n;
}

static void v2_render_block(void *instance, int16_t *out_interleaved_lr, int frames) {
    xsynth_instance_t *inst = instance;
    if (!inst || !inst->synth) {
        memset(out_interleaved_lr, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    if (inst->pending_load) {
        if (inst->debounce_remaining > 0) {
            inst->debounce_remaining--;
            memset(out_interleaved_lr, 0, frames * 2 * sizeof(int16_t));
            return;
        }
        inst->pending_load = 0;
        do_load_preset(inst);  /* async dispatch — returns immediately */
    }

    /* Poll the async load worker. Render silence while loading; swap in the
     * new soundfont when status flips to Ready. */
    int load_st = xshim_load_status(inst->synth);
    if (load_st == XSHIM_LOAD_LOADING) {
        memset(out_interleaved_lr, 0, frames * 2 * sizeof(int16_t));
        return;
    } else if (load_st == XSHIM_LOAD_READY) {
        xshim_load_apply(inst->synth);
        plugin_log("xsynth: async SFZ ready and applied");
    } else if (load_st == XSHIM_LOAD_ERROR) {
        char err[256] = {0};
        xshim_last_error(err, sizeof(err));
        snprintf(inst->load_error, sizeof(inst->load_error),
                 "xsynth load failed: %s", err[0] ? err : "unknown");
        xshim_load_clear_status(inst->synth);
    } else if (load_st == XSHIM_LOAD_CANCELLED) {
        xshim_load_clear_status(inst->synth);
    }

    /* One-shot CPU/scheduler setup. Matches what sfz_plugin.c does — pin to
     * a core and request SCHED_FIFO. Best-effort; SCHED_FIFO needs
     * CAP_SYS_NICE which we don't have, so it'll usually fail silently. */
    static int rt_setup_done = 0;
    if (!rt_setup_done) {
        rt_setup_done = 1;
        cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(3, &cs);
        sched_setaffinity(0, sizeof(cs), &cs);
        struct sched_param sp = { .sched_priority = 50 };
        sched_setscheduler(0, SCHED_FIFO, &sp);
    }

    /* Snapshot NoteOn count for THIS block before render. Splits perf log
     * into spawn-heavy (sequence chord lands) vs sustain-heavy (voices ring
     * under pedal) categories. */
    uint32_t noteons_this_block = xshim_take_noteon_count(inst->synth);
    uint64_t voices_before = xshim_voice_count(inst->synth);

    /* xsynth renders interleaved f32 stereo directly into render_buf. */
    struct timespec _t0, _t1;
    clock_gettime(CLOCK_MONOTONIC, &_t0);
    xshim_render(inst->synth, inst->render_buf, (size_t)(frames * 2));
    clock_gettime(CLOCK_MONOTONIC, &_t1);
    long render_us = (_t1.tv_sec - _t0.tv_sec) * 1000000L +
                     (_t1.tv_nsec - _t0.tv_nsec) / 1000L;

    /* Log perf when there was a spawn burst, render was heavy, or every
     * 256 blocks for baseline. Breakdown: par = event drain + per-key
     * render inside rayon. sum = serial mix of per-key bufs. fx = volume
     * + pan + cutoff sweep. tot = total inside push_key_events_and_render
     * (xsynth side). The (render - tot) gap is shim/render_to wrapping
     * overhead. */
    static int log_throttle = 0;
    if (render_us > 800 || noteons_this_block > 0 || ++log_throttle >= 256) {
        log_throttle = 0;
        uint64_t voices_after = xshim_voice_count(inst->synth);
        uint32_t bd[4] = {0};
        xshim_take_render_breakdown(inst->synth, bd);
        char msg[224];
        snprintf(msg, sizeof(msg),
                 "perf: render=%ld us  vb=%llu va=%llu noteon=%u  par=%u sum=%u fx=%u tot=%u",
                 render_us,
                 (unsigned long long)voices_before,
                 (unsigned long long)voices_after,
                 (unsigned)noteons_this_block,
                 bd[0], bd[1], bd[2], bd[3]);
        plugin_log(msg);
    }

    /* Convert interleaved f32 → int16 with gain and tanh soft-clip. */
    float g = inst->gain;
    for (int i = 0; i < frames; i++) {
        float l = tanhf(inst->render_buf[i * 2]     * g);
        float r = tanhf(inst->render_buf[i * 2 + 1] * g);
        out_interleaved_lr[i * 2]     = (int16_t)(l * 32767.0f);
        out_interleaved_lr[i * 2 + 1] = (int16_t)(r * 32767.0f);
    }
}

static plugin_api_v2_t g_plugin_api_v2 = {
    .api_version = MOVE_PLUGIN_API_VERSION_2,
    .create_instance = v2_create_instance,
    .destroy_instance = v2_destroy_instance,
    .on_midi = v2_on_midi,
    .set_param = v2_set_param,
    .get_param = v2_get_param,
    .get_error = v2_get_error,
    .render_block = v2_render_block,
};

plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    plugin_log("V2 API initialized (xsynth)");
    return &g_plugin_api_v2;
}
