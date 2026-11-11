// Automation and debugging helpers for net games.
//
// Command line flags:
//   --net-name NAME          local player name
//   --net-autostart N        (host) start the match once N players (including the host) are in the lobby
//   --net-stage NUM          (host) stage for the auto-started match, e.g. 0x1f (Complex)
//   --net-sims N             (host) number of simulants in the auto-started match
//   --net-simdiff N          (host) simulant profile: 0 meat, 1 easy, 2 normal, 3 hard, 4 perfect, 5 dark
//   --net-scenario N         (host) MPSCENARIO_*
//   --net-coop STAGE         (host) start a co-op mission on STAGE (e.g. 0x30 Defection) instead of Combat Sim
//   --net-difficulty N       (host) co-op difficulty: 0 agent, 1 special agent, 2 perfect agent
//   --net-autoquit SECS      quit the game SECS seconds after the match starts
//   --net-autoabort SECS     (host) abort the mission SECS seconds after it starts, to test the end screens
//   --net-autokill SECS      (host) every SECS seconds, shoot the chr nearest to one of the players
//   --net-autopilot [SEED]   drive the local player with scripted input
//   --net-dump FILE          write chr state to FILE every --net-dump-interval ticks (default 10)
//   --net-lag MS             delay every outgoing packet by MS milliseconds
//   --net-jitter MS          add up to MS milliseconds of random extra delay
//   --net-loss PCT           drop PCT percent of outgoing unreliable packets

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "platform.h"
#include "net/netenet.h"
#include "net/net.h"
#include "net/netauto.h"
#include "types.h"
#include "constants.h"
#include "data.h"
#include "bss.h"
#include "game/chraction.h"
#include "game/menu.h"
#include "game/mplayer/mplayer.h"
#include "lib/model.h"
#include "lib/main.h"
#include "system.h"

s32 g_NetAutopilot = 0;

static s32 g_NetAutoStartPlayers = 0;
static s32 g_NetAutoStage = -1;
static s32 g_NetAutoSims = -1;
static s32 g_NetAutoSimDiff = BOTDIFF_NORMAL;
static s32 g_NetAutoScenario = -1;
static s32 g_NetAutoCoopStage = -1;
static s32 g_NetAutoDifficulty = 0;
static s32 g_NetAutoQuitSecs = 0;
static s32 g_NetAutoAbortSecs = 0;
static s32 g_NetAutoAborted = 0;
static s32 g_NetAutoKillSecs = 0;
static const char *g_NetAutoName = NULL;

static s32 g_NetAutoStarted = 0;
static u32 g_NetAutoReadyTick = 0;
static u32 g_NetAutoGameTicks = 0;

static FILE *g_NetDumpFile = NULL;
static s32 g_NetDumpInterval = 10;

static u32 g_NetSimLag = 0;
static u32 g_NetSimJitter = 0;
static u32 g_NetSimLoss = 0;

static u32 g_NetAutopilotSeed = 1;

/* init */

void netAutoInit(void)
{
	g_NetAutoName = sysArgGetString("--net-name");
	g_NetAutoStartPlayers = sysArgGetInt("--net-autostart", 0);
	g_NetAutoStage = sysArgGetInt("--net-stage", -1);
	g_NetAutoSims = sysArgGetInt("--net-sims", -1);
	g_NetAutoSimDiff = sysArgGetInt("--net-simdiff", BOTDIFF_NORMAL);
	g_NetAutoScenario = sysArgGetInt("--net-scenario", -1);
	g_NetAutoCoopStage = sysArgGetInt("--net-coop", -1);
	g_NetAutoDifficulty = sysArgGetInt("--net-difficulty", 0);
	g_NetAutoQuitSecs = sysArgGetInt("--net-autoquit", 0);
	g_NetAutoAbortSecs = sysArgGetInt("--net-autoabort", 0);
	g_NetAutoKillSecs = sysArgGetInt("--net-autokill", 0);

	if (g_NetAutoSims > MAX_BOTS) {
		g_NetAutoSims = MAX_BOTS;
	}

	if (sysArgCheck("--net-autopilot")) {
		g_NetAutopilot = 1;
		const s32 seed = sysArgGetInt("--net-autopilot", 0);
		g_NetAutopilotSeed = seed > 0 ? seed : 1;
	}

	const s32 lag = sysArgGetInt("--net-lag", 0);
	const s32 jitter = sysArgGetInt("--net-jitter", 0);
	const s32 loss = sysArgGetInt("--net-loss", 0);
	g_NetSimLag = lag > 0 ? lag : 0;
	g_NetSimJitter = jitter > 0 ? jitter : 0;
	g_NetSimLoss = (loss > 0) ? (loss > 100 ? 100 : loss) : 0;
	if (g_NetSimLag || g_NetSimJitter || g_NetSimLoss) {
		sysLogPrintf(LOG_NOTE, "NET: simulating %ums lag, %ums jitter, %u%% loss", g_NetSimLag, g_NetSimJitter, g_NetSimLoss);
	}

	const char *dumppath = sysArgGetString("--net-dump");
	if (dumppath && dumppath[0]) {
		g_NetDumpFile = fopen(dumppath, "wb");
		if (g_NetDumpFile) {
			fprintf(g_NetDumpFile, "tick,syncid,type,x,y,z,theta,act,anim,frame,dead,damage,weapon,room\n");
			sysLogPrintf(LOG_NOTE, "NET: dumping chr state to %s", dumppath);
		} else {
			sysLogPrintf(LOG_WARNING, "NET: could not open dump file %s", dumppath);
		}
		const s32 interval = sysArgGetInt("--net-dump-interval", 10);
		g_NetDumpInterval = interval > 0 ? interval : 1;
	}
}

void netAutoApplyName(void)
{
	if (g_NetAutoName && g_NetAutoName[0]) {
		snprintf(g_PlayerConfigsArray[0].base.name, sizeof(g_PlayerConfigsArray[0].base.name), "%s\n", g_NetAutoName);
	}
}

/* autostart */

extern MenuItemHandlerResult menuhandlerAcceptMission(s32 operation, struct menuitem *item, union handlerdata *data);

static void netAutoStartCoop(void)
{
	g_MissionConfig.iscoop = true;
	g_MissionConfig.isanti = false;
	g_MissionConfig.pdmode = false;
	g_MissionConfig.stagenum = g_NetAutoCoopStage;
	g_MissionConfig.difficulty = (g_NetAutoDifficulty >= DIFF_A && g_NetAutoDifficulty <= DIFF_PA) ? g_NetAutoDifficulty : DIFF_A;
	g_MissionConfig.stageindex = 0;
	for (s32 i = 0; i < NUM_SOLOSTAGES; ++i) {
		if (g_SoloStages[i].stagenum == (u32)g_NetAutoCoopStage) {
			g_MissionConfig.stageindex = i;
			break;
		}
	}

	sysLogPrintf(LOG_NOTE, "NET: autostart: co-op on stage 0x%02x, difficulty %d", g_MissionConfig.stagenum, g_MissionConfig.difficulty);

	menuhandlerAcceptMission(MENUOP_SET, NULL, NULL);
}

static void netAutoStartMatch(void)
{
	if (g_NetAutoCoopStage > 0) {
		netAutoStartCoop();
		return;
	}

	if (g_NetAutoStage > 0) {
		g_MpSetup.stagenum = g_NetAutoStage;
	}

	if (g_NetAutoScenario >= 0) {
		g_MpSetup.scenario = g_NetAutoScenario;
	}

	if (g_NetAutoSims >= 0) {
		g_MpSetup.chrslots &= MP_PLAYERSLOTS_MASK;
		const s32 profile = (g_NetAutoSimDiff >= 0 && g_NetAutoSimDiff < NUM_BOTDIFFS) ? g_NetAutoSimDiff : BOTDIFF_NORMAL;
		for (s32 i = 0; i < g_NetAutoSims; ++i) {
			mpCreateBotFromProfile(i, profile);
		}
	}

	sysLogPrintf(LOG_NOTE, "NET: autostart: stage 0x%02x, scenario %u, chrslots 0x%04x",
		g_MpSetup.stagenum, g_MpSetup.scenario, g_MpSetup.chrslots);

	mpStartMatch();
	menuStop();
}

static void netAutoTickStart(void)
{
	if (g_NetMode != NETMODE_SERVER || g_NetAutoStartPlayers <= 0 || g_NetAutoStarted) {
		return;
	}

	if (g_NetLocalClient->state != CLSTATE_LOBBY || g_StageNum != STAGE_CITRAINING) {
		return;
	}

	s32 numready = 0;
	for (s32 i = 0; i < g_NetMaxClients; ++i) {
		if (g_NetClients[i].state >= CLSTATE_LOBBY) {
			++numready;
		}
	}

	if (numready < g_NetAutoStartPlayers) {
		g_NetAutoReadyTick = 0;
		return;
	}

	// give the clients a couple seconds to settle in
	if (!g_NetAutoReadyTick) {
		g_NetAutoReadyTick = g_NetTick + 120;
	} else if (g_NetTick >= g_NetAutoReadyTick) {
		g_NetAutoStarted = 1;
		netAutoStartMatch();
	}
}

/* autoquit */

static void netAutoTickQuit(void)
{
	if (g_NetLocalClient && g_NetLocalClient->state == CLSTATE_GAME) {
		++g_NetAutoGameTicks;
	}

	if (g_NetMode == NETMODE_SERVER && g_NetAutoAbortSecs > 0 && !g_NetAutoAborted
			&& g_NetAutoGameTicks >= (u32)g_NetAutoAbortSecs * 60 && g_Vars.players[0]) {
		g_NetAutoAborted = 1;
		sysLogPrintf(LOG_NOTE, "NET: autoabort after %d seconds in game", g_NetAutoAbortSecs);
		g_Vars.players[0]->aborted = true; // the host's local player is always 0
		mainEndStage();
	}

	if (g_NetAutoQuitSecs <= 0) {
		return;
	}

	if (g_NetAutoGameTicks >= (u32)g_NetAutoQuitSecs * 60) {
		sysLogPrintf(LOG_NOTE, "NET: autoquit after %d seconds in game; sent %u bytes (%u/s), received %u bytes (%u/s)",
			g_NetAutoQuitSecs, netStatBytesSent(), netStatBytesSent() / g_NetAutoQuitSecs,
			netStatBytesReceived(), netStatBytesReceived() / g_NetAutoQuitSecs);
		if (g_NetDumpFile) {
			fclose(g_NetDumpFile);
			g_NetDumpFile = NULL;
		}
		netDisconnect();
		exit(0);
	}
}

/* state dump */

static void netAutoTickDump(void)
{
	if (!g_NetDumpFile || !g_NetLocalClient || g_NetLocalClient->state != CLSTATE_GAME) {
		return;
	}

	if (g_NetTick % g_NetDumpInterval) {
		return;
	}

	for (s32 i = 0; i < g_NumChrSlots; ++i) {
		struct chrdata *chr = &g_ChrSlots[i];
		struct prop *prop = chr->prop;
		if (chr->chrnum < 0 || !prop || !prop->syncid) {
			continue;
		}

		s32 weaponnum = -1;
		if (chr->weapons_held[HAND_RIGHT] && chr->weapons_held[HAND_RIGHT]->weapon) {
			weaponnum = chr->weapons_held[HAND_RIGHT]->weapon->weaponnum;
		}

		const s32 anim = chr->model ? modelGetAnimNum(chr->model) : -1;
		const f32 frame = chr->model ? modelGetCurAnimFrame(chr->model) : 0.f;

		fprintf(g_NetDumpFile, "%u,%u,%u,%.2f,%.2f,%.2f,%.4f,%d,%d,%.2f,%d,%.3f,%d,%d\n",
			g_NetTick, prop->syncid, prop->type, prop->pos.x, prop->pos.y, prop->pos.z,
			chrGetInverseTheta(chr), chr->actiontype, anim, frame, chrIsDead(chr) ? 1 : 0,
			chr->damage, weaponnum, prop->rooms[0]);
	}

	// guns lying in the world (dropped by chrs or placed by the stage), to catch
	// drops that only happen on one side; activeprops runs on into pausedprops
	for (struct prop *prop = g_Vars.activeprops; prop; prop = prop->next) {
		if (prop->type != PROPTYPE_WEAPON || !prop->syncid || !prop->weapon) {
			continue;
		}
		fprintf(g_NetDumpFile, "%u,%u,%u,%.2f,%.2f,%.2f,0,-1,-1,0,0,0,%d,%d\n",
			g_NetTick, prop->syncid, prop->type, prop->pos.x, prop->pos.y, prop->pos.z,
			prop->weapon->weaponnum, prop->rooms[0]);
	}

	fflush(g_NetDumpFile);
}

/* autokill: the host hits the guard nearest to a player every few seconds */

static void netAutoTickKill(void)
{
	static u32 nextkill = 0;
	static s32 victimplayer = 0;

	if (g_NetMode != NETMODE_SERVER || g_NetAutoKillSecs <= 0 || !g_NetLocalClient || g_NetLocalClient->state != CLSTATE_GAME) {
		return;
	}

	if (!nextkill) {
		nextkill = g_NetAutoGameTicks + 5 * 60;
	}
	if (g_NetAutoGameTicks < nextkill) {
		return;
	}
	nextkill = g_NetAutoGameTicks + (u32)g_NetAutoKillSecs * 60;

	// alternate between the players so drops land near each of them
	const s32 numplayers = PLAYERCOUNT();
	victimplayer = (victimplayer + 1) % (numplayers > 0 ? numplayers : 1);
	struct player *player = g_Vars.players[victimplayer];
	if (!player || !player->prop) {
		return;
	}

	struct chrdata *best = NULL;
	f32 bestdist = 0.f;
	for (s32 i = 0; i < g_NumChrSlots; ++i) {
		struct chrdata *chr = &g_ChrSlots[i];
		if (chr->chrnum < 0 || !chr->prop || !chr->model || chr->prop->type != PROPTYPE_CHR) {
			continue;
		}
		if (chrIsDead(chr) || chr->actiontype == ACT_DIE || chr->actiontype == ACT_DEAD) {
			continue;
		}
		const f32 dx = chr->prop->pos.x - player->prop->pos.x;
		const f32 dy = chr->prop->pos.y - player->prop->pos.y;
		const f32 dz = chr->prop->pos.z - player->prop->pos.z;
		const f32 dist = dx * dx + dy * dy + dz * dz;
		if (!best || dist < bestdist) {
			best = chr;
			bestdist = dist;
		}
	}

	if (!best) {
		return;
	}

	struct gset gset = { WEAPON_FALCON2, 0, 0, FUNC_PRIMARY };
	struct coord dir = {
		best->prop->pos.x - player->prop->pos.x,
		0.f,
		best->prop->pos.z - player->prop->pos.z,
	};

	sysLogPrintf(LOG_NOTE, "NET: autokill: player %d hits chr %d (syncid %u)", victimplayer, best->chrnum, best->prop->syncid);
	chrDamageByImpact(best, 50.f, &dir, &gset, player->prop, HITPART_TORSO);
}

void netAutoTick(void)
{
	netAutoTickStart();
	netAutoTickDump();
	netAutoTickKill();
	netAutoTickQuit();
}

/* autopilot */

static u32 netAutopilotRand(void)
{
	// xorshift32
	u32 x = g_NetAutopilotSeed;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	g_NetAutopilotSeed = x;
	return x;
}

s32 netAutopilotRead(s32 idx, OSContPad *pad)
{
	static s32 action = 0;
	static u32 nextchange = 0;
	static s32 firing = 0;

	if (!g_NetAutopilot || idx != 0 || !g_NetMode || !g_NetLocalClient || g_NetLocalClient->state != CLSTATE_GAME) {
		return false;
	}

	if (g_NetTick >= nextchange) {
		// pick a new thing to do every 0.5-1.5 seconds
		action = netAutopilotRand() % 8;
		firing = (netAutopilotRand() % 100) < 35;
		nextchange = g_NetTick + 30 + netAutopilotRand() % 60;
	}

	pad->button = 0;
	pad->stick_x = 0;
	pad->stick_y = 0;
	pad->rstick_x = 0;
	pad->rstick_y = 0;

	switch (action) {
		case 0: case 1: case 2: pad->stick_y = 100; break; // forward
		case 3: pad->stick_y = 100; pad->stick_x = 50; break; // forward and turn right
		case 4: pad->stick_y = 100; pad->stick_x = -50; break; // forward and turn left
		case 5: pad->stick_x = 80; break; // turn right
		case 6: pad->button |= R_CBUTTONS; pad->stick_y = 60; break; // strafe
		case 7: pad->stick_y = -80; break; // back up
	}

	if (firing) {
		pad->button |= Z_TRIG;
	}

	// open doors and respawn
	if ((g_NetTick % 120) < 4) {
		pad->button |= B_BUTTON;
	}

	return true;
}

/* network condition simulation */

#define NETSIM_QUEUE_SIZE 2048

struct netsimpacket {
	u64 due;
	ENetHost *host;
	ENetPeer *peer;
	ENetPacket *pkt;
	u8 chan;
};

static struct netsimpacket g_NetSimQueue[NETSIM_QUEUE_SIZE];
static u32 g_NetSimHead = 0; // next to send
static u32 g_NetSimTail = 0; // next free slot
static u64 g_NetSimLastDue = 0;

static inline void netSimRelease(ENetPacket *pkt)
{
	if (pkt && pkt->referenceCount == 0) {
		enet_packet_destroy(pkt);
	}
}

static inline void netSimSendNow(ENetHost *host, ENetPeer *peer, u8 chan, ENetPacket *pkt)
{
	if (peer) {
		if (enet_peer_send(peer, chan, pkt) < 0) {
			netSimRelease(pkt);
		}
	} else {
		enet_host_broadcast(host, chan, pkt);
	}
}

void netSimSend(ENetHost *host, ENetPeer *peer, u8 chan, ENetPacket *pkt, s32 reliable)
{
	if (!reliable && g_NetSimLoss && (u32)(rand() % 100) < g_NetSimLoss) {
		netSimRelease(pkt);
		return;
	}

	if (!g_NetSimLag && !g_NetSimJitter) {
		netSimSendNow(host, peer, chan, pkt);
		return;
	}

	const u32 next = (g_NetSimTail + 1) % NETSIM_QUEUE_SIZE;
	if (next == g_NetSimHead) {
		// queue full, just send it
		netSimSendNow(host, peer, chan, pkt);
		return;
	}

	u64 due = sysGetMicroseconds() + (u64)g_NetSimLag * 1000;
	if (g_NetSimJitter) {
		due += (u64)(rand() % (g_NetSimJitter + 1)) * 1000;
	}
	// keep packets in order, otherwise reliable messages would arrive out of order
	// and unreliable ones would get dropped as stale by enet
	if (due < g_NetSimLastDue) {
		due = g_NetSimLastDue;
	}
	g_NetSimLastDue = due;

	struct netsimpacket *sp = &g_NetSimQueue[g_NetSimTail];
	sp->due = due;
	sp->host = host;
	sp->peer = peer;
	sp->pkt = pkt;
	sp->chan = chan;
	g_NetSimTail = next;
}

void netSimFlush(void)
{
	const u64 now = sysGetMicroseconds();
	while (g_NetSimHead != g_NetSimTail) {
		struct netsimpacket *sp = &g_NetSimQueue[g_NetSimHead];
		if (sp->due > now) {
			break;
		}
		if (sp->peer && enet_peer_get_state(sp->peer) != ENET_PEER_STATE_CONNECTED) {
			netSimRelease(sp->pkt);
		} else {
			netSimSendNow(sp->host, sp->peer, sp->chan, sp->pkt);
		}
		g_NetSimHead = (g_NetSimHead + 1) % NETSIM_QUEUE_SIZE;
	}
}

void netSimClear(void)
{
	while (g_NetSimHead != g_NetSimTail) {
		netSimRelease(g_NetSimQueue[g_NetSimHead].pkt);
		g_NetSimHead = (g_NetSimHead + 1) % NETSIM_QUEUE_SIZE;
	}
	g_NetSimLastDue = 0;
}
