/* Quick converter inspector — emit the SFZ that convert_dspreset_to_xsynth_sfz
 * produces for a given .dspreset, without spinning up xsynth. Mac-only test
 * harness; not compiled in the on-device build. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dspreset_to_xsynth_sfz.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <preset.dspreset>\n", argv[0]);
        return 2;
    }
    char *path = convert_dspreset_to_xsynth_sfz(argv[1], NULL, NULL, NULL, NULL,
                                                NULL, NULL, NULL, NULL);
    if (!path) { fprintf(stderr, "convert failed\n"); return 1; }
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); return 1; }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) fwrite(buf, 1, n, stdout);
    fclose(f);
    fprintf(stderr, "(emitted: %s)\n", path);
    return 0;
}
