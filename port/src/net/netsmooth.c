// Smooth movement for the other players, see netsmooth.h.

#include <string.h>
#include "types.h"
#include "net/netchr.h"
#include "net/netsmooth.h"

#define DELAY_MIN 1.5f
#define DELAY_MAX 15.f

s32 g_NetSmoothPlayers = 1;

static inline f32 clampf(f32 v, f32 lo, f32 hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

void netsmoothReset(struct netsmooth *s)
{
	memset(s, 0, sizeof(*s));
}

static void netsmoothPush(struct netsmooth *s, f32 t, const struct coord *pos)
{
	s->head = (s->head + 1) % NETSMOOTH_HIST;
	s->hist[s->head].t = t;
	s->hist[s->head].pos = *pos;
	if (s->num < NETSMOOTH_HIST) {
		++s->num;
	}
}

void netsmoothAdd(struct netsmooth *s, f32 srctime, const struct coord *pos)
{
	const f32 now = netchrClock();
	const f32 sample = srctime - now;

	if (srctime != srctime) {
		return;
	}

	if (s->synced && s->num > 0) {
		const f32 last = s->hist[s->head].t;
		if (srctime <= last) {
			if (srctime < last - 600.f) {
				// their clock started over (a new stage)
				netsmoothReset(s);
			} else {
				return; // old or duplicate
			}
		}
	}

	if (!s->synced) {
		s->synced = true;
		s->offset = sample;
		s->jitter = 1.f;
		s->interval = 1.f;
		s->lastrx = now;
		s->renderofs = sample - (DELAY_MIN + 2.f);
		s->num = 0;
		s->head = 0;
		netsmoothPush(s, srctime, pos);
		return;
	}

	const f32 elapsed = clampf(now - s->lastrx, 0.f, 60.f);
	s->lastrx = now;

	// track the fastest packets; drift down slowly so a clock that runs at a slightly
	// different rate, or a route that got slower for good, is picked up again
	s->offset -= elapsed * 0.001f;
	if (sample > s->offset) {
		s->offset = sample;
	}

	// how late the others come: a peak that decays over a few seconds
	const f32 late = s->offset - sample;
	s->jitter -= elapsed * 0.01f;
	if (late > s->jitter) {
		s->jitter = late;
	}
	if (s->jitter < 0.f) {
		s->jitter = 0.f;
	}

	const f32 gap = srctime - s->hist[s->head].t;
	if (gap < 4.f) {
		s->interval += (gap - s->interval) * 0.05f;
	} else {
		// nothing was sent for a while because they stood still: say so, or they would
		// be shown sliding slowly from the old position to this one
		const struct coord lastpos = s->hist[s->head].pos;
		netsmoothPush(s, srctime - clampf(s->interval, 1.f, 3.f), &lastpos);
	}

	netsmoothPush(s, srctime, pos);
}

static inline f32 netsmoothWantDelay(const struct netsmooth *s)
{
	return clampf(s->interval + s->jitter + 0.75f, DELAY_MIN, DELAY_MAX);
}

f32 netsmoothDelay(const struct netsmooth *s)
{
	if (!g_NetSmoothPlayers || !s->synced || s->num < 2) {
		return 0.f;
	}
	return clampf(s->offset - s->renderofs, 0.f, DELAY_MAX + 2.f);
}

s32 netsmoothSample(struct netsmooth *s, struct coord *out)
{
	if (!g_NetSmoothPlayers || !s->synced || s->num < 2) {
		return false;
	}

	const f32 now = netchrClock();
	const f32 target = s->offset - netsmoothWantDelay(s);
	const f32 diff = target - s->renderofs;

	if (diff > 30.f || diff < -30.f) {
		s->renderofs = target; // way off (a hitch): jump
	} else {
		// otherwise run a little fast or slow until we're there, which nobody notices
		s->renderofs += clampf(diff, -0.05f, 0.05f);
	}

	const f32 rt = now + s->renderofs;

	// newest first
	s32 i = s->head;
	if (rt >= s->hist[i].t) {
		*out = s->hist[i].pos; // nothing newer yet: hold
		return true;
	}

	for (s32 n = 1; n < s->num; ++n) {
		const s32 older = (i + NETSMOOTH_HIST - 1) % NETSMOOTH_HIST;
		const f32 t0 = s->hist[older].t;
		const f32 t1 = s->hist[i].t;
		if (rt >= t0) {
			const f32 f = (t1 > t0) ? (rt - t0) / (t1 - t0) : 1.f;
			out->x = s->hist[older].pos.x + (s->hist[i].pos.x - s->hist[older].pos.x) * f;
			out->y = s->hist[older].pos.y + (s->hist[i].pos.y - s->hist[older].pos.y) * f;
			out->z = s->hist[older].pos.z + (s->hist[i].pos.z - s->hist[older].pos.z) * f;
			return true;
		}
		i = older;
	}

	*out = s->hist[i].pos; // older than everything we remember
	return true;
}
