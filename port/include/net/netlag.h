#ifndef _IN_NETLAG_H
#define _IN_NETLAG_H

#include <PR/ultratypes.h>
#include "types.h"
#include "net/net.h"

// Lag compensation and the ping readout, see netlag.c.

extern s32 g_NetLagComp;       // Net.Server.LagCompensation
extern s32 g_NetLagCompMaxMs;  // Net.Server.LagCompensationMaxMs
extern s32 g_NetShowPing;      // Net.ShowPing

// for the test tools
extern u32 g_NetLagShotFrames;   // frames in which a remote player's shots were tested against the past
extern u32 g_NetLagMovedTargets; // targets moved back for those
extern f32 g_NetLagRewindSum;    // total rewind over those targets, in 60ths
extern f32 g_NetLagShiftSum;     // total distance they were moved, in world units
extern f32 g_NetLagBias;
#define NETLAG_MEASURE_NUM 9
extern s32 g_NetLagMeasure;      // --net-aimtest: measure how well the rewind matches the shooter's aim
extern const f32 g_NetLagMeasureOfs[NETLAG_MEASURE_NUM];
extern f32 g_NetLagMeasureSum[NETLAG_MEASURE_NUM];
extern f32 g_NetLagMeasureNowSum;
extern u32 g_NetLagMeasureCount;
extern u32 g_NetLagFireFrames;   // frames in which a remote player's gun was firing
extern u32 g_NetLagHits;         // hit markers sent to remote players

void netlagReset(void);

// host: a move from this client arrived, stamped with the server time it was looking at
void netlagOnClientMove(struct netclient *cl, f32 viewtime, f32 chrdelay);

// host, once per update: remember where everyone is, and send the pings out
void netlagServerTick(void);

// host, around handsTickAttack() for the current player
void netlagBeginShots(void);
void netlagEndShots(void);

Gfx *netlagDrawPing(Gfx *gdl);

#endif
