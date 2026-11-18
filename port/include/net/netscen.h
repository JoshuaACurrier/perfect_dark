#ifndef _IN_NETSCEN_H
#define _IN_NETSCEN_H

// Combat Simulator scenario state (King of the Hill, Pop a Cap, Hold the Briefcase,
// Hacker Central, Capture the Case).
//
// Each machine used to run the scenario by itself: where the hill moves to and who the
// next Pop a Cap victim is came from its own random numbers and its own player numbering,
// so they drifted apart, and someone joining a running match started from the beginning.
// Now the host decides and sends SVC_SCENARIO when the state changes, every ten seconds
// anyway, and to a joiner with its snapshot. Clients only display it: the scenario code
// in src/game/mplayer/scenarios makes no decisions when g_NetMode is NETMODE_CLIENT.

#include <PR/ultratypes.h>
#include "net/netbuf.h"

struct netclient;

// host: once per update round
void netscenServerTick(void);
// host: the current state, e.g. for a joiner's snapshot (writes nothing in plain Combat)
u32 netscenWrite(struct netbuf *dst);
u32 netscenRead(struct netbuf *src, struct netclient *srccl);
// both ends, every frame: logs the state when it changes, for comparing machines
void netscenLogState(void);

// in the scenario code
extern s32 g_KohNetDual;
extern u8 g_KohNetScoreSeq;
void kohNetApply(s32 hillindex, s32 occupiedteam, s32 movehill, s32 dual, s32 elapsed240, s32 scoreseq);
void pacAnnounceVictim(void);

#endif
