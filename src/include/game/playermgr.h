#ifndef _IN_GAME_PLAYERMGR_H
#define _IN_GAME_PLAYERMGR_H
#include <ultra64.h>
#include "data.h"
#include "types.h"

void playermgrInit(void);
void playermgrReset(void);
void playermgrAllocatePlayers(s32 count);
void playermgrAllocatePlayer(s32 index);
void playermgrCalculateAiBuddyNums(void);
void setCurrentPlayerNum(s32 playernum);
s32 playermgrGetPlayerNumByProp(struct prop *prop);

// Co-op with any number of players (the original had exactly two: g_Vars.bond and g_Vars.coop).
// g_Vars.coop is still the first buddy; these cover "the other player" and "both players".
bool coopAllDead(void);         // every player is dead
bool coopAllDeadAndDone(void);  // ...and their death animations are over
bool coopAnyAborted(void);
// a living player other than this one (Joanna if she is alive); this one if there is nobody
s32 coopOtherPlayerNum(s32 playernum);
// the living player with the most health other than this one, or -1
s32 coopHealthiestOtherPlayerNum(s32 playernum);
// the player whose turn it is this frame (cameras and guns look at one player a frame)
struct prop *coopPlayerPropForFrame(void);
void playermgrSetViewSize(s32 viewx, s32 viewy);
void playermgrSetViewPosition(s32 viewleft, s32 viewtop);
void playermgrSetFovY(f32 fovy);
void playermgrSetAspectRatio(f32 aspect);
s32 playermgrGetModelOfWeapon(s32 weapon);
void playermgrDeleteWeapon(s32 hand);
void playermgrCreateWeapon(s32 hand);
void playermgrShuffle(void);
s32 playermgrGetOrderOfPlayer(s32 playernum);
s32 playermgrGetPlayerAtOrder(s32 ordernum);

#endif
