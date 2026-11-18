// The Slayer's guided rocket, see netrocket.h.

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "types.h"
#include "data.h"
#include "bss.h"
#include "constants.h"
#include "game/player.h"
#include "game/playermgr.h"
#include "game/prop.h"
#include "game/propobj.h"
#include "game/smoke.h"
#include "system.h"
#include "net/net.h"
#include "net/netbuf.h"
#include "net/netmsg.h"
#include "net/netrocket.h"

// SVC_ROCKET kinds
#define RK_GONE    0 // to the pilot: it blew up on the host
#define RK_RELEASE 1 // to the pilot: the flight is over, the rocket flies on by itself
#define RK_START   2 // to the pilot: this rocket is yours to fly
#define RK_MOVE    3 // to everyone: where a guided rocket is

// CLC_ROCKET flags
#define RF_EXPLODED 1
#define RF_RELEASED 2
#define RF_NOSTATE  4 // it was gone here before we were told to fly it: no position to give

struct pilot {
	struct weaponobj *rocket;
	u32 syncid;
	struct coord lastpos;
	u32 nummoves;
	u32 lasttick; // when we last heard about it
	u8 puppet;    // someone else's machine flies it: it doesn't blow up by itself here
};

#define WATCH_TIMEOUT 120 // ticks without news before a watched rocket is left to itself

struct rocketstate {
	struct coord pos;
	RoomNum rooms[8];
	f32 rot[3][3];
	struct coord speed;
};

static struct pilot g_SvPilots[MAX_PLAYERS]; // host: by player number
static struct pilot g_ClPilot;               // client: our own flight
static struct rocketstate g_ClLast;          // client: the last state we flew it to
static struct pilot g_ClWatch[MAX_PLAYERS];  // client: other people's rockets in flight
static u32 g_ClGoneReply = 0;                // client: tell the host this rocket is gone here already

void netrocketReset(void)
{
	memset(g_SvPilots, 0, sizeof(g_SvPilots));
	memset(&g_ClPilot, 0, sizeof(g_ClPilot));
	memset(g_ClWatch, 0, sizeof(g_ClWatch));
	g_ClGoneReply = 0;
}

static struct weaponobj *netrocketFind(u32 syncid)
{
	if (syncid) {
		for (s32 i = 0; i < g_Vars.maxprops; ++i) {
			struct prop *prop = &g_Vars.props[i];
			if (prop->syncid == syncid) {
				if (prop->type == PROPTYPE_WEAPON && prop->weapon && !prop->parent
						&& !(prop->obj->hidden & (OBJHFLAG_DELETING | OBJHFLAG_GONE))) {
					return prop->weapon;
				}
				break;
			}
		}
	}
	return NULL;
}

// is the rocket of this flight still in the world
static bool netrocketAlive(const struct pilot *pilot)
{
	return pilot->rocket && pilot->rocket->base.prop && pilot->rocket->base.prop->syncid == pilot->syncid
		&& pilot->rocket->base.prop->type == PROPTYPE_WEAPON
		&& !(pilot->rocket->base.hidden & (OBJHFLAG_DELETING | OBJHFLAG_GONE));
}

// the rocket this player is flying right now, if any
static struct weaponobj *netrocketOf(const struct player *pl)
{
	if (pl && !pl->isdead && pl->visionmode == VISIONMODE_SLAYERROCKET && pl->slayerrocket
			&& pl->slayerrocket->base.prop && pl->slayerrocket->base.prop->syncid
			&& !(pl->slayerrocket->base.hidden & OBJHFLAG_DELETING)) {
		return pl->slayerrocket;
	}
	return NULL;
}

static void netrocketGetState(const struct weaponobj *rocket, struct rocketstate *st)
{
	const struct prop *prop = rocket->base.prop;
	st->pos = prop->pos;
	// (the list ends at the first -1; what follows it is whatever was there before)
	bool end = false;
	for (s32 i = 0; i < 8; ++i) {
		end = end || prop->rooms[i] < 0;
		st->rooms[i] = end ? -1 : prop->rooms[i];
	}
	memcpy(st->rot, rocket->base.realrot, sizeof(st->rot));
	if (rocket->base.hidden & OBJHFLAG_PROJECTILE) {
		st->speed = rocket->base.projectile->speed;
	} else {
		st->speed.x = st->speed.y = st->speed.z = 0.f;
	}
}

static void netrocketWriteState(struct netbuf *dst, const struct rocketstate *st)
{
	netbufWriteCoord(dst, &st->pos);
	for (s32 i = 0; i < 8; ++i) {
		netbufWriteS16(dst, st->rooms[i]);
	}
	for (s32 i = 0; i < 3; ++i) {
		for (s32 j = 0; j < 3; ++j) {
			netbufWriteF32(dst, st->rot[i][j]);
		}
	}
	netbufWriteCoord(dst, &st->speed);
}

static bool netrocketReadState(struct netbuf *src, struct rocketstate *st)
{
	bool ok = true;
	netbufReadCoord(src, &st->pos);
	for (s32 i = 0; i < 8; ++i) {
		st->rooms[i] = netbufReadS16(src);
		if (st->rooms[i] < -1 || st->rooms[i] >= g_Vars.roomcount) {
			ok = false;
		}
	}
	for (s32 i = 0; i < 3; ++i) {
		for (s32 j = 0; j < 3; ++j) {
			st->rot[i][j] = netbufReadF32(src);
			ok = ok && isfinite(st->rot[i][j]);
		}
	}
	netbufReadCoord(src, &st->speed);
	for (s32 i = 0; i < 3; ++i) {
		ok = ok && isfinite(st->pos.f[i]) && isfinite(st->speed.f[i]) && fabsf(st->speed.f[i]) < 100.f;
	}
	return ok && !src->error;
}

static void netrocketApplyState(struct weaponobj *rocket, const struct rocketstate *st)
{
	struct prop *prop = rocket->base.prop;

	prop->pos = st->pos;

	if (memcmp(prop->rooms, st->rooms, sizeof(st->rooms))) {
		if (prop->active) {
			propDeregisterRooms(prop);
		}
		memcpy(prop->rooms, st->rooms, sizeof(st->rooms));
		if (prop->active) {
			propRegisterRooms(prop);
		}
	}

	memcpy(rocket->base.realrot, st->rot, sizeof(st->rot));

	if (rocket->base.hidden & OBJHFLAG_PROJECTILE) {
		rocket->base.projectile->speed = st->speed;
	}
}

static u8 netrocketClientIdOf(const struct player *pl)
{
	for (s32 i = 0; i < NET_MAX_CLIENTS; ++i) {
		if (g_NetClients[i].state >= CLSTATE_GAME && g_NetClients[i].player == pl) {
			return g_NetClients[i].id;
		}
	}
	return NET_NULL_CLIENT;
}

// A copy of a guided rocket on a machine that is not flying it only follows what it is
// told. It runs a frame or two ahead on its last known course between updates, so it can
// clip a wall the pilot went around; it must not go off for that. Called when the rocket's
// timer has run out (it hit something): true puts the timer back.
static bool netrocketIsPuppet(const struct weaponobj *rocket)
{
	bool puppet = false;

	if (g_NetMode == NETMODE_SERVER) {
		for (s32 p = 0; p < MAX_PLAYERS && !puppet; ++p) {
			puppet = (g_SvPilots[p].rocket == rocket && g_SvPilots[p].puppet);
		}
	} else if (g_NetMode == NETMODE_CLIENT) {
		for (s32 i = 0; i < ARRAYCOUNT(g_ClWatch) && !puppet; ++i) {
			puppet = (g_ClWatch[i].rocket == rocket && g_ClWatch[i].puppet);
		}
	}

	return puppet;
}

s32 netrocketHoldDetonation(struct weaponobj *rocket)
{
	if (netrocketIsPuppet(rocket)) {
		rocket->timer240 = -1;
		return true;
	}
	return false;
}

// In place of the game's own flight for a rocket someone else is flying: carry on along
// the last course we were told, through anything, and leave the smoke trail. No collision:
// what it hits is up to its pilot's machine.
s32 netrocketPuppetTick(struct defaultobj *obj)
{
	if (!g_NetMode || obj->type != OBJTYPE_WEAPON || !(obj->hidden & OBJHFLAG_PROJECTILE) || !obj->projectile
			|| !obj->prop || !netrocketIsPuppet((struct weaponobj *)obj)) {
		return false;
	}

	if (g_Vars.lvupdate240 > 0) {
		struct projectile *projectile = obj->projectile;
		struct prop *prop = obj->prop;

		prop->pos.x += projectile->speed.x * g_Vars.lvupdate60freal;
		prop->pos.y += projectile->speed.y * g_Vars.lvupdate60freal;
		prop->pos.z += projectile->speed.z * g_Vars.lvupdate60freal;
		func0f069c70(obj, true, true);

		if (projectile->smoketimer240 <= 0) {
			projectile->smoketimer240 = TICKS(24);
			smokeCreateSimple(&prop->pos, prop->rooms, SMOKETYPE_ROCKETTAIL);
		} else {
			projectile->smoketimer240 -= g_Vars.lvupdate240;
		}
	}

	return true;
}

/* host */

static void netrocketServerTick(s32 sendmoves)
{
	for (s32 p = 0; p < PLAYERCOUNT() && p < MAX_PLAYERS; ++p) {
		struct player *pl = g_Vars.players[p];
		struct pilot *cur = &g_SvPilots[p];
		struct weaponobj *rocket = netrocketOf(pl);
		const u8 clid = netrocketClientIdOf(pl);
		const bool remote = pl && pl->isremote && clid != NET_NULL_CLIENT;

		if (cur->rocket && (!rocket || rocket != cur->rocket || rocket->base.prop->syncid != cur->syncid)) {
			// that flight is over
			const bool alive = netrocketAlive(cur);
			netbufWriteU8(&g_NetMsgRel, SVC_ROCKET);
			netbufWriteU8(&g_NetMsgRel, alive ? RK_RELEASE : RK_GONE);
			netbufWriteU8(&g_NetMsgRel, clid);
			netbufWriteU32(&g_NetMsgRel, cur->syncid);
			netbufWriteCoord(&g_NetMsgRel, &cur->lastpos);
			sysLogPrintf(LOG_NOTE, "NET: rocket %u of client %u: flight over (%s) at %.0f %.0f %.0f", cur->syncid, clid,
				alive ? "let go" : "blew up", cur->lastpos.x, cur->lastpos.y, cur->lastpos.z);
			cur->rocket = NULL;
			cur->syncid = 0;
			cur->puppet = 0;
		}

		if (!rocket) {
			continue;
		}

		if (!cur->rocket) {
			cur->rocket = rocket;
			cur->syncid = rocket->base.prop->syncid;
			cur->puppet = remote;
			cur->lasttick = g_NetTick;
			if (remote) {
				netbufWriteU8(&g_NetMsgRel, SVC_ROCKET);
				netbufWriteU8(&g_NetMsgRel, RK_START);
				netbufWriteU8(&g_NetMsgRel, clid);
				netbufWriteU32(&g_NetMsgRel, cur->syncid);
			}
			sysLogPrintf(LOG_NOTE, "NET: rocket %u of client %u: guided flight starts", cur->syncid, clid);
		}

		cur->lastpos = rocket->base.prop->pos;

		if (cur->puppet && g_NetTick - cur->lasttick > WATCH_TIMEOUT) {
			// the pilot went quiet: the rocket is on its own again
			cur->puppet = 0;
			sysLogPrintf(LOG_NOTE, "NET: rocket %u of client %u: no word from its pilot, flying on by itself", cur->syncid, clid);
		}

		if (sendmoves) {
			struct rocketstate st;
			netrocketGetState(rocket, &st);
			netbufWriteU8(&g_NetMsg, SVC_ROCKET);
			netbufWriteU8(&g_NetMsg, RK_MOVE);
			netbufWriteU8(&g_NetMsg, clid);
			netbufWriteU32(&g_NetMsg, cur->syncid);
			netrocketWriteState(&g_NetMsg, &st);
		}
	}
}

u32 netrocketClcRead(struct netbuf *src, struct netclient *srccl)
{
	const u32 syncid = netbufReadU32(src);
	const u8 flags = netbufReadU8(src);
	struct rocketstate st;
	const bool ok = netrocketReadState(src, &st);

	if (src->error || !ok || srccl->state < CLSTATE_GAME || !srccl->player || srccl->playernum >= MAX_PLAYERS) {
		return src->error;
	}

	struct player *pl = srccl->player;
	struct pilot *cur = &g_SvPilots[srccl->playernum];
	if (g_Vars.players[srccl->playernum] != pl || !cur->rocket || cur->syncid != syncid
			|| !netrocketAlive(cur) || netrocketOf(pl) != cur->rocket) {
		// not flying that one (any more)
		return src->error;
	}

	cur->lasttick = g_NetTick;
	if (!(flags & RF_NOSTATE)) {
		netrocketApplyState(cur->rocket, &st);
		cur->lastpos = st.pos;
	}

	if (flags & RF_EXPLODED) {
		// it goes off when it next ticks, where the pilot saw it go off
		cur->puppet = 0;
		cur->rocket->timer240 = 0;
		sysLogPrintf(LOG_NOTE, "NET: rocket %u of client %u: blew up for its pilot at %.0f %.0f %.0f",
			syncid, srccl->id, cur->lastpos.x, cur->lastpos.y, cur->lastpos.z);
	} else if (flags & RF_RELEASED) {
		cur->puppet = 0;
		pl->slayerrocket = NULL;
		pl->visionmode = VISIONMODE_SLAYERROCKETSTATIC;
	}

	return src->error;
}

/* client */

// The host says this prop is gone. If it is a guided rocket in flight, it blew up there:
// let our copy go off where it is instead of just vanishing.
s32 netrocketOnRemove(struct prop *prop)
{
	if (g_NetMode != NETMODE_CLIENT || prop->type != PROPTYPE_WEAPON || !prop->weapon) {
		return false;
	}

	struct weaponobj *rocket = prop->weapon;

	if (g_ClPilot.rocket == rocket && netrocketAlive(&g_ClPilot)) {
		// ours: the tick sees it go and winds the flight up
		rocket->timer240 = 0;
		return true;
	}

	for (s32 i = 0; i < ARRAYCOUNT(g_ClWatch); ++i) {
		struct pilot *w = &g_ClWatch[i];
		if (w->rocket == rocket && netrocketAlive(w)) {
			sysLogPrintf(LOG_NOTE, "NET: rocket %u: followed it for %u updates, blew up at %.0f %.0f %.0f", w->syncid,
				w->nummoves, prop->pos.x, prop->pos.y, prop->pos.z);
			w->puppet = 0;
			w->rocket = NULL;
			rocket->timer240 = 0;
			return true;
		}
	}

	return false;
}

static void netrocketClientTick(void)
{
	for (s32 i = 0; i < ARRAYCOUNT(g_ClWatch); ++i) {
		struct pilot *w = &g_ClWatch[i];
		if (w->rocket && (!netrocketAlive(w) || g_NetTick - w->lasttick > WATCH_TIMEOUT)) {
			// gone some other way, or the host went quiet about it
			sysLogPrintf(LOG_NOTE, "NET: rocket %u: lost track of it after %u updates (%s)", w->syncid, w->nummoves,
				netrocketAlive(w) ? "no news" : "gone here");
			w->rocket = NULL;
			w->puppet = 0;
		}
	}

	if (g_ClGoneReply) {
		memset(&g_ClLast, 0, sizeof(g_ClLast));
		netbufWriteU8(&g_NetMsgRel, CLC_ROCKET);
		netbufWriteU32(&g_NetMsgRel, g_ClGoneReply);
		netbufWriteU8(&g_NetMsgRel, RF_EXPLODED | RF_NOSTATE);
		netrocketWriteState(&g_NetMsgRel, &g_ClLast);
		g_ClGoneReply = 0;
	}

	if (!g_ClPilot.rocket) {
		return;
	}

	struct weaponobj *rocket = netrocketOf(g_NetLocalClient->player);

	if (rocket == g_ClPilot.rocket && rocket->base.prop->syncid == g_ClPilot.syncid) {
		netrocketGetState(rocket, &g_ClLast);
		netbufWriteU8(&g_NetMsg, CLC_ROCKET);
		netbufWriteU32(&g_NetMsg, g_ClPilot.syncid);
		netbufWriteU8(&g_NetMsg, 0);
		netrocketWriteState(&g_NetMsg, &g_ClLast);
		return;
	}

	// over: it blew up here, or we stopped flying it (died)
	const bool alive = netrocketAlive(&g_ClPilot);
	if (alive) {
		netrocketGetState(g_ClPilot.rocket, &g_ClLast);
	}
	netbufWriteU8(&g_NetMsgRel, CLC_ROCKET);
	netbufWriteU32(&g_NetMsgRel, g_ClPilot.syncid);
	netbufWriteU8(&g_NetMsgRel, alive ? RF_RELEASED : RF_EXPLODED);
	netrocketWriteState(&g_NetMsgRel, &g_ClLast);
	sysLogPrintf(LOG_NOTE, "NET: rocket %u: our flight is over (%s) at %.0f %.0f %.0f", g_ClPilot.syncid,
		alive ? "let go" : "blew up", g_ClLast.pos.x, g_ClLast.pos.y, g_ClLast.pos.z);
	g_ClPilot.rocket = NULL;
	g_ClPilot.syncid = 0;
}

u32 netrocketSvcRead(struct netbuf *src, struct netclient *srccl)
{
	const u8 kind = netbufReadU8(src);
	const u8 clid = netbufReadU8(src);
	const u32 syncid = netbufReadU32(src);
	struct rocketstate st;
	struct coord endpos = { 0.f, 0.f, 0.f };
	bool ok = true;
	if (kind == RK_MOVE) {
		ok = netrocketReadState(src, &st);
	} else if (kind == RK_GONE || kind == RK_RELEASE) {
		netbufReadCoord(src, &endpos);
		ok = isfinite(endpos.x) && isfinite(endpos.y) && isfinite(endpos.z);
	}

	if (src->error || !ok || srccl->state < CLSTATE_GAME || !g_NetLocalClient || !g_NetLocalClient->player) {
		return src->error;
	}

	const bool mine = (clid == g_NetLocalClient->id);
	struct player *pl = g_NetLocalClient->player;

	switch (kind) {
	case RK_START:
		if (mine && !pl->isdead && pl->prop) {
			struct weaponobj *rocket = netrocketFind(syncid);
			if (rocket) {
				const s32 prevplayernum = g_Vars.currentplayernum;
				setCurrentPlayerNum(playermgrGetPlayerNumByProp(pl->prop));
				playerLaunchSlayerRocket(rocket);
				setCurrentPlayerNum(prevplayernum);
				g_ClPilot.rocket = rocket;
				g_ClPilot.syncid = syncid;
				netrocketGetState(rocket, &g_ClLast);
				sysLogPrintf(LOG_NOTE, "NET: rocket %u: ours to fly", syncid);
			} else {
				// (fired straight into a wall: it blew up before the message got here)
				sysLogPrintf(LOG_NOTE, "NET: rocket %u: told to fly it, but it is gone already", syncid);
				g_ClGoneReply = syncid;
			}
		} else if (mine) {
			g_ClGoneReply = syncid;
		}
		break;
	case RK_GONE:
	case RK_RELEASE:
		if (mine && g_ClPilot.syncid == syncid && g_ClPilot.rocket) {
			if (netrocketAlive(&g_ClPilot)) {
				if (kind == RK_GONE) {
					g_ClPilot.rocket->timer240 = 0;
				}
				if (pl->slayerrocket == g_ClPilot.rocket) {
					pl->slayerrocket = NULL;
					pl->visionmode = VISIONMODE_SLAYERROCKETSTATIC;
				}
			}
			sysLogPrintf(LOG_NOTE, "NET: rocket %u: the host ended our flight (%s)", syncid, kind == RK_GONE ? "blew up" : "let go");
			// the host knows; nothing to report back
			g_ClPilot.rocket = NULL;
			g_ClPilot.syncid = 0;
		} else if (!mine) {
			for (s32 i = 0; i < ARRAYCOUNT(g_ClWatch); ++i) {
				struct pilot *w = &g_ClWatch[i];
				if (w->rocket && w->syncid == syncid) {
					w->puppet = 0;
					if (netrocketAlive(w) && kind == RK_GONE) {
						w->rocket->base.prop->pos = endpos;
						w->rocket->timer240 = 0;
					}
					sysLogPrintf(LOG_NOTE, "NET: rocket %u: followed it for %u updates, %s at %.0f %.0f %.0f", syncid,
						w->nummoves, kind == RK_GONE ? "blew up" : "let go", endpos.x, endpos.y, endpos.z);
					w->rocket = NULL;
				}
			}
		}
		break;
	case RK_MOVE:
		if (!(mine && g_ClPilot.syncid == syncid)) {
			struct weaponobj *rocket = netrocketFind(syncid);
			if (rocket) {
				netrocketApplyState(rocket, &st);
				struct pilot *w = NULL;
				for (s32 i = 0; i < ARRAYCOUNT(g_ClWatch); ++i) {
					if (g_ClWatch[i].rocket == rocket && g_ClWatch[i].syncid == syncid) {
						w = &g_ClWatch[i];
						break;
					}
					if (!w && !g_ClWatch[i].rocket) {
						w = &g_ClWatch[i];
					}
				}
				if (w) {
					if (!w->rocket) {
						w->rocket = rocket;
						w->syncid = syncid;
						w->nummoves = 0;
						sysLogPrintf(LOG_NOTE, "NET: rocket %u of client %u: following it", syncid, clid);
					}
					w->puppet = 1;
					w->lasttick = g_NetTick;
					++w->nummoves;
					w->lastpos = st.pos;
				}
			}
		}
		break;
	default:
		break;
	}

	return src->error;
}

void netrocketTick(s32 sendmoves)
{
	if (!g_NetLocalClient || g_NetLocalClient->state < CLSTATE_GAME || !g_NetLocalClient->player) {
		return;
	}
	if (g_NetMode == NETMODE_SERVER) {
		netrocketServerTick(sendmoves);
	} else if (g_NetMode == NETMODE_CLIENT) {
		netrocketClientTick();
	}
}
