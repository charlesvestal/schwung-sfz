/*
 * xsynth_bench: load an SFZ via xsynth, fire N voices on channel 0, render
 * M blocks of 128 frames, report µs/block. Apples-to-apples vs sfizz_bench.
 *
 * Usage: ./xsynth_bench <path/to/file.sfz> <voices> <blocks>
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define FRAMES_PER_BLOCK 128
#define SAMPLE_RATE 44100
#define INTERLEAVED_F32_PER_BLOCK (FRAMES_PER_BLOCK * 2)

typedef struct XSynthHandle XSynthHandle;
extern XSynthHandle* xshim_create(uint32_t sample_rate, uint32_t channels);
extern void xshim_destroy(XSynthHandle*);
extern int  xshim_load_sfz(XSynthHandle*, const char *path);
extern void xshim_note_on(XSynthHandle*, uint8_t ch, uint8_t key, uint8_t vel);
extern void xshim_note_off(XSynthHandle*, uint8_t ch, uint8_t key);
extern void xshim_render(XSynthHandle*, float *out, size_t num_samples);
extern uint64_t xshim_voice_count(const XSynthHandle*);

static long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000L + ts.tv_nsec;
}

static int cmp_long(const void *a, const void *b) {
    long x = *(const long*)a, y = *(const long*)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <sfz> <voices> <blocks>\n", argv[0]);
        return 1;
    }
    const char *sfz = argv[1];
    int voices = atoi(argv[2]);
    int blocks = atoi(argv[3]);
    if (voices < 1 || voices > 127 || blocks < 1) {
        fprintf(stderr, "bad args\n");
        return 1;
    }

    XSynthHandle *h = xshim_create(SAMPLE_RATE, 2);
    if (!h) { fprintf(stderr, "xshim_create failed\n"); return 1; }

    long t0 = now_ns();
    if (xshim_load_sfz(h, sfz) != 0) {
        fprintf(stderr, "load failed: %s\n", sfz);
        xshim_destroy(h);
        return 1;
    }
    long t_load_ms = (now_ns() - t0) / 1000000L;
    fprintf(stderr, "loaded in %ld ms\n", t_load_ms);

    float buf[INTERLEAVED_F32_PER_BLOCK];

    /* Pre-roll: render a few blocks before notes to warm caches. */
    for (int i = 0; i < 8; i++) xshim_render(h, buf, INTERLEAVED_F32_PER_BLOCK);

    /* Fire `voices` notes at moderate velocity, spread across a chord. */
    int base = 36; /* C2 */
    for (int v = 0; v < voices; v++) {
        uint8_t key = (uint8_t)(base + v);
        xshim_note_on(h, 0, key, 100);
    }

    /* Warm-render one block so attack happens before timed loop. */
    xshim_render(h, buf, INTERLEAVED_F32_PER_BLOCK);

    /* Timed loop. Record per-block µs. */
    long *times = malloc(sizeof(long) * blocks);
    if (!times) { xshim_destroy(h); return 1; }
    long total_ns = 0;
    for (int i = 0; i < blocks; i++) {
        long a = now_ns();
        xshim_render(h, buf, INTERLEAVED_F32_PER_BLOCK);
        long d = now_ns() - a;
        times[i] = d;
        total_ns += d;
    }

    qsort(times, blocks, sizeof(long), cmp_long);
    long p50 = times[blocks / 2];
    long p95 = times[(blocks * 95) / 100];
    long p99 = times[(blocks * 99) / 100];
    long worst = times[blocks - 1];
    long mean = total_ns / blocks;
    uint64_t vc = xshim_voice_count(h);

    printf("xsynth voices=%d blocks=%d active=%llu  "
           "mean=%ld µs  p50=%ld  p95=%ld  p99=%ld  max=%ld\n",
           voices, blocks, (unsigned long long)vc,
           mean / 1000, p50 / 1000, p95 / 1000, p99 / 1000, worst / 1000);

    free(times);
    xshim_destroy(h);
    return 0;
}
