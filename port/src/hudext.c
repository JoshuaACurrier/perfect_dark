// HUD additions that aren't in the original game, see hudext.h.

#include <stdio.h>
#include <string.h>
#include <ultra64.h>
#include "constants.h"
#include "bss.h"
#include "data.h"
#include "types.h"
#include "game/camera.h"
#include "game/chraction.h"
#include "game/game_1531a0.h"
#include "game/mplayer/mplayer.h"
#include "game/player.h"
#include "game/playermgr.h"
#include "lib/mtx.h"
#include "lib/snd.h"
#include "lib/vi.h"
#include "net/net.h"
#include "net/netmsg.h"
#include "hudext.h"
#include "system.h"
#include "video.h"

static s32 g_NetAutoDebugHud = -1;

#define HITMARK_TICKS      18
#define HITMARK_KILL_TICKS 36

s32 g_MpAutoRespawnSecs = 5;

s32 hudextRespawnSecs(void)
{
	if (g_NetMode == NETMODE_CLIENT) {
		return g_NetHostRespawnSecs;
	}
	return g_MpAutoRespawnSecs;
}

/* hit markers and the kill sound */

void hudextHitmark(struct player *pl, bool kill)
{
	if (!pl) {
		return;
	}

	const s32 playernum = playermgrGetPlayerNumByProp(pl->prop);
	const s32 extnum = (playernum >= 0) ? playernum % MAX_LOCAL_PLAYERS : 0;

	if (kill) {
		pl->hitmarkkill = true;
		pl->hitmarktime60 = TICKS(HITMARK_KILL_TICKS);

		if (g_PlayerExtCfg[extnum].killsound) {
			sndStart(var80095200, SFX_MENU_SELECT, NULL, -1, -1, 1.4f, -1, -1);
		}
	} else if (!pl->hitmarkkill || pl->hitmarktime60 <= 0) {
		pl->hitmarkkill = false;
		pl->hitmarktime60 = TICKS(HITMARK_TICKS);
	}
}

s32 hudextChrIsDead(struct chrdata *chr)
{
	if (!chr || !chr->prop) {
		return true;
	}

	if (chr->prop->type == PROPTYPE_PLAYER) {
		const s32 playernum = playermgrGetPlayerNumByProp(chr->prop);
		return playernum < 0 || !g_Vars.players[playernum] || g_Vars.players[playernum]->isdead;
	}

	return chrIsDead(chr);
}

void hudextOnDamage(struct chrdata *victim, struct prop *aprop, f32 damage, s32 wasdead)
{
	// clients get theirs from the host (SVC_HITMARK)
	if (g_NetMode == NETMODE_CLIENT || wasdead || damage <= 0.f) {
		return;
	}

	if (!victim || !aprop || aprop->type != PROPTYPE_PLAYER || aprop == victim->prop) {
		return;
	}

	const s32 playernum = playermgrGetPlayerNumByProp(aprop);
	if (playernum < 0 || !g_Vars.players[playernum]) {
		return;
	}

	struct player *pl = g_Vars.players[playernum];
	const bool kill = hudextChrIsDead(victim);

	if (g_NetMode == NETMODE_SERVER && pl->isremote) {
		netmsgSvcHitmarkWrite(pl->client, kill);
	} else {
		hudextHitmark(pl, kill);
	}
}

Gfx *hudextDrawHitmark(Gfx *gdl, s32 x, s32 y)
{
	struct player *pl = g_Vars.currentplayer;

	if (pl->hitmarktime60 <= 0) {
		return gdl;
	}

	pl->hitmarktime60 -= g_Vars.lvupdate60;

	if (!PLAYER_EXTCFG().hitmarkers || pl->isdead) {
		return gdl;
	}

	// fade out over the last few ticks
	s32 alpha = 0xff;
	if (pl->hitmarktime60 < TICKS(8)) {
		alpha = 0xff * (pl->hitmarktime60 > 0 ? pl->hitmarktime60 : 0) / TICKS(8);
	}

	const u32 colour = (pl->hitmarkkill ? 0xff302000 : 0xffffff00) | (alpha & 0xff);
	const s32 inner = 3;
	const s32 outer = pl->hitmarkkill ? 8 : 6;

	gdl = textSetPrimColour(gdl, colour);
	gSPSetExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);
	gDPSetSubpixelOffsetEXT(gdl++, -2, -2);

	for (s32 i = inner; i <= outer; ++i) {
		gDPHudRectangle(gdl++, x - i, y - i, x - i, y - i);
		gDPHudRectangle(gdl++, x + i, y - i, x + i, y - i);
		gDPHudRectangle(gdl++, x - i, y + i, x - i, y + i);
		gDPHudRectangle(gdl++, x + i, y + i, x + i, y + i);
	}

	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);
	gDPSetSubpixelOffsetEXT(gdl++, 0, 0);
	gdl = text0f153838(gdl);

	return gdl;
}

/* death screen: who killed you, and the respawn countdown */

static void hudextCopyName(char *dst, size_t dstlen, const char *src)
{
	snprintf(dst, dstlen, "%s", src ? src : "");
	char *nl = strchr(dst, '\n');
	if (nl) {
		*nl = '\0';
	}
}

static Gfx *hudextTextCentred(Gfx *gdl, const char *text, s32 cx, s32 y, u32 colour, struct fontchar *chars, struct font *font)
{
	s32 textheight, textwidth;
	char buf[128];
	snprintf(buf, sizeof(buf), "%s", text);
	textMeasure(&textheight, &textwidth, buf, chars, font, 0);
	s32 x = cx - textwidth / 2;
	gdl = textRender(gdl, &x, &y, buf, chars, font, colour, 0x000000a0, viGetWidth(), viGetHeight(), 0, 0);
	return gdl;
}

Gfx *hudextDrawDeath(Gfx *gdl)
{
	struct player *pl = g_Vars.currentplayer;
	char line1[96] = "";
	char line2[64] = "";
	char name[MAX_PLAYERNAME + 1];

	if (!pl->isdead || !g_Vars.normmplayerisrunning || pl->isremote) {
		return gdl;
	}

	if (g_Vars.coopplayernum >= 0 || g_Vars.antiplayernum >= 0) {
		return gdl;
	}

	if (pl->killedby >= 0 && pl->killedby < g_MpNumChrs && g_MpAllChrConfigPtrs[pl->killedby]) {
		if (pl->killedby == g_Vars.currentplayernum) {
			snprintf(line1, sizeof(line1), "You killed yourself");
		} else {
			hudextCopyName(name, sizeof(name), g_MpAllChrConfigPtrs[pl->killedby]->name);
			snprintf(line1, sizeof(line1), "Killed by %s", name);
		}
	}

	const s32 secs = hudextRespawnSecs();
	if (secs > 0 && g_NumReasonsToEndMpMatch == 0) {
		const s32 left = secs * 60 - pl->deadtime60;
		if (left > 0) {
			snprintf(line2, sizeof(line2), "Respawning in %d", (left + 59) / 60);
		} else {
			snprintf(line2, sizeof(line2), "Respawning...");
		}
	}

	if (!line1[0] && !line2[0]) {
		return gdl;
	}

	if (!g_CharsHandelGothicMd || !g_FontHandelGothicMd || !g_CharsHandelGothicSm || !g_FontHandelGothicSm) {
		return gdl;
	}

	const s32 cx = viGetViewLeft() / g_ScaleX + viGetViewWidth() / g_ScaleX / 2;
	s32 y = viGetViewTop() + viGetViewHeight() / 2 - 24;

	gdl = text0f153628(gdl);
	gSPSetExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);

	if (line1[0]) {
		gdl = hudextTextCentred(gdl, line1, cx, y, 0xffe060ff, g_CharsHandelGothicMd, g_FontHandelGothicMd);
		y += 18;
	}

	if (line2[0]) {
		gdl = hudextTextCentred(gdl, line2, cx, y, 0xffffffff, g_CharsHandelGothicSm, g_FontHandelGothicSm);
	}

	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);
	gdl = text0f153780(gdl);

	return gdl;
}

/* teammates' names over their heads */

// is this chr alive and on screen-able terms (not hidden, not dead)?
static bool hudextChrShowable(s32 mpchrnum)
{
	struct chrdata *chr = g_MpAllChrPtrs[mpchrnum];
	if (!chr || !chr->prop || (chr->chrflags & CHRCFLAG_HIDDEN)) {
		return false;
	}
	if (mpchrnum < PLAYERCOUNT()) {
		const struct player *pl = g_Vars.players[mpchrnum];
		return pl && pl->prop && !pl->isdead;
	}
	return !chrIsDead(chr) && chr->actiontype != ACT_DIE && chr->actiontype != ACT_DEAD;
}

Gfx *hudextDrawTeamNames(Gfx *gdl)
{
	struct player *me = g_Vars.currentplayer;
	const bool coop = g_Vars.coopplayernum >= 0 && g_Vars.antiplayernum < 0;
	const bool teams = g_Vars.normmplayerisrunning && (g_MpSetup.options & MPOPTION_TEAMSENABLED);

	// remote players' HUDs get "drawn" too on the host, for their timers; skip those
	if (!PLAYER_EXTCFG().teamnames || (!coop && !teams) || me->isdead || !me->prop || me->isremote) {
		return gdl;
	}

	if (!g_CharsHandelGothicXs || !g_FontHandelGothicXs) {
		return gdl;
	}

	const s32 mynum = g_Vars.currentplayernum;
	const s32 count = coop ? PLAYERCOUNT() : g_MpNumChrs;
	const struct mpchrconfig *mycfg = (!coop && mynum < g_MpNumChrs) ? g_MpAllChrConfigPtrs[mynum] : NULL;
	const s32 left = viGetViewLeft() / g_ScaleX;
	const s32 top = viGetViewTop();
	const s32 right = left + viGetViewWidth() / g_ScaleX;
	const s32 bottom = top + viGetViewHeight();
	bool started = false;

	if (!coop && !mycfg) {
		return gdl;
	}

	for (s32 i = 0; i < count; ++i) {
		struct prop *prop;
		const char *rawname;

		if (i == mynum) {
			continue;
		}

		if (coop) {
			const struct player *pl = g_Vars.players[i];
			if (!pl || !pl->prop || pl->isdead) {
				continue;
			}
			prop = pl->prop;
			rawname = g_PlayerConfigsArray[i].base.name;
		} else {
			const struct mpchrconfig *cfg = g_MpAllChrConfigPtrs[i];
			if (!cfg || cfg->team != mycfg->team || !hudextChrShowable(i)) {
				continue;
			}
			prop = g_MpAllChrPtrs[i]->prop;
			rawname = cfg->name;
		}

		// a bit above the head
		struct coord pos = prop->pos;
		pos.y += (prop->type == PROPTYPE_PLAYER) ? 30.f : 90.f;

		struct coord campos;
		mtx4TransformVec(camGetWorldToScreenMtxf(), &pos, &campos);
		if (campos.z > -10.f) {
			continue; // behind us
		}

		f32 screenpos[2];
		cam0f0b4d68(&campos, screenpos);

		if (g_NetAutoDebugHud < 0) {
			g_NetAutoDebugHud = sysArgCheck("--net-hudtest");
		}
		if (g_NetAutoDebugHud && (g_Vars.lvframe60 % 60) == 0) {
			sysLogPrintf(LOG_NOTE, "HUD: name %d at cam (%.0f %.0f %.0f) screen (%.0f %.0f)",
				i, campos.x, campos.y, campos.z, screenpos[0], screenpos[1]);
		}

		if (screenpos[0] < left || screenpos[0] > right || screenpos[1] < top || screenpos[1] > bottom) {
			continue;
		}

		// the 3D view spans the whole (maybe widescreen) window; the text is drawn in the
		// centred 4:3 HUD space, like the crosshair
		const s32 x = (s32)((f32)(SCREEN_WIDTH_LO / 2) + (screenpos[0] - (f32)(SCREEN_WIDTH_LO / 2)) * (videoGetAspect() / SCREEN_ASPECT));

		// fade with distance
		const f32 dist = -campos.z;
		s32 alpha = 0xff;
		if (dist > 4000.f) {
			alpha = dist > 8000.f ? 0x60 : (s32)(0xff - (dist - 4000.f) / 4000.f * (0xff - 0x60));
		}

		char name[MAX_PLAYERNAME + 1];
		hudextCopyName(name, sizeof(name), rawname);

		if (!started) {
			gdl = text0f153628(gdl);
			gSPSetExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);
			started = true;
		}

		gdl = hudextTextCentred(gdl, name, x, (s32)screenpos[1] - 10, 0x80ff8000 | (alpha & 0xff),
			g_CharsHandelGothicXs, g_FontHandelGothicXs);
	}

	if (started) {
		gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);
		gdl = text0f153780(gdl);
	}

	return gdl;
}
