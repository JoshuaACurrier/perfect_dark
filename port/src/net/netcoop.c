// Mission state sync for online co-op (and later counter-op).
//
// The server runs every mission script. Clients don't (see chraiExecute), so
// whatever a script changes that clients need to know about is sent here:
// - objective statuses (SVC_OBJECTIVES), so the HUD and end screen agree
// - HUD messages created by scripts for remote players (SVC_HUDMSG)
// - alarm, gas and the countdown timer (SVC_MISSIONSTATE)
// - music changes (SVC_MUSIC)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include "types.h"
#include "constants.h"
#include "data.h"
#include "bss.h"
#include "game/hudmsg.h"
#include "game/chr.h"
#include "game/inv.h"
#include "game/bondgun.h"
#include "game/player.h"
#include "game/objectives.h"
#include "game/playermgr.h"
#include "game/propobj.h"
#include "game/music.h"
#include "game/options.h"
#include "system.h"
#include "net/net.h"
#include "net/netbuf.h"
#include "net/netmsg.h"
#include "net/netcoop.h"
#include "net/netchr.h"
#include "net/netjoin.h"

// > 0 while the server is executing an AI or mission script
s32 g_NetInScript = 0;

// client: objective statuses from the server
static u8 g_NetObjStatus[MAX_OBJECTIVES];
static s32 g_NetObjValid = false;

// server: what we last sent
static u8 g_NetObjSent[MAX_OBJECTIVES];
static s32 g_NetObjSentCount = -1;

static void netcoopMissionStateReset(void);
static void netcoopMissionStateTick(void);
static void netcoopObjStateTick(void);
static void netcoopAntiTick(void);

/* object state: what scripts (and damage) change about the things in the level */

// The parts of an object that a mission script switches: shown or hidden, lockable,
// collectable, invincible, a door's locks, and how destroyed it is. Clients run no scripts,
// so the host compares each object with what it last sent and sends what changed.
struct netobjstate {
	u32 flags;
	u32 flags2;
	u32 flags3;
	u32 keyflags;
	s16 damage;
	u8 destroyed;
	u8 valid;
};

static struct netobjstate *g_ObjInit = NULL; // host: as the stage loaded
static struct netobjstate *g_ObjSent = NULL; // host: as the clients know it
static s32 g_ObjNum = 0;
static s32 g_ObjCursor = 0;

// which bits of an object's flags are synced; the same on both ends
static void netcoopObjMasks(const struct defaultobj *obj, u32 *m1, u32 *m2, u32 *m3)
{
	*m1 = OBJFLAG_FORCEMORTAL | OBJFLAG_INVINCIBLE | OBJFLAG_COLLECTABLE | OBJFLAG_UNCOLLECTABLE
		| OBJFLAG_CANNOT_ACTIVATE;
	*m2 = OBJFLAG2_SKIPDOORLOCKEDMSG | OBJFLAG2_IMMUNETOGUNFIRE | OBJFLAG2_INVISIBLE
		| OBJFLAG2_IMMUNETOEXPLOSIONS | OBJFLAG2_LOCKEDFRONT | OBJFLAG2_LOCKEDBACK;
	*m3 = OBJFLAG3_PUSHABLE | OBJFLAG3_GRABBABLE | OBJFLAG3_WALKTHROUGH | OBJFLAG3_SHOWSHIELD
		| OBJFLAG3_REACTTOSIGHT | OBJFLAG3_INTERACTABLE | OBJFLAG3_RTRACKED_YELLOW
		| OBJFLAG3_RTRACKED_BLUE | OBJFLAG3_INFRARED;

	// the top bits of the first word mean something different for each kind of object, and
	// for some they are the machine's own business (a hovercar's "set up" mark, what a
	// security camera sees)
	switch (obj->type) {
	case OBJTYPE_DOOR:
		*m1 |= OBJFLAG_DOOR_KEEPOPEN;
		break;
	case OBJTYPE_LIFT:
		*m1 |= OBJFLAG_DEACTIVATED | OBJFLAG_LIFT_TRIGGERDISABLE;
		break;
	case OBJTYPE_CCTV:
		*m1 |= OBJFLAG_DEACTIVATED | OBJFLAG_CAMERA_DISABLED;
		break;
	case OBJTYPE_AUTOGUN:
		*m1 |= OBJFLAG_DEACTIVATED | OBJFLAG_AUTOGUN_DAMAGED;
		*m2 |= OBJFLAG2_AUTOGUN_MALFUNCTIONING1 | OBJFLAG2_AUTOGUN_MALFUNCTIONING2;
		break;
	case OBJTYPE_BASIC:
	case OBJTYPE_ALARM:
	case OBJTYPE_SINGLEMONITOR:
	case OBJTYPE_MULTIMONITOR:
	case OBJTYPE_HANGINGMONITORS:
	case OBJTYPE_KEY:
	case OBJTYPE_SHIELD:
	case OBJTYPE_HOVERPROP:
		*m1 |= OBJFLAG_DEACTIVATED;
		break;
	default:
		break;
	}
}

// an object that was in the level when it loaded (those have the same id on every machine)
static s32 netcoopObjStateNow(s32 index, struct netobjstate *st)
{
	const struct prop *prop = &g_Vars.props[index];
	memset(st, 0, sizeof(*st));

	if (prop->syncid != (u32)(index + 1) || !prop->obj
			|| (prop->type != PROPTYPE_OBJ && prop->type != PROPTYPE_WEAPON && prop->type != PROPTYPE_DOOR)) {
		return false;
	}

	const struct defaultobj *obj = prop->obj;
	u32 m1, m2, m3;
	netcoopObjMasks(obj, &m1, &m2, &m3);
	st->flags = obj->flags & m1;
	st->flags2 = obj->flags2 & m2;
	st->flags3 = obj->flags3 & m3;
	st->keyflags = (prop->type == PROPTYPE_DOOR) ? prop->door->keyflags : 0;
	st->damage = obj->damage;
	st->destroyed = (obj->hidden2 & OBJH2FLAG_DESTROYED) ? 1 : 0;
	st->valid = 1;
	return true;
}

static inline s32 netcoopObjStateSame(const struct netobjstate *a, const struct netobjstate *b)
{
	return a->flags == b->flags && a->flags2 == b->flags2 && a->flags3 == b->flags3
		&& a->keyflags == b->keyflags && a->damage == b->damage && a->destroyed == b->destroyed;
}

void netcoopOnStageLoaded(void)
{
	free(g_ObjInit);
	free(g_ObjSent);
	g_ObjInit = NULL;
	g_ObjSent = NULL;
	g_ObjNum = 0;
	g_ObjCursor = 0;

	if (g_NetMode != NETMODE_SERVER || g_Vars.normmplayerisrunning
			|| g_StageNum == STAGE_TITLE || g_StageNum == STAGE_CITRAINING || g_Vars.maxprops <= 0) {
		return;
	}

	g_ObjInit = calloc(g_Vars.maxprops, sizeof(*g_ObjInit));
	g_ObjSent = calloc(g_Vars.maxprops, sizeof(*g_ObjSent));
	if (!g_ObjInit || !g_ObjSent) {
		free(g_ObjInit);
		free(g_ObjSent);
		g_ObjInit = NULL;
		g_ObjSent = NULL;
		return;
	}

	g_ObjNum = g_Vars.maxprops;
	for (s32 i = 0; i < g_ObjNum; ++i) {
		netcoopObjStateNow(i, &g_ObjInit[i]);
		g_ObjSent[i] = g_ObjInit[i];
	}
}

// Someone has just loaded the stage as it was at the start: send everything that has
// changed since then again (to everyone; those who have it already see no difference).
void netcoopOnJoin(void)
{
	g_NetObjSentCount = -1;
	netcoopMissionStateReset();
	if (g_ObjInit && g_ObjSent) {
		memcpy(g_ObjSent, g_ObjInit, g_ObjNum * sizeof(*g_ObjSent));
		g_ObjCursor = 0;
	}
}

static void netcoopObjStateTick(void)
{
	if (!g_ObjSent || g_ObjNum > g_Vars.maxprops) {
		return;
	}

	// a few hundred objects a pass, and no more per pass than fits in the frame's packet
	s32 numsent = 0;
	for (s32 n = 0; n < g_ObjNum; ++n) {
		if (netbufWriteLeft(&g_NetMsgRel) < 256) {
			break;
		}
		const s32 i = g_ObjCursor;
		g_ObjCursor = (g_ObjCursor + 1) % g_ObjNum;

		if (!g_ObjSent[i].valid) {
			continue;
		}
		struct netobjstate now;
		if (!netcoopObjStateNow(i, &now) || netcoopObjStateSame(&now, &g_ObjSent[i])) {
			// (gone: the removal has its own message)
			continue;
		}

		g_ObjSent[i] = now;
		netbufWriteU8(&g_NetMsgRel, SVC_PROP_STATE);
		netbufWriteU16(&g_NetMsgRel, i + 1);
		netbufWriteU32(&g_NetMsgRel, now.flags);
		netbufWriteU32(&g_NetMsgRel, now.flags2);
		netbufWriteU32(&g_NetMsgRel, now.flags3);
		netbufWriteU32(&g_NetMsgRel, now.keyflags);
		netbufWriteS16(&g_NetMsgRel, now.damage);
		netbufWriteU8(&g_NetMsgRel, now.destroyed);
		++numsent;
	}

	if (numsent) {
		sysLogPrintf(LOG_NOTE | LOGFLAG_NOCON, "NET: object state: sent %d change(s)", numsent);
	}
}

u32 netmsgSvcPropStateRead(struct netbuf *src, struct netclient *srccl)
{
	const u16 syncid = netbufReadU16(src);
	const u32 flags = netbufReadU32(src);
	const u32 flags2 = netbufReadU32(src);
	const u32 flags3 = netbufReadU32(src);
	const u32 keyflags = netbufReadU32(src);
	const s16 damage = netbufReadS16(src);
	const u8 destroyed = netbufReadU8(src);

	if (src->error || srccl->state < CLSTATE_GAME || !syncid || syncid > g_Vars.maxprops) {
		return src->error;
	}

	struct prop *prop = &g_Vars.props[syncid - 1];
	if (prop->syncid != syncid || !prop->obj
			|| (prop->type != PROPTYPE_OBJ && prop->type != PROPTYPE_WEAPON && prop->type != PROPTYPE_DOOR)) {
		// gone here already
		return src->error;
	}

	struct defaultobj *obj = prop->obj;
	u32 m1, m2, m3;
	netcoopObjMasks(obj, &m1, &m2, &m3);
	obj->flags = (obj->flags & ~m1) | (flags & m1);
	obj->flags2 = (obj->flags2 & ~m2) | (flags2 & m2);
	obj->flags3 = (obj->flags3 & ~m3) | (flags3 & m3);
	if (prop->type == PROPTYPE_DOOR) {
		prop->door->keyflags = keyflags;
	}
	obj->damage = damage;
	if (destroyed) {
		obj->hidden2 |= OBJH2FLAG_DESTROYED;
	} else {
		obj->hidden2 &= ~OBJH2FLAG_DESTROYED;
	}

	static u32 numapplied = 0;
	++numapplied;
	if ((numapplied & (numapplied - 1)) == 0) {
		// (1st, 2nd, 4th, 8th...: enough to see in a log that it works)
		sysLogPrintf(LOG_NOTE | LOGFLAG_NOCON, "NET: object state: %u applied so far (last: syncid %u)", numapplied, syncid);
	}

	return src->error;
}

/* counter-op: which guard a player took over */

// The host picks the guard a counter-operative spawns into (the one nearest Joanna that she
// isn't looking at), removes it, and gives the player its place, looks, gun and health. Place
// and health travel with the normal player messages; the rest is here.
struct netantispawn {
	u8 valid;
	u8 unsent;      // host: made while the stage was loading, goes out with the first update
	u8 dual;
	s8 weaponnum;
	s16 bodynum;
	s16 headnum;
	u32 guardsyncid;
};

// host: by player number, the last one for each. client: by local player number, not applied yet.
static struct netantispawn g_AntiSpawn[MAX_PLAYERS];

static void netcoopAntiSpawnWrite(struct netbuf *dst, s32 playernum)
{
	const struct netantispawn *as = &g_AntiSpawn[playernum];
	const struct player *pl = g_Vars.players[playernum];
	const struct netclient *cl = (pl && pl->client) ? pl->client : NULL;
	if (!as->valid || !cl) {
		return;
	}
	netbufWriteU8(dst, SVC_ANTI_SPAWN);
	netbufWriteU8(dst, cl->id);
	netbufWriteU32(dst, as->guardsyncid);
	netbufWriteS16(dst, as->bodynum);
	netbufWriteS16(dst, as->headnum);
	netbufWriteS8(dst, as->weaponnum);
	netbufWriteU8(dst, as->dual);
}

void netcoopOnAntiSpawn(struct chrdata *hostchr)
{
	const s32 p = g_Vars.currentplayernum;
	if (g_NetMode != NETMODE_SERVER || p < 0 || p >= MAX_PLAYERS || !hostchr || !hostchr->prop) {
		return;
	}

	struct netantispawn *as = &g_AntiSpawn[p];
	struct prop *held = hostchr->weapons_held[0] ? hostchr->weapons_held[0] : hostchr->weapons_held[1];
	as->valid = 1;
	as->guardsyncid = hostchr->prop->syncid;
	as->bodynum = g_Vars.antibodynum;
	as->headnum = g_Vars.antiheadnum;
	as->dual = (hostchr->weapons_held[0] && hostchr->weapons_held[1]) ? 1 : 0;
	as->weaponnum = (held && held->weapon) ? held->weapon->weaponnum : WEAPON_UNARMED;
	if (as->weaponnum == WEAPON_SUPERDRAGON) {
		as->weaponnum = WEAPON_DRAGON;
	}

	sysLogPrintf(LOG_NOTE, "NET: counter-op: player %d takes over guard %d (syncid %u): body %d, head %d, weapon %d%s",
		p, hostchr->chrnum, as->guardsyncid, as->bodynum, as->headnum, as->weaponnum, as->dual ? " x2" : "");

	if (g_Vars.lvframenum > 0 && g_NetLocalClient && g_NetLocalClient->state >= CLSTATE_GAME) {
		as->unsent = 0;
		netcoopAntiSpawnWrite(&g_NetMsgRel, p);
	} else {
		// the stage is still loading: nothing sent now would arrive
		as->unsent = 1;
	}
}

// for someone who joins mid-mission: who is which guard now
void netcoopAntiWriteAll(struct netbuf *dst)
{
	if (g_Vars.antiplayernum < 0) {
		return;
	}
	for (s32 p = 0; p < MAX_PLAYERS; ++p) {
		if (g_Vars.players[p] && !g_Vars.players[p]->isdead && !netjoinIsParked(p)) {
			netcoopAntiSpawnWrite(dst, p);
		}
	}
}

static void netcoopAntiTick(void)
{
	for (s32 p = 0; p < MAX_PLAYERS; ++p) {
		if (g_AntiSpawn[p].valid && g_AntiSpawn[p].unsent) {
			g_AntiSpawn[p].unsent = 0;
			netcoopAntiSpawnWrite(&g_NetMsgRel, p);
		}
	}
}

// client: make local player p the guard the host said it is now
static void netcoopAntiApply(s32 p)
{
	struct netantispawn *as = &g_AntiSpawn[p];
	struct player *pl = g_Vars.players[p];
	if (!as->valid || !pl || !pl->prop || !pl->prop->chr) {
		return;
	}
	as->valid = 0;

	const s32 prevplayernum = g_Vars.currentplayernum;
	setCurrentPlayerNum(p);

	// the guard itself is gone
	struct prop *guard = netPropBySyncId(as->guardsyncid);
	if (guard && guard->type == PROPTYPE_CHR && guard->chr && !guard->chr->aibot) {
		netchrAllowRemove(guard->chr);
		guard->chr->chrflags |= CHRCFLAG_HIDDEN;
		guard->chr->hidden |= CHRHFLAG_DELETING;
	}

	// its gun (other players' guns come with their stats)
	if (g_NetLocalClient && pl == g_NetLocalClient->player && as->weaponnum > WEAPON_UNARMED) {
		if (as->dual) {
			invGiveSingleWeapon(as->weaponnum);
			invGiveDoubleWeapon(as->weaponnum, as->weaponnum);
			bgunEquipWeapon2(HAND_RIGHT, as->weaponnum);
			bgunEquipWeapon2(HAND_LEFT, as->weaponnum);
		} else {
			invGiveSingleWeapon(as->weaponnum);
			bgunEquipWeapon2(HAND_RIGHT, as->weaponnum);
		}
		pl->invdowntime = TICKS(-40);
		pl->usedowntime = TICKS(-40);
	}

	// and its looks: drop the model and build a new one, as the host did
	if (as->bodynum >= 0 && pl->haschrbody && pl->prop->chr->model) {
		g_Vars.antibodynum = as->bodynum;
		g_Vars.antiheadnum = as->headnum;
		pl->haschrbody = false;
		pl->model00d4 = NULL;
		chrRemove(pl->prop, false);
		playerTickChrBody();
	}

	sysLogPrintf(LOG_NOTE, "NET: counter-op: player %d is now guard syncid %u: body %d, head %d, weapon %d%s",
		p, as->guardsyncid, pl->prop->chr->bodynum, pl->prop->chr->headnum, as->weaponnum, as->dual ? " x2" : "");

	setCurrentPlayerNum(prevplayernum);
}

// host: the player spawned while the stage was loading; it takes over a guard once it is up
static u8 g_AntiDeferred[MAX_PLAYERS];

void netcoopAntiDeferSpawn(void)
{
	if (g_Vars.currentplayernum >= 0 && g_Vars.currentplayernum < MAX_PLAYERS) {
		g_AntiDeferred[g_Vars.currentplayernum] = 1;
	}
}

// every frame. Host: the takeovers put off at load. Client: a takeover that arrived while
// its player could not be touched yet.
void netcoopClientTick(void)
{
	if (!g_NetMode || g_Vars.antiplayernum < 0 || g_Vars.tickmode != TICKMODE_NORMAL || g_Vars.lvframenum <= 2
			|| !g_NetLocalClient || g_NetLocalClient->state < CLSTATE_GAME) {
		return;
	}
	if (g_NetMode == NETMODE_SERVER) {
		for (s32 p = 0; p < MAX_PLAYERS; ++p) {
			if (g_AntiDeferred[p]) {
				g_AntiDeferred[p] = 0;
				if (g_Vars.players[p] && g_Vars.players[p] != g_Vars.bond && !netjoinIsParked(p)) {
					g_Vars.players[p]->dostartnewlife = true;
				}
			}
		}
		return;
	}
	for (s32 p = 0; p < MAX_PLAYERS; ++p) {
		if (g_AntiSpawn[p].valid && g_Vars.players[p] && !g_Vars.players[p]->isdead) {
			netcoopAntiApply(p);
		}
	}
}

void netcoopAntiClientSpawn(void)
{
	if (g_Vars.currentplayernum >= 0 && g_Vars.currentplayernum < MAX_PLAYERS) {
		netcoopAntiApply(g_Vars.currentplayernum);
	}
}

u32 netmsgSvcAntiSpawnRead(struct netbuf *src, struct netclient *srccl)
{
	const u8 clid = netbufReadU8(src);
	const u32 guardsyncid = netbufReadU32(src);
	const s16 bodynum = netbufReadS16(src);
	const s16 headnum = netbufReadS16(src);
	const s8 weaponnum = netbufReadS8(src);
	const u8 dual = netbufReadU8(src);

	if (src->error || srccl->state < CLSTATE_GAME || clid >= NET_MAX_CLIENTS) {
		return src->error;
	}

	const struct netclient *cl = &g_NetClients[clid];
	if (cl->state < CLSTATE_GAME || !cl->player || cl->playernum >= MAX_PLAYERS
			|| bodynum < 0 || bodynum >= ARRAYCOUNT(g_HeadsAndBodies) || headnum >= ARRAYCOUNT(g_HeadsAndBodies)
			|| weaponnum < WEAPON_UNARMED || weaponnum > WEAPON_COMBATBOOST) {
		return src->error;
	}

	struct netantispawn *as = &g_AntiSpawn[cl->playernum];
	as->valid = 1;
	as->guardsyncid = guardsyncid;
	as->bodynum = bodynum;
	as->headnum = headnum;
	as->weaponnum = weaponnum;
	as->dual = dual;

	// usually this is here before the player's "alive again", and is applied when they
	// spawn; if they are up already, now
	if (!cl->player->isdead && g_Vars.lvframenum > 2) {
		netcoopAntiApply(cl->playernum);
	}

	return src->error;
}

void netcoopReset(void)
{
	// (on the host this runs while the stage loads, after the players have spawned: the
	// takeovers made during this load are still waiting to be sent)
	for (s32 p = 0; p < MAX_PLAYERS; ++p) {
		if (g_NetMode != NETMODE_SERVER || !g_AntiSpawn[p].unsent) {
			memset(&g_AntiSpawn[p], 0, sizeof(g_AntiSpawn[p]));
		}
	}
	memset(g_NetObjStatus, OBJECTIVE_INCOMPLETE, sizeof(g_NetObjStatus));
	memset(g_NetObjSent, 0xff, sizeof(g_NetObjSent));
	g_NetObjValid = false;
	g_NetObjSentCount = -1;
	netcoopMissionStateReset();
}

s32 netcoopGetObjectiveStatus(s32 index, s32 *status)
{
	if (g_NetMode != NETMODE_CLIENT || !g_NetObjValid || index < 0 || index >= MAX_OBJECTIVES) {
		return false;
	}
	*status = g_NetObjStatus[index];
	return true;
}

/* objectives */

// in the log of every machine when they change (0 incomplete, 1 complete, 2 failed)
static void netcoopLogObjectives(const u8 *statuses, s32 count)
{
	static char prev[64];
	char line[64];
	s32 len = 0;
	line[0] = 0;
	for (s32 i = 0; i < count && len < (s32)sizeof(line) - 4; ++i) {
		len += snprintf(line + len, sizeof(line) - len, " %u", statuses[i]);
	}
	if (strcmp(line, prev)) {
		strcpy(prev, line);
		sysLogPrintf(LOG_NOTE | LOGFLAG_NOCON, "NET: objectives:%s", line);
	}
}

void netcoopServerTick(void)
{
	if (g_NetMode != NETMODE_SERVER || g_NetLocalClient->state < CLSTATE_GAME) {
		return;
	}

	// only missions have objectives
	if (g_Vars.normmplayerisrunning) {
		return;
	}

	// about six times a second. (Not `g_NetTick % 10`: this is only called on update frames,
	// and with updates every other frame those can all be odd ticks.)
	static u32 nexttick = 0;
	if (g_NetTick < nexttick && nexttick - g_NetTick <= 10) {
		return;
	}
	nexttick = g_NetTick + 10;

	netcoopMissionStateTick();
	netcoopObjStateTick();
	netcoopAntiTick();

	const s32 count = objectiveGetCount();
	if (count <= 0 || count > MAX_OBJECTIVES) {
		return;
	}

	s32 changed = (count != g_NetObjSentCount);
	u8 now[MAX_OBJECTIVES];
	for (s32 i = 0; i < count; ++i) {
		now[i] = (u8)objectiveCheck(i);
		if (now[i] != g_NetObjSent[i]) {
			changed = true;
		}
	}

	if (!changed) {
		return;
	}

	memcpy(g_NetObjSent, now, count);
	g_NetObjSentCount = count;

	netbufWriteU8(&g_NetMsgRel, SVC_OBJECTIVES);
	netbufWriteU8(&g_NetMsgRel, count);
	for (s32 i = 0; i < count; ++i) {
		netbufWriteU8(&g_NetMsgRel, now[i]);
	}
	netcoopLogObjectives(now, count);
}

u32 netmsgSvcObjectivesRead(struct netbuf *src, struct netclient *srccl)
{
	const u8 count = netbufReadU8(src);
	u8 statuses[256];
	for (u32 i = 0; i < count; ++i) {
		statuses[i] = netbufReadU8(src);
	}

	if (src->error || srccl->state < CLSTATE_GAME) {
		return src->error;
	}

	for (u32 i = 0; i < count && i < MAX_OBJECTIVES; ++i) {
		g_NetObjStatus[i] = statuses[i];
	}
	g_NetObjValid = true;
	netcoopLogObjectives(statuses, count < MAX_OBJECTIVES ? count : MAX_OBJECTIVES);

	return src->error;
}

/* hud messages */

void netcoopForwardHudmsg(const char *text, s32 type, u32 flags)
{
	if (g_NetMode != NETMODE_SERVER || !g_NetInScript || !text) {
		return;
	}

	struct player *pl = g_Vars.currentplayer;
	if (!pl || !pl->isremote || !pl->client) {
		return;
	}

	struct netclient *cl = pl->client;
	if (cl->state < CLSTATE_GAME || !cl->peer) {
		return;
	}

	u8 data[512];
	struct netbuf buf = { .data = data, .size = sizeof(data) };
	netbufStartWrite(&buf);
	netbufWriteU8(&buf, SVC_HUDMSG);
	netbufWriteU8(&buf, (u8)type);
	netbufWriteU32(&buf, flags);
	netbufWriteStr(&buf, text);
	if (!buf.error) {
		netSend(cl, &buf, true, NETCHAN_DEFAULT);
	}
}

u32 netmsgSvcHudmsgRead(struct netbuf *src, struct netclient *srccl)
{
	const u8 type = netbufReadU8(src);
	const u32 flags = netbufReadU32(src);
	char *text = netbufReadStr(src);

	if (src->error || !text || srccl->state < CLSTATE_GAME) {
		return src->error;
	}

	const s32 prevplayernum = g_Vars.currentplayernum;
	setCurrentPlayerNum(0);
	hudmsgCreateWithFlags(text, type, flags);
	setCurrentPlayerNum(prevplayernum);

	return src->error;
}

/* mission state: alarm, gas, countdown timer */

struct netmissionstate {
	u8 alarm;
	u8 gas;
	u8 running;
	u32 timeroff;
	f32 timer60;
};

static struct netmissionstate g_NetMsSent;
static s32 g_NetMsSentValid = false;
static u32 g_NetMsLastSendTick = 0;

static void netcoopMissionStateReset(void)
{
	memset(&g_NetMsSent, 0, sizeof(g_NetMsSent));
	g_NetMsSentValid = false;
	g_NetMsLastSendTick = 0;
}

static void netcoopMissionStateNow(struct netmissionstate *ms)
{
	ms->alarm = alarmIsActive() ? 1 : 0;
	ms->gas = (g_GasReleasing || g_GasReleaseTimer240 > 0) ? 1 : 0;
	ms->running = g_CountdownTimerRunning ? 1 : 0;
	ms->timeroff = g_CountdownTimerOff;
	ms->timer60 = g_CountdownTimerValue60;
}

static void netcoopMissionStateTick(void)
{
	struct netmissionstate now;
	netcoopMissionStateNow(&now);

	s32 send = !g_NetMsSentValid
		|| now.alarm != g_NetMsSent.alarm
		|| now.gas != g_NetMsSent.gas
		|| now.running != g_NetMsSent.running
		|| now.timeroff != g_NetMsSent.timeroff;

	if (!send) {
		// the timer runs on the client too; correct it now and then, and right
		// away if a script set it to something new
		const f32 expected = g_NetMsSent.timer60 - (g_NetMsSent.running ? (f32)(g_NetTick - g_NetMsLastSendTick) : 0.f);
		const f32 diff = now.timer60 - expected;
		if (diff > 30.f || diff < -30.f || (now.running && g_NetTick - g_NetMsLastSendTick >= 120)) {
			send = true;
		}
	}

	if (!send) {
		return;
	}

	g_NetMsSent = now;
	g_NetMsSentValid = true;
	g_NetMsLastSendTick = g_NetTick;

	netbufWriteU8(&g_NetMsgRel, SVC_MISSIONSTATE);
	netbufWriteU8(&g_NetMsgRel, now.alarm);
	netbufWriteU8(&g_NetMsgRel, now.gas);
	if (now.gas) {
		netbufWriteCoord(&g_NetMsgRel, &g_GasPos);
	}
	netbufWriteU8(&g_NetMsgRel, now.running);
	netbufWriteU32(&g_NetMsgRel, now.timeroff);
	netbufWriteF32(&g_NetMsgRel, now.timer60);
}

u32 netmsgSvcMissionStateRead(struct netbuf *src, struct netclient *srccl)
{
	const u8 alarm = netbufReadU8(src);
	const u8 gas = netbufReadU8(src);
	struct coord gaspos = { 0.f, 0.f, 0.f };
	if (gas) {
		netbufReadCoord(src, &gaspos);
	}
	const u8 running = netbufReadU8(src);
	const u32 timeroff = netbufReadU32(src);
	const f32 timer60 = netbufReadF32(src);

	if (src->error || srccl->state < CLSTATE_GAME) {
		return src->error;
	}

	if (alarm && !alarmIsActive()) {
		sysLogPrintf(LOG_NOTE | LOGFLAG_NOCON, "NET: mission state: alarm on");
		alarmActivate();
	} else if (!alarm && alarmIsActive()) {
		sysLogPrintf(LOG_NOTE | LOGFLAG_NOCON, "NET: mission state: alarm off");
		alarmDeactivate();
	}

	if (gas && !g_GasReleasing && g_GasReleaseTimer240 <= 0) {
		sysLogPrintf(LOG_NOTE | LOGFLAG_NOCON, "NET: mission state: gas released");
		gasReleaseFromPos(&gaspos);
	}

	if (timeroff != g_CountdownTimerOff || running != (g_CountdownTimerRunning ? 1 : 0)) {
		sysLogPrintf(LOG_NOTE | LOGFLAG_NOCON, "NET: mission state: countdown %s, %s, %.0f s",
			timeroff ? "hidden" : "shown", running ? "running" : "stopped", timer60 / 60.f);
	}

	g_CountdownTimerOff = timeroff;
	countdownTimerSetRunning(running);
	countdownTimerSetValue60(timer60);

	return src->error;
}

/* music changes made by mission scripts */

void netcoopMusic(u8 op, s32 a, s32 b, s32 c)
{
	if (g_NetMode != NETMODE_SERVER || !g_NetLocalClient || g_NetLocalClient->state < CLSTATE_GAME) {
		return;
	}
	netbufWriteU8(&g_NetMsgRel, SVC_MUSIC);
	netbufWriteU8(&g_NetMsgRel, op);
	netbufWriteS32(&g_NetMsgRel, a);
	netbufWriteS32(&g_NetMsgRel, b);
	netbufWriteS32(&g_NetMsgRel, c);
}

u32 netmsgSvcMusicRead(struct netbuf *src, struct netclient *srccl)
{
	const u8 op = netbufReadU8(src);
	const s32 a = netbufReadS32(src);
	const s32 b = netbufReadS32(src);
	const s32 c = netbufReadS32(src);

	if (src->error || srccl->state < CLSTATE_GAME) {
		return src->error;
	}

	sysLogPrintf(LOG_NOTE | LOGFLAG_NOCON, "NET: music op %u (%d)", op, a);

	switch (op) {
		case NETMUSIC_SETXREASON: musicSetXReason(a, b, c); break;
		case NETMUSIC_UNSETXREASON: musicUnsetXReason(a); break;
		case NETMUSIC_ISOLATED: {
			const u16 volume = optionsGetMusicVolume();
			musicPlayTrackIsolated(a);
			if (a == MUSIC_CI_TRAINING) {
				optionsSetMusicVolume(volume);
			}
			break;
		}
		case NETMUSIC_DEFAULT: musicPlayDefaultTracks(); break;
		case NETMUSIC_CUTSCENE: musicStartCutscene(a); break;
		case NETMUSIC_ENDCUTSCENE: musicEndCutscene(); break;
		case NETMUSIC_AMBIENT: musicStartTemporaryAmbient(a); break;
		case NETMUSIC_ENDAMBIENT: musicEndTemporaryAmbient(); break;
		case NETMUSIC_PRIMARY: musicStartTemporaryPrimary(a); break;
		default: break;
	}

	return src->error;
}
