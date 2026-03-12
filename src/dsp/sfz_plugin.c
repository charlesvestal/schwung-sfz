/*
 * SFZ Player DSP Plugin
 *
 * Uses sfizz to render SFZ and DecentSampler (.dspreset) instruments.
 * Instruments are organized as folders under instruments/,
 * each containing one or more .sfz/.dspreset files (treated as presets).
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
#define MAX_PRESETS 256
#define MAX_PATH_LEN 512
#define MAX_NAME_LEN 128

typedef struct {
    char path[MAX_PATH_LEN];    /* Full path to instrument folder */
    char name[MAX_NAME_LEN];    /* Folder display name */
} instrument_entry_t;

typedef struct {
    char path[MAX_PATH_LEN];    /* Full path to .sfz file */
    char name[MAX_NAME_LEN];    /* Display name (filename without .sfz) */
} preset_entry_t;

/* Per-Instance State */
typedef struct {
    sfizz_synth_t *synth;
    int current_instrument;
    int current_preset;
    int instrument_count;
    int preset_count;
    int octave_transpose;
    float gain;
    instrument_entry_t instruments[MAX_INSTRUMENTS];
    preset_entry_t presets[MAX_PRESETS];
    char instrument_name[MAX_NAME_LEN];
    char preset_name[MAX_NAME_LEN];
    char module_dir[MAX_PATH_LEN];
    char load_error[256];
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

/* Check if a directory (or its immediate subdirs) contains instrument files.
 * Returns 1 if found, and sets sfz_subdir to the path containing them. */
static int dir_has_instruments(const char *dir_path, char *sfz_subdir, int sfz_subdir_len) {
    DIR *dir = opendir(dir_path);
    if (!dir) return 0;

    struct dirent *entry;
    /* First pass: check top level */
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        const char *ext = strrchr(entry->d_name, '.');
        if (ext && is_supported_instrument(ext)) {
            closedir(dir);
            snprintf(sfz_subdir, sfz_subdir_len, "%s", dir_path);
            return 1;
        }
    }
    closedir(dir);

    /* Second pass: check one level of subdirectories */
    dir = opendir(dir_path);
    if (!dir) return 0;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char sub_path[MAX_PATH_LEN];
        snprintf(sub_path, sizeof(sub_path), "%s/%s", dir_path, entry->d_name);
        struct stat st;
        if (stat(sub_path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        DIR *subdir = opendir(sub_path);
        if (!subdir) continue;
        struct dirent *sub_entry;
        while ((sub_entry = readdir(subdir)) != NULL) {
            const char *ext = strrchr(sub_entry->d_name, '.');
            if (ext && is_supported_instrument(ext)) {
                closedir(subdir);
                closedir(dir);
                snprintf(sfz_subdir, sfz_subdir_len, "%s", sub_path);
                return 1;
            }
        }
        closedir(subdir);
    }
    closedir(dir);
    return 0;
}

/* Sort helpers */
static int instrument_entry_cmp(const void *a, const void *b) {
    const instrument_entry_t *ia = (const instrument_entry_t *)a;
    const instrument_entry_t *ib = (const instrument_entry_t *)b;
    return strcasecmp(ia->name, ib->name);
}

static int preset_entry_cmp(const void *a, const void *b) {
    const preset_entry_t *pa = (const preset_entry_t *)a;
    const preset_entry_t *pb = (const preset_entry_t *)b;
    return strcasecmp(pa->name, pb->name);
}

/* Scan instruments/ directory for instrument folders */
static void scan_instruments(sfz_instance_t *inst, const char *module_dir) {
    char dir_path[MAX_PATH_LEN];
    snprintf(dir_path, sizeof(dir_path), "%s/instruments", module_dir);

    inst->instrument_count = 0;

    DIR *dir = opendir(dir_path);
    if (!dir) {
        plugin_log("No instruments/ directory found");
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        /* Check if it's a directory */
        char full_path[MAX_PATH_LEN];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        /* Check if directory (or subdirs) contains instrument files */
        char sfz_dir[MAX_PATH_LEN];
        if (!dir_has_instruments(full_path, sfz_dir, sizeof(sfz_dir))) continue;

        if (inst->instrument_count >= MAX_INSTRUMENTS) {
            plugin_log("Instrument list full, skipping extras");
            break;
        }

        instrument_entry_t *instr = &inst->instruments[inst->instrument_count++];
        /* Store the path where SFZ files actually live */
        strncpy(instr->path, sfz_dir, sizeof(instr->path) - 1);
        instr->path[sizeof(instr->path) - 1] = '\0';
        strncpy(instr->name, entry->d_name, sizeof(instr->name) - 1);
        instr->name[sizeof(instr->name) - 1] = '\0';
    }

    closedir(dir);

    if (inst->instrument_count > 1) {
        qsort(inst->instruments, inst->instrument_count,
              sizeof(instrument_entry_t), instrument_entry_cmp);
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "Found %d instruments", inst->instrument_count);
    plugin_log(msg);
}

/* Add a single preset entry from a file path */
static void add_preset_entry(sfz_instance_t *inst, const char *dir_path, const char *filename) {
    if (inst->preset_count >= MAX_PRESETS) return;

    const char *ext = strrchr(filename, '.');
    if (!ext || !is_supported_instrument(ext)) return;

    preset_entry_t *p = &inst->presets[inst->preset_count++];
    snprintf(p->path, sizeof(p->path), "%s/%s", dir_path, filename);

    /* Display name = filename without extension */
    int name_len = ext - filename;
    if (name_len >= MAX_NAME_LEN) name_len = MAX_NAME_LEN - 1;
    strncpy(p->name, filename, name_len);
    p->name[name_len] = '\0';
}

/* Scan .sfz files within an instrument folder (these become presets).
 * Also checks one level of subdirectories for instruments with nested structures. */
static void scan_presets(sfz_instance_t *inst, const char *instrument_path) {
    inst->preset_count = 0;

    DIR *dir = opendir(instrument_path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        const char *ext = strrchr(entry->d_name, '.');
        if (ext && is_supported_instrument(ext)) {
            add_preset_entry(inst, instrument_path, entry->d_name);
        } else {
            /* Check subdirectories for more SFZ files */
            char sub_path[MAX_PATH_LEN];
            snprintf(sub_path, sizeof(sub_path), "%s/%s", instrument_path, entry->d_name);
            struct stat st;
            if (stat(sub_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                DIR *subdir = opendir(sub_path);
                if (subdir) {
                    struct dirent *sub_entry;
                    while ((sub_entry = readdir(subdir)) != NULL) {
                        if (sub_entry->d_name[0] == '.') continue;
                        add_preset_entry(inst, sub_path, sub_entry->d_name);
                    }
                    closedir(subdir);
                }
            }
        }
    }

    closedir(dir);

    if (inst->preset_count > 1) {
        qsort(inst->presets, inst->preset_count,
              sizeof(preset_entry_t), preset_entry_cmp);
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "Found %d presets in instrument", inst->preset_count);
    plugin_log(msg);
}

/* Load a .sfz or .dspreset file into the synth */
static int load_sfz_file(sfz_instance_t *inst, const char *path) {
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

    const char *format = NULL;
    if (!sfizz_load_or_import_file(inst->synth, path, &format)) {
        snprintf(msg, sizeof(msg), "Failed to load: %s", path);
        plugin_log(msg);
        /* Check extension for more specific error */
        const char *ext = strrchr(path, '.');
        if (ext && strcasecmp(ext, ".dspreset") == 0) {
            snprintf(inst->load_error, sizeof(inst->load_error),
                     "DecentSampler import failed - check XML format");
        } else {
            snprintf(inst->load_error, sizeof(inst->load_error),
                     "Failed to load instrument file");
        }
        return -1;
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
            snprintf(inst->load_error, sizeof(inst->load_error),
                     "Sample files not found - upload complete instrument with audio files");
            plugin_log("WARNING: 0 preloaded samples - files missing from disk");
        } else {
            inst->load_error[0] = '\0';
        }
    }

    return 0;
}

/* Find instrument index by name, returns -1 if not found */
static int find_instrument_by_name(sfz_instance_t *inst, const char *name) {
    for (int i = 0; i < inst->instrument_count; i++) {
        if (strcmp(inst->instruments[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* Switch to an instrument folder and load its first preset */
static void set_instrument_index(sfz_instance_t *inst, int index) {
    if (inst->instrument_count <= 0) return;

    if (index < 0) index = inst->instrument_count - 1;
    if (index >= inst->instrument_count) index = 0;

    inst->current_instrument = index;
    strncpy(inst->instrument_name, inst->instruments[index].name,
            sizeof(inst->instrument_name) - 1);

    /* Silence current notes */
    sfizz_all_sound_off(inst->synth);

    /* Scan presets in this instrument folder */
    scan_presets(inst, inst->instruments[index].path);

    /* Load first preset */
    if (inst->preset_count > 0) {
        inst->current_preset = 0;
        strncpy(inst->preset_name, inst->presets[0].name,
                sizeof(inst->preset_name) - 1);
        load_sfz_file(inst, inst->presets[0].path);
    } else {
        inst->current_preset = 0;
        strcpy(inst->preset_name, "No presets");
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "Instrument %d: %s (%d presets)",
             index, inst->instrument_name, inst->preset_count);
    plugin_log(msg);
}

/* Switch preset within current instrument */
static void select_preset(sfz_instance_t *inst, int index) {
    if (inst->preset_count <= 0) return;

    if (index < 0) index = inst->preset_count - 1;
    if (index >= inst->preset_count) index = 0;

    /* Silence current notes */
    if (inst->current_preset != index) {
        sfizz_all_sound_off(inst->synth);
    }

    inst->current_preset = index;
    strncpy(inst->preset_name, inst->presets[index].name,
            sizeof(inst->preset_name) - 1);

    load_sfz_file(inst, inst->presets[index].path);

    char msg[128];
    snprintf(msg, sizeof(msg), "Preset %d: %s", index, inst->preset_name);
    plugin_log(msg);
}

/* V2 API Implementation */

static void* v2_create_instance(const char *module_dir, const char *json_defaults) {
    char msg[256];
    snprintf(msg, sizeof(msg), "Creating instance from: %s", module_dir);
    plugin_log(msg);

    sfz_instance_t *inst = calloc(1, sizeof(sfz_instance_t));
    if (!inst) return NULL;

    strncpy(inst->module_dir, module_dir, sizeof(inst->module_dir) - 1);
    strcpy(inst->instrument_name, "No instrument");
    strcpy(inst->preset_name, "");
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
    sfizz_set_num_voices(inst->synth, 64);
    sfizz_set_volume(inst->synth, inst->gain);

    snprintf(msg, sizeof(msg), "sfizz initialized: sample_rate=%d, block=%d",
             sample_rate, MOVE_FRAMES_PER_BLOCK);
    plugin_log(msg);

    /* Scan instruments */
    scan_instruments(inst, module_dir);

    /* Restore from defaults or load first instrument */
    int start_instrument = 0;
    int start_preset = 0;

    if (json_defaults) {
        float f;
        char name[MAX_NAME_LEN];
        if (json_get_string(json_defaults, "instrument_name", name, sizeof(name)) > 0) {
            int idx = find_instrument_by_name(inst, name);
            if (idx >= 0) start_instrument = idx;
        }
        if (json_get_number(json_defaults, "instrument_index", &f) == 0) {
            int idx = (int)f;
            if (idx >= 0 && idx < inst->instrument_count) {
                start_instrument = idx;
            }
        }
        if (json_get_number(json_defaults, "preset", &f) == 0) {
            start_preset = (int)f;
        }
    }

    if (inst->instrument_count > 0) {
        set_instrument_index(inst, start_instrument);
        if (start_preset > 0 && start_preset < inst->preset_count) {
            select_preset(inst, start_preset);
        }
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
                /* sfizz expects signed value: -8191 to +8191, center = 0 */
                sfizz_send_pitch_wheel(inst->synth, 0, bend);
            }
            break;
        case 0xC0:  /* Program change - map to preset list */
            if (data1 < inst->preset_count) {
                select_preset(inst, data1);
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

    if (strcmp(key, "instrument_index") == 0) {
        int idx = atoi(val);
        if (idx == inst->current_instrument) return;
        set_instrument_index(inst, idx);
    } else if (strcmp(key, "next_instrument") == 0) {
        set_instrument_index(inst, inst->current_instrument + 1);
    } else if (strcmp(key, "prev_instrument") == 0) {
        set_instrument_index(inst, inst->current_instrument - 1);
    } else if (strcmp(key, "preset") == 0) {
        int idx = atoi(val);
        if (idx == inst->current_preset) return;
        select_preset(inst, idx);
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
        if (inst->synth) {
            sfizz_all_sound_off(inst->synth);
        }
    } else if (strcmp(key, "state") == 0) {
        /* Restore state from JSON */
        float f;
        char name[MAX_NAME_LEN];
        int instr_idx = -1;

        if (json_get_string(val, "instrument_name", name, sizeof(name)) > 0) {
            instr_idx = find_instrument_by_name(inst, name);
        }
        if (instr_idx < 0 && json_get_number(val, "instrument_index", &f) == 0) {
            int idx = (int)f;
            if (idx >= 0 && idx < inst->instrument_count) {
                instr_idx = idx;
            }
        }
        if (instr_idx >= 0) {
            set_instrument_index(inst, instr_idx);
        }
        if (json_get_number(val, "preset", &f) == 0) {
            select_preset(inst, (int)f);
        }
        if (json_get_number(val, "octave_transpose", &f) == 0) {
            inst->octave_transpose = (int)f;
            if (inst->octave_transpose < -4) inst->octave_transpose = -4;
            if (inst->octave_transpose > 4) inst->octave_transpose = 4;
        }
        if (json_get_number(val, "gain", &f) == 0) {
            inst->gain = f;
            if (inst->gain < 0.0f) inst->gain = 0.0f;
            if (inst->gain > 2.0f) inst->gain = 2.0f;
            if (inst->synth) {
                sfizz_set_volume(inst->synth, inst->gain);
            }
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
    } else if (strcmp(key, "instrument_name") == 0) {
        strncpy(buf, inst->instrument_name, buf_len - 1);
        buf[buf_len - 1] = '\0';
        return strlen(buf);
    } else if (strcmp(key, "instrument_count") == 0) {
        return snprintf(buf, buf_len, "%d", inst->instrument_count);
    } else if (strcmp(key, "instrument_index") == 0) {
        return snprintf(buf, buf_len, "%d", inst->current_instrument);
    } else if (strcmp(key, "preset") == 0 || strcmp(key, "current_patch") == 0) {
        return snprintf(buf, buf_len, "%d", inst->current_preset);
    } else if (strcmp(key, "preset_name") == 0 || strcmp(key, "patch_name") == 0 || strcmp(key, "name") == 0) {
        strncpy(buf, inst->preset_name, buf_len - 1);
        buf[buf_len - 1] = '\0';
        return strlen(buf);
    } else if (strcmp(key, "preset_count") == 0 || strcmp(key, "total_patches") == 0) {
        return snprintf(buf, buf_len, "%d", inst->preset_count);
    } else if (strcmp(key, "octave_transpose") == 0) {
        return snprintf(buf, buf_len, "%d", inst->octave_transpose);
    } else if (strcmp(key, "gain") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->gain);
    }
    /* Unified bank/preset parameters for Chain compatibility */
    else if (strcmp(key, "bank_name") == 0) {
        strncpy(buf, inst->instrument_name, buf_len - 1);
        buf[buf_len - 1] = '\0';
        return strlen(buf);
    } else if (strcmp(key, "patch_in_bank") == 0) {
        return snprintf(buf, buf_len, "%d", inst->current_preset + 1);
    } else if (strcmp(key, "bank_count") == 0) {
        return snprintf(buf, buf_len, "%d", inst->instrument_count);
    }
    /* Dynamic instrument list for Shadow UI menu - rescan each time */
    else if (strcmp(key, "instrument_list") == 0 || strcmp(key, "soundfont_list") == 0) {
        /* Rescan instruments directory */
        scan_instruments(inst, inst->module_dir);

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
    /* State serialization for save/load */
    else if (strcmp(key, "state") == 0) {
        const char *instr_name = "";
        if (inst->instrument_count > 0 && inst->current_instrument < inst->instrument_count) {
            instr_name = inst->instruments[inst->current_instrument].name;
        }
        return snprintf(buf, buf_len,
            "{\"instrument_name\":\"%s\",\"instrument_index\":%d,\"preset\":%d,"
            "\"octave_transpose\":%d,\"gain\":%.2f}",
            instr_name, inst->current_instrument, inst->current_preset,
            inst->octave_transpose, inst->gain);
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
                        "{\"level\":\"instrument\",\"label\":\"Choose Instrument\"}"
                    "]"
                "},"
                "\"instrument\":{"
                    "\"label\":\"Instrument\","
                    "\"items_param\":\"instrument_list\","
                    "\"select_param\":\"instrument_index\","
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
