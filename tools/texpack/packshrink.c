// Makes a smaller copy of an HD texture pack for the PC port.
//
// usage: packshrink <pack folder> <output folder> [jpeg quality, default 90]
//
// Only the numbered images the game loads (0a3f.png, 0a3f.jpg) are copied; the
// pack's other folders are left out. A PNG with no transparency is rewritten as
// a JPG, which is several times smaller; one with transparency is copied as it
// is. The original pack is not touched.
//
// build: gcc -O2 -o packshrink tools/texpack/packshrink.c -Iport/include/external -lm
// (stb_image_write.h, from https://github.com/nothings/stb, goes next to this file)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static unsigned char *outbuf = NULL;
static size_t outlen = 0, outcap = 0;

static void collect(void *ctx, void *data, int size)
{
	if (outlen + size > outcap) {
		outcap = (outlen + size) * 2;
		outbuf = realloc(outbuf, outcap);
	}
	memcpy(outbuf + outlen, data, size);
	outlen += size;
}

static unsigned char *readfile(const char *path, long *size)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	*size = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *data = malloc(*size > 0 ? *size : 1);
	if (fread(data, 1, *size, f) != (size_t)*size) {
		free(data);
		data = NULL;
	}
	fclose(f);
	return data;
}

static int writefile(const char *path, const void *data, size_t size)
{
	FILE *f = fopen(path, "wb");
	if (!f) {
		return 0;
	}
	const int ok = fwrite(data, 1, size, f) == size;
	fclose(f);
	return ok;
}

// four hex digits and an image extension
static int numbered(const char *name, int *ispng)
{
	for (int i = 0; i < 4; ++i) {
		if (!isxdigit((unsigned char)name[i])) {
			return 0;
		}
	}
	*ispng = !strcasecmp(name + 4, ".png");
	return *ispng || !strcasecmp(name + 4, ".jpg") || !strcasecmp(name + 4, ".jpeg");
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: packshrink <pack folder> <output folder> [jpeg quality, default 90]\n");
		return 1;
	}
	const char *src = argv[1], *dst = argv[2];
	const int quality = argc > 3 ? atoi(argv[3]) : 90;

	DIR *d = opendir(src);
	if (!d) {
		fprintf(stderr, "cannot open %s\n", src);
		return 1;
	}
	mkdir(dst);

	// names first, so a JPG that has a PNG of the same number can be left out (the game prefers the PNG)
	static char names[8192][16];
	int count = 0;
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL && count < 8192) {
		int ispng;
		if (strlen(ent->d_name) < sizeof(names[0]) && numbered(ent->d_name, &ispng)) {
			strcpy(names[count++], ent->d_name);
		}
	}
	closedir(d);

	long long inbytes = 0, outbytes = 0;
	int converted = 0, keptpng = 0, keptjpg = 0, skipped = 0;
	double psnrsum = 0, psnrmin = 1000;
	char psnrminname[16] = "";

	for (int n = 0; n < count; ++n) {
		char in[2048], out[2048];
		int ispng = 0;
		numbered(names[n], &ispng);
		snprintf(in, sizeof(in), "%s/%s", src, names[n]);

		if (!ispng) {
			int dup = 0;
			for (int m = 0; m < count && !dup; ++m) {
				dup = m != n && !strncasecmp(names[m], names[n], 4) && !strcasecmp(names[m] + 4, ".png");
			}
			if (dup) {
				++skipped;
				continue;
			}
		}

		long size = 0;
		unsigned char *data = readfile(in, &size);
		if (!data) {
			fprintf(stderr, "cannot read %s\n", in);
			continue;
		}
		inbytes += size;

		int w = 0, h = 0, comp = 0, opaque = 0;
		unsigned char *rgba = NULL;
		if (ispng) {
			rgba = stbi_load_from_memory(data, (int)size, &w, &h, &comp, 4);
			if (rgba) {
				opaque = 1;
				for (size_t i = 0; i < (size_t)w * h && opaque; ++i) {
					opaque = rgba[i * 4 + 3] == 255;
				}
			}
		}

		if (opaque) {
			outlen = 0;
			stbi_write_jpg_to_func(collect, NULL, w, h, 4, rgba, quality);
			if (outlen > 0 && outlen < (size_t)size) {
				// how close the result is to the original
				int w2, h2, c2;
				unsigned char *back = stbi_load_from_memory(outbuf, (int)outlen, &w2, &h2, &c2, 4);
				if (back) {
					double err = 0;
					for (size_t i = 0; i < (size_t)w * h * 4; ++i) {
						if ((i & 3) != 3) {
							const double e = (double)rgba[i] - back[i];
							err += e * e;
						}
					}
					err /= (double)w * h * 3;
					const double psnr = err > 0 ? 10 * log10(255.0 * 255.0 / err) : 99;
					psnrsum += psnr;
					if (psnr < psnrmin) {
						psnrmin = psnr;
						strcpy(psnrminname, names[n]);
					}
					stbi_image_free(back);
				}
				snprintf(out, sizeof(out), "%s/%.4s.jpg", dst, names[n]);
				writefile(out, outbuf, outlen);
				outbytes += outlen;
				++converted;
				stbi_image_free(rgba);
				free(data);
				continue;
			}
		}

		// as it is
		snprintf(out, sizeof(out), "%s/%s", dst, names[n]);
		writefile(out, data, size);
		outbytes += size;
		if (ispng) {
			++keptpng;
		} else {
			++keptjpg;
		}
		if (rgba) {
			stbi_image_free(rgba);
		}
		free(data);
	}

	printf("%d images: %d PNG -> JPG (quality %d), %d PNG kept (transparency), %d JPG kept, %d JPG left out (PNG of the same number)\n",
		count, converted, quality, keptpng, keptjpg, skipped);
	printf("%.1f MB -> %.1f MB\n", inbytes / 1048576.0, outbytes / 1048576.0);
	if (converted) {
		printf("converted images vs originals: average PSNR %.1f dB, worst %.1f dB (%s)\n",
			psnrsum / converted, psnrmin, psnrminname);
	}
	return 0;
}
