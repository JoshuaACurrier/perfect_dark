#ifndef _IN_HUDEXT_H
#define _IN_HUDEXT_H

#include <PR/ultratypes.h>
#include "types.h"

// HUD additions that aren't in the original game: hit markers, a kill sound, the
// "killed by" / respawn countdown on the death screen, and teammates' names.

// Combat Sim: respawn this many seconds after dying without pressing anything (0 = off).
// In net games the host's setting applies to everyone.
extern s32 g_MpAutoRespawnSecs;

// the auto respawn delay this match uses
s32 hudextRespawnSecs(void);

// a local player's shot hit someone (kill: and killed them)
void hudextHitmark(struct player *pl, bool kill);

// chrDamage just ran: works out whether a player shot someone and tells them
s32 hudextChrIsDead(struct chrdata *chr);
void hudextOnDamage(struct chrdata *victim, struct prop *aprop, f32 damage, s32 wasdead);

// drawing, for g_Vars.currentplayer
Gfx *hudextDrawHitmark(Gfx *gdl, s32 x, s32 y);
Gfx *hudextDrawDeath(Gfx *gdl);
Gfx *hudextDrawTeamNames(Gfx *gdl);

#endif
