/*
 * sfz_render — convert a .dspreset, play a MIDI sequence through
 * xsynth, and write the rendered stereo audio to a WAV file. Mac-side
 * harness used to A/B against DecentSampler's own AU plugin so we can
 * ground-truth the converter.
 *
 *   ./sfz_render <preset.dspreset> <output.wav> [note=60] [vel=100] \
 *                [duration_s=2.0] [tail_s=2.0] [rate=44100]
 *
 * Builds:
 *   cc -O2 sfz_render.c \
 *       ../src/dsp/dspreset_to_xsynth_sfz.c \
 *       ../src/dsp/third_party/xsynth_shim/target/release/libxsynth_shim.a \
 *       -I../src/dsp -lm -o sfz_render
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "dspreset_to_xsynth_sfz.h"

typedef struct XSynthHandle XSynthHandle;
extern XSynthHandle *xshim_create(uint32_t sample_rate, uint32_t channels);
extern void          xshim_destroy(XSynthHandle*);
extern int           xshim_load_sfz_async(XSynthHandle*, const char *path);
extern int           xshim_load_status(const XSynthHandle*);
extern int           xshim_load_apply(XSynthHandle*);
extern void          xshim_load_clear_status(XSynthHandle*);
extern void          xshim_note_on(XSynthHandle*, uint8_t ch, uint8_t key, uint8_t vel);
extern void          xshim_note_off(XSynthHandle*, uint8_t ch, uint8_t key);
extern void          xshim_cc(XSynthHandle*, uint8_t ch, uint8_t cc, uint8_t val);
extern void          xshim_render(XSynthHandle*, float *out, size_t num_samples);
extern size_t        xshim_last_error(char *out, size_t len);
extern void          xshim_set_polyphony_cap(XSynthHandle*, uint32_t cap);
extern uint64_t      xshim_voice_count(const XSynthHandle*);

/* xsynth FX install (so .dspreset reverb/delay/chorus/phaser sound
 * the way the plugin renders them on Move). */
extern void xshim_set_reverb(XSynthHandle*, uint32_t enable,
                             float room, float time, float damp);
extern void xshim_set_reverb_wet(XSynthHandle*, float wet);
extern void xshim_set_delay(XSynthHandle*, uint32_t enable,
                            float time, float feedback);
extern void xshim_set_delay_mix(XSynthHandle*, float mix);
extern void xshim_set_chorus(XSynthHandle*, uint32_t enable,
                             float rate, float depth);
extern void xshim_set_chorus_mix(XSynthHandle*, float mix);
extern void xshim_set_phaser(XSynthHandle*, uint32_t enable,
                             float rate, float depth, float feedback);
extern void xshim_set_phaser_mix(XSynthHandle*, float mix);

/* Block size (samples per render call) must match what xsynth was
 * tuned for. The Move host calls at 128; matching here keeps any
 * block-size-dependent rounding identical. */
#define BLOCK_FRAMES 128

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s <preset.dspreset> <out.wav>"
            " [note=60] [vel=100] [duration_s=2.0]"
            " [tail_s=2.0] [rate=44100]\n",
            argv0);
}

static void write_wav_header(FILE *f, uint32_t sample_rate,
                             uint32_t num_frames) {
    uint32_t num_samples = num_frames * 2;            /* stereo */
    uint32_t byte_rate   = sample_rate * 2 * 2;       /* 16-bit stereo */
    uint32_t data_bytes  = num_samples * 2;
    uint16_t one         = 1;
    uint16_t chans       = 2;
    uint16_t bits        = 16;
    uint16_t blk_align   = 4;
    uint32_t fmt_size    = 16;
    uint32_t riff_size   = 36 + data_bytes;
    fwrite("RIFF", 1, 4, f);
    fwrite(&riff_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    fwrite(&fmt_size, 4, 1, f);
    fwrite(&one, 2, 1, f);              /* PCM */
    fwrite(&chans, 2, 1, f);
    fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&blk_align, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data_bytes, 4, 1, f);
}

static int16_t to_i16(float s) {
    if (s >  1.0f) s =  1.0f;
    if (s < -1.0f) s = -1.0f;
    return (int16_t)(s * 32767.0f);
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(argv[0]); return 1; }

    const char *preset_path = argv[1];
    const char *out_path    = argv[2];
    int   note      = argc > 3 ? atoi(argv[3]) : 60;
    int   vel       = argc > 4 ? atoi(argv[4]) : 100;
    float dur_s     = argc > 5 ? atof(argv[5]) : 2.0f;
    float tail_s    = argc > 6 ? atof(argv[6]) : 2.0f;
    uint32_t rate   = argc > 7 ? (uint32_t)atoi(argv[7]) : 44100;

    /* If the input is a .sfz file, load it directly. Otherwise treat
     * as a .dspreset and run the converter. */
    ds_knob_t  knobs[DS_MAX_KNOBS] = {0};
    ds_tab_t   tabs[DS_MAX_TABS]   = {0};
    ds_reverb_cfg_t rev = {0};
    ds_delay_cfg_t  del = {0};
    ds_chorus_cfg_t cho = {0};
    ds_phaser_cfg_t pha = {0};
    int knob_count = 0, tab_count = 0;
    char *sfz_path = NULL;
    const char *ext = strrchr(preset_path, '.');
    if (ext && strcasecmp(ext, ".sfz") == 0) {
        sfz_path = strdup(preset_path);
        fprintf(stderr, "[sfz_render] direct SFZ load: %s\n", sfz_path);
    } else {
        sfz_path = convert_dspreset_to_xsynth_sfz(
            preset_path, knobs, &knob_count, tabs, &tab_count,
            &rev, &del, &cho, &pha);
        if (!sfz_path) {
            fprintf(stderr, "convert_dspreset_to_xsynth_sfz failed\n");
            return 2;
        }
        fprintf(stderr, "[sfz_render] converted: %s (%d knobs, %d tabs)\n",
                sfz_path, knob_count, tab_count);
    }

    /* Step 2: spin up xsynth, load the converted SFZ. */
    XSynthHandle *synth = xshim_create(rate, 2);
    if (!synth) { fprintf(stderr, "xshim_create failed\n"); return 3; }
    xshim_set_polyphony_cap(synth, 32);

    if (xshim_load_sfz_async(synth, sfz_path) != 0) {
        char err[256] = {0};
        xshim_last_error(err, sizeof(err));
        fprintf(stderr, "load_sfz_async: %s\n", err[0] ? err : "unknown");
        return 4;
    }
    /* Spin until ready. */
    for (int i = 0; i < 1000; i++) {
        int st = xshim_load_status(synth);
        if (st == 2 /* Ready */) { xshim_load_apply(synth); break; }
        if (st == 3 /* Error */ || st == 4 /* Cancelled */) {
            char err[256] = {0};
            xshim_last_error(err, sizeof(err));
            fprintf(stderr, "load failed: %s\n", err[0] ? err : "unknown");
            return 5;
        }
        usleep(10000);
    }

    /* Step 3: install dspreset-declared FX (mirror plugin's flow). */
    if (rev.enabled) {
        float rs = (float)rev.room_size;
        if (rs < 0) rs = 0; if (rs > 1) rs = 1;
        float room = rs * 15.0f + 3.0f;
        float t    = rs * 1.8f + 0.4f;
        float damp = (float)rev.damping;
        if (damp < 0) damp = 0; if (damp > 1) damp = 1;
        xshim_set_reverb(synth, 1, room, t, damp);
        float wet = (float)rev.wet_level;
        if (wet < 0) wet = 0; if (wet > 1) wet = 1;
        xshim_set_reverb_wet(synth, wet);
    }
    if (del.enabled) {
        float t  = (float)del.delay_seconds;
        if (t < 0.001f) t = 0.001f; if (t > 2) t = 2;
        float fb = (float)del.feedback;
        if (fb < 0) fb = 0; if (fb > 0.95f) fb = 0.95f;
        float mx = (float)del.mix;
        if (mx < 0) mx = 0; if (mx > 1) mx = 1;
        xshim_set_delay(synth, 1, t, fb);
        xshim_set_delay_mix(synth, mx);
    }
    if (cho.enabled) {
        xshim_set_chorus(synth, 1, (float)cho.rate, (float)cho.depth);
        float cmx = (float)cho.mix;
        if (cmx < 0) cmx = 0; if (cmx > 1) cmx = 1;
        xshim_set_chorus_mix(synth, cmx);
    }
    if (pha.enabled) {
        xshim_set_phaser(synth, 1, (float)pha.rate, (float)pha.depth,
                         (float)pha.feedback);
        float pmx = (float)pha.mix;
        if (pmx < 0) pmx = 0; if (pmx > 1) pmx = 1;
        xshim_set_phaser_mix(synth, pmx);
    }

    /* Step 4: push each knob's default CC value so voices observe the
     * authored start positions (same as the plugin does at load). */
    for (int i = 0; i < knob_count; i++) {
        ds_knob_t *k = &knobs[i];
        double t = (k->max_value != k->min_value)
            ? (k->default_value - k->min_value) / (k->max_value - k->min_value)
            : 0.0;
        if (t < 0) t = 0; if (t > 1) t = 1;
        int cc_val = (int)(t * 127.0 + 0.5);
        if (k->cc_number >= 0) {
            xshim_cc(synth, 0, (uint8_t)k->cc_number, (uint8_t)cc_val);
        }
        double abs_v = k->default_value;
        if (k->reverb_wet)      xshim_set_reverb_wet(synth, (float)t);
        if (k->delay_mix)       xshim_set_delay_mix(synth, (float)abs_v);
        if (k->chorus_mix)      xshim_set_chorus_mix(synth, (float)abs_v);
        if (k->phaser_mix)      xshim_set_phaser_mix(synth, (float)abs_v);
    }

    /* Step 5: render. NoteOn, dur_s of sustain, NoteOff, tail_s release. */
    uint32_t note_frames = (uint32_t)(dur_s  * rate);
    uint32_t tail_frames = (uint32_t)(tail_s * rate);
    uint32_t total_frames = note_frames + tail_frames;

    FILE *wf = fopen(out_path, "wb");
    if (!wf) { perror("fopen"); return 6;}
    write_wav_header(wf, rate, total_frames);

    xshim_note_on(synth, 0, (uint8_t)note, (uint8_t)vel);

    float  buf_f[BLOCK_FRAMES * 2];
    int16_t buf_i[BLOCK_FRAMES * 2];
    uint32_t rendered = 0;
    int note_released = 0;
    while (rendered < total_frames) {
        if (!note_released && rendered >= note_frames) {
            xshim_note_off(synth, 0, (uint8_t)note);
            note_released = 1;
        }
        uint32_t want = total_frames - rendered;
        if (want > BLOCK_FRAMES) want = BLOCK_FRAMES;
        xshim_render(synth, buf_f, BLOCK_FRAMES * 2);
        for (uint32_t i = 0; i < want * 2; i++) buf_i[i] = to_i16(buf_f[i]);
        fwrite(buf_i, 2, want * 2, wf);
        rendered += want;
    }

    fclose(wf);
    xshim_destroy(synth);
    free(sfz_path);
    fprintf(stderr, "[sfz_render] wrote %u frames to %s\n",
            total_frames, out_path);
    return 0;
}
