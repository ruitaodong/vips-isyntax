/* isyntax-crop: reference reader for testing vips-isyntax. Reads one region straight through
 * libisyntax (libisyntax_read_region, RGBA) and writes it as raw RGB bytes, nothing else.
 *   isyntax-crop slide.isyntax LEVEL X Y W H out.rgb
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "libisyntax.h"

int main(int argc, char **argv) {
	if (argc != 8) { fprintf(stderr, "usage: %s slide.isyntax LEVEL X Y W H out.rgb\n", argv[0]); return 2; }
	int level = atoi(argv[2]); long long x = atoll(argv[3]), y = atoll(argv[4]), w = atoll(argv[5]), h = atoll(argv[6]);
	isyntax_t *s; isyntax_cache_t *c;
	if (libisyntax_init() || libisyntax_open(argv[1], 0, &s)) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
	if (libisyntax_cache_create("crop", 2000, &c) || libisyntax_cache_inject(c, s)) { fprintf(stderr, "cache failed\n"); return 1; }
	uint32_t *px = malloc((size_t) w * h * 4); uint8_t *rgb = malloc((size_t) w * h * 3);
	if (!px || !rgb) { fprintf(stderr, "out of memory\n"); return 1; }
	if (libisyntax_read_region(s, c, level, x, y, w, h, px, LIBISYNTAX_PIXEL_FORMAT_RGBA)) { fprintf(stderr, "read_region failed\n"); return 1; }
	const uint8_t *p = (const uint8_t *) px;
	for (size_t i = 0; i < (size_t) w * h; i++) { rgb[3*i] = p[4*i]; rgb[3*i+1] = p[4*i+1]; rgb[3*i+2] = p[4*i+2]; }
	FILE *f = fopen(argv[7], "wb"); if (!f || fwrite(rgb, 1, (size_t) w * h * 3, f) != (size_t) w * h * 3) { fprintf(stderr, "write failed\n"); return 1; }
	fclose(f); libisyntax_cache_destroy(c); libisyntax_close(s); return 0;
}
