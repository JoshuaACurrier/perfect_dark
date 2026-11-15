// HD texture packs, see texpack.h.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <SDL.h>
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

// the loading half of this file
static void texpackPrefetch(s32 texnum);
static void texpackDropJobs(void);
static SDL_mutex *joblock;

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

	// texture-packs/ next to the game data, the save folder or the exe, or one folder
	// above the exe (so one copy serves several builds unpacked side by side)
	static const char *roots[] = { "$B/" TEXPACK_DIR, "$S/" TEXPACK_DIR, "$E/" TEXPACK_DIR, "$E/../" TEXPACK_DIR };

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
	// (the workers read packdir and packfiles: nothing may be queued while those change)
	texpackDropJobs();
	if (joblock) {
		SDL_LockMutex(joblock);
	}
	texpackScan();
	if (joblock) {
		SDL_UnlockMutex(joblock);
	}
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
			texpackPrefetch(texnum); // start reading its image now, before it's drawn
			return;
		}
	}
}

void texpackForgetAll(void)
{
	memset(registry, 0, sizeof(registry));
	texpackDropJobs();
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

/* loading
 *
 * Reading and decoding an image takes from under a millisecond to over a hundred, so
 * it happens on worker threads. The renderer asks for a texture's image every time it
 * uses the texture; until the image is ready it draws the original.
 *
 * Textures are also queued when the game loads them (usually while a stage loads),
 * so most images are ready before they are first drawn. Those are held in memory
 * until the renderer takes them, up to TEXPACK_HELD_MAX; past that only images the
 * renderer has asked for are decoded. */

#define TEXPACK_WORKERS 3
#define TEXPACK_HELD_MAX (384u * 1024 * 1024) // bytes of decoded images waiting to be taken
#define TEXPACK_FRAME_PIXELS (1024 * 1024)    // images handed to the renderer per frame, beyond the first

enum {
	JOB_IDLE,
	JOB_QUEUED, // waiting for a worker
	JOB_BUSY,   // a worker is decoding it
	JOB_DONE,   // rgba is ready to be taken
	JOB_FAILED, // unreadable; not tried again
};

struct texjob {
	u8 state;
	u8 wanted;  // the renderer asked for it (the rest are read ahead)
	u8 discard; // the stage changed while a worker was on it
	s32 width, height;
	u8 *rgba;
};

static struct texjob *jobs = NULL;
static SDL_cond *jobcond = NULL;
static s32 jobsqueued = 0;      // jobs in JOB_QUEUED
static s32 jobswanted = 0;      // of those, how many the renderer asked for
static size_t heldbytes = 0;    // decoded images in JOB_DONE
static s32 framepixels = 0;     // handed to the renderer this frame
static s32 threadsup = 0;

u32 g_TexpackDecoded = 0;       // images decoded so far
u32 g_TexpackWaitedFrames = 0;  // times the renderer drew an original because the image wasn't ready
u32 g_TexpackSlowestMs = 0;     // longest single read + decode

static u8 *texpackDecodeFile(const char *path, s32 *outWidth, s32 *outHeight)
{
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

// with joblock held: the next job a worker should take, or -1
static s32 texpackNextJob(void)
{
	if (jobswanted > 0) {
		for (s32 i = 0; i < TEXPACK_MAX_TEXTURES; ++i) {
			if (jobs[i].state == JOB_QUEUED && jobs[i].wanted) {
				return i;
			}
		}
	}
	if (jobsqueued > jobswanted && heldbytes < TEXPACK_HELD_MAX) {
		for (s32 i = 0; i < TEXPACK_MAX_TEXTURES; ++i) {
			if (jobs[i].state == JOB_QUEUED) {
				return i;
			}
		}
	}
	return -1;
}

static int texpackWorker(void *arg)
{
	SDL_LockMutex(joblock);
	for (;;) {
		const s32 num = texpackNextJob();
		if (num < 0) {
			SDL_CondWait(jobcond, joblock);
			continue;
		}

		struct texjob *job = &jobs[num];
		job->state = JOB_BUSY;
		job->discard = 0;
		--jobsqueued;
		if (job->wanted) {
			--jobswanted;
		}
		char path[FS_MAXPATH + 1];
		snprintf(path, sizeof(path), "%s/%s", packdir, packfiles[num]);
		SDL_UnlockMutex(joblock);

		const u32 start = SDL_GetTicks();
		s32 w = 0, h = 0;
		u8 *rgba = texpackDecodeFile(path, &w, &h);
		const u32 took = SDL_GetTicks() - start;
		if (!rgba) {
			sysLogPrintf(LOG_WARNING, "texpack: could not read %s", path);
		}

		SDL_LockMutex(joblock);
		if (job->discard) {
			// the stage changed meanwhile: nobody is waiting for this any more
			if (rgba) {
				stbi_image_free(rgba);
			}
			job->state = rgba ? JOB_IDLE : JOB_FAILED;
		} else if (rgba) {
			job->rgba = rgba;
			job->width = w;
			job->height = h;
			job->state = JOB_DONE;
			heldbytes += (size_t)w * h * 4;
		} else {
			job->state = JOB_FAILED;
		}
		job->discard = 0;
		job->wanted = 0;
		++g_TexpackDecoded;
		if (took > g_TexpackSlowestMs) {
			g_TexpackSlowestMs = took;
		}
	}
	return 0;
}

// starts the workers the first time there is something for them to do
static s32 texpackThreadsUp(void)
{
	if (threadsup) {
		return threadsup > 0;
	}
	threadsup = -1;
	jobs = calloc(TEXPACK_MAX_TEXTURES, sizeof(*jobs));
	joblock = SDL_CreateMutex();
	jobcond = SDL_CreateCond();
	if (!jobs || !joblock || !jobcond) {
		return 0;
	}
	s32 started = 0;
	for (s32 i = 0; i < TEXPACK_WORKERS; ++i) {
		SDL_Thread *t = SDL_CreateThread(texpackWorker, "texpack", NULL);
		if (t) {
			SDL_DetachThread(t);
			++started;
		}
	}
	if (started) {
		threadsup = 1;
	}
	return started > 0;
}

// with joblock held
static void texpackQueue(s32 texnum, s32 wanted)
{
	struct texjob *job = &jobs[texnum];
	if (job->state == JOB_IDLE) {
		job->state = JOB_QUEUED;
		job->wanted = 0;
		++jobsqueued;
	}
	if (job->state == JOB_QUEUED && wanted && !job->wanted) {
		job->wanted = 1;
		++jobswanted;
	}
	if (job->state == JOB_BUSY) {
		job->discard = 0; // it is of use again after a stage change
	}
	SDL_CondSignal(jobcond);
}

static void texpackPrefetch(s32 texnum)
{
	if (texnum < 0 || texnum >= TEXPACK_MAX_TEXTURES || !texpackActive() || !packfiles[texnum][0] || !texpackThreadsUp()) {
		return;
	}
	SDL_LockMutex(joblock);
	texpackQueue(texnum, 0);
	SDL_UnlockMutex(joblock);
}

// forgets everything queued or decoded: the stage's textures are gone
static void texpackDropJobs(void)
{
	if (threadsup <= 0) {
		return;
	}
	SDL_LockMutex(joblock);
	for (s32 i = 0; i < TEXPACK_MAX_TEXTURES; ++i) {
		struct texjob *job = &jobs[i];
		if (job->state == JOB_DONE) {
			stbi_image_free(job->rgba);
			job->rgba = NULL;
			job->state = JOB_IDLE;
		} else if (job->state == JOB_QUEUED) {
			job->state = JOB_IDLE;
		} else if (job->state == JOB_BUSY) {
			job->discard = 1;
		}
		job->wanted = 0;
	}
	jobsqueued = 0;
	jobswanted = 0;
	heldbytes = 0;
	SDL_UnlockMutex(joblock);
}

void texpackNewFrame(void)
{
	framepixels = 0;
}

s32 texpackPollReplacement(const void *data, u8 **outRgba, s32 *outWidth, s32 *outHeight)
{
	if (!texpackActive()) {
		return TEXPACK_NONE;
	}

	const s32 texnum = texpackGetTextureNum(data);
	if (texnum < 0 || texnum >= TEXPACK_MAX_TEXTURES || !packfiles[texnum][0] || !texpackThreadsUp()) {
		return TEXPACK_NONE;
	}

	s32 ret = TEXPACK_PENDING;
	struct texjob *job = &jobs[texnum];

	SDL_LockMutex(joblock);
	if (job->state == JOB_FAILED) {
		ret = TEXPACK_NONE;
	} else if (job->state == JOB_DONE) {
		// spread the uploads of a burst over a few frames
		const s32 pixels = job->width * job->height;
		if (framepixels == 0 || framepixels + pixels <= TEXPACK_FRAME_PIXELS) {
			framepixels += pixels;
			*outRgba = job->rgba;
			*outWidth = job->width;
			*outHeight = job->height;
			job->rgba = NULL;
			job->state = JOB_IDLE;
			heldbytes -= (size_t)pixels * 4;
			SDL_CondSignal(jobcond); // room to read ahead again
			ret = TEXPACK_READY;
		}
	} else {
		texpackQueue(texnum, 1);
	}
	SDL_UnlockMutex(joblock);

	if (ret == TEXPACK_PENDING) {
		++g_TexpackWaitedFrames;
	}
	return ret;
}

void texpackFreeReplacement(u8 *rgba)
{
	if (rgba) {
		stbi_image_free(rgba);
	}
}
