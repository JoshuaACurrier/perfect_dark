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

// client: makes the chr follow the server's snapshots instead of running AI
void netchrPuppetTick(struct chrdata *chr);

#endif
