#ifndef _IN_NETCOOP_H
#define _IN_NETCOOP_H

#include "types.h"
#include "net/netbuf.h"

struct netclient;

// clears per-stage mission state; called when a stage starts
void netcoopReset(void);

// server: sends objective changes etc; called every frame while in game
void netcoopServerTick(void);

// client: if the server has told us objective statuses, puts the given one in *status and returns true
s32 netcoopGetObjectiveStatus(s32 index, s32 *status);

// server: forwards a hud message that a script created for a remote player
void netcoopForwardHudmsg(const char *text, s32 type, u32 flags);

u32 netmsgSvcObjectivesRead(struct netbuf *src, struct netclient *srccl);
u32 netmsgSvcHudmsgRead(struct netbuf *src, struct netclient *srccl);

#endif
