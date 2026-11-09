// Automation and debugging helpers for net games.
//
// Command line flags:
//   --net-name NAME          local player name
//   --net-autostart N        (host) start the match once N players (including the host) are in the lobby
//   --net-stage NUM          (host) stage for the auto-started match, e.g. 0x1f (Complex)
//   --net-sims N             (host) number of simulants in the auto-started match
//   --net-simdiff N          (host) simulant profile: 0 meat, 1 easy, 2 normal, 3 hard, 4 perfect, 5 dark
//   --net-scenario N         (host) MPSCENARIO_*
//   --net-autoquit SECS      quit the game SECS seconds after the match starts
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
#include "system.h"

s32 g_NetAutopilot = 0;

static s32 g_NetAutoStartPlayers = 0;
static s32 g_NetAutoStage = -1;
static s32 g_NetAutoSims = -1;
static s32 g_NetAutoSimDiff = BOTDIFF_NORMAL;
static s32 g_NetAutoScenario = -1;
static s32 g_NetAutoQuitSecs = 0;
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
	g_NetAutoQuitSecs = sysArgGetInt("--net-autoquit", 0);

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

static void netAutoStartMatch(void)
{
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
	if (g_NetAutoQuitSecs <= 0) {
		return;
	}

	if (g_NetLocalClient && g_NetLocalClient->state == CLSTATE_GAME) {
		++g_NetAutoGameTicks;
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

	fflush(g_NetDumpFile);
}

void netAutoTick(void)
{
	netAutoTickStart();
	netAutoTickDump();
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
