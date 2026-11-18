#ifndef _IN_NETROCKET_H
#define _IN_NETROCKET_H

// The Slayer's guided rocket (fly-by-wire).
//
// A rocket is made by the host, and after that every machine flies its own copy in a
// straight line. That is fine for a normal rocket and useless for one that is steered:
// the host has no stick or mouse for a remote pilot, so the rocket flew straight while
// its pilot stood frozen, and nobody else saw the host's own rocket turn either.
//
// Here the pilot's own machine steers its copy, so it responds at once, and decides when
// it hits something:
//   * host -> pilot (SVC_ROCKET start): the rocket you just fired is yours to fly.
//   * pilot -> host (CLC_ROCKET, every frame): where the rocket is, which way it points
//     and how fast it goes. The last one says whether it blew up or was let go (the
//     pilot died), and where.
//   * host -> everyone (SVC_ROCKET move, every update): the same for each guided rocket
//     in flight, the host's own included. The pilot ignores the ones about its own rocket.
//   * host -> everyone (SVC_ROCKET gone / release): the flight is over and where. For the
//     pilot that means the host ended it (the pilot died).
// Only the pilot's copy decides that the rocket hit something. The other copies run a
// frame or two ahead on the last course they were told and would clip corners the pilot
// went around, so they do not collide with anything (netrocketPuppetTick) and do not go
// off by themselves (netrocketHoldDetonation). The blast and its damage are still the
// host's, at the place the pilot reported.

#include <PR/ultratypes.h>
#include "net/netbuf.h"

struct netclient;
struct weaponobj;
struct prop;
struct defaultobj;

void netrocketReset(void);
// every frame from netEndFrame(); sendmoves is set on the host's update rounds
void netrocketTick(s32 sendmoves);
u32 netrocketSvcRead(struct netbuf *src, struct netclient *srccl);
u32 netrocketClcRead(struct netbuf *src, struct netclient *srccl);
// a rocket that hit something: true if it must not go off here (someone else flies it)
s32 netrocketHoldDetonation(struct weaponobj *rocket);
// true if this object is a guided rocket someone else flies; it was then moved along here
// and the game's own flight (with its collisions) must be skipped
s32 netrocketPuppetTick(struct defaultobj *obj);
// client: the host removed this prop; true if it was a guided rocket, which then goes off
// here instead of vanishing
s32 netrocketOnRemove(struct prop *prop);

#endif
