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
#include <pthread.h>
#include <stdatomic.h>

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
extern void          xshim_set_reverb(XSynthHandle*, uint32_t enable,
                                        float room, float time, float damp);
extern void          xshim_set_reverb_wet(XSynthHandle*, float wet);
extern void          xshim_set_delay(XSynthHandle*, uint32_t enable,
                                       float time, float feedback);
extern void          xshim_set_delay_time(XSynthHandle*, float time);
extern void          xshim_set_delay_feedback(XSynthHandle*, float fb);
extern void          xshim_set_delay_mix(XSynthHandle*, float mix);
extern void          xshim_set_chorus(XSynthHandle*, uint32_t enable,
                                        float rate, float depth);
extern void          xshim_set_chorus_rate(XSynthHandle*, float rate);
extern void          xshim_set_chorus_depth(XSynthHandle*, float depth);
extern void          xshim_set_chorus_mix(XSynthHandle*, float mix);
extern void          xshim_set_phaser(XSynthHandle*, uint32_t enable,
                                        float rate, float depth, float feedback);
extern void          xshim_set_phaser_rate(XSynthHandle*, float rate);
extern void          xshim_set_phaser_depth(XSynthHandle*, float depth);
extern void          xshim_set_phaser_feedback(XSynthHandle*, float fb);
extern void          xshim_set_phaser_mix(XSynthHandle*, float mix);
extern void          xshim_set_widener(XSynthHandle*, float width);
extern uint32_t      xshim_take_noteon_count(XSynthHandle*);
extern void          xshim_take_render_breakdown(XSynthHandle*, uint32_t *out4);
extern void          xshim_fine_tune(XSynthHandle*, uint8_t ch, float cents);
extern uint32_t      xshim_take_underrun_count(XSynthHandle*);
extern void          xshim_take_underrun_breakdown(XSynthHandle*, uint32_t *out3);
extern float         xshim_estimated_voice_peak(const XSynthHandle*);
extern uint32_t      xshim_max_region_stacking(const XSynthHandle*);
extern uint32_t      xshim_declared_polyphony(const XSynthHandle*);
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
    /* MOVE FORK / 2026-05-17: per-preset auto-gain. Computed from
     * xshim_estimated_voice_peak after each load_apply, used as a
     * multiplier alongside `gain` in the output stage. Brings hot SFZ
     * libraries (Salamander piano per-voice peak ≈ 0.82) into the
     * same loudness ballpark as DecentSampler; quiet presets land at
     * 1.0 (no change). */
    float preset_attenuation;
    /* MOVE FORK / 2026-05-16: peak limiter state for the output stage.
     * Replaces tanh saturation with a proper feedback limiter:
     * instant attack, ~100 ms exponential release. Below threshold
     * the path is linear (transparent); above threshold it attenuates
     * just enough to keep peaks under the ceiling. Avoids the
     * compressed/distorted character tanh imposed on hot SFZ libraries
     * like Salamander where 4-note chords easily hit ±3-5 before the
     * gain stage. */
    float limiter_env;
    instrument_entry_t instruments[MAX_INSTRUMENTS];
    preset_entry_t presets[MAX_PRESETS];
    char preset_name[MAX_NAME_LEN];
    char instrument_name[MAX_NAME_LEN];
    char module_dir[MAX_PATH_LEN];
    char load_error[256];
    int debounce_remaining;
    int pending_load;
    int is_loading;                /* 1 from preset-switch request until the
                                    * async load applies. Drives the
                                    * "Loading... <name> <spinner>" prefix on
                                    * preset_name so the browser shows
                                    * activity during the 5-15s sample read
                                    * window for heavy DS libraries. */
    int suppress_next_preset_set;
    /* Per-preset DS knobs (populated by the converter). Each knob is
     * surfaced as a Move param (`knob_0`..`knob_15`); moving it sends
     * the assigned CC via `xshim_cc`. Live response only when the knob's
     * bindings hit xsynth-recognized targets (currently ampeg_*). */
    ds_knob_t knobs[DS_MAX_KNOBS];
    int knob_count;
    /* Phase 6.5: tab metadata + current tab index. The encoder row
     * shows only knobs whose `tab_idx` matches `current_tab`. */
    ds_tab_t tabs[DS_MAX_TABS];
    int tab_count;
    int current_tab;
    /* Phase 9 prototype: fundsp reverb wet level (0..1). */
    float reverb_wet;
    double knob_current[DS_MAX_KNOBS]; /* current logical value in [min,max] */
    int user_voices_override;      /* set when user has explicitly set "voices"
                                     * via set_param or json_defaults; pins
                                     * the value across preset changes. */
    /* MOVE FORK / 2026-05-19: global ADSR knob positions, normalized
     * 0..1. Default 0.5 = unity multiplier (no change from preset's
     * authored envelope). At every preset load we re-push these via the
     * existing CC dispatch so the new soundfont's voices start at the
     * current knob positions. */
    float adsr_attack;
    float adsr_decay;
    float adsr_sustain;
    float adsr_release;
    /* MOVE FORK / 2026-05-19: master fine tune in cents, range ±100.
     * Composes with MIDI pitchbend via xsynth's process_pitch sum so
     * external pitchbend keeps working. UI exposes as 0..1 mapped to
     * -100..+100 cents, default 0.5 = 0 cents. */
    float fine_tune;
    /* MOVE FORK / 2026-05-19: global filter shaping. cutoff drives the
     * standard CC74 (channel filter freq); reso drives CC71 (channel
     * filter Q). Stored normalized 0..1; UI shows -50..+50. The
     * channel-level filter exists independently of per-region cutoffs
     * so this works on every preset, even SFZs that don't declare one. */
    float filter_cutoff;
    float filter_reso;
    float *render_buf;             /* interleaved L/R/L/R..., 2 * frames */
} xsynth_instance_t;

/* MOVE FORK / 2026-05-18: async log SPSC ring. plugin_log() is called
 * from the audio thread (render_block path) with a 256-byte string;
 * g_host->log() goes through Move's unified-log path which eventually
 * does a write() syscall. On busy blocks (clip outliers, CLICK detector,
 * perf line every 2048 blocks) the audio thread can stack 1-3 log calls
 * back-to-back — small individually, but each is a syscall on the SCHED_FIFO
 * 90 SPI thread we're paying ~70% of the frame budget to keep within.
 *
 * Replace with a fixed-size SPSC ring (256 slots × 256 bytes = 64 KiB),
 * drop-on-full so the audio thread never blocks waiting. A background
 * pthread (created lazily on first log) drains and calls g_host->log.
 * Single-producer (audio thread), single-consumer (drain thread).
 *
 * Ordering: producer atomic_store(write_pos, Release) after filling
 * the slot; consumer atomic_load(write_pos, Acquire) before reading.
 * Capacity is power of 2 so we can mask instead of mod.
 */
#define LOG_RING_SLOTS  256
#define LOG_RING_MASK   (LOG_RING_SLOTS - 1)
#define LOG_MSG_LEN     256
static char         log_ring[LOG_RING_SLOTS][LOG_MSG_LEN];
static _Atomic uint64_t log_write_pos = 0;
static _Atomic uint64_t log_read_pos  = 0;
static _Atomic uint64_t log_dropped   = 0;
static pthread_t    log_drain_thread;
static int          log_drain_thread_started = 0;
static pthread_mutex_t log_drain_start_mu = PTHREAD_MUTEX_INITIALIZER;

static void *log_drain_thread_fn(void *arg) {
    (void)arg;
    /* Pin off core 3 (SPI audio core) so the drain thread can't cause
     * the same cross-core preemption we just fixed for the IO worker. */
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(0, &mask); CPU_SET(1, &mask); CPU_SET(2, &mask);
    sched_setaffinity(0, sizeof(mask), &mask);

    char tmp[LOG_MSG_LEN + 32];
    for (;;) {
        uint64_t r = atomic_load_explicit(&log_read_pos, memory_order_relaxed);
        uint64_t w = atomic_load_explicit(&log_write_pos, memory_order_acquire);
        if (r == w) {
            /* Empty ring. Sleep ~10 ms — far below human perception of
             * log latency, far above any sane wake-up overhead. */
            struct timespec ts = { 0, 10 * 1000 * 1000 };
            nanosleep(&ts, NULL);
            continue;
        }
        /* Copy out, advance read pos. The producer may overwrite this
         * slot the instant we advance, so do the copy first. */
        size_t idx = (size_t)(r & LOG_RING_MASK);
        strncpy(tmp, log_ring[idx], LOG_MSG_LEN);
        tmp[LOG_MSG_LEN - 1] = 0;
        atomic_store_explicit(&log_read_pos, r + 1, memory_order_release);
        if (g_host && g_host->log) g_host->log(tmp);

        /* Periodically surface drop count so we know if the ring is
         * undersized for the current logging cadence. */
        uint64_t drops = atomic_load_explicit(&log_dropped, memory_order_relaxed);
        static uint64_t last_drops_reported = 0;
        if (drops > last_drops_reported + 64) {
            char dm[64];
            snprintf(dm, sizeof(dm), "[sfz] log: dropped %llu messages",
                     (unsigned long long)(drops - last_drops_reported));
            if (g_host && g_host->log) g_host->log(dm);
            last_drops_reported = drops;
        }
    }
    return NULL;
}

static void plugin_log(const char *msg) {
    if (!g_host || !g_host->log) return;

    /* Lazy-start the drain thread on first log call. Mutex guards the
     * one-shot start; never contended after the initial fire. */
    if (!log_drain_thread_started) {
        pthread_mutex_lock(&log_drain_start_mu);
        if (!log_drain_thread_started) {
            if (pthread_create(&log_drain_thread, NULL,
                               log_drain_thread_fn, NULL) == 0) {
                pthread_detach(log_drain_thread);
                log_drain_thread_started = 1;
            }
        }
        pthread_mutex_unlock(&log_drain_start_mu);
        /* If creation failed, fall through to the sync path below. */
        if (!log_drain_thread_started) {
            char buf[LOG_MSG_LEN];
            snprintf(buf, sizeof(buf), "[sfz] %s", msg);
            g_host->log(buf);
            return;
        }
    }

    /* Single producer (audio thread) — no need for CAS. */
    uint64_t w = atomic_load_explicit(&log_write_pos, memory_order_relaxed);
    uint64_t r = atomic_load_explicit(&log_read_pos,  memory_order_acquire);
    if (w - r >= LOG_RING_SLOTS) {
        /* Ring full — drop. Audio thread never blocks. */
        atomic_fetch_add_explicit(&log_dropped, 1, memory_order_relaxed);
        return;
    }
    size_t idx = (size_t)(w & LOG_RING_MASK);
    snprintf(log_ring[idx], LOG_MSG_LEN, "[sfz] %s", msg);
    atomic_store_explicit(&log_write_pos, w + 1, memory_order_release);
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

    inst->knob_count = 0;

    /* Phase 9/10/12: reset post-mix effects on every preset load so
     * state from the previous preset doesn't leak. .dspreset path
     * below re-installs as authored; .sfz path leaves all off. */
    if (inst->synth) {
        xshim_set_reverb(inst->synth, 0, 0.0f, 0.0f, 0.0f);
        xshim_set_reverb_wet(inst->synth, 0.0f);
        inst->reverb_wet = 0.0f;
        xshim_set_delay(inst->synth, 0, 0.0f, 0.0f);
        xshim_set_delay_mix(inst->synth, 0.0f);
        xshim_set_chorus(inst->synth, 0, 0.0f, 0.0f);
        xshim_set_chorus_mix(inst->synth, 0.0f);
        xshim_set_phaser(inst->synth, 0, 0.0f, 0.0f, 0.0f);
        xshim_set_phaser_mix(inst->synth, 0.0f);
        xshim_set_widener(inst->synth, 1.0f);
    }

    /* .dspreset → convert to .converted.sfz next to source, load that. */
    const char *ext = strrchr(path, '.');
    const char *load_path = path;
    char *converted = NULL;
    if (ext && strcasecmp(ext, ".dspreset") == 0) {
        /* Enumerate DS knobs at convert time so the plugin can surface them
         * as Move params. Defaults seed `knob_current[]` so the encoder row
         * shows the dspreset's authored start position. */
        inst->tab_count = 0;
        inst->current_tab = 0;
        ds_reverb_cfg_t reverb_cfg = {0};
        ds_delay_cfg_t  delay_cfg  = {0};
        ds_chorus_cfg_t chorus_cfg = {0};
        ds_phaser_cfg_t phaser_cfg = {0};
        converted = convert_dspreset_to_xsynth_sfz(path,
                                                    inst->knobs,
                                                    &inst->knob_count,
                                                    inst->tabs,
                                                    &inst->tab_count,
                                                    &reverb_cfg,
                                                    &delay_cfg,
                                                    &chorus_cfg,
                                                    &phaser_cfg);
        /* Phase 10: install reverb from dspreset config (or remove
         * when not present). roomSize 0..1 maps to fundsp room param
         * (×30 to expand to seconds-of-prop-time). damping 0..1 →
         * fundsp damp. Wet level becomes the channel's reverb_wet. */
        if (inst->synth) {
            if (reverb_cfg.enabled) {
                /* DS spec only gives roomSize/damping/wetLevel — no
                 * explicit decay-time attribute. Derive both fundsp
                 * room (in seconds of prop time) and time (decay) from
                 * roomSize so larger rooms → longer tails. Defaults
                 * tuned for a tighter "plate"-ish character at
                 * roomSize=0.5. Track tighter behavior parity with
                 * sfizz/fverb in a follow-up. */
                float rs = (float)reverb_cfg.room_size;
                if (rs < 0.0f) rs = 0.0f; if (rs > 1.0f) rs = 1.0f;
                float room = rs * 15.0f + 3.0f;   /* 3..18 */
                float time = rs * 1.8f + 0.4f;    /* 0.4..2.2 sec */
                float damp = (float)reverb_cfg.damping;
                if (damp < 0.0f) damp = 0.0f; if (damp > 1.0f) damp = 1.0f;
                xshim_set_reverb(inst->synth, 1, room, time, damp);
                float wet = (float)reverb_cfg.wet_level;
                if (wet < 0.0f) wet = 0.0f; if (wet > 1.0f) wet = 1.0f;
                inst->reverb_wet = wet;
                xshim_set_reverb_wet(inst->synth, wet);
            } else {
                xshim_set_reverb(inst->synth, 0, 0.0f, 0.0f, 0.0f);
                inst->reverb_wet = 0.0f;
                xshim_set_reverb_wet(inst->synth, 0.0f);
            }
            /* Phase 12: install/remove chorus per dspreset.
             * SAFETY: force mix=0 at install — chorus auto-engagement
             * caused distortion artifacts on some presets; user opts
             * in via the dspreset's FX_MIX knob movement. */
            if (chorus_cfg.enabled) {
                xshim_set_chorus(inst->synth, 1,
                                 (float)chorus_cfg.rate,
                                 (float)chorus_cfg.depth);
                /* Honor the `<effect mix=>` attribute — DS presets
                 * without an FX_MIX knob still expect the authored
                 * mix to be applied at load. Knob bindings (when
                 * present) overwrite this later in the load path. */
                float cmx = (float)chorus_cfg.mix;
                if (cmx < 0.0f) cmx = 0.0f; if (cmx > 1.0f) cmx = 1.0f;
                xshim_set_chorus_mix(inst->synth, cmx);
            } else {
                xshim_set_chorus(inst->synth, 0, 0.0f, 0.0f);
                xshim_set_chorus_mix(inst->synth, 0.0f);
            }
            /* Phase 9: install/remove delay per dspreset. mix becomes
             * the channel's delay_mix; time/feedback live-tweakable
             * via knob bindings. */
            if (delay_cfg.enabled) {
                float t  = (float)delay_cfg.delay_seconds;
                if (t < 0.001f) t = 0.001f; if (t > 2.0f) t = 2.0f;
                float fb = (float)delay_cfg.feedback;
                if (fb < 0.0f) fb = 0.0f; if (fb > 0.95f) fb = 0.95f;
                float mx = (float)delay_cfg.mix;
                if (mx < 0.0f) mx = 0.0f; if (mx > 1.0f) mx = 1.0f;
                xshim_set_delay(inst->synth, 1, t, fb);
                xshim_set_delay_mix(inst->synth, mx);
            } else {
                xshim_set_delay(inst->synth, 0, 0.0f, 0.0f);
                xshim_set_delay_mix(inst->synth, 0.0f);
            }
            /* Phase 12: install/remove phaser per dspreset. Same
             * mix-at-0 install policy as chorus — author opts in via
             * the dspreset's FX_MIX knob movement. */
            if (phaser_cfg.enabled) {
                xshim_set_phaser(inst->synth, 1,
                                 (float)phaser_cfg.rate,
                                 (float)phaser_cfg.depth,
                                 (float)phaser_cfg.feedback);
                /* Same as chorus — honor the authored <effect mix=>. */
                float pmx = (float)phaser_cfg.mix;
                if (pmx < 0.0f) pmx = 0.0f; if (pmx > 1.0f) pmx = 1.0f;
                xshim_set_phaser_mix(inst->synth, pmx);
            } else {
                xshim_set_phaser(inst->synth, 0, 0.0f, 0.0f, 0.0f);
                xshim_set_phaser_mix(inst->synth, 0.0f);
            }
        }
        for (int i = 0; i < inst->knob_count; i++) {
            inst->knob_current[i] = inst->knobs[i].default_value;
        }
        snprintf(msg, sizeof(msg), "Knobs enumerated: %d", inst->knob_count);
        plugin_log(msg);
        for (int i = 0; i < inst->knob_count && i < 4; i++) {
            ds_knob_t *k = &inst->knobs[i];
            snprintf(msg, sizeof(msg), "  knob[%d] key=%s label='%s' range=[%g,%g] cc=%d",
                     i, k->key, k->label, k->min_value, k->max_value, k->cc_number);
            plugin_log(msg);
        }
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
    inst->is_loading = 1;
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
    inst->is_loading = 1;
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
    /* MOVE FORK: master gain default 0.7. Paired with the stateless
     * soft-clip in v2_render_block, this preserves prior preset
     * loudness while making hot chord peaks (Salamander) clean —
     * peak compression at 0.7 is ~0.06 dB (basically transparent),
     * vs the old tanh which compressed every sample above ~0.5. */
    inst->gain = 0.7f;
    inst->preset_attenuation = 1.0f;  /* 1.0 until a preset loads */
    inst->reverb_wet = 0.0f;
    inst->voices = 14;     /* polyphony cap (note-groups). 14 polyphony
                            * × WörliTzer's 5 voices/note ≈ 70 voices,
                            * which is the safe ceiling at current
                            * per-voice render cost. Per-preset heuristic
                            * (see apply_polyphony_for_loaded_preset) may
                            * lower this on layer-heavy libraries. */
    inst->user_voices_override = 0; /* flips true on set_param("voices") or
                                     * json_defaults "voices" — that pins
                                     * the value across preset changes. */
    inst->adsr_attack  = 0.5f;     /* 0.5 maps to CC=64 = unity multiplier */
    inst->adsr_decay   = 0.5f;
    inst->adsr_sustain = 0.5f;
    inst->adsr_release = 0.5f;
    inst->fine_tune    = 0.5f;     /* 0.5 = 0 cents (unity) */
    inst->filter_cutoff = 0.5f;    /* 0.5 = CC=64 = open (no extra filter) */
    inst->filter_reso   = 0.5f;    /* 0.5 = CC=64 = Q at Butterworth */
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

    /* MOVE FORK: disable xsynth-core's per-key layer cap (default
     * Some(4)). For sample-based DS presets that intentionally stack
     * 5+ layers per key (e.g. WörliTzer: 10 regions at E5 across 6
     * groups), the layer cap silently drops voices and clipped the
     * authored sound. Performance is managed via the polyphony cap
     * (set_polyphony_cap, exposed as `voices` param) which limits
     * total active note-groups across the channel — the right knob
     * for tradeoffs. 0 → None means unlimited per key. */
    xshim_set_layer_count(inst->synth, 0);

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
            if (v < 1)   v = 1;
            if (v > 128) v = 128;
            inst->voices = v;
            inst->user_voices_override = 1;
        }
        /* MOVE FORK / 2026-05-19: restore saved ADSR knob positions. */
        if (json_get_number(json_defaults, "attack",  &f) == 0)
            inst->adsr_attack  = f < 0 ? 0 : (f > 1 ? 1 : f);
        if (json_get_number(json_defaults, "decay",   &f) == 0)
            inst->adsr_decay   = f < 0 ? 0 : (f > 1 ? 1 : f);
        if (json_get_number(json_defaults, "sustain", &f) == 0)
            inst->adsr_sustain = f < 0 ? 0 : (f > 1 ? 1 : f);
        if (json_get_number(json_defaults, "release", &f) == 0)
            inst->adsr_release = f < 0 ? 0 : (f > 1 ? 1 : f);
        if (json_get_number(json_defaults, "tune", &f) == 0)
            inst->fine_tune = f < 0 ? 0 : (f > 1 ? 1 : f);
        if (json_get_number(json_defaults, "cutoff", &f) == 0)
            inst->filter_cutoff = f < 0 ? 0 : (f > 1 ? 1 : f);
        if (json_get_number(json_defaults, "reso", &f) == 0)
            inst->filter_reso = f < 0 ? 0 : (f > 1 ? 1 : f);
    }

    /* Polyphony cap applies at the channel level, dropping oldest releasing
     * groups when the user-set N polyphony budget is exceeded. We push the
     * default (typically 64 → ~13 notes for a 5-layer WörliTzer, plenty for
     * a Bass patch) so even tracks that never touch the knob get a bounded
     * worst-case render time. */
    if (inst->synth) {
        xshim_set_polyphony_cap(inst->synth, (uint32_t)inst->voices);
        xshim_set_spawn_burst_limit(inst->synth, (uint32_t)inst->spawn_burst);
        /* Phase 9 prototype: install a fundsp reverb with sensible
         * defaults. Wet level defaults to 0 so existing presets are
         * unchanged until users opt in via the chain param. */
        xshim_set_reverb(inst->synth, 1, 20.0f, 3.0f, 0.5f);
        xshim_set_reverb_wet(inst->synth, 0.0f);
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
        if (v < 1)   v = 1;
        if (v > 128) v = 128;
        inst->voices = v;
        inst->user_voices_override = 1;
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
    } else if (strcmp(key, "cutoff") == 0 || strcmp(key, "reso") == 0) {
        /* MOVE FORK / 2026-05-19: global filter shaping via channel CCs.
         * UI -50..+50 maps to CC 0..127. cutoff → CC74, reso → CC71.
         * xsynth's channel control updates control_event_data.cutoff /
         * resonance; apply_channel_effects runs a biquad once per block
         * on the channel mix. Works on every preset, no per-region
         * cutoff= required. */
        float v = (float)atof(val);
        if (v < -50.0f) v = -50.0f;
        if (v >  50.0f) v =  50.0f;
        float t = (v + 50.0f) / 100.0f;
        uint8_t cc_num;
        if (strcmp(key, "cutoff") == 0) { cc_num = 74; inst->filter_cutoff = t; }
        else                            { cc_num = 71; inst->filter_reso   = t; }
        if (inst->synth) {
            uint8_t cc_val = (uint8_t)(t * 127.0f + 0.5f);
            xshim_cc(inst->synth, 0, cc_num, cc_val);
        }
    } else if (strcmp(key, "tune") == 0) {
        /* MOVE FORK / 2026-05-19: master fine tune. UI value is cents
         * directly (-100..+100, default 0). xsynth composes with MIDI
         * pitchbend in channel.process_pitch so external pitchbend
         * keeps working. */
        float cents = (float)atof(val);
        if (cents < -100.0f) cents = -100.0f;
        if (cents >  100.0f) cents =  100.0f;
        inst->fine_tune = (cents + 100.0f) / 200.0f;  /* keep 0..1 normalized
                                                         storage so re-push at
                                                         preset load uses one
                                                         formula */
        if (inst->synth) xshim_fine_tune(inst->synth, 0, cents);
    } else if (strcmp(key, "attack") == 0 || strcmp(key, "decay") == 0 ||
               strcmp(key, "sustain") == 0 || strcmp(key, "release") == 0) {
        /* MOVE FORK / 2026-05-19: global ADSR knobs. UI value is
         * -50..+50 with 0 = no change (preset's authored value).
         * Internally remapped to 0..1 → MIDI CC 0..127:
         *   attack  → CC73
         *   decay   → CC75
         *   sustain → CC79  (no standard CC for sustain; we claim 79)
         *   release → CC72
         * xsynth dispatches these to voice_control_data.envelope.* and
         * calls propagate_voice_controls → each held voice's
         * SIMDVoiceEnvelope.modify_envelope rebuilds its EnvelopeParameters.
         * Result: live ADSR on every preset, SFZ or DS, without
         * requiring author-defined knobs. CC=64 is the curve's identity
         * point — value 0 maps there exactly. */
        float v = (float)atof(val);
        if (v < -50.0f) v = -50.0f;
        if (v >  50.0f) v =  50.0f;
        float t = (v + 50.0f) / 100.0f;
        uint8_t cc_num;
        if      (strcmp(key, "attack")  == 0) { cc_num = 73; inst->adsr_attack  = t; }
        else if (strcmp(key, "decay")   == 0) { cc_num = 75; inst->adsr_decay   = t; }
        else if (strcmp(key, "sustain") == 0) { cc_num = 79; inst->adsr_sustain = t; }
        else                                  { cc_num = 72; inst->adsr_release = t; }
        if (inst->synth) {
            uint8_t cc_val = (uint8_t)(t * 127.0f + 0.5f);
            xshim_cc(inst->synth, 0, cc_num, cc_val);
        }
    } else if (strcmp(key, "funverb") == 0 || strcmp(key, "reverb") == 0) {
        /* Phase 9 prototype: fundsp reverb wet level (0..1). */
        float w = (float)atof(val);
        if (w < 0.0f) w = 0.0f;
        if (w > 1.0f) w = 1.0f;
        inst->reverb_wet = w;
        if (inst->synth) xshim_set_reverb_wet(inst->synth, w);
    } else if (strcmp(key, "knob_preset") == 0) {
        /* Phase 6.5: select which DS <tab> the encoder row mirrors.
         * Clamped to [0, tab_count - 1]. No xsynth interaction —
         * change is purely a UI re-filter on the next chain_params
         * read. */
        int v = atoi(val);
        if (v < 0) v = 0;
        if (inst->tab_count > 0 && v >= inst->tab_count) v = inst->tab_count - 1;
        inst->current_tab = v;
    } else if (strncmp(key, "knob_", 5) == 0) {
        /* knob_N — DS UI knob. Shell sends a normalized 0..1 fraction
         * (chain_params declares min=0,max=1,step=0.02 for every slot,
         * matching the OLD sfizz plugin's working pattern). Map back to
         * the DS author's [min,max] range, store as the canonical value,
         * forward to xsynth as a 0..127 CC. Live response is
         * opcode-dependent (ampeg_* responsive; others take effect on
         * next preset reload). */
        const char *tail = key + 5;
        if (tail[0] && (tail[0] >= '0' && tail[0] <= '9')) {
            int idx = atoi(tail);
            if (idx >= 0 && idx < inst->knob_count) {
                double t = atof(val);
                if (t < 0.0) t = 0.0; if (t > 1.0) t = 1.0;
                ds_knob_t *k = &inst->knobs[idx];
                inst->knob_current[idx] = k->min_value +
                                          t * (k->max_value - k->min_value);
                int cc_val = (int)(t * 127.0 + 0.5);
                if (cc_val < 0) cc_val = 0; if (cc_val > 127) cc_val = 127;
                if (inst->synth) xshim_cc(inst->synth, 0,
                                          (uint8_t)k->cc_number, (uint8_t)cc_val);
                /* Phase 10: route knobs whose DS binding is
                 * FX_REVERB_WET_LEVEL straight to the channel reverb. */
                if (k->reverb_wet && inst->synth) {
                    /* DS knob value is k->min_value..k->max_value; we
                     * already stored the absolute value above. Reverb
                     * expects 0..1 — use the same fraction t (knob is
                     * always normalized at the chain-param surface). */
                    float wet = (float)t;
                    if (wet < 0.0f) wet = 0.0f; if (wet > 1.0f) wet = 1.0f;
                    inst->reverb_wet = wet;
                    xshim_set_reverb_wet(inst->synth, wet);
                }
                /* Phase 9: route delay knob bindings. Absolute knob
                 * value (not the normalized fraction) is what the
                 * delay expects — seconds for time, 0..1 for fb/mix. */
                if (inst->synth && (k->delay_time || k->delay_feedback || k->delay_mix)) {
                    double abs_v = inst->knob_current[idx];
                    if (k->delay_time)     xshim_set_delay_time(inst->synth, (float)abs_v);
                    if (k->delay_feedback) xshim_set_delay_feedback(inst->synth, (float)abs_v);
                    if (k->delay_mix)      xshim_set_delay_mix(inst->synth, (float)abs_v);
                }
                /* Phase 12: route chorus knob bindings. */
                if (inst->synth && (k->chorus_rate || k->chorus_depth || k->chorus_mix)) {
                    double abs_v = inst->knob_current[idx];
                    if (k->chorus_rate)  xshim_set_chorus_rate(inst->synth, (float)abs_v);
                    if (k->chorus_depth) xshim_set_chorus_depth(inst->synth, (float)abs_v);
                    if (k->chorus_mix)   xshim_set_chorus_mix(inst->synth, (float)abs_v);
                }
                /* Phase 12: route phaser knob bindings. */
                if (inst->synth && (k->phaser_rate || k->phaser_depth ||
                                    k->phaser_feedback || k->phaser_mix)) {
                    double abs_v = inst->knob_current[idx];
                    if (k->phaser_rate)     xshim_set_phaser_rate(inst->synth, (float)abs_v);
                    if (k->phaser_depth)    xshim_set_phaser_depth(inst->synth, (float)abs_v);
                    if (k->phaser_feedback) xshim_set_phaser_feedback(inst->synth, (float)abs_v);
                    if (k->phaser_mix)      xshim_set_phaser_mix(inst->synth, (float)abs_v);
                }
                /* Phase 13: stereo widener (FX_STEREO_OFFSET). 0..1 →
                 * width 1..1.5 (default = neutral, full = subtle wider). */
                if (inst->synth && k->widener) {
                    double t = (k->max_value != k->min_value)
                        ? (inst->knob_current[idx] - k->min_value)
                          / (k->max_value - k->min_value)
                        : 0.0;
                    if (t < 0.0) t = 0.0; if (t > 1.0) t = 1.0;
                    xshim_set_widener(inst->synth, (float)(1.0 + 1.0 * t));
                }
            }
        }
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
            if (v < 1)   v = 1;
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
        /* Knob restore — happens after preset reload above so inst->knobs[]
         * is populated for the new preset. Saved values are 0..1 fractions. */
        for (int i = 0; i < inst->knob_count; i++) {
            char k[16];
            snprintf(k, sizeof(k), "knob_%d", i);
            if (json_get_number(val, k, &f) == 0) {
                double t = f;
                if (t < 0.0) t = 0.0; if (t > 1.0) t = 1.0;
                ds_knob_t *kn = &inst->knobs[i];
                inst->knob_current[i] = kn->min_value +
                                        t * (kn->max_value - kn->min_value);
                int cc_val = (int)(t * 127.0 + 0.5);
                if (cc_val < 0) cc_val = 0; if (cc_val > 127) cc_val = 127;
                if (inst->synth) xshim_cc(inst->synth, 0,
                                          (uint8_t)kn->cc_number,
                                          (uint8_t)cc_val);
            }
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
    /* Host polls this to re-fetch ui_hierarchy/chain_params after an
     * async preset switch finishes — set_preset_index defers the
     * actual SFZ convert + load by DEBOUNCE_BLOCKS audio blocks, so a
     * naive refetch right after setSlotParam() observes the previous
     * preset's knob list. */
    else if (strcmp(key, "is_loading") == 0) {
        return snprintf(buf, buf_len, "%d", (inst->is_loading || inst->pending_load) ? 1 : 0);
    }
    else if (strcmp(key, "preset") == 0 || strcmp(key, "current_patch") == 0)
        return snprintf(buf, buf_len, "%d", inst->current_preset);
    else if (strcmp(key, "preset_count") == 0 || strcmp(key, "total_patches") == 0)
        return snprintf(buf, buf_len, "%d", inst->preset_count);
    else if (strcmp(key, "preset_name") == 0 || strcmp(key, "patch_name") == 0) {
        /* During an async load (set_preset_index → apply), prepend a
         * 4-frame spinner so the preset browser shows the device is
         * still working through the 5-15s sample-read window heavy DS
         * libraries need. ~10 Hz spinner driven off CLOCK_MONOTONIC,
         * independent of UI poll rate. */
        if (inst->is_loading) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            int frame = (int)(((ts.tv_sec * 10) + (ts.tv_nsec / 100000000L)) & 3);
            static const char spin[4] = {'|', '/', '-', '\\'};
            /* The '|' glyph is roughly half the width of '/' '-' '\' in
             * the UI's proportional font, so it visually shifts the
             * following name when its frame is shown. Pad the '|' frame
             * with an extra trailing space — '|' + 2 spaces ≈ wide
             * glyph + 1 space in practice, keeping the name's column
             * stable. */
            const char *fmt = (spin[frame] == '|') ? "%c  %s" : "%c %s";
            return snprintf(buf, buf_len, fmt, spin[frame], inst->preset_name);
        }
        strncpy(buf, inst->preset_name, buf_len - 1);
        buf[buf_len - 1] = '\0';
        return strlen(buf);
    }
    else if (strcmp(key, "name") == 0)
        return snprintf(buf, buf_len, "Multisample");
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
    else if (strcmp(key, "attack")  == 0)
        return snprintf(buf, buf_len, "%.1f", inst->adsr_attack  * 100.0f - 50.0f);
    else if (strcmp(key, "decay")   == 0)
        return snprintf(buf, buf_len, "%.1f", inst->adsr_decay   * 100.0f - 50.0f);
    else if (strcmp(key, "sustain") == 0)
        return snprintf(buf, buf_len, "%.1f", inst->adsr_sustain * 100.0f - 50.0f);
    else if (strcmp(key, "release") == 0)
        return snprintf(buf, buf_len, "%.1f", inst->adsr_release * 100.0f - 50.0f);
    else if (strcmp(key, "tune") == 0)
        return snprintf(buf, buf_len, "%.1f", inst->fine_tune * 200.0f - 100.0f);
    else if (strcmp(key, "cutoff") == 0)
        return snprintf(buf, buf_len, "%.1f", inst->filter_cutoff * 100.0f - 50.0f);
    else if (strcmp(key, "reso") == 0)
        return snprintf(buf, buf_len, "%.1f", inst->filter_reso   * 100.0f - 50.0f);
    else if (strcmp(key, "knob_preset") == 0)
        return snprintf(buf, buf_len, "%d", inst->current_tab);
    else if (strcmp(key, "funverb") == 0 || strcmp(key, "reverb") == 0)
        return snprintf(buf, buf_len, "%.2f", inst->reverb_wet);
    else if (strcmp(key, "spawn_burst") == 0)
        return snprintf(buf, buf_len, "%d", inst->spawn_burst);
    else if (strncmp(key, "knob_", 5) == 0) {
        /* Return current position as 0..1 fraction (matches set_param's
         * input space). Unused slots and stale indices report 0. */
        int idx = atoi(key + 5);
        if (idx >= 0 && idx < inst->knob_count) {
            ds_knob_t *k = &inst->knobs[idx];
            double t = (k->max_value != k->min_value)
                ? (inst->knob_current[idx] - k->min_value) /
                  (k->max_value - k->min_value)
                : 0.0;
            if (t < 0.0) t = 0.0; if (t > 1.0) t = 1.0;
            return snprintf(buf, buf_len, "%g", t);
        }
        return snprintf(buf, buf_len, "0");
    }
    else if (strcmp(key, "chain_params") == 0) {
        /* FIXED SHAPE — always 4 built-ins + 8 knob slots, unused slots
         * padded with `name:"—"` placeholders. The OLD sfizz plugin proved
         * this is the only shape that doesn't freeze the Move shell: the
         * JS knob-context cache (shadow_ui.js:7935) keys by slot index,
         * so a shrinking array between preset switches leaves stale
         * bindings that loop on render. Stable count, only labels and
         * defaults vary per preset. Every knob uses normalized 0..1 with
         * step=0.02; set_param maps back to the DS author's range. */
        int written = 0;
        /* Phase 6.5: knob_preset = tab dropdown. Always present so the
         * encoder slot layout doesn't shrink between presets (which
         * would break the host's per-slot JS knob-context cache).
         * `max` clamps to tab_count - 1; presets with no tabs show a
         * disabled-feeling [0..0] dropdown. */
        int tab_max = (inst->tab_count > 1) ? (inst->tab_count - 1) : 0;
        /* chain_params is metadata sidecar — types/min/max for every
         * key the ui_hierarchy references. The hierarchy (defined in
         * get_param("ui_hierarchy") above) is what builds the menu. */
        written += snprintf(buf + written, buf_len - written,
            "[{\"key\":\"preset\",\"name\":\"Instrument\","
             "\"type\":\"int\",\"min\":0,\"max_param\":\"preset_count\","
             "\"default\":0},"
             "{\"key\":\"octave_transpose\",\"name\":\"Octave\","
             "\"type\":\"int\",\"min\":-4,\"max\":4,\"default\":0},"
             "{\"key\":\"gain\",\"name\":\"Gain\","
             "\"type\":\"float\",\"min\":0,\"max\":2,\"default\":0.7,\"step\":0.02},"
             "{\"key\":\"voices\",\"name\":\"Polyphony\","
             "\"type\":\"int\",\"min\":1,\"max\":128,\"default\":14},"
             "{\"key\":\"attack\",\"name\":\"Atk +/-\","
             "\"type\":\"float\",\"min\":-50,\"max\":50,\"default\":0,\"step\":1},"
             "{\"key\":\"decay\",\"name\":\"Dec +/-\","
             "\"type\":\"float\",\"min\":-50,\"max\":50,\"default\":0,\"step\":1},"
             "{\"key\":\"sustain\",\"name\":\"Sus +/-\","
             "\"type\":\"float\",\"min\":-50,\"max\":50,\"default\":0,\"step\":1},"
             "{\"key\":\"release\",\"name\":\"Rel +/-\","
             "\"type\":\"float\",\"min\":-50,\"max\":50,\"default\":0,\"step\":1},"
             "{\"key\":\"tune\",\"name\":\"Tune +/-\","
             "\"type\":\"float\",\"min\":-100,\"max\":100,\"default\":0,\"step\":1,\"unit\":\"\\u00a2\"},"
             "{\"key\":\"cutoff\",\"name\":\"Cutoff +/-\","
             "\"type\":\"float\",\"min\":-50,\"max\":50,\"default\":0,\"step\":1},"
             "{\"key\":\"reso\",\"name\":\"Reso +/-\","
             "\"type\":\"float\",\"min\":-50,\"max\":50,\"default\":0,\"step\":1},"
             "{\"key\":\"knob_preset\",\"name\":\"Knob Preset\","
             "\"type\":\"int\",\"min\":0,\"max\":%d,\"default\":0}",
             tab_max);
        /* Emit one chain_params entry per knob — the host needs both
         * ui_hierarchy AND chain_params metadata to render a knob with
         * a real label. Without a matching chain_params entry the
         * host falls back to a generic "Knob N" placeholder (same
         * symptom as the FUNVERB miss earlier in this branch). The
         * encoder row hardware limit only shows the first ~8 at once,
         * but the params menu walks the full list. */
        for (int i = 0; i < inst->knob_count; i++) {
            ds_knob_t *k = &inst->knobs[i];
            if (inst->tab_count > 1 && k->tab_idx != inst->current_tab) continue;
            double t = (k->max_value != k->min_value)
                ? (inst->knob_current[i] - k->min_value) /
                  (k->max_value - k->min_value)
                : 0.0;
            if (t < 0.0) t = 0.0; if (t > 1.0) t = 1.0;
            written += snprintf(buf + written, buf_len - written,
                ",{\"key\":\"%s\",\"name\":\"%s\","
                "\"type\":\"float\",\"min\":0,\"max\":1,"
                "\"default\":%g,\"step\":0.02,\"unit\":\"%%\"}",
                k->key, k->label, t);
        }
        written += snprintf(buf + written, buf_len - written, "]");
        return written;
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
        /* MOVE FORK / 2026-05-17: was re-scanning every request, which
         * walked 56 library dirs (~hundreds of dirs/files each) and
         * blew the host's 100ms shadow_param timeout — the host gave
         * up, JS got null, UI showed "No parameters". The instrument
         * list is established at load time and doesn't change at
         * runtime, so we just emit the cached list. */
        {
            char dmsg[128];
            snprintf(dmsg, sizeof(dmsg),
                "instrument_list req: buf_len=%d instruments=%d presets=%d",
                buf_len, inst->instrument_count, inst->preset_count);
            plugin_log(dmsg);
        }
        /* Format each entry into a local buffer first so we can check
         * whether it fits before committing. The previous version used
         * snprintf return values directly, but snprintf returns the size
         * that WOULD have been written — so `written` could exceed
         * `buf_len`, leaving truncated JSON (e.g. `[ent1,{"la\0`) that
         * the host couldn't parse, surfacing as "no parameters" in the
         * Jump To Library UI once the library count grew past ~30. */
        int written = snprintf(buf, buf_len, "[");
        if (written < 0 || written >= buf_len) return -1;
        char entry[MAX_NAME_LEN + 64];
        int emitted = 0;
        for (int i = 0; i < inst->instrument_count; i++) {
            /* JSON labels go through an HTML/template stage in the host
             * UI before parsing — raw `&` triggers an entity parse that
             * truncates the whole response (verified by bisecting:
             * cap=12 worked, cap=13 with "Fable & Clare" broke). Escape
             * &, <, >, ', " to their `\uXXXX` forms; JSON parsers
             * decode them transparently, but the HTML/template pre-pass
             * sees harmless backslash sequences. */
            char esc[MAX_NAME_LEN * 6 + 1];
            int ei = 0;
            for (int j = 0; inst->instruments[i].name[j] && ei + 6 < (int)sizeof(esc); j++) {
                unsigned char c = (unsigned char)inst->instruments[i].name[j];
                switch (c) {
                    case '"':  ei += snprintf(esc + ei, sizeof(esc) - ei, "\\u0022"); break;
                    case '\\': ei += snprintf(esc + ei, sizeof(esc) - ei, "\\u005c"); break;
                    case '&':  ei += snprintf(esc + ei, sizeof(esc) - ei, "\\u0026"); break;
                    case '<':  ei += snprintf(esc + ei, sizeof(esc) - ei, "\\u003c"); break;
                    case '>':  ei += snprintf(esc + ei, sizeof(esc) - ei, "\\u003e"); break;
                    case '\'': ei += snprintf(esc + ei, sizeof(esc) - ei, "\\u0027"); break;
                    default:
                        if (c < 0x20) {
                            ei += snprintf(esc + ei, sizeof(esc) - ei, "\\u%04x", c);
                        } else {
                            esc[ei++] = (char)c;
                        }
                }
            }
            esc[ei] = '\0';
            int entry_len = snprintf(entry, sizeof(entry),
                "%s{\"label\":\"%s\",\"index\":%d}",
                (i > 0) ? "," : "",
                esc, i);
            if (entry_len < 0) continue;
            if (entry_len > (int)sizeof(entry)) entry_len = (int)sizeof(entry) - 1;
            /* Need room for the entry plus the closing ']'. */
            if (written + entry_len + 1 >= buf_len) break;
            memcpy(buf + written, entry, entry_len);
            written += entry_len;
            emitted++;
        }
        buf[written++] = ']';
        if (written < buf_len) buf[written] = '\0';
        {
            char dmsg[256];
            snprintf(dmsg, sizeof(dmsg),
                "instrument_list resp: emitted=%d written=%d",
                emitted, written);
            plugin_log(dmsg);
            /* Dump full response in 200-byte chunks so we can recover
             * the entire JSON from debug.log. */
            char chunk[256];
            for (int off = 0; off < written; off += 200) {
                int n = written - off;
                if (n > 200) n = 200;
                int prefix = snprintf(chunk, sizeof(chunk),
                    "instrument_list dump[%d..%d]: ", off, off + n);
                int copy = n;
                if (prefix + copy + 1 > (int)sizeof(chunk))
                    copy = (int)sizeof(chunk) - prefix - 1;
                memcpy(chunk + prefix, buf + off, copy);
                chunk[prefix + copy] = '\0';
                plugin_log(chunk);
            }
        }
        return written;
    }
    else if (strcmp(key, "state") == 0) {
        /* Knob positions saved as normalized 0..1 fractions so they
         * restore cleanly even if a future build changes a knob's
         * underlying DS range. */
        int written = snprintf(buf, buf_len,
            "{\"preset_name\":\"%s\",\"instrument_name\":\"%s\","
            "\"preset\":%d,\"octave_transpose\":%d,\"gain\":%.2f,"
            "\"voices\":%d,\"spawn_burst\":%d",
            inst->preset_name, inst->instrument_name,
            inst->current_preset, inst->octave_transpose, inst->gain,
            inst->voices, inst->spawn_burst);
        for (int i = 0; i < inst->knob_count && written < buf_len - 32; i++) {
            ds_knob_t *k = &inst->knobs[i];
            double t = (k->max_value != k->min_value)
                ? (inst->knob_current[i] - k->min_value) /
                  (k->max_value - k->min_value)
                : 0.0;
            if (t < 0.0) t = 0.0; if (t > 1.0) t = 1.0;
            written += snprintf(buf + written, buf_len - written,
                                ",\"knob_%d\":%g", i, t);
        }
        written += snprintf(buf + written, buf_len - written, "}");
        return written;
    }
    else if (strcmp(key, "ui_hierarchy") == 0) {
        /* Encoder row: 8 fixed knob slots (knob_0..knob_7) — matches the
         * OLD sfizz plugin. Octave/gain/voices live in the params menu so
         * they don't fight the DS knobs for encoder real estate. Per-knob
         * entries with declared min/max are appended to params so the menu
         * shows author-meaningful ranges (labels override the chain_params
         * 0..1 normalized space at render time). */
        int written = 0;
        written += snprintf(buf + written, buf_len - written,
            "{\"modes\":null,\"levels\":{\"root\":{"
            "\"label\":\"SFZ\","
            "\"list_param\":\"preset\","
            "\"count_param\":\"preset_count\","
            "\"name_param\":\"preset_name\","
            "\"children\":null,"
            "\"knobs\":[");
        for (int i = 0; i < 8; i++) {
            written += snprintf(buf + written, buf_len - written,
                                "%s\"knob_%d\"", (i ? "," : ""), i);
        }
        written += snprintf(buf + written, buf_len - written,
            "],\"params\":["
                "{\"key\":\"octave_transpose\",\"label\":\"Octave\"},"
                "{\"key\":\"gain\",\"label\":\"Gain\"},"
                "{\"key\":\"voices\",\"label\":\"Polyphony\"},"
                "{\"key\":\"attack\",\"label\":\"Atk +/-\"},"
                "{\"key\":\"decay\",\"label\":\"Dec +/-\"},"
                "{\"key\":\"sustain\",\"label\":\"Sus +/-\"},"
                "{\"key\":\"release\",\"label\":\"Rel +/-\"},"
                "{\"key\":\"tune\",\"label\":\"Tune +/-\"},"
                "{\"key\":\"cutoff\",\"label\":\"Cutoff +/-\"},"
                "{\"key\":\"reso\",\"label\":\"Reso +/-\"},"
                "{\"key\":\"knob_preset\",\"label\":\"Knob Preset\"}");
        /* The host caches ui_hierarchy at module-load and never
         * re-queries it across preset switches, while chain_params
         * IS re-queried per preset. Emit ALL possible knob slots
         * (knob_0..knob_DS_MAX_KNOBS-1) here as placeholders; the
         * host drops any hierarchy entry whose key is missing from
         * the live chain_params metadata, so non-existent knobs for
         * the current preset disappear automatically. Result:
         * preset switches show the right knob set without
         * re-querying the hierarchy. */
        /* Per-preset dynamic hierarchy. Host's changeHierPreset
         * re-fetches ui_hierarchy + reloads the level when preset
         * changes within the menu (added 2026-05-15) so emitting only
         * the knobs that exist in the current preset/tab keeps the
         * menu clean. */
        for (int i = 0; i < inst->knob_count; i++) {
            ds_knob_t *k = &inst->knobs[i];
            if (inst->tab_count > 1 && k->tab_idx != inst->current_tab) continue;
            written += snprintf(buf + written, buf_len - written,
                ",{\"key\":\"%s\",\"label\":\"%s\"}",
                k->key, k->label);
        }
        written += snprintf(buf + written, buf_len - written,
            ",{\"level\":\"jump\",\"label\":\"Jump To Library\"}"
            "]},"
            "\"jump\":{"
                "\"label\":\"Library\","
                "\"items_param\":\"instrument_list\","
                "\"select_param\":\"bank_index\","
                "\"children\":null,"
                "\"knobs\":[],\"params\":[]"
            "}}}");
        return written;
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
        inst->is_loading = 0;
        /* MOVE FORK / 2026-05-18: per-preset polyphony default keyed to
         * region stacking. Move's audio engine sustains ~60 active voices
         * comfortably before the SCHED_FIFO 90 SPI thread starts missing
         * its 2902 µs deadline; above that, the host's "audio-dropouts:1"
         * counter ticks and we hear release-tail clicks even though SFZ's
         * own render is well under budget.
         *
         * The polyphony cap counts note-groups, so total voices ≈ groups
         * × layers-per-note. We compute layers-per-note from the loaded
         * soundfont's worst-case (key,vel) region stacking and pick a cap
         * that keeps total voices under VOICE_TARGET. Floors at 4 so
         * leads still feel responsive; never exceeds the historical 14
         * default. Respects user overrides (`set_param("voices", N)`
         * pins the value across preset changes).
         *
         * Reference points:
         *   Single-layer bass:           stacking=1 → cap=14 (full)
         *   4-mic piano (Salamander):    stacking=4 → cap=14 (full)
         *   8-layer pad:                 stacking=8 → cap=8
         *   16-mic EAP:                  stacking=~7 → cap=8-10 */
        if (!inst->user_voices_override) {
            /* MOVE FORK / 2026-05-19: if the SFZ author explicitly
             * declared `polyphony=N` in the file, honor it as the cap
             * (community report: monophonic SFZs ignored after the
             * heuristic landed). Author intent overrides the auto-
             * sizing — they know what their preset needs. */
            uint32_t declared = xshim_declared_polyphony(inst->synth);
            if (declared > 0) {
                int cap = (int)declared;
                if (cap < 1)   cap = 1;
                if (cap > 128) cap = 128;
                if (cap != inst->voices) {
                    inst->voices = cap;
                    xshim_set_polyphony_cap(inst->synth, (uint32_t)inst->voices);
                }
                char msg[160];
                snprintf(msg, sizeof(msg),
                         "polyphony: SFZ-declared polyphony=%u honored", declared);
                plugin_log(msg);
            } else {
                const int VOICE_TARGET = 60;   /* sustained-load ceiling */
                const int POLY_FLOOR   = 1;
                const int POLY_CEIL    = 14;
                uint32_t stacking = xshim_max_region_stacking(inst->synth);
                if (stacking < 1) stacking = 1;
                int recommended = VOICE_TARGET / (int)stacking;
                if (recommended < POLY_FLOOR) recommended = POLY_FLOOR;
                if (recommended > POLY_CEIL)  recommended = POLY_CEIL;
                if (recommended != inst->voices) {
                    inst->voices = recommended;
                    xshim_set_polyphony_cap(inst->synth, (uint32_t)inst->voices);
                }
                char msg[160];
                snprintf(msg, sizeof(msg),
                         "polyphony auto: stacking=%u target=%d -> voices=%d",
                         stacking, VOICE_TARGET, inst->voices);
                plugin_log(msg);
            }
        }
        /* MOVE FORK / 2026-05-19: re-push the current ADSR knob CCs
         * after the new soundfont applies. New voices spawned on this
         * preset should reflect the user's current knob positions, not
         * fall back to the engine's CC=0 default (= 0 ms attack & near-
         * zero release). Channel CC dispatch routes 73/75/79/72 into
         * voice_control_data.envelope.*. */
        if (inst->synth) {
            xshim_cc(inst->synth, 0, 73, (uint8_t)(inst->adsr_attack  * 127.0f + 0.5f));
            xshim_cc(inst->synth, 0, 75, (uint8_t)(inst->adsr_decay   * 127.0f + 0.5f));
            xshim_cc(inst->synth, 0, 79, (uint8_t)(inst->adsr_sustain * 127.0f + 0.5f));
            xshim_cc(inst->synth, 0, 72, (uint8_t)(inst->adsr_release * 127.0f + 0.5f));
            xshim_fine_tune(inst->synth, 0, (inst->fine_tune - 0.5f) * 200.0f);
            xshim_cc(inst->synth, 0, 74, (uint8_t)(inst->filter_cutoff * 127.0f + 0.5f));
            xshim_cc(inst->synth, 0, 71, (uint8_t)(inst->filter_reso   * 127.0f + 0.5f));
        }
        /* MOVE FORK / 2026-05-17: per-preset auto-gain. Compute an
         * attenuation factor from the soundfont's estimated worst-case
         * single-voice peak so hot SFZ libraries don't dominate the
         * output level. Model: aim for a 4-voice random-phase chord
         * (sqrt(4) × per_voice_peak) to land at ~-3 dBFS post-master.
         *   target = 0.7  (-3 dBFS)
         *   chord_peak ≈ master * preset * sqrt(4) * voice_peak
         *   → preset = target / (master * 2 * voice_peak)
         * Clamped to (0, 1]: quieter presets keep their original level.
         * Result for Salamander 44/16 (voice_peak ≈ 0.82):
         *   preset = 0.7 / (0.7 * 2 * 0.82) = 0.61 (-4.3 dB attenuation)
         * Result for a preset with voice_peak ≤ 0.5:
         *   preset = 1.0 (no change). */
        {
            float voice_peak = xshim_estimated_voice_peak(inst->synth);
            float pa = 1.0f;
            if (voice_peak > 0.5f && inst->gain > 0.0f) {
                pa = 0.7f / (inst->gain * 2.0f * voice_peak);
                if (pa > 1.0f) pa = 1.0f;
                /* 2026-05-17: lowered the floor from 0.05 to 0.001 after
                 * DS libraries like Mickleburgh exposed voice_peaks > 7000
                 * (author-set +8 dB group volume × +80 dB knob ceiling).
                 * 0.05 capped the attenuation at -26 dB; we need ~-80 dB
                 * to bring those peaks below unity. The new floor still
                 * keeps anything with voice_peak < 700 audible. */
                if (pa < 1e-4f) pa = 1e-4f;
            }
            inst->preset_attenuation = pa;
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "auto-gain: voice_peak=%.3f preset_attenuation=%.3f",
                     voice_peak, pa);
            plugin_log(msg);
        }
        /* MOVE FORK: push each knob's current CC value so the NEW
         * soundfont's voices observe the authored start position from
         * the first block. State-restore has its own resend path; this
         * covers fresh loads (e.g. switching to a different preset).
         * The voice-side SIMDVoiceOnccAmp generator samples the same
         * channel cc_state per render block, so once these CCs land
         * any volume_oncc<CC> bindings spawn at the correct amp. */
        for (int i = 0; i < inst->knob_count; i++) {
            ds_knob_t *k = &inst->knobs[i];
            double t = (k->max_value != k->min_value)
                ? (inst->knob_current[i] - k->min_value) /
                  (k->max_value - k->min_value)
                : 0.0;
            if (t < 0.0) t = 0.0; if (t > 1.0) t = 1.0;
            int cc_val = (int)(t * 127.0 + 0.5);
            if (cc_val < 0) cc_val = 0; if (cc_val > 127) cc_val = 127;
            xshim_cc(inst->synth, 0, (uint8_t)k->cc_number, (uint8_t)cc_val);
            /* Phase 9/10: knobs routed to channel effects need their
             * default value pushed at load, otherwise the effect
             * stays at the baked-in install defaults (which sounded
             * "wildly loud and cartoonish" on K4-Acoustic — DS
             * preset had no <effect> attrs, only knob value=). */
            double abs_v = inst->knob_current[i];
            if (k->reverb_wet) {
                inst->reverb_wet = (float)t;  /* fraction is the wet level */
                xshim_set_reverb_wet(inst->synth, (float)t);
            }
            if (k->delay_time)     xshim_set_delay_time(inst->synth, (float)abs_v);
            if (k->delay_feedback) xshim_set_delay_feedback(inst->synth, (float)abs_v);
            if (k->delay_mix)      xshim_set_delay_mix(inst->synth, (float)abs_v);
            if (k->chorus_rate)    xshim_set_chorus_rate(inst->synth, (float)abs_v);
            if (k->chorus_depth)   xshim_set_chorus_depth(inst->synth, (float)abs_v);
            if (k->chorus_mix)     xshim_set_chorus_mix(inst->synth, (float)abs_v);
            if (k->phaser_rate)    xshim_set_phaser_rate(inst->synth, (float)abs_v);
            if (k->phaser_depth)   xshim_set_phaser_depth(inst->synth, (float)abs_v);
            if (k->phaser_feedback) xshim_set_phaser_feedback(inst->synth, (float)abs_v);
            if (k->phaser_mix)     xshim_set_phaser_mix(inst->synth, (float)abs_v);
            if (k->widener)        xshim_set_widener(inst->synth, (float)(1.0 + 0.5 * t));
        }
        plugin_log("xsynth: async SFZ ready and applied");
    } else if (load_st == XSHIM_LOAD_ERROR) {
        char err[256] = {0};
        xshim_last_error(err, sizeof(err));
        snprintf(inst->load_error, sizeof(inst->load_error),
                 "xsynth load failed: %s", err[0] ? err : "unknown");
        /* MOVE FORK / 2026-05-16: also log to debug.log so we have a
         * persistent record of what went wrong. The UI-side error
         * surface (v2_get_error → inst->load_error) is transient and
         * only flagged on the next render block. */
        plugin_log(inst->load_error);
        xshim_load_clear_status(inst->synth);
        inst->is_loading = 0;
    } else if (load_st == XSHIM_LOAD_CANCELLED) {
        xshim_load_clear_status(inst->synth);
        inst->is_loading = 0;
    }

    /* MOVE FORK: SCHED_FIFO 50 disabled — when our render path spikes
     * to 100ms+ (e.g. rebuild_matrix on Splendid → Rhodes switch with
     * heavy soundfont teardown), the kernel can't preempt us at FIFO
     * priority and the ENTIRE device locks up (display, USB, network).
     * Running at normal priority means occasional audio glitches under
     * load, but the device stays responsive. We'd ALSO drop CPU
     * affinity — pinning to core 3 isn't worth it without RT priority. */

    /* Snapshot NoteOn count for THIS block before render. Splits perf log
     * into spawn-heavy (sequence chord lands) vs sustain-heavy (voices ring
     * under pedal) categories. */
    uint32_t noteons_this_block = xshim_take_noteon_count(inst->synth);
    uint64_t voices_before = xshim_voice_count(inst->synth);

    /* xsynth renders interleaved f32 stereo directly into render_buf. */
    struct timespec _t0, _t1, _t_sil, _t_clip0, _t_clip1;
    clock_gettime(CLOCK_MONOTONIC, &_t0);
    xshim_render(inst->synth, inst->render_buf, (size_t)(frames * 2));
    clock_gettime(CLOCK_MONOTONIC, &_t1);
    long render_us = (_t1.tv_sec - _t0.tv_sec) * 1000000L +
                     (_t1.tv_nsec - _t0.tv_nsec) / 1000L;

    /* Fast path: when the synth produced exact silence (idle render —
     * xsynth's ChannelGroup::render_to early-returned with a zeroed
     * buffer), memset the output and skip the tanh soft-clip loop
     * entirely. The per-sample tanhf is ~1.5-2 µs on ARM; 256 samples
     * × 2 ch was ~500 µs every block of an idle slot. This costs one
     * memcmp-like scan (~0.3 µs).
     *
     * We OR-reduce the f32 bit patterns: an all-zero render_buf has
     * every word == 0, so a non-zero OR result means real audio. This
     * catches denormals as non-silent (safe — we want to clip them),
     * and false-positives only on +/-0.0 which is harmless.
     */
    int silent_path;
    long silence_us, clip_us;
    {
        clock_gettime(CLOCK_MONOTONIC, &_t_sil);
        const uint32_t *bits = (const uint32_t *)inst->render_buf;
        size_t n = (size_t)frames * 2;
        uint32_t acc = 0;
        for (size_t i = 0; i < n; i++) acc |= bits[i];
        silent_path = (acc == 0);
        clock_gettime(CLOCK_MONOTONIC, &_t_clip0);
        silence_us = (_t_clip0.tv_sec - _t_sil.tv_sec) * 1000000L +
                     (_t_clip0.tv_nsec - _t_sil.tv_nsec) / 1000L;
        if (silent_path) {
            memset(out_interleaved_lr, 0, frames * 2 * sizeof(int16_t));
        }
    }

    /* MOVE FORK / 2026-05-16 v2: stateless soft-clip waveshaper.
     * Replaces the feedback limiter, which ducked attack transients
     * via instant-attack gain reduction → audibly compressed piano
     * hits (Salamander measured 3.3 dB louder attack vs DS reference,
     * pumping on release recovery).
     *
     * Curve: linear below `knee` (transparent — exact pass-through),
     *        smoothly approaches ±1.0 above via tanh on the overshoot.
     * No state, no timing artifacts. The cost: signals far above the
     * knee still get capped near ±1.0; that's the safety net for
     * accidental peaks (effect feedback, extreme dynamics).
     *
     * With master gain default lowered to 0.5, Salamander's raw
     * +2.74 dBFS chord peak lands at -3.3 dBFS post-gain — below
     * the 0.9 knee — so it passes through fully linear, matching DS
     * (-3.7 dBFS). Other presets that used to clip get the same
     * clean treatment; presets the user has tuned at 0.7 can be
     * recovered via the per-preset gain knob (+3 dB). */
    if (!silent_path) {
        clock_gettime(CLOCK_MONOTONIC, &_t_clip0);
        /* MOVE FORK / 2026-05-17: effective gain = master × per-preset
         * auto-attenuation. preset_attenuation is 1.0 for normal/quiet
         * presets (no change), ≤1.0 for hot SFZ libraries to bring them
         * into the same loudness ballpark as DecentSampler. */
        const float g = inst->gain * inst->preset_attenuation;
        const float knee = 0.9f;            /* -0.92 dBFS: transparent below */
        const float space = 1.0f - knee;    /* 0.1: smooth approach to ±1 above */
        for (int i = 0; i < frames; i++) {
            float l = inst->render_buf[i * 2]     * g;
            float r = inst->render_buf[i * 2 + 1] * g;
            /* Per-channel stateless soft-clip. */
            float al = l < 0.0f ? -l : l;
            if (al > knee) {
                float over = al - knee;
                float shaped = knee + space * tanhf(over / space);
                l = l < 0.0f ? -shaped : shaped;
            }
            float ar = r < 0.0f ? -r : r;
            if (ar > knee) {
                float over = ar - knee;
                float shaped = knee + space * tanhf(over / space);
                r = r < 0.0f ? -shaped : shaped;
            }
            /* tanh asymptotes to 1, so |l|,|r| < 1.0 mathematically.
             * Convert to i16. */
            out_interleaved_lr[i * 2]     = (int16_t)(l * 32767.0f);
            out_interleaved_lr[i * 2 + 1] = (int16_t)(r * 32767.0f);
        }
        clock_gettime(CLOCK_MONOTONIC, &_t_clip1);
        clip_us = (_t_clip1.tv_sec - _t_clip0.tv_sec) * 1000000L +
                  (_t_clip1.tv_nsec - _t_clip0.tv_nsec) / 1000L;
    } else {
        clip_us = 0;
    }

    /* Log perf when there was a spawn burst, render was heavy, or every
     * 256 blocks for baseline. Breakdown: par = event drain + per-key
     * render inside rayon. sum = serial mix of per-key bufs. fx = volume
     * + pan + cutoff sweep. tot = total inside push_key_events_and_render
     * (xsynth side). The (render - tot) gap is shim/render_to wrapping
     * overhead.
     *
     * 2026-05-16: added silent_path / silence_us / clip_us so we can
     * answer "did the OR-reduce fire?" and "what fraction of v2_render_
     * block went to tanhf vs xshim_render?" without re-deploying.
     *
     * Outlier capture: when v2_render_block exceeds 5 ms we also dump
     * voice state + last note number so we can correlate spikes against
     * note-on activity vs steady idle. */
    static int log_throttle = 0;
    long total_us = render_us + silence_us + clip_us;
    /* MOVE FORK / 2026-05-17: lowered outlier dump threshold from 5 ms
     * to 2 ms so we catch the host-reported 2-5 ms residual spikes
     * with full voice-state context. */
    int is_outlier = (total_us > 2000);
    /* MOVE FORK / 2026-05-16: ring-underrun count for this render block.
     * Sampled (and reset) every block so per-block counts are exact.
     * Non-zero means voices read past head into an unfilled ring slot
     * — audible discontinuity. */
    uint32_t underruns = xshim_take_underrun_count(inst->synth);
    uint32_t ubd[3] = {0};
    xshim_take_underrun_breakdown(inst->synth, ubd);
    /* MOVE FORK / 2026-05-17: tightened log trigger to keep SD-card
     * file I/O off the render thread during steady playback. The host
     * uses its own SPI frame counter for overrun detection; our log
     * only needs to fire when something interesting happens.
     *   - outlier: render exceeded 5 ms (real spike)
     *   - underruns > 0: ring starvation (audible glitch)
     *   - render > 2500 µs: ~85% of 2902 µs frame budget (near miss)
     *   - log_throttle every 2048 blocks (~6 s): baseline heartbeat
     * Dropped the per-noteon and render>800 µs triggers — those were
     * firing constantly during active play and adding ~600 µs of file
     * I/O per block. */
    if (is_outlier || underruns > 0 || render_us > 2500 ||
            ++log_throttle >= 2048) {
        log_throttle = 0;
        uint64_t voices_after = xshim_voice_count(inst->synth);
        uint32_t bd[4] = {0};
        xshim_take_render_breakdown(inst->synth, bd);
        char msg[320];
        snprintf(msg, sizeof(msg),
                 "perf: tot=%ld render=%ld sil=%ld clip=%ld silent=%d "
                 "vb=%llu va=%llu noteon=%u underrun=%u(ahead=%u,behind=%u,beforehead=%u) par=%u sum=%u fx=%u tot_in=%u",
                 total_us, render_us, silence_us, clip_us, silent_path,
                 (unsigned long long)voices_before,
                 (unsigned long long)voices_after,
                 (unsigned)noteons_this_block, (unsigned)underruns,
                 ubd[0], ubd[1], ubd[2],
                 bd[0], bd[1], bd[2], bd[3]);
        plugin_log(msg);
        if (is_outlier) {
            char om[160];
            snprintf(om, sizeof(om),
                     "outlier: tot=%ld us  vb=%llu va=%llu noteon=%u "
                     "silent=%d silence_us=%ld clip_us=%ld",
                     total_us,
                     (unsigned long long)voices_before,
                     (unsigned long long)voices_after,
                     (unsigned)noteons_this_block,
                     silent_path, silence_us, clip_us);
            plugin_log(om);
        }
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
