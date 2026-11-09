// Mission state sync for online co-op (and later counter-op).
//
// The server runs every mission script. Clients don't (see chraiExecute), so
// whatever a script changes that clients need to know about is sent here:
// - objective statuses (SVC_OBJECTIVES), so the HUD and end screen agree
// - HUD messages created by scripts for remote players (SVC_HUDMSG)

#include <string.h>
#include "platform.h"
#include "types.h"
#include "constants.h"
#include "data.h"
#include "bss.h"
#include "game/hudmsg.h"
#include "game/objectives.h"
#include "game/playermgr.h"
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

void netcoopReset(void)
{
	memset(g_NetObjStatus, OBJECTIVE_INCOMPLETE, sizeof(g_NetObjStatus));
	memset(g_NetObjSent, 0xff, sizeof(g_NetObjSent));
	g_NetObjValid = false;
	g_NetObjSentCount = -1;
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
