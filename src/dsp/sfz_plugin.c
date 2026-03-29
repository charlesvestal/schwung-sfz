/*
 * SFZ Player DSP Plugin
 *
 * Uses sfizz to render SFZ and DecentSampler (.dspreset) instruments.
 * Instruments are organized under instruments/:
 *   - Folders containing .sfz files = one preset per folder (with variants)
 *   - Loose .sfz files = one preset each (samples in adjacent folders)
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
#define MAX_VARIANTS 256
#define MAX_PATH_LEN 512
#define MAX_NAME_LEN 128
/* ~150ms debounce at 48kHz/128 frames per block */
#define DEBOUNCE_BLOCKS 56

typedef struct {
    char path[MAX_PATH_LEN];    /* Path where .sfz files live (may be subdir) */
    char root[MAX_PATH_LEN];    /* Instrument root folder (for sample fallback) */
    char name[MAX_NAME_LEN];    /* Folder display name */
    char sfz_file[MAX_PATH_LEN]; /* If non-empty: single loose .sfz file (no folder) */
} instrument_entry_t;

typedef struct {
    char path[MAX_PATH_LEN];    /* Full path to .sfz file */
    char name[MAX_NAME_LEN];    /* Display name (filename without .sfz) */
} variant_entry_t;

/* Per-Instance State */
typedef struct {
    sfizz_synth_t *synth;
    int current_instrument;
    int current_variant;
    int instrument_count;
    int variant_count;
    int octave_transpose;
    float gain;
    instrument_entry_t instruments[MAX_INSTRUMENTS];
    variant_entry_t variants[MAX_VARIANTS];
    int last_variant[MAX_INSTRUMENTS];  /* Remember last variant per instrument */
    char instrument_name[MAX_NAME_LEN];
    char variant_name[MAX_NAME_LEN];
    char module_dir[MAX_PATH_LEN];
    char load_error[256];
    int debounce_remaining;      /* Blocks remaining before loading */
    int pending_load;            /* 1 if a deferred load is pending */
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

static int variant_entry_cmp(const void *a, const void *b) {
    const variant_entry_t *pa = (const variant_entry_t *)a;
    const variant_entry_t *pb = (const variant_entry_t *)b;
    return strcasecmp(pa->name, pb->name);
}

/* Scan instruments/ directory for instrument folders and loose .sfz files */
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

        char full_path[MAX_PATH_LEN];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            /* Directory: check if it contains instrument files */
            char sfz_dir[MAX_PATH_LEN];
            if (!dir_has_instruments(full_path, sfz_dir, sizeof(sfz_dir))) continue;

            if (inst->instrument_count >= MAX_INSTRUMENTS) {
                plugin_log("Instrument list full, skipping extras");
                break;
            }

            instrument_entry_t *instr = &inst->instruments[inst->instrument_count];
            strncpy(instr->path, sfz_dir, sizeof(instr->path) - 1);
            instr->path[sizeof(instr->path) - 1] = '\0';
            strncpy(instr->root, full_path, sizeof(instr->root) - 1);
            instr->root[sizeof(instr->root) - 1] = '\0';
            strncpy(instr->name, entry->d_name, sizeof(instr->name) - 1);
            instr->name[sizeof(instr->name) - 1] = '\0';
            instr->sfz_file[0] = '\0';  /* Folder-based instrument */
            inst->last_variant[inst->instrument_count] = 0;
            inst->instrument_count++;
        } else {
            /* Loose file: check if it's a supported instrument */
            const char *ext = strrchr(entry->d_name, '.');
            if (!ext || !is_supported_instrument(ext)) continue;

            if (inst->instrument_count >= MAX_INSTRUMENTS) {
                plugin_log("Instrument list full, skipping extras");
                break;
            }

            instrument_entry_t *instr = &inst->instruments[inst->instrument_count];
            /* Path and root both point to instruments/ dir for sample resolution */
            strncpy(instr->path, dir_path, sizeof(instr->path) - 1);
            instr->path[sizeof(instr->path) - 1] = '\0';
            strncpy(instr->root, dir_path, sizeof(instr->root) - 1);
            instr->root[sizeof(instr->root) - 1] = '\0';
            /* Name = filename without extension */
            int name_len = ext - entry->d_name;
            if (name_len >= MAX_NAME_LEN) name_len = MAX_NAME_LEN - 1;
            strncpy(instr->name, entry->d_name, name_len);
            instr->name[name_len] = '\0';
            /* Store full path to the .sfz file */
            strncpy(instr->sfz_file, full_path, sizeof(instr->sfz_file) - 1);
            instr->sfz_file[sizeof(instr->sfz_file) - 1] = '\0';
            inst->last_variant[inst->instrument_count] = 0;
            inst->instrument_count++;
        }
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

/* Add a single variant entry from a file path */
static void add_variant_entry(sfz_instance_t *inst, const char *dir_path, const char *filename) {
    if (inst->variant_count >= MAX_VARIANTS) return;

    const char *ext = strrchr(filename, '.');
    if (!ext || !is_supported_instrument(ext)) return;

    variant_entry_t *v = &inst->variants[inst->variant_count++];
    snprintf(v->path, sizeof(v->path), "%s/%s", dir_path, filename);

    /* Display name = filename without extension */
    int name_len = ext - filename;
    if (name_len >= MAX_NAME_LEN) name_len = MAX_NAME_LEN - 1;
    strncpy(v->name, filename, name_len);
    v->name[name_len] = '\0';
}

/* Scan .sfz files within an instrument folder (these become variants).
 * Also checks one level of subdirectories for instruments with nested structures. */
static void scan_variants(sfz_instance_t *inst, const char *instrument_path) {
    inst->variant_count = 0;

    DIR *dir = opendir(instrument_path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        const char *ext = strrchr(entry->d_name, '.');
        if (ext && is_supported_instrument(ext)) {
            add_variant_entry(inst, instrument_path, entry->d_name);
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
                        add_variant_entry(inst, sub_path, sub_entry->d_name);
                    }
                    closedir(subdir);
                }
            }
        }
    }

    closedir(dir);

    if (inst->variant_count > 1) {
        qsort(inst->variants, inst->variant_count,
              sizeof(variant_entry_t), variant_entry_cmp);
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "Found %d variants in instrument", inst->variant_count);
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
 * Finds attr="value" in a tag string, handling missing spaces between attrs.
 * Returns value in out_val (null-terminated), or empty string if not found. */
static void xml_get_attr(const char *tag, const char *attr_name, char *out_val, int max_len) {
    out_val[0] = '\0';
    char search[64];
    snprintf(search, sizeof(search), "%s=\"", attr_name);
    const char *pos = strstr(tag, search);
    if (!pos) return;
    pos += strlen(search);
    const char *end = strchr(pos, '"');
    if (!end) return;
    int len = end - pos;
    if (len >= max_len) len = max_len - 1;
    memcpy(out_val, pos, len);
    out_val[len] = '\0';
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

    /* Find <groups> attributes */
    char *groups_tag = strstr(src, "<groups");
    if (groups_tag) {
        char *groups_end = strchr(groups_tag, '>');
        if (groups_end) {
            char tag_buf[1024];
            int tlen = groups_end - groups_tag;
            if (tlen >= (int)sizeof(tag_buf)) tlen = sizeof(tag_buf) - 1;
            memcpy(tag_buf, groups_tag, tlen);
            tag_buf[tlen] = '\0';

            pos += snprintf(sfz + pos, size * 2 - pos, "<global>\n");
            xml_get_attr(tag_buf, "volume", val, sizeof(val));
            if (val[0]) pos += snprintf(sfz + pos, size * 2 - pos, "volume=%s\n", val);
        }
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
            if (val[0]) pos += snprintf(sfz + pos, size * 2 - pos, "sample=%s\n", val);
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
            unlink(converted_path);
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
            if (root_path && root_path[0] &&
                ext && strcasecmp(ext, ".sfz") == 0 &&
                strcmp(root_path, inst->instruments[inst->current_instrument].path) != 0 &&
                try_load_with_root(inst, path, root_path) == 0) {
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

/* Find instrument index by name, returns -1 if not found */
static int find_instrument_by_name(sfz_instance_t *inst, const char *name) {
    for (int i = 0; i < inst->instrument_count; i++) {
        if (strcmp(inst->instruments[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* Select a variant (.sfz file) within the current instrument */
static void select_variant(sfz_instance_t *inst, int index) {
    if (inst->variant_count <= 0) return;

    if (index < 0) index = inst->variant_count - 1;
    if (index >= inst->variant_count) index = 0;

    /* Silence current notes */
    if (inst->current_variant != index) {
        sfizz_all_sound_off(inst->synth);
    }

    inst->current_variant = index;
    strncpy(inst->variant_name, inst->variants[index].name,
            sizeof(inst->variant_name) - 1);

    /* Remember this variant for the current instrument */
    if (inst->current_instrument >= 0 && inst->current_instrument < MAX_INSTRUMENTS) {
        inst->last_variant[inst->current_instrument] = index;
    }

    const char *root = inst->instruments[inst->current_instrument].root;
    load_sfz_file(inst, inst->variants[index].path, root);

    char msg[128];
    snprintf(msg, sizeof(msg), "Variant %d: %s", index, inst->variant_name);
    plugin_log(msg);
}

/* Actually load the current instrument's variant (called after debounce) */
static void do_load_instrument(sfz_instance_t *inst) {
    int index = inst->current_instrument;
    if (index < 0 || index >= inst->instrument_count) return;

    instrument_entry_t *instr = &inst->instruments[index];

    if (instr->sfz_file[0]) {
        /* Single loose .sfz file - one variant, load directly */
        inst->variant_count = 1;
        variant_entry_t *v = &inst->variants[0];
        strncpy(v->path, instr->sfz_file, sizeof(v->path) - 1);
        v->path[sizeof(v->path) - 1] = '\0';
        strncpy(v->name, instr->name, sizeof(v->name) - 1);
        v->name[sizeof(v->name) - 1] = '\0';

        inst->current_variant = 0;
        strncpy(inst->variant_name, v->name, sizeof(inst->variant_name) - 1);
        load_sfz_file(inst, v->path, instr->root);
    } else {
        /* Folder-based instrument - scan for variants */
        scan_variants(inst, instr->path);

        if (inst->variant_count > 0) {
            int variant_idx = inst->last_variant[index];
            if (variant_idx < 0 || variant_idx >= inst->variant_count)
                variant_idx = 0;
            inst->current_variant = variant_idx;
            strncpy(inst->variant_name, inst->variants[variant_idx].name,
                    sizeof(inst->variant_name) - 1);
            load_sfz_file(inst, inst->variants[variant_idx].path, instr->root);
        } else {
            inst->current_variant = 0;
            strcpy(inst->variant_name, "No variants");
        }
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "Instrument %d: %s (%d variants)",
             index, inst->instrument_name, inst->variant_count);
    plugin_log(msg);
}

/* Switch to an instrument folder (deferred load with debounce) */
static void set_instrument_index(sfz_instance_t *inst, int index) {
    if (inst->instrument_count <= 0) return;

    if (index < 0) index = inst->instrument_count - 1;
    if (index >= inst->instrument_count) index = 0;

    inst->current_instrument = index;
    strncpy(inst->instrument_name, inst->instruments[index].name,
            sizeof(inst->instrument_name) - 1);

    /* Silence current notes */
    sfizz_all_sound_off(inst->synth);

    /* Defer the actual load to allow rapid jog browsing */
    inst->pending_load = 1;
    inst->debounce_remaining = DEBOUNCE_BLOCKS;
}

/* Switch to an instrument and load immediately (for startup/state restore) */
static void set_instrument_index_immediate(sfz_instance_t *inst, int index) {
    if (inst->instrument_count <= 0) return;

    if (index < 0) index = inst->instrument_count - 1;
    if (index >= inst->instrument_count) index = 0;

    inst->current_instrument = index;
    strncpy(inst->instrument_name, inst->instruments[index].name,
            sizeof(inst->instrument_name) - 1);

    sfizz_all_sound_off(inst->synth);
    inst->pending_load = 0;
    inst->debounce_remaining = 0;
    do_load_instrument(inst);
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
    strcpy(inst->variant_name, "");
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

    /* Restore from defaults or load first instrument */
    int start_instrument = 0;
    int start_variant = 0;

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
        /* Support both old "preset" key and new "variant" key */
        if (json_get_number(json_defaults, "variant", &f) == 0) {
            start_variant = (int)f;
        } else if (json_get_number(json_defaults, "preset", &f) == 0) {
            start_variant = (int)f;
        }
    }

    if (inst->instrument_count > 0) {
        set_instrument_index_immediate(inst, start_instrument);
        if (start_variant > 0 && start_variant < inst->variant_count) {
            select_variant(inst, start_variant);
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
                sfizz_send_pitch_wheel(inst->synth, 0, bend);
            }
            break;
        case 0xC0:  /* Program change - map to instrument list */
            if (data1 < inst->instrument_count) {
                set_instrument_index(inst, data1);
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

    /* Preset browser navigates instrument folders */
    if (strcmp(key, "preset") == 0 || strcmp(key, "instrument_index") == 0) {
        int idx = atoi(val);
        if (idx == inst->current_instrument) return;
        set_instrument_index(inst, idx);
    } else if (strcmp(key, "variant") == 0) {
        int idx = atoi(val);
        if (idx == inst->current_variant) return;
        select_variant(inst, idx);
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
            set_instrument_index_immediate(inst, instr_idx);
        }
        /* Support both old "preset" and new "variant" keys */
        if (json_get_number(val, "variant", &f) == 0) {
            select_variant(inst, (int)f);
        } else if (json_get_number(val, "preset", &f) == 0) {
            select_variant(inst, (int)f);
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
    }
    /* Preset browser shows instrument folders */
    else if (strcmp(key, "preset") == 0 || strcmp(key, "current_patch") == 0) {
        return snprintf(buf, buf_len, "%d", inst->current_instrument);
    } else if (strcmp(key, "preset_count") == 0 || strcmp(key, "total_patches") == 0) {
        return snprintf(buf, buf_len, "%d", inst->instrument_count);
    } else if (strcmp(key, "preset_name") == 0 || strcmp(key, "patch_name") == 0 || strcmp(key, "name") == 0) {
        strncpy(buf, inst->instrument_name, buf_len - 1);
        buf[buf_len - 1] = '\0';
        return strlen(buf);
    }
    /* Instrument params (alias for preset browser) */
    else if (strcmp(key, "instrument_name") == 0) {
        strncpy(buf, inst->instrument_name, buf_len - 1);
        buf[buf_len - 1] = '\0';
        return strlen(buf);
    } else if (strcmp(key, "instrument_count") == 0) {
        return snprintf(buf, buf_len, "%d", inst->instrument_count);
    } else if (strcmp(key, "instrument_index") == 0) {
        return snprintf(buf, buf_len, "%d", inst->current_instrument);
    }
    /* Variant params (.sfz files within instrument) */
    else if (strcmp(key, "variant") == 0) {
        return snprintf(buf, buf_len, "%d", inst->current_variant);
    } else if (strcmp(key, "variant_count") == 0) {
        return snprintf(buf, buf_len, "%d", inst->variant_count);
    } else if (strcmp(key, "variant_name") == 0) {
        strncpy(buf, inst->variant_name, buf_len - 1);
        buf[buf_len - 1] = '\0';
        return strlen(buf);
    }
    /* Knob params */
    else if (strcmp(key, "octave_transpose") == 0) {
        return snprintf(buf, buf_len, "%d", inst->octave_transpose);
    } else if (strcmp(key, "gain") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->gain);
    }
    /* Chain compatibility */
    else if (strcmp(key, "bank_name") == 0) {
        strncpy(buf, inst->instrument_name, buf_len - 1);
        buf[buf_len - 1] = '\0';
        return strlen(buf);
    } else if (strcmp(key, "patch_in_bank") == 0) {
        return snprintf(buf, buf_len, "%d", inst->current_instrument + 1);
    } else if (strcmp(key, "bank_count") == 0) {
        return snprintf(buf, buf_len, "%d", inst->instrument_count);
    }
    /* Dynamic variant list for menu */
    else if (strcmp(key, "variant_list") == 0) {
        int written = 0;
        written += snprintf(buf + written, buf_len - written, "[");
        for (int i = 0; i < inst->variant_count && written < buf_len - 50; i++) {
            if (i > 0) written += snprintf(buf + written, buf_len - written, ",");
            written += snprintf(buf + written, buf_len - written,
                "{\"label\":\"%s\",\"index\":%d}",
                inst->variants[i].name, i);
        }
        written += snprintf(buf + written, buf_len - written, "]");
        return written;
    }
    /* Dynamic instrument list for menu - rescan each time */
    else if (strcmp(key, "instrument_list") == 0 || strcmp(key, "soundfont_list") == 0) {
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
            "{\"instrument_name\":\"%s\",\"instrument_index\":%d,\"variant\":%d,"
            "\"octave_transpose\":%d,\"gain\":%.2f}",
            instr_name, inst->current_instrument, inst->current_variant,
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
                        "{\"level\":\"variant\",\"label\":\"Choose Variant\"}"
                    "]"
                "},"
                "\"variant\":{"
                    "\"label\":\"Variant\","
                    "\"items_param\":\"variant_list\","
                    "\"select_param\":\"variant\","
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

    /* Handle debounced instrument loading */
    if (inst->pending_load) {
        if (inst->debounce_remaining > 0) {
            inst->debounce_remaining--;
            /* Output silence while waiting */
            memset(out_interleaved_lr, 0, frames * 2 * sizeof(int16_t));
            return;
        }
        inst->pending_load = 0;
        do_load_instrument(inst);
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
