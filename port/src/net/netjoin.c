// Joining a Combat Simulator match that is already running, and leaving one.
// See netjoin.h for the idea.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "data.h"
#include "bss.h"
#include "constants.h"
#include "game/chr.h"
#include "game/lv.h"
#include "game/mplayer/mplayer.h"
#include "game/player.h"
#include "game/playermgr.h"
#include "game/prop.h"
#include "game/propobj.h"
#include "system.h"
#include "net/net.h"
#include "net/netbuf.h"
#include "net/netmsg.h"
#include "net/netjoin.h"
#include "net/netscen.h"
#include "net/netcoop.h"

extern s32 g_StageTimeElapsed60;
extern f32 g_StageTimeElapsed1f;

s32 g_NetServerSpareSlots = 2;
s32 g_NetServerJoinInProgress = 1;
s32 g_NetQuietDeath = false;

// by local player number
static u8 g_Vacant[MAX_PLAYERS];
static u8 g_Hold[MAX_PLAYERS]; // host: has a new owner who is still loading
static u8 g_Rebody[MAX_PLAYERS]; // the slot's body or head changed: rebuild its character

// body, head and team of the empty slots, by server player number (from the host)
static u8 g_VacantMask = 0;
static u8 g_VacantBody[MAX_PLAYERS];
static u8 g_VacantHead[MAX_PLAYERS];
static u8 g_VacantTeam[MAX_PLAYERS];

static s32 g_MySvNum = 0;

// host: what the stage looked like when the match started
static u64 g_StartSeeds[2];
static u8 *g_InitProps = NULL;         // 1 = this prop index held a prop at the start
static struct coord *g_InitPos = NULL; // and where it was
static u16 *g_InitParent = NULL;       // and the id of whoever held it (0: nobody)
static s32 g_InitNum = 0;
static u32 g_NextJoinTick = 0;

// client: a mid-match join in progress
#define LOAD_NONE    0
#define LOAD_WAITING 1 // told to load, stage not up yet
#define LOAD_READY   2 // stage is up, kept packets not played back yet

struct keptpacket {
	struct keptpacket *next;
	u32 len;
	u8 data[];
};

static s32 g_Loading = LOAD_NONE;
static struct keptpacket *g_KeptHead = NULL;
static struct keptpacket *g_KeptTail = NULL;
static s32 g_JoinElapsed60 = 0;
static u64 g_JoinStartUs = 0;
static u32 g_LoadedTick = 0;

static void netjoinResyncReset(void);
static s32 g_AwaitSpawn = false;

static inline bool netjoinSlotInMatch(s32 p)
{
	if (p < 0 || p >= MAX_PLAYERS || !g_Vars.players[p]) {
		return false;
	}
	return !g_Vars.normmplayerisrunning || (g_MpSetup.chrslots & (1 << p)) != 0;
}

s32 netjoinSpareSlots(void)
{
	return (g_NetServerJoinInProgress && g_NetServerSpareSlots > 0) ? g_NetServerSpareSlots : 0;
}

s32 netjoinIsVacant(s32 playernum)
{
	return g_NetMode && playernum >= 0 && playernum < MAX_PLAYERS && g_Vacant[playernum];
}

s32 netjoinIsParked(s32 playernum)
{
	return g_NetMode && playernum >= 0 && playernum < MAX_PLAYERS && (g_Vacant[playernum] || g_Hold[playernum]);
}

s32 netjoinIsVacantConfig(const struct mpchrconfig *cfg)
{
	for (s32 i = 0; i < MAX_PLAYERS; ++i) {
		if (cfg == &g_PlayerConfigsArray[i].base) {
			return netjoinIsVacant(i);
		}
	}
	return false;
}

void netjoinSetMySvNum(s32 svplayernum)
{
	g_MySvNum = svplayernum;
}

s32 netjoinLocalNum(s32 svplayernum)
{
	if (g_NetMode != NETMODE_CLIENT) {
		return svplayernum;
	}
	if (svplayernum == g_MySvNum) {
		return 0;
	}
	if (svplayernum == 0) {
		return g_MySvNum;
	}
	return svplayernum;
}

void netjoinReset(void)
{
	while (g_KeptHead) {
		struct keptpacket *kp = g_KeptHead;
		g_KeptHead = kp->next;
		free(kp);
	}
	g_KeptTail = NULL;
	g_Loading = LOAD_NONE;
	g_AwaitSpawn = false;

	free(g_InitProps);
	free(g_InitPos);
	free(g_InitParent);
	g_InitProps = NULL;
	g_InitPos = NULL;
	g_InitParent = NULL;
	g_InitNum = 0;

	// (g_NetTick starts over with every connection)
	g_NextJoinTick = 0;
	netjoinResyncReset();

	memset(g_Vacant, 0, sizeof(g_Vacant));
	memset(g_Hold, 0, sizeof(g_Hold));
	g_VacantMask = 0;
}

/* the tail of SVC_STAGE_START */

void netjoinSaveSeeds(void)
{
	g_StartSeeds[0] = g_RngSeed;
	g_StartSeeds[1] = g_Rng2Seed;
}

void netjoinGetSeeds(u64 *seed1, u64 *seed2)
{
	*seed1 = g_StartSeeds[0];
	*seed2 = g_StartSeeds[1];
}

void netjoinStageStartWrite(struct netbuf *dst, s32 join)
{
	// every player slot of the match that no client in the match owns
	u8 mask = g_MpSetup.chrslots & 0xff;
	for (s32 i = 0; i < g_NetMaxClients; ++i) {
		const struct netclient *cl = &g_NetClients[i];
		if (cl->state >= (join ? CLSTATE_GAME : CLSTATE_LOBBY) && (join || cl->config) && cl->playernum < MAX_PLAYERS) {
			mask &= ~(1 << cl->playernum);
		}
	}

	netbufWriteU8(dst, mask);
	for (s32 p = 0; p < MAX_PLAYERS; ++p) {
		if (mask & (1 << p)) {
			netbufWriteU8(dst, g_PlayerConfigsArray[p].base.mpbodynum);
			netbufWriteU8(dst, g_PlayerConfigsArray[p].base.mpheadnum);
			netbufWriteU8(dst, g_PlayerConfigsArray[p].base.team);
		}
	}

	netbufWriteU8(dst, join ? 1 : 0);
	if (join) {
		netbufWriteS32(dst, g_StageTimeElapsed60);
	}
}

void netjoinStageStartRead(struct netbuf *src)
{
	g_VacantMask = netbufReadU8(src);
	for (s32 p = 0; p < MAX_PLAYERS; ++p) {
		if (g_VacantMask & (1 << p)) {
			g_VacantBody[p] = netbufReadU8(src);
			g_VacantHead[p] = netbufReadU8(src);
			g_VacantTeam[p] = netbufReadU8(src);
		}
	}

	const u8 join = netbufReadU8(src);
	if (join) {
		g_JoinElapsed60 = netbufReadS32(src);
		g_JoinStartUs = sysGetMicroseconds();
		g_Loading = LOAD_WAITING;
		g_AwaitSpawn = true;
		sysLogPrintf(LOG_NOTE, "NET: joining a match in progress (%d s in)", g_JoinElapsed60 / 60);
	}
}

s32 netjoinClientAwaitingSpawn(s32 isdead)
{
	if (!g_AwaitSpawn) {
		return false;
	}
	if (!isdead) {
		g_AwaitSpawn = false;
		return false;
	}
	return true;
}

/* empty slots */

static void netjoinSetupVacant(s32 p)
{
	struct mpplayerconfig *cfg = &g_PlayerConfigsArray[p];
	cfg->controlmode = CONTROLMODE_NA;
	cfg->client = NULL;
	cfg->handicap = 0x80;
	cfg->options |= OPTION_ASKEDSAVEPLAYER;
	snprintf(cfg->base.name, sizeof(cfg->base.name), "Open\n");
	if (g_Vars.players[p]) {
		g_Vars.players[p]->client = NULL;
		g_Vars.players[p]->isremote = false;
	}
	g_Vacant[p] = 1;
	g_Hold[p] = 0;
}

void netjoinOnPlayersAllocated(void)
{
	memset(g_Vacant, 0, sizeof(g_Vacant));
	memset(g_Hold, 0, sizeof(g_Hold));
	memset(g_Rebody, 0, sizeof(g_Rebody));

	u8 owned = 0;
	for (s32 i = 0; i < g_NetMaxClients; ++i) {
		const struct netclient *cl = &g_NetClients[i];
		if (cl->state >= CLSTATE_LOBBY && cl->config && cl->playernum < MAX_PLAYERS) {
			owned |= 1 << cl->playernum;
		}
	}

	s32 num = 0;
	for (s32 p = 0; p < MAX_PLAYERS; ++p) {
		// (a mission's player slots are only written to chrslots after this)
		const bool inmatch = (g_Vars.coopplayernum >= 0 || g_Vars.antiplayernum >= 0) ? (g_Vars.players[p] != NULL) : ((g_MpSetup.chrslots & (1 << p)) != 0);
		if (inmatch && !(owned & (1 << p))) {
			if (g_NetMode == NETMODE_CLIENT) {
				// the same body on every machine
				for (s32 sv = 0; sv < MAX_PLAYERS; ++sv) {
					if ((g_VacantMask & (1 << sv)) && netjoinLocalNum(sv) == p) {
						g_PlayerConfigsArray[p].base.mpbodynum = g_VacantBody[sv];
						g_PlayerConfigsArray[p].base.mpheadnum = g_VacantHead[sv];
						g_PlayerConfigsArray[p].base.team = g_VacantTeam[sv];
					}
				}
			}
			netjoinSetupVacant(p);
			++num;
		}
	}

	if (num) {
		sysLogPrintf(LOG_NOTE, "NET: %d empty player slot(s) in this match", num);
	}
}

void netjoinOnStageLoaded(void)
{
	free(g_InitProps);
	free(g_InitPos);
	free(g_InitParent);
	g_InitProps = NULL;
	g_InitPos = NULL;
	g_InitParent = NULL;
	g_InitNum = 0;
	g_NextJoinTick = 0;
	netjoinResyncReset();

	if (g_StageNum == STAGE_TITLE || g_StageNum == STAGE_CITRAINING) {
		g_Loading = LOAD_NONE;
		g_AwaitSpawn = false;
		return;
	}

	if (g_NetMode == NETMODE_SERVER) {
		g_InitNum = g_Vars.maxprops;
		g_InitProps = calloc(g_InitNum, 1);
		g_InitPos = calloc(g_InitNum, sizeof(*g_InitPos));
		g_InitParent = calloc(g_InitNum, sizeof(*g_InitParent));
		if (!g_InitProps || !g_InitPos || !g_InitParent) {
			g_InitNum = 0;
			return;
		}
		for (s32 i = 0; i < g_InitNum; ++i) {
			if (g_Vars.props[i].syncid == (u32)(i + 1)) {
				g_InitProps[i] = 1;
				g_InitPos[i] = g_Vars.props[i].pos;
				if (g_Vars.props[i].parent) {
					g_InitParent[i] = g_Vars.props[i].parent->syncid;
				}
			}
		}
	} else if (g_Loading == LOAD_WAITING) {
		g_Loading = LOAD_READY;
		g_LoadedTick = g_NetTick;
	}
}

// keep an empty slot's player dead and out of sight
static void netjoinPark(s32 p)
{
	struct player *pl = g_Vars.players[p];
	if (!pl || !pl->prop || !pl->prop->chr) {
		return;
	}

	pl->prop->chr->chrflags |= CHRCFLAG_HIDDEN;

	if (!pl->isdead && g_Vars.tickmode == TICKMODE_NORMAL && g_Vars.lvframenum > 2) {
		const s32 prevplayernum = g_Vars.currentplayernum;
		setCurrentPlayerNum(p);
		g_NetQuietDeath = true;
		playerDieByShooter(p, true);
		g_NetQuietDeath = false;
		setCurrentPlayerNum(prevplayernum);
	}

	if (pl->isdead) {
		pl->redbloodfinished = true;
		pl->deathanimfinished = true;
	}
}

// A spare slot was loaded with a stand-in body. Its new owner's own body and head go in the
// slot's config; the character is rebuilt from it on the next tick.
static void netjoinSetBody(s32 p, u8 body, u8 head)
{
	struct mpchrconfig *cfg = &g_PlayerConfigsArray[p].base;
	if (body >= ARRAYCOUNT(g_MpBodies) || head >= ARRAYCOUNT(g_MpHeads)) {
		// (custom camera heads only exist on their owner's machine)
		return;
	}
	if (cfg->mpbodynum != body || cfg->mpheadnum != head) {
		cfg->mpbodynum = body;
		cfg->mpheadnum = head;
		g_Rebody[p] = 1;
	}
}

// the same thing counter-op does when a player takes over a guard: drop the model, build a new one
static void netjoinRebuildBody(s32 p)
{
	struct player *pl = g_Vars.players[p];
	if (!pl || !pl->prop || !pl->prop->chr || !pl->haschrbody || !pl->prop->chr->model) {
		return;
	}

	const s32 prevplayernum = g_Vars.currentplayernum;
	setCurrentPlayerNum(p);
	pl->haschrbody = false;
	pl->model00d4 = NULL;
	chrRemove(pl->prop, false);
	playerTickChrBody();
	setCurrentPlayerNum(prevplayernum);

	const struct chrdata *chr = pl->prop->chr;
	sysLogPrintf(LOG_NOTE, "NET: player slot %d now has body %d, head %d", p, chr->bodynum, chr->headnum);
}

// in the log of every machine, by client id, so they can be compared
static void netjoinLogBodies(void)
{
	static char prev[256];
	char line[256];
	s32 len = 0;
	line[0] = '\0';
	for (s32 i = 0; i < NET_MAX_CLIENTS && len < (s32)sizeof(line) - 24; ++i) {
		const struct netclient *cl = &g_NetClients[i];
		if (cl->state >= CLSTATE_GAME && cl->player && cl->player->prop && cl->player->prop->chr) {
			const struct chrdata *chr = cl->player->prop->chr;
			len += snprintf(line + len, sizeof(line) - len, " %d=%d/%d", i, chr->bodynum, chr->headnum);
		}
	}
	if (strcmp(line, prev)) {
		strcpy(prev, line);
		sysLogPrintf(LOG_NOTE, "NET: bodies (client=body/head):%s", line);
	}
}

void netjoinTick(void)
{
	if (!g_NetMode || !g_NetLocalClient || g_NetLocalClient->state < CLSTATE_GAME
			|| !(g_Vars.normmplayerisrunning || g_Vars.coopplayernum >= 0 || g_Vars.antiplayernum >= 0)) {
		return;
	}

	if (g_Vars.tickmode == TICKMODE_NORMAL && g_Vars.lvframenum > 2) {
		for (s32 p = 0; p < MAX_PLAYERS; ++p) {
			if (g_Rebody[p] && netjoinSlotInMatch(p)) {
				g_Rebody[p] = 0;
				netjoinRebuildBody(p);
			}
		}
		netjoinLogBodies();

		// Broken glass comes back 20 seconds after it is cleared away, and it is only cleared
		// away when it next ticks, which a pane nobody on this machine is near never does. So
		// machines disagreed about which panes were back. Clear it away at once everywhere.
		for (s32 i = 0; i < g_Vars.maxprops && g_Vars.normmplayerisrunning; ++i) {
			struct prop *prop = &g_Vars.props[i];
			if (prop->type == PROPTYPE_OBJ && prop->obj && !prop->parent && prop->timetoregen <= 0
					&& (prop->obj->type == OBJTYPE_GLASS || prop->obj->type == OBJTYPE_TINTEDGLASS)
					&& (prop->obj->hidden & OBJHFLAG_DELETING) && (prop->obj->hidden2 & OBJH2FLAG_CANREGEN)) {
				objFree(prop->obj, false, true);
				propExecuteTickOperation(prop, TICKOP_FREE);
			}
		}
	}

	for (s32 p = 0; p < MAX_PLAYERS; ++p) {
		if (g_Vacant[p] && netjoinSlotInMatch(p)) {
			netjoinPark(p);
		}
	}
}

/* SVC_SLOT */

static void netjoinSlotWrite(struct netbuf *dst, s32 svplayernum, const struct netclient *cl)
{
	netbufWriteU8(dst, SVC_SLOT);
	netbufWriteU8(dst, svplayernum);
	netbufWriteU8(dst, cl ? cl->id : NET_NULL_CLIENT);
	if (cl) {
		netbufWriteStr(dst, cl->settings.name);
		netbufWriteU8(dst, g_PlayerConfigsArray[svplayernum].base.mpbodynum);
		netbufWriteU8(dst, g_PlayerConfigsArray[svplayernum].base.mpheadnum);
	}
}

u32 netjoinSlotRead(struct netbuf *src, struct netclient *srccl)
{
	const u8 sv = netbufReadU8(src);
	const u8 clid = netbufReadU8(src);
	const char *name = (clid != NET_NULL_CLIENT) ? netbufReadStr(src) : NULL;
	const u8 body = (clid != NET_NULL_CLIENT) ? netbufReadU8(src) : 0;
	const u8 head = (clid != NET_NULL_CLIENT) ? netbufReadU8(src) : 0;

	if (src->error || srccl->state < CLSTATE_GAME || sv >= MAX_PLAYERS) {
		return src->error;
	}

	const s32 p = netjoinLocalNum(sv);
	if (!netjoinSlotInMatch(p) || !g_Vars.players[p]) {
		return src->error;
	}

	// whoever had it before is gone
	for (s32 i = 0; i < NET_MAX_CLIENTS; ++i) {
		struct netclient *cl = &g_NetClients[i];
		if (cl != g_NetLocalClient && cl->state && cl->player == g_Vars.players[p]) {
			cl->state = CLSTATE_DISCONNECTED;
			cl->player = NULL;
			cl->config = NULL;
		}
	}

	if (clid == NET_NULL_CLIENT) {
		if (p != 0) {
			netjoinSetupVacant(p);
			sysLogPrintf(LOG_NOTE, "NET: player slot %d is now empty", p);
		}
		return src->error;
	}

	if (clid >= NET_MAX_CLIENTS || !name || &g_NetClients[clid] == g_NetLocalClient) {
		// (our own slot: the stage start message already said so)
		return src->error;
	}

	struct netclient *cl = &g_NetClients[clid];
	struct mpplayerconfig *cfg = &g_PlayerConfigsArray[p];
	cl->id = clid;
	cl->state = CLSTATE_GAME;
	cl->playernum = p;
	cl->player = g_Vars.players[p];
	cl->config = cfg;
	strncpy(cl->settings.name, name, sizeof(cl->settings.name) - 1);
	cl->settings.team = cfg->base.team;
	memset(cl->inmove, 0, sizeof(cl->inmove));
	memset(cl->outmove, 0, sizeof(cl->outmove));
	cl->lerpticks = 0;
	cfg->client = cl;
	cfg->controlmode = CONTROLMODE_NA;
	snprintf(cfg->base.name, sizeof(cfg->base.name), "%s\n", cl->settings.name);
	cl->player->client = cl;
	cl->player->isremote = true;
	g_Vacant[p] = 0;
	g_Hold[p] = 0;
	netjoinSetBody(p, body, head);

	sysLogPrintf(LOG_NOTE, "NET: %s (client %u) takes player slot %d", cl->settings.name, clid, p);

	return src->error;
}

/* host */

void netjoinServerRequest(struct netclient *cl)
{
	if (g_NetMode != NETMODE_SERVER || !g_NetServerJoinInProgress) {
		return;
	}
	cl->joinpending = 1;
}

void netjoinServerOnLeave(struct netclient *cl)
{
	if (g_NetMode != NETMODE_SERVER || cl->state < CLSTATE_GAME || !cl->player || !g_NetLocalClient
			|| g_NetLocalClient->state < CLSTATE_GAME) {
		return;
	}

	const s32 p = cl->playernum;
	if (!netjoinSlotInMatch(p) || g_Vars.players[p] != cl->player) {
		return;
	}

	if (!g_NetServerJoinInProgress) {
		if (cl->config) {
			cl->config->client = NULL;
		}
		return;
	}

	netjoinSetupVacant(p);

	// (sent at once: this runs while packets are being read, and the frame's message
	// buffers are cleared after that)
	u8 data[16];
	struct netbuf buf = { NULL };
	buf.data = data;
	buf.size = sizeof(data);
	netbufStartWrite(&buf);
	netjoinSlotWrite(&buf, p, NULL);
	netSend(NULL, &buf, true, NETCHAN_DEFAULT);
	sysLogPrintf(LOG_NOTE, "NET: client %u left, player slot %d is now empty", cl->id, p);
}

void netjoinServerOnMove(struct netclient *cl)
{
	const s32 p = cl->playernum;
	if (g_NetMode != NETMODE_SERVER || p >= MAX_PLAYERS || !g_Hold[p] || cl->player != g_Vars.players[p]) {
		return;
	}

	// the new owner's game is up: let them in
	g_Hold[p] = 0;
	cl->player->dostartnewlife = true;
	sysLogPrintf(LOG_NOTE, "NET: %s finished loading, spawning in slot %d", cl->settings.name, p);
	netChatPrintf(NULL, "%s joined the match", cl->settings.name);
}

static u8 g_JoinBuf[16384];

// A prop that was there when the stage loaded: -1 if it still is, 0 if it is gone for good,
// 1 if it was picked up and is waiting to come back.
static s32 netjoinInitialPropGone(s32 i)
{
	const struct prop *prop = &g_Vars.props[i];
	if (prop->syncid != (u32)(i + 1)) {
		return 0;
	}
	if (!prop->parent && prop->obj && (prop->type == PROPTYPE_OBJ || prop->type == PROPTYPE_WEAPON)) {
		if (prop->obj->hidden & OBJHFLAG_GONE) {
			return (prop->obj->hidden2 & OBJH2FLAG_CANREGEN) ? 1 : 0;
		}
		if (prop->obj->hidden & OBJHFLAG_DELETING) {
			// broken glass and the like: it only leaves the prop list when it next ticks,
			// and things out of the host's sight don't tick
			return 0;
		}
	}
	return -1;
}

static void netjoinFlush(struct netclient *cl, struct netbuf *buf, u32 *total)
{
	if (buf->wp) {
		*total += buf->wp;
		netSend(cl, buf, true, NETCHAN_DEFAULT);
	}
}

// the state of the world for someone who has just loaded the stage as it was at the start
static void netjoinWriteSnapshot(struct netclient *joiner, struct netbuf *buf, u32 *total)
{
	s32 numremoved = 0, numregen = 0, numspawned = 0, nummoved = 0, numdoors = 0, numdropped = 0, numguards = 0;

	// everyone else's health, weapon and whether they're alive
	for (s32 i = 0; i < g_NetMaxClients; ++i) {
		struct netclient *cl = &g_NetClients[i];
		if (cl != joiner && cl->state >= CLSTATE_GAME && cl->player) {
			netmsgSvcPlayerStatsWrite(buf, cl);
		}
	}

	// things a guard held at the start and has let go of (before the lists below say where
	// they are now, or that they are gone)
	for (s32 i = 0; i < g_InitNum && i < g_Vars.maxprops; ++i) {
		const struct prop *prop = &g_Vars.props[i];
		if (g_InitProps[i] && g_InitParent[i] && (prop->syncid != (u32)(i + 1) || !prop->parent)) {
			netbufWriteU8(buf, SVC_CHR_DROP);
			netbufWriteU32(buf, g_InitParent[i]);
			netbufWriteU32(buf, i + 1);
			netbufWriteU8(buf, DROPTYPE_DEFAULT);
			++numdropped;
			if (buf->wp > sizeof(g_JoinBuf) - 2048) {
				netjoinFlush(joiner, buf, total);
			}
		}
	}

	// things that were there at the start and aren't now
	for (s32 i = 0; i < g_InitNum && i < g_Vars.maxprops; ++i) {
		if (!g_InitProps[i]) {
			continue;
		}
		const s32 regen = netjoinInitialPropGone(i);
		if (regen >= 0) {
			// (with how long until it comes back, so it does at the same moment as on the host)
			const s16 timeleft = (regen == 1) ? g_Vars.props[i].timetoregen : 0;
			netbufWriteU8(buf, SVC_PROP_REMOVE);
			netbufWriteU32(buf, i + 1);
			netbufWriteU8(buf, regen | (timeleft > 0 ? 2 : 0));
			if (timeleft > 0) {
				netbufWriteS16(buf, timeleft);
			}
			if (g_Vars.props[i].syncid != (u32)(i + 1)) {
				++numremoved;
			} else {
				++numregen;
			}
		}
		if (buf->wp > sizeof(g_JoinBuf) - 2048) {
			netjoinFlush(joiner, buf, total);
		}
	}

	// things in the world now
	for (s32 i = 0; i < g_Vars.maxprops; ++i) {
		struct prop *prop = &g_Vars.props[i];
		if (!prop->syncid || prop->parent || !prop->obj) {
			continue;
		}

		const bool initial = (prop->syncid == (u32)(i + 1)) && i < g_InitNum && g_InitProps[i];

		if (prop->type == PROPTYPE_CHR) {
			// guards that a mission script has spawned since
			if (!initial && prop->chr && !prop->chr->aibot && !(prop->chr->hidden & CHRHFLAG_DELETING)) {
				netmsgSvcChrSpawnWrite(buf, prop);
				++numguards;
			}
			continue;
		}

		if (prop->type == PROPTYPE_DOOR) {
			const struct doorobj *door = prop->door;
			if (initial && (door->mode == DOORMODE_OPENING || door->mode == DOORMODE_WAITING
					|| (door->mode == DOORMODE_IDLE && door->frac > 0.f))) {
				// open here: open it there too, it closes again by itself like any door
				netbufWriteU8(buf, SVC_PROP_DOOR);
				netbufWriteU32(buf, prop->syncid);
				netbufWriteU8(buf, NET_NULL_CLIENT);
				netbufWriteS8(buf, DOORMODE_OPENING);
				netbufWriteU32(buf, door->base.flags);
				netbufWriteU32(buf, door->base.hidden);
				++numdoors;
			}
		} else if (prop->type == PROPTYPE_OBJ || prop->type == PROPTYPE_WEAPON) {
			if (prop->obj->hidden & (OBJHFLAG_GONE | OBJHFLAG_DELETING)) {
				continue;
			}
			if (!initial) {
				// dropped guns, thrown things, mines
				if (prop->type == PROPTYPE_WEAPON && prop->weapon && !(prop->obj->flags & OBJFLAG_HELDROCKET)) {
					netmsgSvcPropSpawnWrite(buf, prop);
					++numspawned;
				}
			} else if (prop->obj->type == OBJTYPE_LIFT) {
				netmsgSvcPropLiftWrite(buf, prop);
			} else {
				const f32 dx = prop->pos.x - g_InitPos[i].x;
				const f32 dy = prop->pos.y - g_InitPos[i].y;
				const f32 dz = prop->pos.z - g_InitPos[i].z;
				if (dx * dx + dy * dy + dz * dz > 4.f || (prop->obj->hidden & OBJHFLAG_PROJECTILE)) {
					netmsgSvcPropMoveWrite(buf, prop, NULL);
					++nummoved;
				}
			}
		}

		if (buf->wp > sizeof(g_JoinBuf) - 2048) {
			netjoinFlush(joiner, buf, total);
		}
	}

	if (g_Vars.normmplayerisrunning) {
		// the hill, the victim, who is carrying the briefcase (after the props it refers to)
		netscenWrite(buf);
		netmsgSvcMpScoresWrite(buf);
	} else {
		// which guard each counter-operative is at the moment
		netcoopAntiWriteAll(buf);
		// objectives, alarm and timer, and what scripts have changed about objects: all of
		// it goes out again with the next updates
		netcoopOnJoin();
	}
	netjoinFlush(joiner, buf, total);

	sysLogPrintf(LOG_NOTE, "NET: snapshot for client %u: %d removed, %d waiting to regen, %d spawned, %d moved, %d doors open, %d let go, %d new guards, %u bytes",
		joiner->id, numremoved, numregen, numspawned, nummoved, numdoors, numdropped, numguards, *total);
}

static s32 netjoinFindSlot(void)
{
	for (s32 p = 1; p < MAX_PLAYERS; ++p) {
		if (g_Vacant[p] && !g_Hold[p] && netjoinSlotInMatch(p) && g_Vars.players[p] && g_Vars.players[p]->prop) {
			return p;
		}
	}
	return -1;
}

static void netjoinServerJoin(struct netclient *cl, s32 p)
{
	struct mpplayerconfig *cfg = &g_PlayerConfigsArray[p];
	struct player *pl = g_Vars.players[p];

	g_Vacant[p] = 0;
	g_Hold[p] = 1;

	cl->playernum = p;
	cl->player = pl;
	cl->config = cfg;
	cl->settings.team = cfg->base.team;
	memset(cl->inmove, 0, sizeof(cl->inmove));
	memset(cl->outmove, 0, sizeof(cl->outmove));
	cl->lerpticks = 0;
	cl->outmoveack = 0;
	cl->inmovetick = 0;
	cl->forcetick = 0;
	cl->laginit = 0;
	cl->joinpending = 0;
	cl->state = CLSTATE_GAME;

	cfg->client = cl;
	cfg->controlmode = CONTROLMODE_NA;
	snprintf(cfg->base.name, sizeof(cfg->base.name), "%s\n", cl->settings.name);
	// a fresh score for the slot
	if (g_Vars.normmplayerisrunning) {
		func0f187838(&cfg->base);
	}
	// and the joiner's own character (before the stage start below, which tells the joiner)
	netjoinSetBody(p, cl->settings.bodynum, cl->settings.headnum);

	pl->client = cl;
	pl->isremote = true;

	sysLogPrintf(LOG_NOTE, "NET: %s (client %u) joins the running match in player slot %d", cl->settings.name, cl->id, p);

	struct netbuf buf = { NULL };
	buf.data = g_JoinBuf;
	buf.size = sizeof(g_JoinBuf);
	netbufStartWrite(&buf);

	// the stage start goes in a packet of its own: the joiner keeps everything after it
	// until its stage is up
	u32 total = 0;
	netmsgSvcStageStartWriteJoin(&buf);
	netjoinFlush(cl, &buf, &total);

	netjoinWriteSnapshot(cl, &buf, &total);

	// tell everyone (the joiner ignores its own)
	netjoinSlotWrite(&g_NetMsgRel, p, cl);
}

static void netjoinServerResync(void);

void netjoinServerTick(void)
{
	if (g_NetMode != NETMODE_SERVER || !g_NetLocalClient || g_NetLocalClient->state < CLSTATE_GAME) {
		return;
	}

	netjoinServerResync();

	if (!(g_Vars.normmplayerisrunning || g_Vars.coopplayernum >= 0 || g_Vars.antiplayernum >= 0)
			|| g_Vars.tickmode != TICKMODE_NORMAL || g_Vars.lvframenum < 60 || g_NetTick < g_NextJoinTick) {
		return;
	}

	for (s32 i = 1; i < g_NetMaxClients; ++i) {
		struct netclient *cl = &g_NetClients[i];
		if (cl->state == CLSTATE_LOBBY && cl->joinpending && cl->peer) {
			const s32 p = netjoinFindSlot();
			if (p < 0) {
				break;
			}
			netjoinServerJoin(cl, p);
			// one at a time
			g_NextJoinTick = g_NetTick + 30;
			break;
		}
	}
}

/* client: packets that arrive while the stage loads */

s32 netjoinClientLoading(void)
{
	return g_Loading != LOAD_NONE;
}

s32 netjoinClientKeepPacket(const u8 *data, u32 len, s32 reliable)
{
	if (g_NetMode != NETMODE_CLIENT || g_Loading == LOAD_NONE) {
		return false;
	}

	if (!reliable || !len) {
		return true; // positions and such: newer ones will follow
	}

	struct keptpacket *kp = malloc(sizeof(*kp) + len);
	if (kp) {
		kp->next = NULL;
		kp->len = len;
		memcpy(kp->data, data, len);
		if (g_KeptTail) {
			g_KeptTail->next = kp;
		} else {
			g_KeptHead = kp;
		}
		g_KeptTail = kp;
	}

	return true;
}

u8 *netjoinClientNextPacket(u32 *len)
{
	if (g_NetMode != NETMODE_CLIENT || g_Loading != LOAD_READY) {
		return NULL;
	}

	// give the stage a few frames so every player has spawned
	if (g_NetTick < g_LoadedTick + 5 || g_Vars.tickmode != TICKMODE_NORMAL || !g_NetLocalClient->player
			|| !g_NetLocalClient->player->prop) {
		return NULL;
	}

	if (g_JoinElapsed60 >= 0) {
		// the match clock, counting the time the load took
		const s32 loadticks = (s32)((sysGetMicroseconds() - g_JoinStartUs) * 60 / 1000000);
		g_StageTimeElapsed60 = g_JoinElapsed60 + loadticks;
		g_StageTimeElapsed1f = g_StageTimeElapsed60;
		g_JoinElapsed60 = -1;
		sysLogPrintf(LOG_NOTE, "NET: stage is up after %d ticks, applying the snapshot", loadticks);
	}

	struct keptpacket *kp = g_KeptHead;
	if (!kp) {
		g_KeptTail = NULL;
		g_Loading = LOAD_NONE;
		sysLogPrintf(LOG_NOTE, "NET: joined the match");
		return NULL;
	}

	g_KeptHead = kp->next;
	u8 *data = malloc(kp->len ? kp->len : 1);
	if (data) {
		memcpy(data, kp->data, kp->len);
		*len = kp->len;
	}
	free(kp);
	return data;
}

/* self-healing: SVC_RESYNC and CLC_PROPREQ */

#define RESYNC_MAX_DYN   512
#define RESYNC_MAX_DOORS 64
#define RESYNC_MAX_REQ   8

s32 g_NetServerResyncSecs = 10;

static u32 g_NextResyncTick = 0;
static s32 g_DesyncTest = -1;
static u32 g_DesyncTestCount = 0;

// doors that differed in the last resync: only acted on if they still do in the next
static u32 g_DoorMismatch[RESYNC_MAX_DOORS];
static s32 g_NumDoorMismatch = 0;

s32 netjoinTestLoseMessage(void)
{
	if (g_DesyncTest < 0) {
		g_DesyncTest = sysArgCheck("--net-desynctest");
	}
	return g_DesyncTest && g_NetMode == NETMODE_SERVER && (g_DesyncTestCount++ & 1);
}

#define RESYNC_IN_FLIGHT 0x80000000u

// a gun, mine or thrown thing in the world that the host made after the stage loaded
static inline bool netjoinIsLooseGunOrFlying(const struct prop *prop)
{
	return prop->syncid > (u32)g_Vars.maxprops && !prop->parent && prop->obj && prop->type == PROPTYPE_WEAPON
		&& prop->weapon && !(prop->obj->hidden & (OBJHFLAG_GONE | OBJHFLAG_DELETING))
		// (the rocket sitting in a launcher is a prop of its own with no parent: not loose)
		&& !(prop->obj->flags & OBJFLAG_HELDROCKET);
}

// ...and lying still. Each machine flies its own copy of a rocket or a thrown thing, and
// they do not land or go off on the same frame, so only things at rest are compared.
static inline bool netjoinIsLooseGun(const struct prop *prop)
{
	return netjoinIsLooseGunOrFlying(prop) && !(prop->obj->hidden & OBJHFLAG_PROJECTILE);
}

static void netjoinResyncReset(void)
{
	g_NextResyncTick = 0;
	g_NumDoorMismatch = 0;
}

// 0 closed, 1 open, 2 moving
static inline u8 netjoinDoorState(const struct doorobj *door)
{
	if (door->mode == DOORMODE_CLOSING) {
		return 2;
	}
	if (door->mode == DOORMODE_OPENING || door->mode == DOORMODE_WAITING || door->frac > 0.f) {
		return (door->mode == DOORMODE_OPENING && door->frac < door->maxfrac) ? 2 : 1;
	}
	return 0;
}

static void netjoinServerResync(void)
{
	if (g_NetServerResyncSecs <= 0 || g_NetTick < g_NextResyncTick || g_NetNumClients < 2
			|| g_Vars.tickmode != TICKMODE_NORMAL || g_Vars.lvframenum < 120 || !g_InitNum) {
		return;
	}

	g_NextResyncTick = g_NetTick + g_NetServerResyncSecs * 60;

	struct netbuf buf = { NULL };
	buf.data = g_JoinBuf;
	buf.size = sizeof(g_JoinBuf);
	netbufStartWrite(&buf);

	netbufWriteU8(&buf, SVC_RESYNC);

	// things from the start of the level that are gone, or waiting to come back
	const u32 numpos = buf.wp;
	u16 num = 0;
	netbufWriteU16(&buf, 0);
	for (s32 i = 0; i < g_InitNum && i < g_Vars.maxprops && buf.wp < sizeof(g_JoinBuf) - 4096; ++i) {
		if (!g_InitProps[i]) {
			continue;
		}
		const s32 regen = netjoinInitialPropGone(i);
		if (regen >= 0) {
			netbufWriteU16(&buf, i + 1);
			netbufWriteU8(&buf, regen);
			++num;
		}
	}
	const u32 afterremoved = buf.wp;
	buf.wp = numpos;
	netbufWriteU16(&buf, num);
	buf.wp = afterremoved;

	// loose guns and other thrown or dropped weapons
	const u32 dynpos = buf.wp;
	u16 numdyn = 0;
	netbufWriteU16(&buf, 0);
	for (s32 i = 0; i < g_Vars.maxprops && numdyn < RESYNC_MAX_DYN; ++i) {
		const struct prop *prop = &g_Vars.props[i];
		if (netjoinIsLooseGunOrFlying(prop)) {
			netbufWriteU32(&buf, prop->syncid | ((prop->obj->hidden & OBJHFLAG_PROJECTILE) ? RESYNC_IN_FLIGHT : 0));
			++numdyn;
		}
	}
	const u32 afterdyn = buf.wp;
	buf.wp = dynpos;
	netbufWriteU16(&buf, numdyn);
	buf.wp = afterdyn;

	// doors
	const u32 doorpos = buf.wp;
	u16 numdoors = 0;
	netbufWriteU16(&buf, 0);
	for (s32 i = 0; i < g_Vars.maxprops && buf.wp < sizeof(g_JoinBuf) - 16; ++i) {
		const struct prop *prop = &g_Vars.props[i];
		if (prop->syncid && prop->type == PROPTYPE_DOOR && prop->door) {
			netbufWriteU16(&buf, prop->syncid);
			netbufWriteU8(&buf, netjoinDoorState(prop->door));
			++numdoors;
		}
	}
	const u32 end = buf.wp;
	buf.wp = doorpos;
	netbufWriteU16(&buf, numdoors);
	buf.wp = end;

	netSend(NULL, &buf, true, NETCHAN_DEFAULT);
}

u32 netjoinResyncRead(struct netbuf *src, struct netclient *srccl)
{
	static u32 dyn[RESYNC_MAX_DYN];
	const bool apply = (srccl->state >= CLSTATE_GAME) && g_Vars.tickmode == TICKMODE_NORMAL;
	s32 fixedgone = 0, fixedstale = 0, fixeddoors = 0, nummissing = 0;
	u32 missing[RESYNC_MAX_REQ];

	const u16 numgone = netbufReadU16(src);
	for (s32 i = 0; i < numgone && !src->error; ++i) {
		const u16 syncid = netbufReadU16(src);
		const u8 regen = netbufReadU8(src);
		if (apply && !src->error && netmsgPropRemoveApply(syncid, regen, 0)) {
			++fixedgone;
		}
	}

	const u16 numdyn = netbufReadU16(src);
	for (s32 i = 0; i < numdyn && !src->error; ++i) {
		const u32 syncid = netbufReadU32(src);
		if (i < RESYNC_MAX_DYN) {
			dyn[i] = syncid;
		}
	}

	if (apply && !src->error && numdyn <= RESYNC_MAX_DYN) {
		// loose guns here that the host doesn't have any more
		for (s32 i = 0; i < g_Vars.maxprops; ++i) {
			struct prop *prop = &g_Vars.props[i];
			if (!netjoinIsLooseGun(prop)) {
				continue;
			}
			bool found = false;
			for (s32 j = 0; j < numdyn && !found; ++j) {
				found = ((dyn[j] & ~RESYNC_IN_FLIGHT) == prop->syncid);
			}
			if (!found) {
				netmsgPropRemoveApply(prop->syncid, 0, 0);
				++fixedstale;
			}
		}
		// and ones the host has that aren't here in any form
		for (s32 j = 0; j < numdyn && nummissing < RESYNC_MAX_REQ; ++j) {
			// (not the ones still flying on the host: ours may have gone off already)
			if (!(dyn[j] & RESYNC_IN_FLIGHT) && !netPropBySyncId(dyn[j])) {
				missing[nummissing++] = dyn[j];
			}
		}
	}

	u32 newmismatch[RESYNC_MAX_DOORS];
	s32 numnew = 0;
	const u16 numdoors = netbufReadU16(src);
	for (s32 i = 0; i < numdoors && !src->error; ++i) {
		const u16 syncid = netbufReadU16(src);
		const u8 hoststate = netbufReadU8(src);
		if (!apply || src->error || hoststate > 1) {
			continue;
		}
		struct prop *prop = netPropBySyncId(syncid);
		if (!prop || prop->type != PROPTYPE_DOOR || !prop->door) {
			continue;
		}
		const u8 mine = netjoinDoorState(prop->door);
		if (mine > 1 || mine == hoststate) {
			continue;
		}
		// act only if it was already different last time (doors run on their own timers
		// on every machine, a moment's difference is normal)
		bool before = false;
		for (s32 j = 0; j < g_NumDoorMismatch && !before; ++j) {
			before = (g_DoorMismatch[j] == syncid);
		}
		if (before) {
			doorSetMode(prop->door, hoststate ? DOORMODE_OPENING : DOORMODE_CLOSING);
			++fixeddoors;
		} else if (numnew < RESYNC_MAX_DOORS) {
			newmismatch[numnew++] = syncid;
		}
	}

	if (src->error) {
		return src->error;
	}

	memcpy(g_DoorMismatch, newmismatch, sizeof(u32) * numnew);
	g_NumDoorMismatch = numnew;

	if (nummissing) {
		u8 data[8 + RESYNC_MAX_REQ * 4];
		struct netbuf buf = { NULL };
		buf.data = data;
		buf.size = sizeof(data);
		netbufStartWrite(&buf);
		netbufWriteU8(&buf, CLC_PROPREQ);
		netbufWriteU8(&buf, nummissing);
		for (s32 i = 0; i < nummissing; ++i) {
			netbufWriteU32(&buf, missing[i]);
		}
		netSend(g_NetLocalClient, &buf, true, NETCHAN_DEFAULT);
	}

	if (fixedgone || fixedstale || fixeddoors || nummissing) {
		sysLogPrintf(LOG_NOTE, "NET: resync repaired: %d pickup(s) that were gone, %d stale gun(s) removed, %d missing gun(s) asked for, %d door(s)",
			fixedgone, fixedstale, nummissing, fixeddoors);
	}

	// --net-desynctest: lose a loose gun now and then
	if (g_DesyncTest < 0) {
		g_DesyncTest = sysArgCheck("--net-desynctest");
	}
	if (g_DesyncTest && apply && !nummissing) {
		for (s32 i = 0; i < g_Vars.maxprops; ++i) {
			if (netjoinIsLooseGun(&g_Vars.props[i]) && !(g_Vars.props[i].obj->hidden & OBJHFLAG_PROJECTILE)) {
				sysLogPrintf(LOG_NOTE, "NET: desynctest: deleting loose gun %u here", g_Vars.props[i].syncid);
				netmsgPropRemoveApply(g_Vars.props[i].syncid, 0, 0);
				break;
			}
		}
	}

	return src->error;
}

u32 netjoinPropReqRead(struct netbuf *src, struct netclient *srccl)
{
	const u8 num = netbufReadU8(src);
	u32 ids[RESYNC_MAX_REQ];

	if (src->error || num > RESYNC_MAX_REQ) {
		return 1;
	}
	for (s32 i = 0; i < num; ++i) {
		ids[i] = netbufReadU32(src);
	}
	if (src->error || srccl->state < CLSTATE_GAME) {
		return src->error;
	}

	struct netbuf buf = { NULL };
	buf.data = g_JoinBuf;
	buf.size = sizeof(g_JoinBuf);
	netbufStartWrite(&buf);

	s32 sent = 0;
	for (s32 i = 0; i < num; ++i) {
		struct prop *prop = netPropBySyncId(ids[i]);
		if (prop && netjoinIsLooseGun(prop)) {
			netmsgSvcPropSpawnWrite(&buf, prop);
			++sent;
		}
	}

	if (buf.wp) {
		netSend(srccl, &buf, true, NETCHAN_DEFAULT);
	}

	sysLogPrintf(LOG_NOTE, "NET: client %u asked for %u missing gun(s), sent %d", srccl->id, num, sent);

	return src->error;
}
