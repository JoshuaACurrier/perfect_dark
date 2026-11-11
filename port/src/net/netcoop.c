// Mission state sync for online co-op (and later counter-op).
//
// The server runs every mission script. Clients don't (see chraiExecute), so
// whatever a script changes that clients need to know about is sent here:
// - objective statuses (SVC_OBJECTIVES), so the HUD and end screen agree
// - HUD messages created by scripts for remote players (SVC_HUDMSG)
// - alarm, gas and the countdown timer (SVC_MISSIONSTATE)
// - music changes (SVC_MUSIC)

#include <string.h>
#include "platform.h"
#include "types.h"
#include "constants.h"
#include "data.h"
#include "bss.h"
#include "game/hudmsg.h"
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

void netcoopReset(void)
{
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

void netcoopServerTick(void)
{
	if (g_NetMode != NETMODE_SERVER || g_NetLocalClient->state < CLSTATE_GAME) {
		return;
	}

	// only missions have objectives
	if (g_Vars.normmplayerisrunning || (g_NetTick % 10) != 0) {
		return;
	}

	netcoopMissionStateTick();

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
		alarmActivate();
	} else if (!alarm && alarmIsActive()) {
		alarmDeactivate();
	}

	if (gas && !g_GasReleasing && g_GasReleaseTimer240 <= 0) {
		gasReleaseFromPos(&gaspos);
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
