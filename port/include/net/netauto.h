#ifndef _IN_NETAUTO_H
#define _IN_NETAUTO_H

// Automation and debugging helpers for net games: starting matches from the
// command line, scripted input, network condition simulation and state dumps.
// None of this is active unless the matching command line flag is given.

#include <PR/ultratypes.h>
#include <PR/os_cont.h>

struct _ENetPeer;
struct _ENetPacket;

extern s32 g_NetAutopilot;

void netAutoInit(void);

// called once per frame from netEndFrame()
void netAutoTick(void);

// override player name for the local player, if --net-name was given
void netAutoApplyName(void);

// fills in scripted input for controller idx if autopilot is active; returns true if it did
s32 netAutopilotRead(s32 idx, OSContPad *pad);

// network condition simulation (--net-lag, --net-jitter, --net-loss)
// netSimSend takes ownership of the packet; peer == NULL means broadcast on host
struct _ENetHost;
void netSimSend(struct _ENetHost *host, struct _ENetPeer *peer, u8 chan, struct _ENetPacket *pkt, s32 reliable);
void netSimFlush(void);
void netSimClear(void);

#endif
