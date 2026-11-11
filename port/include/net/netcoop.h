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
u32 netmsgSvcMissionStateRead(struct netbuf *src, struct netclient *srccl);

// server: a mission script changed the music; clients do the same
#define NETMUSIC_SETXREASON   0
#define NETMUSIC_UNSETXREASON 1
#define NETMUSIC_ISOLATED     2
#define NETMUSIC_DEFAULT      3
#define NETMUSIC_CUTSCENE     4
#define NETMUSIC_ENDCUTSCENE  5
#define NETMUSIC_AMBIENT      6
#define NETMUSIC_ENDAMBIENT   7
#define NETMUSIC_PRIMARY      8
void netcoopMusic(u8 op, s32 a, s32 b, s32 c);
u32 netmsgSvcMusicRead(struct netbuf *src, struct netclient *srccl);

#endif
