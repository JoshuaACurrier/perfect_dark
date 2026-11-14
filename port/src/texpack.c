// HD texture packs, see texpack.h.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "fs.h"
#include "system.h"
#include "texpack.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "external/stb_image.h"

#define TEXPACK_MAX_TEXTURES 0x1000 // texture numbers are 12 bits
#define TEXPACK_DIR "texture-packs"
#define TEXPACK_REG_SIZE 8192 // a power of two, well over the number of textures a stage loads

s32 g_TexpackEnabled = 1;
char g_TexpackName[TEXPACK_MAX_NAME] = "";

// file for each texture number, relative to packdir; "" if the pack has none
static char (*packfiles)[16] = NULL;
static char packdir[FS_MAXPATH + 1];
static s32 packcount = 0;
static s32 packscanned = 0;

// which texture number the texels at an address are
static struct { const void *data; s32 texnum; } registry[TEXPACK_REG_SIZE];

static s32 texpackIsDir(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// "0a3f.png" -> 0x0a3f; -1 if the name isn't four hex digits and an image extension
static s32 texpackParseName(const char *name)
{
	s32 num = 0;
	for (s32 i = 0; i < 4; ++i) {
		const char c = name[i];
		if (!isxdigit((unsigned char)c)) {
			return -1;
		}
		num = num * 16 + (isdigit((unsigned char)c) ? c - '0' : (tolower((unsigned char)c) - 'a' + 10));
	}
	const char *ext = name + 4;
	if (strcasecmp(ext, ".png") && strcasecmp(ext, ".jpg") && strcasecmp(ext, ".jpeg")) {
		return -1;
	}
	return num;
}

// indexes the images in dir; returns how many it found
static s32 texpackScanDir(const char *dir)
{
	DIR *d = opendir(dir);
	if (!d) {
		return 0;
	}

	s32 count = 0;
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		const s32 num = texpackParseName(ent->d_name);
		if (num < 0 || num >= TEXPACK_MAX_TEXTURES || strlen(ent->d_name) >= sizeof(packfiles[0])) {
			continue;
		}
		// a PNG wins over a JPG of the same texture (it may have transparency)
		if (packfiles[num][0] && !strcasecmp(packfiles[num] + 4, ".png")) {
			continue;
		}
		if (!packfiles[num][0]) {
			++count;
		}
		strcpy(packfiles[num], ent->d_name);
	}

	closedir(d);
	return count;
}

static void texpackScan(void)
{
	packscanned = 1;
	packcount = 0;
	packdir[0] = '\0';

	if (!packfiles) {
		packfiles = calloc(TEXPACK_MAX_TEXTURES, sizeof(*packfiles));
		if (!packfiles) {
			return;
		}
	}
	memset(packfiles, 0, TEXPACK_MAX_TEXTURES * sizeof(*packfiles));

	// texture-packs/ next to the game data, the save folder or the exe
	static const char *roots[] = { "$B/" TEXPACK_DIR, "$S/" TEXPACK_DIR, "$E/" TEXPACK_DIR };

	for (s32 r = 0; r < (s32)(sizeof(roots) / sizeof(roots[0])) && !packcount; ++r) {
		char root[FS_MAXPATH + 1];
		snprintf(root, sizeof(root), "%s", fsFullPath(roots[r]));
		if (root[0] == '$' || !texpackIsDir(root)) {
			continue;
		}

		if (g_TexpackName[0]) {
			// the one the player named
			snprintf(packdir, sizeof(packdir), "%s/%s", root, g_TexpackName);
			packcount = texpackScanDir(packdir);
			continue;
		}

		// otherwise the first folder with textures in it (in name order), or the root itself
		DIR *d = opendir(root);
		if (!d) {
			continue;
		}
		char best[TEXPACK_MAX_NAME] = "";
		struct dirent *ent;
		while ((ent = readdir(d)) != NULL) {
			if (ent->d_name[0] == '.' || strlen(ent->d_name) >= sizeof(best)) {
				continue;
			}
			char sub[FS_MAXPATH + 1];
			snprintf(sub, sizeof(sub), "%s/%s", root, ent->d_name);
			if (texpackIsDir(sub) && (!best[0] || strcasecmp(ent->d_name, best) < 0)) {
				memset(packfiles, 0, TEXPACK_MAX_TEXTURES * sizeof(*packfiles));
				if (texpackScanDir(sub) > 0) {
					snprintf(best, sizeof(best), "%s", ent->d_name);
				}
			}
		}
		closedir(d);

		memset(packfiles, 0, TEXPACK_MAX_TEXTURES * sizeof(*packfiles));
		if (best[0]) {
			snprintf(packdir, sizeof(packdir), "%s/%s", root, best);
		} else {
			snprintf(packdir, sizeof(packdir), "%s", root);
		}
		packcount = texpackScanDir(packdir);
	}

	if (packcount > 0) {
		sysLogPrintf(LOG_NOTE, "texpack: using %s (%d textures)", packdir, packcount);
	} else {
		sysLogPrintf(LOG_NOTE, "texpack: no texture pack found in " TEXPACK_DIR "/");
	}
}

void texpackRescan(void)
{
	packscanned = 0;
}

s32 texpackGetNumTextures(void)
{
	if (!packscanned) {
		texpackScan();
	}
	return packcount;
}

s32 texpackActive(void)
{
	return g_TexpackEnabled && texpackGetNumTextures() > 0;
}

/* address -> texture number */

static inline u32 texpackHash(const void *data)
{
	return (u32)(((uintptr_t)data >> 3) * 2654435761u) & (TEXPACK_REG_SIZE - 1);
}

void texpackRegisterTexture(const void *data, s32 texnum)
{
	if (!data) {
		return;
	}
	u32 i = texpackHash(data);
	for (s32 n = 0; n < TEXPACK_REG_SIZE; ++n, i = (i + 1) & (TEXPACK_REG_SIZE - 1)) {
		if (!registry[i].data || registry[i].data == data) {
			registry[i].data = data;
			registry[i].texnum = texnum;
			return;
		}
	}
}

void texpackForgetAll(void)
{
	memset(registry, 0, sizeof(registry));
}

s32 texpackGetTextureNum(const void *data)
{
	if (!data) {
		return -1;
	}
	u32 i = texpackHash(data);
	for (s32 n = 0; n < TEXPACK_REG_SIZE; ++n, i = (i + 1) & (TEXPACK_REG_SIZE - 1)) {
		if (!registry[i].data) {
			return -1;
		}
		if (registry[i].data == data) {
			return registry[i].texnum;
		}
	}
	return -1;
}

/* loading */

u8 *texpackLoadReplacement(const void *data, s32 *outWidth, s32 *outHeight)
{
	if (!texpackActive()) {
		return NULL;
	}

	const s32 texnum = texpackGetTextureNum(data);
	if (texnum < 0 || texnum >= TEXPACK_MAX_TEXTURES || !packfiles[texnum][0]) {
		return NULL;
	}

	char path[FS_MAXPATH + 1];
	snprintf(path, sizeof(path), "%s/%s", packdir, packfiles[texnum]);

	FILE *f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	u8 *filedata = (size > 0) ? malloc(size) : NULL;
	if (!filedata || fread(filedata, 1, size, f) != (size_t)size) {
		free(filedata);
		fclose(f);
		return NULL;
	}
	fclose(f);

	int w = 0, h = 0, comp = 0;
	u8 *rgba = stbi_load_from_memory(filedata, (int)size, &w, &h, &comp, 4);
	free(filedata);

	if (!rgba || w <= 0 || h <= 0 || w > 4096 || h > 4096) {
		if (rgba) {
			stbi_image_free(rgba);
		}
		sysLogPrintf(LOG_WARNING, "texpack: could not read %s", path);
		packfiles[texnum][0] = '\0'; // don't try again
		return NULL;
	}

	// the game keeps its textures bottom row first; pack images are the right way up
	for (int y = 0; y < h / 2; ++y) {
		u32 *a = (u32 *)(rgba + (size_t)y * w * 4);
		u32 *b = (u32 *)(rgba + (size_t)(h - 1 - y) * w * 4);
		for (int x = 0; x < w; ++x) {
			const u32 t = a[x];
			a[x] = b[x];
			b[x] = t;
		}
	}

	*outWidth = w;
	*outHeight = h;
	return rgba;
}

void texpackFreeReplacement(u8 *rgba)
{
	if (rgba) {
		stbi_image_free(rgba);
	}
}
