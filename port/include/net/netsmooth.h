#ifndef _IN_NETSMOOTH_H
#define _IN_NETSMOOTH_H

#include <PR/ultratypes.h>
#include "types.h"

// Smooth movement for the other players. Each position that arrives carries the sender's
// clock. Instead of chasing the newest one, a player is shown a little in the past, moving
// between two known positions, and how far in the past adapts to how unevenly the packets
// arrive: about 2 frames on a steady connection, more on a jittery one. The same idea as
// netchr.c uses for guards and simulants. Net.SmoothPlayers=0 goes back to the old way.

#define NETSMOOTH_HIST 24

struct netsmooth {
	s32 synced;
	f32 offset;    // sender time - our time, for the fastest packets
	f32 jitter;    // how much later than those the others tend to come (decaying peak)
	f32 interval;  // time between positions while the player is moving
	f32 lastrx;
	f32 renderofs; // offset - delay, smoothed
	s32 num;
	s32 head;      // index of the newest sample
	struct {
		f32 t;
		struct coord pos;
	} hist[NETSMOOTH_HIST];
};

extern s32 g_NetSmoothPlayers;

void netsmoothReset(struct netsmooth *s);
// a position stamped srctime (60ths on the sender's stage clock) arrived now
void netsmoothAdd(struct netsmooth *s, f32 srctime, const struct coord *pos);
// where to show the player this frame; false if there isn't enough to go on yet
s32 netsmoothSample(struct netsmooth *s, struct coord *out);
// how far behind the newest position that is, in 60ths (0 if not in use)
f32 netsmoothDelay(const struct netsmooth *s);

#endif
