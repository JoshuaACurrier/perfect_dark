#ifndef _IN_NETHUD_H
#define _IN_NETHUD_H

#include <PR/ultratypes.h>
#include <PR/gbi.h>
#include "types.h"
#include "net/net.h"
#include "net/netbuf.h"

// On-screen things for network games that aren't in the original game: the lobby's
// player list and chat window, chat while playing, the score and timer bar, the kill
// feed and the hold-Tab scoreboard. See nethud.c.

extern s32 g_NetHudScoreBar; // Game.MpScoreBar
extern s32 g_NetHudKillFeed; // Game.MpKillFeed

void nethudReset(void);

// every frame: keys (chat, ready, scoreboard) and, on the host, the roster broadcast
void nethudTick(void);
Gfx *nethudRender(Gfx *gdl);

// is the chat line being typed? (the game and menus get no input meanwhile)
s32 nethudIsTyping(void);

void nethudChatAdd(const char *text);
void nethudKill(const struct mpchrconfig *attacker, const struct mpchrconfig *victim);

// SVC_NETINFO: who is connected, their ping, loss and ready mark
u32 nethudRosterWrite(struct netbuf *dst);
u32 nethudRosterRead(struct netbuf *src, struct netclient *srccl);

// CLC_READY
u32 nethudReadyWrite(struct netbuf *dst, s32 ready);
u32 nethudReadyRead(struct netbuf *src, struct netclient *srccl);

#endif
