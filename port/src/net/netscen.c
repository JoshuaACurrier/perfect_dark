// Combat Simulator scenario state from the host, see netscen.h.

#include <stdio.h>
#include <string.h>
#include "types.h"
#include "data.h"
#include "bss.h"
#include "constants.h"
#include "game/inv.h"
#include "game/mplayer/mplayer.h"
#include "game/mplayer/scenarios.h"
#include "game/player.h"
#include "game/playermgr.h"
#include "game/prop.h"
#include "system.h"
#include "net/net.h"
#include "net/netbuf.h"
#include "net/netmsg.h"
#include "net/netjoin.h"
#include "net/netscen.h"

#define NO_CHR 0xff
#define RESEND_TICKS 600

enum { TOKEN_BRIEFCASE, TOKEN_UPLINK, TOKEN_CASE };

static u8 g_LastKey[256];
static u32 g_LastKeyLen = 0;
static u32 g_NextSendTick = 0;

// Characters are numbered players first, then Simulants, on every machine; a client
// swaps itself with player 0.
static s32 netscenLocalChr(s32 hostindex)
{
	if (hostindex < 0 || hostindex >= g_MpNumChrs) {
		return -1;
	}
	return hostindex < PLAYERCOUNT() ? netjoinLocalNum(hostindex) : hostindex;
}

// host: which character is this prop, if any
static u8 netscenChrOfProp(const struct prop *prop)
{
	if (prop && (prop->type == PROPTYPE_PLAYER || prop->type == PROPTYPE_CHR)) {
		for (s32 i = 0; i < g_MpNumChrs; ++i) {
			if (g_MpAllChrPtrs[i] && g_MpAllChrPtrs[i]->prop == prop) {
				return i;
			}
		}
	}
	return NO_CHR;
}

static bool netscenActive(void)
{
	return g_Vars.normmplayerisrunning && g_MpSetup.scenario != MPSCENARIO_COMBAT
		&& g_MpSetup.scenario <= MPSCENARIO_CAPTURETHECASE && g_MpNumChrs > 0;
}

// timers are left out of the copy that is compared to see if anything changed
static u32 netscenWriteState(struct netbuf *dst, bool timers)
{
	const s32 n = g_MpNumChrs;

	netbufWriteU8(dst, SVC_SCENARIO);
	netbufWriteU8(dst, g_MpSetup.scenario);

	switch (g_MpSetup.scenario) {
		case MPSCENARIO_HOLDTHEBRIEFCASE:
			netbufWriteU8(dst, netscenChrOfProp(g_ScenarioData.htb.token));
			break;
		case MPSCENARIO_HACKERCENTRAL:
			netbufWriteU8(dst, netscenChrOfProp(g_ScenarioData.htm.uplink));
			netbufWriteU8(dst, n);
			for (s32 i = 0; i < n; ++i) {
				netbufWriteS16(dst, g_ScenarioData.htm.numpoints[i]);
			}
			break;
		case MPSCENARIO_POPACAP:
			netbufWriteU8(dst, n);
			for (s32 i = 0; i < n; ++i) {
				netbufWriteS16(dst, g_ScenarioData.pac.killcounts[i]);
				netbufWriteS16(dst, g_ScenarioData.pac.survivalcounts[i]);
			}
			for (s32 i = 0; i < n; ++i) {
				netbufWriteU8(dst, g_ScenarioData.pac.victims[i]);
			}
			netbufWriteS8(dst, g_ScenarioData.pac.victimindex);
			netbufWriteU16(dst, timers ? g_ScenarioData.pac.age240 : 0);
			break;
		case MPSCENARIO_KINGOFTHEHILL:
			netbufWriteS8(dst, g_ScenarioData.koh.hillindex);
			netbufWriteS8(dst, g_ScenarioData.koh.occupiedteam);
			netbufWriteU8(dst, (g_ScenarioData.koh.movehill ? 1 : 0) | (g_KohNetDual ? 2 : 0));
			netbufWriteU8(dst, g_KohNetScoreSeq);
			netbufWriteU8(dst, g_Vars.mphilltime);
			netbufWriteS16(dst, timers ? g_ScenarioData.koh.elapsed240 : 0);
			break;
		case MPSCENARIO_CAPTURETHECASE:
			for (s32 t = 0; t < ARRAYCOUNT(g_ScenarioData.ctc.tokens); ++t) {
				const struct prop *token = g_ScenarioData.ctc.tokens[t];
				const u8 holder = netscenChrOfProp(token);
				netbufWriteU8(dst, holder);
				netbufWriteU32(dst, (token && holder == NO_CHR && token->type == PROPTYPE_WEAPON) ? token->syncid : 0);
			}
			break;
		default:
			break;
	}

	return dst->error;
}

// The state as this machine has it, in the host's numbering, so the host's and the
// clients' logs can be compared line for line.
static void netscenDescribe(char *out, size_t size)
{
	const s32 n = g_MpNumChrs;
	s32 len = 0;

#define ADD(...) do { if (len < (s32)size) { len += snprintf(out + len, size - len, __VA_ARGS__); } } while (0)
// (the swap goes both ways: host number of a local character)
#define HOSTNUM(prop) ((s32)(s8)netscenLocalChr(netscenChrOfProp(prop) == NO_CHR ? -1 : netscenChrOfProp(prop)))

	out[0] = '\0';

	switch (g_MpSetup.scenario) {
		case MPSCENARIO_HOLDTHEBRIEFCASE:
			ADD("briefcase holder %d", HOSTNUM(g_ScenarioData.htb.token));
			break;
		case MPSCENARIO_HACKERCENTRAL:
			ADD("uplink holder %d, points", HOSTNUM(g_ScenarioData.htm.uplink));
			for (s32 i = 0; i < n; ++i) {
				ADD(" %d", g_ScenarioData.htm.numpoints[netscenLocalChr(i)]);
			}
			break;
		case MPSCENARIO_POPACAP:
			ADD("victim %d, kills/survivals", g_ScenarioData.pac.victimindex < 0 ? -1
				: netscenLocalChr(g_ScenarioData.pac.victims[g_ScenarioData.pac.victimindex]));
			for (s32 i = 0; i < n; ++i) {
				ADD(" %d/%d", g_ScenarioData.pac.killcounts[netscenLocalChr(i)], g_ScenarioData.pac.survivalcounts[netscenLocalChr(i)]);
			}
			break;
		case MPSCENARIO_KINGOFTHEHILL:
			ADD("hill %d (room %d), team %d, moving %d, scored %u", g_ScenarioData.koh.hillindex, g_ScenarioData.koh.hillrooms[0],
				g_ScenarioData.koh.occupiedteam, g_ScenarioData.koh.movehill ? 1 : 0, g_KohNetScoreSeq);
			break;
		case MPSCENARIO_CAPTURETHECASE:
			ADD("cases");
			for (s32 t = 0; t < ARRAYCOUNT(g_ScenarioData.ctc.tokens); ++t) {
				const struct prop *token = g_ScenarioData.ctc.tokens[t];
				if (!token) {
					ADD(" none");
				} else if (token->type == PROPTYPE_WEAPON) {
					ADD(" ground:%u(team %d)", token->syncid, token->weapon ? token->weapon->team : -1);
				} else {
					ADD(" held:%d", HOSTNUM(token));
				}
			}
			break;
		default:
			break;
	}

#undef ADD
#undef HOSTNUM
}

// Logged whenever it changes: "NET: scenario: ..." reads the same on every machine.
void netscenLogState(void)
{
	static char last[256];
	char now[256];

	if (!g_NetMode || !netscenActive()) {
		last[0] = '\0';
		return;
	}

	netscenDescribe(now, sizeof(now));
	if (strcmp(now, last)) {
		snprintf(last, sizeof(last), "%s", now);
		sysLogPrintf(LOG_NOTE, "NET: scenario: %s", now);
	}
}

u32 netscenWrite(struct netbuf *dst)
{
	if (!netscenActive()) {
		return 0;
	}
	return netscenWriteState(dst, true);
}

void netscenServerTick(void)
{
	if (g_NetMode != NETMODE_SERVER || !netscenActive() || !g_NetLocalClient || g_NetLocalClient->state < CLSTATE_GAME) {
		g_LastKeyLen = 0;
		g_NextSendTick = 0;
		return;
	}

	u8 key[sizeof(g_LastKey)];
	struct netbuf buf = { .data = key, .size = sizeof(key) };
	netbufStartWrite(&buf);
	if (netscenWriteState(&buf, false)) {
		return;
	}

	if (buf.wp == g_LastKeyLen && !memcmp(key, g_LastKey, buf.wp) && g_NetTick < g_NextSendTick) {
		return;
	}

	memcpy(g_LastKey, key, buf.wp);
	g_LastKeyLen = buf.wp;
	g_NextSendTick = g_NetTick + RESEND_TICKS;
	netscenWriteState(&g_NetMsgRel, true);
}

// Make exactly the characters in `holders` (local numbers) carry the token.
static s32 netscenSetHolders(s32 kind, u32 holders)
{
	const s32 weaponnum = (kind == TOKEN_UPLINK) ? WEAPON_DATAUPLINK : WEAPON_BRIEFCASE2;
	const s32 prevplayernum = g_Vars.currentplayernum;
	s32 fixed = 0;

	for (s32 i = 0; i < g_MpNumChrs; ++i) {
		struct chrdata *chr = g_MpAllChrPtrs[i];
		const bool should = (holders & (1u << i)) != 0;

		if (!chr || !chr->prop) {
			continue;
		}

		if (i < PLAYERCOUNT()) {
			if (!g_Vars.players[i] || netjoinIsParked(i)) {
				continue;
			}
			setCurrentPlayerNum(i);
			const bool has = (kind == TOKEN_UPLINK) ? invHasDataUplink() : invHasBriefcase();
			if (should && !has) {
				invGiveSingleWeapon(weaponnum);
				++fixed;
			} else if (!should && has) {
				invRemoveItemByNum(weaponnum);
				++fixed;
			}
		} else if (chr->aibot) {
			bool has;
			switch (kind) {
				case TOKEN_UPLINK: has = chr->aibot->hasuplink; chr->aibot->hasuplink = should; break;
				case TOKEN_CASE: has = chr->aibot->hascase; chr->aibot->hascase = should; break;
				default: has = chr->aibot->hasbriefcase; chr->aibot->hasbriefcase = should; break;
			}
			if (has != should) {
				++fixed;
			}
		}
	}

	setCurrentPlayerNum(prevplayernum);
	return fixed;
}

u32 netscenRead(struct netbuf *src, struct netclient *srccl)
{
	const u8 scenario = netbufReadU8(src);
	const bool apply = !src->error && srccl->state >= CLSTATE_GAME && g_NetMode == NETMODE_CLIENT
		&& netscenActive() && scenario == g_MpSetup.scenario;
	s32 fixed = 0;

	switch (scenario) {
		case MPSCENARIO_HOLDTHEBRIEFCASE: {
			const s32 holder = netscenLocalChr(netbufReadU8(src));
			if (apply && !src->error) {
				fixed = netscenSetHolders(TOKEN_BRIEFCASE, holder >= 0 ? (1u << holder) : 0);
			}
			break;
		}
		case MPSCENARIO_HACKERCENTRAL: {
			const u8 hostholder = netbufReadU8(src);
			const u8 n = netbufReadU8(src);
			if (src->error || n > MAX_MPCHRS) {
				return 1;
			}
			s16 points[MAX_MPCHRS];
			for (s32 i = 0; i < n; ++i) {
				points[i] = netbufReadS16(src);
			}
			if (apply && !src->error) {
				const s32 holder = netscenLocalChr(hostholder);
				fixed = netscenSetHolders(TOKEN_UPLINK, holder >= 0 ? (1u << holder) : 0);
				for (s32 i = 0; i < n; ++i) {
					const s32 local = netscenLocalChr(i);
					if (local >= 0) {
						g_ScenarioData.htm.numpoints[local] = points[i];
					}
				}
			}
			break;
		}
		case MPSCENARIO_POPACAP: {
			const u8 n = netbufReadU8(src);
			if (src->error || n > MAX_MPCHRS) {
				return 1;
			}
			s16 kills[MAX_MPCHRS];
			s16 survivals[MAX_MPCHRS];
			u8 victims[MAX_MPCHRS];
			for (s32 i = 0; i < n; ++i) {
				kills[i] = netbufReadS16(src);
				survivals[i] = netbufReadS16(src);
			}
			for (s32 i = 0; i < n; ++i) {
				victims[i] = netbufReadU8(src);
			}
			const s8 victimindex = netbufReadS8(src);
			const u16 age240 = netbufReadU16(src);
			if (apply && !src->error && n == g_MpNumChrs && victimindex >= 0 && victimindex < n) {
				struct scenariodata_pac *data = &g_ScenarioData.pac;
				const s32 prevvictim = data->victimindex >= 0 ? data->victims[data->victimindex] : -1;
				bool ok = true;
				for (s32 i = 0; i < n; ++i) {
					if (netscenLocalChr(victims[i]) < 0) {
						ok = false;
					}
				}
				if (ok) {
					for (s32 i = 0; i < n; ++i) {
						const s32 local = netscenLocalChr(i);
						data->killcounts[local] = kills[i];
						data->survivalcounts[local] = survivals[i];
						data->victims[i] = netscenLocalChr(victims[i]);
					}
					data->victimindex = victimindex;
					data->age240 = age240;
					if (data->victims[victimindex] != prevvictim) {
						pacAnnounceVictim();
						fixed = 1;
					}
				}
			}
			break;
		}
		case MPSCENARIO_KINGOFTHEHILL: {
			const s8 hillindex = netbufReadS8(src);
			const s8 occupiedteam = netbufReadS8(src);
			const u8 flags = netbufReadU8(src);
			const u8 scoreseq = netbufReadU8(src);
			const u8 hilltime = netbufReadU8(src);
			const s16 elapsed240 = netbufReadS16(src);
			if (apply && !src->error) {
				if (hillindex != g_ScenarioData.koh.hillindex) {
					fixed = 1;
				}
				g_Vars.mphilltime = hilltime; // (the host's Time setting, for the timer on the HUD)
				kohNetApply(hillindex, occupiedteam, flags & 1, flags & 2, elapsed240, scoreseq);
			}
			break;
		}
		case MPSCENARIO_CAPTURETHECASE: {
			u8 hostholders[ARRAYCOUNT(g_ScenarioData.ctc.tokens)];
			u32 syncids[ARRAYCOUNT(g_ScenarioData.ctc.tokens)];
			for (s32 t = 0; t < ARRAYCOUNT(hostholders); ++t) {
				hostholders[t] = netbufReadU8(src);
				syncids[t] = netbufReadU32(src);
			}
			if (apply && !src->error) {
				u32 holders = 0;
				for (s32 t = 0; t < ARRAYCOUNT(hostholders); ++t) {
					const s32 holder = netscenLocalChr(hostholders[t]);
					if (holder >= 0 && g_MpAllChrPtrs[holder] && g_MpAllChrPtrs[holder]->prop) {
						holders |= 1u << holder;
						g_ScenarioData.ctc.tokens[t] = g_MpAllChrPtrs[holder]->prop;
					} else if (syncids[t]) {
						// on the ground: it is this team's case, wherever it came from
						struct prop *prop = netPropBySyncId(syncids[t]);
						if (prop && prop->type == PROPTYPE_WEAPON && prop->weapon
								&& prop->weapon->weaponnum == WEAPON_BRIEFCASE2) {
							prop->weapon->team = t;
							g_ScenarioData.ctc.tokens[t] = prop;
						}
					}
				}
				fixed = netscenSetHolders(TOKEN_CASE, holders);
			}
			break;
		}
		default:
			return 1;
	}

	if (fixed && !src->error) {
		sysLogPrintf(LOG_NOTE, "NET: scenario %u state from the host changed %d thing(s) here", scenario, fixed);
	}

	return src->error;
}
