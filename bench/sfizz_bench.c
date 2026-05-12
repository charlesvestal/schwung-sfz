/*
 * sfizz_bench: load an SFZ via sfizz, fire N voices, render M blocks of 128
 * frames, report µs/block. Direct comparison to xsynth_bench.
 *
 * Usage: ./sfizz_bench <path/to/file.sfz> <voices> <blocks>
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <sfizz.h>

#define FRAMES_PER_BLOCK 128
#define SAMPLE_RATE 44100

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

    sfizz_synth_t *synth = sfizz_create_synth();
    if (!synth) { fprintf(stderr, "sfizz_create_synth failed\n"); return 1; }
    sfizz_set_sample_rate(synth, (float)SAMPLE_RATE);
    sfizz_set_samples_per_block(synth, FRAMES_PER_BLOCK);
    sfizz_set_num_voices(synth, 128);

    long t0 = now_ns();
    if (!sfizz_load_file(synth, sfz)) {
        fprintf(stderr, "load failed: %s\n", sfz);
        return 1;
    }
    long t_load_ms = (now_ns() - t0) / 1000000L;
    fprintf(stderr, "loaded in %ld ms\n", t_load_ms);

    float left[FRAMES_PER_BLOCK];
    float right[FRAMES_PER_BLOCK];
    float *channels[2] = { left, right };

    for (int i = 0; i < 8; i++)
        sfizz_render_block(synth, channels, 2, FRAMES_PER_BLOCK);

    int base = 36;
    for (int v = 0; v < voices; v++) {
        sfizz_send_note_on(synth, 0, base + v, 100);
    }

    sfizz_render_block(synth, channels, 2, FRAMES_PER_BLOCK);

    long *times = malloc(sizeof(long) * blocks);
    if (!times) return 1;
    long total_ns = 0;
    for (int i = 0; i < blocks; i++) {
        long a = now_ns();
        sfizz_render_block(synth, channels, 2, FRAMES_PER_BLOCK);
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

    printf("sfizz  voices=%d blocks=%d  "
           "mean=%ld µs  p50=%ld  p95=%ld  p99=%ld  max=%ld\n",
           voices, blocks,
           mean / 1000, p50 / 1000, p95 / 1000, p99 / 1000, worst / 1000);

    free(times);
    return 0;
}
