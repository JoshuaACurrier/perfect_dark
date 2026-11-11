#ifndef _IN_NETCHR_H
#define _IN_NETCHR_H

#include "types.h"
#include "net/netbuf.h"

struct netclient;

// (re)allocates per-chr net state; call after the chrs for a stage have been set up
void netchrReset(void);

// server: sends chr snapshots for this frame
void netchrServerSend(void);

// server: a simulant respawned
void netchrOnRespawn(struct chrdata *chr);

// client: the server removed this chr (SVC_CHR_REMOVE); until then, local code may not
void netchrAllowRemove(struct chrdata *chr);
s32 netchrClientMayRemove(struct chrdata *chr);

// client: a chr was spawned by the server mid-stage (its slot may have been used before)
void netchrOnClientSpawn(struct chrdata *chr);

// client: makes the chr follow the server's snapshots instead of running AI
void netchrPuppetTick(struct chrdata *chr);

#endif
