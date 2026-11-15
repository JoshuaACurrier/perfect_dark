// Lag compensation for shots, and the ping readout.
//
// The host decides every hit. A client aims at what is on its screen, which is the
// host's world as it was a little while ago: the time the snapshot took to arrive, the
// delay the client shows other characters at so they move smoothly, and the time the
// client's input takes to get back. Without help the client has to aim ahead of
// anything that moves.
//
// So the host remembers where every character was for the last second. When it fires
// a remote player's gun, it first moves the other characters' hit shapes back to where
// they were on that player's screen, tests the shot, and puts them back.
//
// How far back comes from the client itself: every move it sends carries the newest
// server time it can know of (its estimate of the host's stage clock, see netchr.c).
// "Host clock now" minus that is the whole round trip, measured in the game rather
// than by the network library, so it includes frame delays and anything the lag
// simulator adds. The same number is the ping shown on screen.
//
// Only the hit shapes move (the model matrices, which the shot tests use, and which
// are rebuilt every frame): positions, collision and AI never see the rewind.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "net/netenet.h"
#include <ultra64.h>
#include "constants.h"
#include "bss.h"
#include "data.h"
#include "types.h"
#include "game/bondgun.h"
#include "game/camera.h"
#include "game/game_1531a0.h"
#include "game/chraction.h"
#include "lib/mtx.h"
#include "lib/vi.h"
#include "net/net.h"
#include "net/netbuf.h"
#include "net/netmsg.h"
#include "net/netchr.h"
#include "net/netlag.h"
#include "system.h"

s32 g_NetLagComp = 1;
s32 g_NetLagCompMaxMs = 300;
s32 g_NetShowPing = 1;

u32 g_NetLagShotFrames = 0;
u32 g_NetLagMovedTargets = 0;
f32 g_NetLagRewindSum = 0.f;
f32 g_NetLagShiftSum = 0.f;
u32 g_NetLagFireFrames = 0;
f32 g_NetLagBias = 0.f; // test tools: extra rewind in 60ths (--net-lagbias), to check the calibration
u32 g_NetLagHits = 0;

// how far behind the newest snapshot a client draws other players (they are moved
// towards each new position over a few frames), in 60ths. Measured with --net-aimtest
// against a human-controlled player: the aim line was closest between 2 and 3.
#define PLAYER_DISPLAY_DELAY 2.5f

// a character that "moved" further than this between then and now was teleported
// (respawned, warped by a script): leave it where it is
#define MAX_SHIFT 400.f

/* where everyone was */

#define HIST_LEN 64 // samples per character: a bit over a second at 60 a second

struct laghist {
	struct prop *prop; // who this slot's samples are about
	f32 t[HIST_LEN];   // host stage clock
	struct coord pos[HIST_LEN];
	u8 head;           // newest sample
	u8 count;
};

static struct laghist *g_LagHist = NULL;
static s32 g_LagHistNum = 0;
static f32 g_LagNextInfo = 0.f;

void netlagReset(void)
{
	free(g_LagHist);
	g_LagHist = NULL;
	g_LagHistNum = 0;
	g_LagNextInfo = 0.f;
	g_NetLagShotFrames = 0;
	g_NetLagMovedTargets = 0;
	g_NetLagRewindSum = 0.f;
	g_NetLagShiftSum = 0.f;
	g_NetLagFireFrames = 0;
	g_NetLagHits = 0;
	g_NetLagMeasureCount = 0;
	g_NetLagMeasureNowSum = 0.f;
	memset(g_NetLagMeasureSum, 0, sizeof(g_NetLagMeasureSum));

	for (s32 i = 0; i < NET_MAX_CLIENTS; ++i) {
		g_NetClients[i].lagticks = 0.f;
		g_NetClients[i].chrdelay = 0.f;
		g_NetClients[i].laginit = 0;
		g_NetClients[i].ping = 0;
		g_NetClients[i].loss = 0;
	}

	if (g_NetMode == NETMODE_SERVER && g_NumChrSlots > 0) {
		g_LagHistNum = g_NumChrSlots;
		g_LagHist = calloc(g_LagHistNum, sizeof(*g_LagHist));
	}
}

void netlagOnClientMove(struct netclient *cl, f32 viewtime, f32 chrdelay)
{
	if (g_NetMode != NETMODE_SERVER || viewtime < 0.f || viewtime != viewtime) {
		return; // the client has no idea of our clock yet
	}

	f32 lag = netchrClock() - viewtime;
	if (lag < 0.f) {
		lag = 0.f;
	} else if (lag > 120.f) {
		lag = 120.f;
	}

	if (!cl->laginit) {
		cl->laginit = 1;
		cl->lagticks = lag;
	} else {
		// follow it gently: single late packets shouldn't move the aim point
		cl->lagticks += (lag - cl->lagticks) * 0.1f;
	}

	cl->chrdelay = chrdelay;
}

void netlagServerTick(void)
{
	if (g_NetMode != NETMODE_SERVER) {
		return;
	}

	const f32 now = netchrClock();

	if (g_LagHist) {
		const s32 num = g_LagHistNum < g_NumChrSlots ? g_LagHistNum : g_NumChrSlots;
		for (s32 i = 0; i < num; ++i) {
			struct chrdata *chr = &g_ChrSlots[i];
			struct laghist *h = &g_LagHist[i];
			if (chr->chrnum < 0 || !chr->prop) {
				h->prop = NULL;
				h->count = 0;
				continue;
			}
			if (h->prop != chr->prop) {
				h->prop = chr->prop;
				h->count = 0;
			}
			h->head = (h->head + 1) % HIST_LEN;
			h->t[h->head] = now;
			h->pos[h->head] = chr->prop->pos;
			if (h->count < HIST_LEN) {
				++h->count;
			}
		}
	}

	// pings, about once a second
	if (now >= g_LagNextInfo || now < g_LagNextInfo - 120.f) {
		g_LagNextInfo = now + 60.f;
		for (s32 i = 0; i < g_NetMaxClients; ++i) {
			struct netclient *cl = &g_NetClients[i];
			if (cl->state < CLSTATE_GAME) {
				continue;
			}
			if (cl == g_NetLocalClient) {
				cl->ping = 0;
				cl->loss = 0;
			} else {
				const f32 ms = cl->lagticks * (1000.f / 60.f);
				cl->ping = cl->laginit ? (u16)(ms > 9999.f ? 9999.f : ms) : 0;
				// (loss comes from the client: the share of our snapshot rounds it never got)
			}
		}
		// (sent to everyone with the roster, see nethud.c)
	}
}

// where the character in this slot was at host time t
static bool netlagPosAt(const struct laghist *h, f32 t, struct coord *out)
{
	if (h->count < 2) {
		return false;
	}

	s32 newer = h->head;
	if (t >= h->t[newer]) {
		return false; // not in the past
	}

	for (s32 n = 1; n < h->count; ++n) {
		const s32 older = (h->head + HIST_LEN - n) % HIST_LEN;
		if (h->t[older] <= t) {
			const f32 span = h->t[newer] - h->t[older];
			const f32 f = span > 0.0001f ? (t - h->t[older]) / span : 0.f;
			out->x = h->pos[older].x + (h->pos[newer].x - h->pos[older].x) * f;
			out->y = h->pos[older].y + (h->pos[newer].y - h->pos[older].y) * f;
			out->z = h->pos[older].z + (h->pos[newer].z - h->pos[older].z) * f;
			return true;
		}
		newer = older;
	}

	// older than anything we have: the oldest will do
	*out = h->pos[newer];
	return true;
}

/* test tools: how well does the rewind match what the shooter aimed at? */

// With --net-aimtest the client keeps its sights on the centre of a character as drawn on
// its screen. So on the host, the distance from that player's aim line to the character
// should be smallest at the right rewind. Measured for the rewind we use and for a few
// ticks either side of it, and for no rewind at all.
s32 g_NetLagMeasure = 0;
const f32 g_NetLagMeasureOfs[NETLAG_MEASURE_NUM] = { -8.f, -6.f, -4.f, -2.f, 0.f, 2.f, 4.f, 6.f, 8.f };
f32 g_NetLagMeasureSum[NETLAG_MEASURE_NUM];
f32 g_NetLagMeasureNowSum = 0.f;
u32 g_NetLagMeasureCount = 0;

static f32 netlagRayDist(const struct coord *from, const struct coord *dir, const struct coord *p)
{
	const f32 dx = p->x - from->x;
	const f32 dy = p->y - from->y;
	const f32 dz = p->z - from->z;
	const f32 along = dx * dir->x + dy * dir->y + dz * dir->z;
	if (along <= 0.f) {
		return 100000.f;
	}
	const f32 px = dx - dir->x * along;
	const f32 py = dy - dir->y * along;
	const f32 pz = dz - dir->z * along;
	return sqrtf(px * px + py * py + pz * pz);
}

static void netlagMeasure(struct player *pl, struct netclient *cl, f32 now)
{
	const f32 theta = pl->vv_theta * (M_PI / 180.f);
	const f32 verta = pl->vv_verta * (M_PI / 180.f);
	struct coord dir = { -sinf(theta) * cosf(verta), sinf(verta), cosf(theta) * cosf(verta) };

	f32 best = 150.f;
	f32 bestd[NETLAG_MEASURE_NUM];
	f32 bestnow = 0.f;
	bool found = false;

	const s32 num = g_LagHistNum < g_NumChrSlots ? g_LagHistNum : g_NumChrSlots;
	for (s32 i = 0; i < num; ++i) {
		struct chrdata *chr = &g_ChrSlots[i];
		struct prop *prop = chr->prop;
		const struct laghist *h = &g_LagHist[i];
		if (chr->chrnum < 0 || !prop || prop == pl->prop || h->prop != prop || h->count < HIST_LEN
				|| !(prop->flags & PROPFLAG_ONTHISSCREENTHISTICK)) {
			continue;
		}

		const f32 back = cl->lagticks + (prop->type == PROPTYPE_PLAYER ? PLAYER_DISPLAY_DELAY : cl->chrdelay);
		f32 d[NETLAG_MEASURE_NUM];
		f32 mind = 100000.f;
		bool teleported = false;
		for (s32 k = 0; k < NETLAG_MEASURE_NUM; ++k) {
			struct coord then = prop->pos;
			netlagPosAt(h, now - (back + g_NetLagMeasureOfs[k]), &then);
			const f32 mx = then.x - prop->pos.x;
			const f32 my = then.y - prop->pos.y;
			const f32 mz = then.z - prop->pos.z;
			if (mx * mx + my * my + mz * mz > MAX_SHIFT * MAX_SHIFT) {
				teleported = true; // respawned in this stretch of history
			}
			d[k] = netlagRayDist(&pl->cam_pos, &dir, &then);
			if (d[k] < mind) {
				mind = d[k];
			}
		}

		if (!teleported && mind < best) {
			best = mind;
			memcpy(bestd, d, sizeof(bestd));
			bestnow = netlagRayDist(&pl->cam_pos, &dir, &prop->pos);
			found = true;
		}
	}

	if (found) {
		for (s32 k = 0; k < NETLAG_MEASURE_NUM; ++k) {
			g_NetLagMeasureSum[k] += bestd[k];
		}
		g_NetLagMeasureNowSum += bestnow;
		++g_NetLagMeasureCount;
	}
}

/* moving the hit shapes */

#define MAX_MOVED 512

static struct {
	Mtxf *matrices;
	s16 num;
	struct coord d; // what was added to each matrix's position, in the shooter's view space
} g_LagMoved[MAX_MOVED];
static s32 g_LagNumMoved = 0;

static struct {
	struct prop *prop;
	f32 dz;
} g_LagMovedProps[MAX_MOVED];
static s32 g_LagNumMovedProps = 0;

static void netlagMoveModel(struct model *model, const struct coord *d)
{
	if (!model || !model->matrices || !model->definition || g_LagNumMoved >= MAX_MOVED) {
		return;
	}

	const s32 num = model->definition->nummatrices;
	for (s32 i = 0; i < num; ++i) {
		model->matrices[i].m[3][0] += d->x;
		model->matrices[i].m[3][1] += d->y;
		model->matrices[i].m[3][2] += d->z;
	}

	g_LagMoved[g_LagNumMoved].matrices = model->matrices;
	g_LagMoved[g_LagNumMoved].num = num;
	g_LagMoved[g_LagNumMoved].d = *d;
	++g_LagNumMoved;
}

// the things a character holds or wears are hit shapes too
static void netlagMoveChildren(struct prop *parent, const struct coord *d, s32 depth)
{
	if (depth > 4) {
		return;
	}

	for (struct prop *child = parent->child; child; child = child->next) {
		// An item's matrices are allocated for the frame it is drawn in. Without this flag
		// they were not set this tick and the pointer is left over from an earlier frame
		// (writing through it crashed the host). The shot test skips such items and
		// everything attached to them the same way (func0f0859a0).
		if (!(child->flags & PROPFLAG_ONTHISSCREENTHISTICK)) {
			continue;
		}
		if ((child->type == PROPTYPE_OBJ || child->type == PROPTYPE_WEAPON || child->type == PROPTYPE_DOOR) && child->obj) {
			netlagMoveModel(child->obj->model, d);
		}
		netlagMoveChildren(child, d, depth + 1);
	}
}

void netlagBeginShots(void)
{
	g_LagNumMoved = 0;
	g_LagNumMovedProps = 0;

	struct player *pl = g_Vars.currentplayer;

	if (g_NetMode != NETMODE_SERVER || !pl || !pl->isremote || !pl->client || !pl->prop) {
		return;
	}

	// only when a shot is about to be tested
	if (!bgunIsFiring(HAND_RIGHT) && !bgunIsFiring(HAND_LEFT)
			&& !pl->hands[HAND_RIGHT].unk0d0f_02 && !pl->hands[HAND_LEFT].unk0d0f_02) {
		return;
	}

	++g_NetLagFireFrames;

	struct netclient *cl = pl->client;

	if (g_NetLagMeasure && g_LagHist && cl->laginit) {
		netlagMeasure(pl, cl, netchrClock());
	}

	if (!g_NetLagComp || !g_LagHist || !cl->laginit) {
		return;
	}

	const f32 now = netchrClock();
	const f32 maxticks = (f32)g_NetLagCompMaxMs * (60.f / 1000.f);
	Mtxf *view = camGetWorldToScreenMtxf();
	bool any = false;

	const s32 num = g_LagHistNum < g_NumChrSlots ? g_LagHistNum : g_NumChrSlots;
	for (s32 i = 0; i < num; ++i) {
		struct chrdata *chr = &g_ChrSlots[i];
		struct prop *prop = chr->prop;
		const struct laghist *h = &g_LagHist[i];

		if (chr->chrnum < 0 || !prop || prop == pl->prop || h->prop != prop || !chr->model
				|| !(prop->flags & PROPFLAG_ONTHISSCREENTHISTICK)) {
			continue;
		}

		f32 back = cl->lagticks + (prop->type == PROPTYPE_PLAYER ? PLAYER_DISPLAY_DELAY : cl->chrdelay) + g_NetLagBias;
		if (back > maxticks) {
			back = maxticks;
		}
		if (back < 0.5f) {
			continue;
		}

		struct coord then;
		if (!netlagPosAt(h, now - back, &then)) {
			continue;
		}

		struct coord dworld;
		dworld.x = then.x - prop->pos.x;
		dworld.y = then.y - prop->pos.y;
		dworld.z = then.z - prop->pos.z;
		const f32 distsq = dworld.x * dworld.x + dworld.y * dworld.y + dworld.z * dworld.z;
		if (distsq < 1.f || distsq > MAX_SHIFT * MAX_SHIFT) {
			continue;
		}

		struct coord d;
		mtx4RotateVec(view, &dworld, &d);

		netlagMoveModel(chr->model, &d);
		netlagMoveChildren(prop, &d, 0);

		if (g_LagNumMovedProps < MAX_MOVED) {
			// the prop's depth on screen (positive into the screen; view space z is negative)
			prop->z -= d.z;
			g_LagMovedProps[g_LagNumMovedProps].prop = prop;
			g_LagMovedProps[g_LagNumMovedProps].dz = d.z;
			++g_LagNumMovedProps;
		}

		++g_NetLagMovedTargets;
		g_NetLagRewindSum += back;
		g_NetLagShiftSum += sqrtf(distsq);
		any = true;
	}

	if (any) {
		++g_NetLagShotFrames;
	}
}

void netlagEndShots(void)
{
	for (s32 i = 0; i < g_LagNumMoved; ++i) {
		for (s32 j = 0; j < g_LagMoved[i].num; ++j) {
			g_LagMoved[i].matrices[j].m[3][0] -= g_LagMoved[i].d.x;
			g_LagMoved[i].matrices[j].m[3][1] -= g_LagMoved[i].d.y;
			g_LagMoved[i].matrices[j].m[3][2] -= g_LagMoved[i].d.z;
		}
	}

	for (s32 i = 0; i < g_LagNumMovedProps; ++i) {
		g_LagMovedProps[i].prop->z += g_LagMovedProps[i].dz;
	}

	g_LagNumMoved = 0;
	g_LagNumMovedProps = 0;
}

/* the readout */

Gfx *netlagDrawPing(Gfx *gdl)
{
	if (!g_NetMode || !g_NetShowPing || !g_NetLocalClient || g_NetLocalClient->state < CLSTATE_GAME) {
		return gdl;
	}

	if (!g_CharsHandelGothicXs || !g_FontHandelGothicXs || g_Vars.currentplayer->isremote) {
		return gdl;
	}

	char buf[256];
	size_t len = 0;
	buf[0] = '\0';

	if (g_NetMode == NETMODE_CLIENT) {
		// just ours
		const struct netclient *cl = g_NetLocalClient;
		if (cl->loss) {
			len += snprintf(buf + len, sizeof(buf) - len, "PING %u  LOSS %u%%\n", cl->ping, cl->loss);
		} else {
			len += snprintf(buf + len, sizeof(buf) - len, "PING %u\n", cl->ping);
		}
	} else {
		// the host has none of its own: show the highest of the others'
		// (the lobby list and the Tab scoreboard have everyone's)
		const struct netclient *worst = NULL;
		for (s32 i = 0; i < g_NetMaxClients; ++i) {
			const struct netclient *cl = &g_NetClients[i];
			if (cl->state >= CLSTATE_GAME && cl != g_NetLocalClient && (!worst || cl->ping > worst->ping)) {
				worst = cl;
			}
		}
		for (s32 i = 0; i < g_NetMaxClients && len < sizeof(buf) - 48; ++i) {
			if (&g_NetClients[i] != worst) {
				continue;
			}
			const struct netclient *cl = &g_NetClients[i];
			if (cl->state < CLSTATE_GAME || cl == g_NetLocalClient) {
				continue;
			}
			if (cl->loss) {
				len += snprintf(buf + len, sizeof(buf) - len, "PING %u  LOSS %u%%\n", cl->ping, cl->loss);
			} else {
				len += snprintf(buf + len, sizeof(buf) - len, "PING %u\n", cl->ping);
			}
		}
	}

	if (!buf[0]) {
		return gdl;
	}

	// worst number decides the colour: green, yellow from 120 ms or any loss, red from 220 ms or 5%
	u32 colour = 0x40ff40c0;
	for (s32 i = 0; i < g_NetMaxClients; ++i) {
		const struct netclient *cl = &g_NetClients[i];
		if (cl->state < CLSTATE_GAME || (g_NetMode == NETMODE_CLIENT && cl != g_NetLocalClient)) {
			continue;
		}
		if (cl->ping >= 220 || cl->loss >= 5) {
			colour = 0xff4040c0;
			break;
		} else if (cl->ping >= 120 || cl->loss >= 1) {
			colour = 0xffe040c0;
		}
	}

	s32 textheight, textwidth;
	textMeasure(&textheight, &textwidth, buf, g_CharsHandelGothicXs, g_FontHandelGothicXs, 0);

	s32 x = viGetViewLeft() / g_ScaleX + viGetViewWidth() / g_ScaleX - textwidth - 6;
	s32 y = viGetViewTop() + 4;

	gdl = text0f153628(gdl);
	gSPSetExtraGeometryModeEXT(gdl++, G_ASPECT_RIGHT_EXT);
	gdl = textRender(gdl, &x, &y, buf, g_CharsHandelGothicXs, g_FontHandelGothicXs, colour, 0x000000a0, viGetWidth(), viGetHeight(), 0, 0);
	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);
	gdl = text0f153780(gdl);

	return gdl;
}
