#ifndef _IN_NETJOIN_H
#define _IN_NETJOIN_H

#include <PR/ultratypes.h>
#include "types.h"
#include "net/net.h"
#include "net/netbuf.h"

// Joining a Combat Simulator match that is already running, and leaving one. See netjoin.c.
//
// The number of players in a match is fixed when the stage loads, so the host starts the
// match with a few spare player slots that nobody controls (Net.Server.SpareSlots). An
// empty slot's player is kept dead and hidden on every machine. Someone who connects during
// the match is given an empty slot: they load the stage with the match's original random
// seeds, get the current state of the world, and spawn. When a player disconnects their
// slot becomes empty again and can be reused.

extern s32 g_NetServerSpareSlots;     // Net.Server.SpareSlots
extern s32 g_NetServerJoinInProgress; // Net.Server.JoinInProgress

// how many spare slots the match that's about to start gets (host)
s32 netjoinSpareSlots(void);

// set while an empty slot's player is being put down: no kill is recorded for it
extern s32 g_NetQuietDeath;

// is nobody playing this player slot (local player number)?
s32 netjoinIsVacant(s32 playernum);
s32 netjoinIsVacantConfig(const struct mpchrconfig *cfg);
// vacant, or waiting for its new owner to finish loading: this player must stay dead
s32 netjoinIsParked(s32 playernum);

// a server player number in this machine's numbering (clients put themselves first)
s32 netjoinLocalNum(s32 svplayernum);
void netjoinSetMySvNum(s32 svplayernum);

// the tail of SVC_STAGE_START: empty slots, and for a mid-match join the match clock
void netjoinStageStartWrite(struct netbuf *dst, s32 join);
void netjoinStageStartRead(struct netbuf *src);
void netjoinSaveSeeds(void);
void netjoinGetSeeds(u64 *seed1, u64 *seed2);

// end of netPlayersAllocate: work out which slots are empty
void netjoinOnPlayersAllocated(void);
// end of netSyncIdsAllocate: the stage is loaded
void netjoinOnStageLoaded(void);

// every frame while in a match (both ends)
void netjoinTick(void);

// host
void netjoinServerRequest(struct netclient *cl); // connected during a match
void netjoinServerTick(void);                    // right after the frame's first flush
void netjoinServerOnMove(struct netclient *cl);  // CLC_MOVE arrived
void netjoinServerOnLeave(struct netclient *cl); // before the client is reset

// client: while the stage loads after a mid-match join, reliable packets are kept and
// played back once it is up
s32 netjoinClientLoading(void);
// the host says our own player is dead (or not): true while that is only because we
// have just joined and it hasn't spawned us yet
s32 netjoinClientAwaitingSpawn(s32 isdead);
s32 netjoinClientKeepPacket(const u8 *data, u32 len, s32 reliable);
// returns the next kept packet once the stage is ready (free() it), or NULL
u8 *netjoinClientNextPacket(u32 *len);

// Self-healing: every Net.Server.ResyncSecs the host lists what exists in its world (pickups
// that are gone, loose guns, which doors are open) and each client repairs what differs:
// removes what shouldn't be there, asks for what it is missing, opens or closes doors.
extern s32 g_NetServerResyncSecs;
u32 netjoinResyncRead(struct netbuf *src, struct netclient *srccl);
u32 netjoinPropReqRead(struct netbuf *src, struct netclient *srccl);
// --net-desynctest: the host loses every other "this was picked up" message and clients
// delete a loose gun now and then, to give the resync something to repair
s32 netjoinTestLoseMessage(void);

// SVC_SLOT: a player slot got a new owner, or lost it
u32 netjoinSlotRead(struct netbuf *src, struct netclient *srccl);

#endif
