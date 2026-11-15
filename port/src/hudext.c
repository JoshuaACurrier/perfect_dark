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
#include "game/options.h"
#include "lib/collision.h"
#include "lib/joy.h"
#include "lib/mtx.h"
#include "lib/snd.h"
#include "lib/vi.h"
#include "net/net.h"
#include "net/netmsg.h"
#include "net/nethud.h"
#include "hudext.h"
#include "game/propsnd.h"
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
	// the same reach as the crosshair's own ticks (which run 1..3 x its size from the
	// centre); a diagonal looks longer than a straight tick, so stop a little short
	const s32 size = PLAYER_EXTCFG().crosshairsize > 0 ? (s32)PLAYER_EXTCFG().crosshairsize : 1;
	const s32 inner = size;
	const s32 outer = size * 2 + (pl->hitmarkkill ? 1 : 0);

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

		if (g_NetAutoDebugHud) {
			// for screenshots: what the sound muffling makes of this character
			const f32 occ = psGetOcclusion(&prop->pos, prop->rooms);
			snprintf(name, sizeof(name), "%s", occ < 0.f ? "?" : (occ == 0.f ? "CLEAR" : (occ < 1.f ? "HALF" : "HIDDEN")));
		}

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

/* watching someone else while dead (network games) */

#define SPEC_DIST   140.f
#define SPEC_HEIGHT 40.f

s32 g_MpSpectate = 1;

static bool g_SpecActive = false;
static s32 g_SpecTarget = -1; // mp chr number (players first, then simulants)
static f32 g_SpecAngle = 0.f;
static bool g_SpecSnap = true;

static bool hudextSpecIsCoop(void)
{
	return g_Vars.coopplayernum >= 0 || g_Vars.antiplayernum >= 0;
}

static s32 hudextSpecCount(void)
{
	return hudextSpecIsCoop() ? PLAYERCOUNT() : g_MpNumChrs;
}

static struct prop *hudextSpecProp(s32 i)
{
	if (i < 0 || i >= hudextSpecCount() || i == g_Vars.currentplayernum) {
		return NULL;
	}

	if (i < PLAYERCOUNT()) {
		const struct player *pl = g_Vars.players[i];
		if (!pl || !pl->prop || !pl->prop->chr || pl->isdead) {
			return NULL;
		}
		return pl->prop;
	}

	return hudextChrShowable(i) ? g_MpAllChrPtrs[i]->prop : NULL;
}

static bool hudextSpecSameTeam(s32 i)
{
	const s32 me = g_Vars.currentplayernum;
	if (hudextSpecIsCoop() || !(g_MpSetup.options & MPOPTION_TEAMSENABLED)) {
		return true;
	}
	if (me >= g_MpNumChrs || i >= g_MpNumChrs || !g_MpAllChrConfigPtrs[me] || !g_MpAllChrConfigPtrs[i]) {
		return false;
	}
	return g_MpAllChrConfigPtrs[me]->team == g_MpAllChrConfigPtrs[i]->team;
}

// the next character worth watching after `from`: teammates first, then anyone
static s32 hudextSpecNext(s32 from)
{
	const s32 count = hudextSpecCount();

	for (s32 pass = 0; pass < 2; ++pass) {
		for (s32 n = 1; n <= count; ++n) {
			const s32 i = (((from < 0 ? -1 : from) + n) % count + count) % count;
			if (hudextSpecProp(i) && (pass == 1 || hudextSpecSameTeam(i))) {
				return i;
			}
		}
	}

	return -1;
}

static const char *hudextSpecName(s32 i)
{
	if (hudextSpecIsCoop()) {
		return (i >= 0 && i < MAX_PLAYERS) ? g_PlayerConfigsArray[i].base.name : "";
	}
	return (i >= 0 && i < g_MpNumChrs && g_MpAllChrConfigPtrs[i]) ? g_MpAllChrConfigPtrs[i]->name : "";
}

void hudextSpectateCamera(void)
{
	struct player *pl = g_Vars.currentplayer;

	// (this runs for every player in the match; the state below is the local one's)
	if (!g_NetMode || pl->isremote) {
		return;
	}

	const bool want = g_MpSpectate && g_Vars.mplayerisrunning
		&& pl->isdead && pl->deathanimfinished && pl->prop;

	if (!want) {
		g_SpecActive = false;
		return;
	}

	if (!g_SpecActive) {
		// wait for the death fade to finish, then open the picture again
		if (!playerIsFadeComplete()) {
			return;
		}
		g_SpecActive = true;
		g_SpecSnap = true;
		playerAdjustFade(20, 0, 0, 0, 0);
	}

	bool cycle = false;
	if (!nethudIsTyping() && !mpIsPaused()) {
		cycle = joyGetButtonsPressedThisFrame(optionsGetContpadNum1(g_Vars.currentplayerstats->mpindex), R_TRIG) != 0;
	}

	if (cycle || !hudextSpecProp(g_SpecTarget)) {
		const s32 next = hudextSpecNext(g_SpecTarget);
		if (next != g_SpecTarget) {
			g_SpecTarget = next;
			g_SpecSnap = true;
			sysLogPrintf(LOG_NOTE, "SPEC: player %d now watching %d", g_Vars.currentplayernum, next);
		}
	}

	struct prop *tp = hudextSpecProp(g_SpecTarget);
	if (!tp) {
		return; // nobody to watch: the view stays on our own body
	}

	// a player's prop sits at eye height, anyone else's near the hips
	struct coord head = tp->pos;
	if (tp->type != PROPTYPE_PLAYER) {
		head.y += 65.f;
	}

	// follow behind them, turning a little slower than they do
	const f32 want_angle = chrGetInverseTheta(tp->chr);
	if (g_SpecSnap) {
		g_SpecAngle = want_angle;
		g_SpecSnap = false;
	} else {
		f32 diff = want_angle - g_SpecAngle;
		while (diff > M_PI) diff -= M_BADTAU;
		while (diff < -M_PI) diff += M_BADTAU;
		f32 frac = 0.12f * g_Vars.lvupdate60freal;
		if (frac > 1.f) frac = 1.f;
		g_SpecAngle += diff * frac;
	}

	struct coord cam;
	cam.x = head.x - sinf(g_SpecAngle) * SPEC_DIST;
	cam.y = head.y + SPEC_HEIGHT;
	cam.z = head.z - cosf(g_SpecAngle) * SPEC_DIST;

	// don't put the camera through a wall
	if (cdExamLos08(&head, tp->rooms, &cam, CDTYPE_BG | CDTYPE_CLOSEDDOORS,
			GEOFLAG_FLOOR1 | GEOFLAG_FLOOR2 | GEOFLAG_WALL | GEOFLAG_BLOCK_SIGHT) == CDRESULT_COLLISION) {
		struct coord hit;
		cdGetPos(&hit, 0, "hudext.c");
		cam.x = head.x + (hit.x - head.x) * 0.8f;
		cam.y = head.y + (hit.y - head.y) * 0.8f;
		cam.z = head.z + (hit.z - head.z) * 0.8f;
	}

	struct coord look = { head.x - cam.x, head.y - cam.y + 8.f, head.z - cam.z };
	struct coord up = { 0.f, 1.f, 0.f };

	if (look.x == 0.f && look.z == 0.f) {
		look.z = 1.f;
	}

	player0f0c1840(&cam, &up, &look, &tp->pos, tp->rooms);
}

Gfx *hudextDrawSpectate(Gfx *gdl)
{
	struct player *pl = g_Vars.currentplayer;

	if (!g_SpecActive || pl->isremote || !pl->isdead || !hudextSpecProp(g_SpecTarget)) {
		return gdl;
	}

	if (!g_CharsHandelGothicMd || !g_FontHandelGothicMd || !g_CharsHandelGothicXs || !g_FontHandelGothicXs) {
		return gdl;
	}

	char name[MAX_PLAYERNAME + 1];
	char line[96];
	hudextCopyName(name, sizeof(name), hudextSpecName(g_SpecTarget));
	snprintf(line, sizeof(line), "Watching %s", name);

	const s32 cx = viGetViewLeft() / g_ScaleX + viGetViewWidth() / g_ScaleX / 2;
	// under the score bar, clear of the character in the middle of the picture
	s32 y = viGetViewTop() + 26;

	gdl = text0f153628(gdl);
	gSPSetExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);
	gdl = hudextTextCentred(gdl, line, cx, y, 0xffffffff, g_CharsHandelGothicMd, g_FontHandelGothicMd);
	gdl = hudextTextCentred(gdl, "Aim button: next player", cx, y + 16, 0xc0c0c0ff, g_CharsHandelGothicXs, g_FontHandelGothicXs);
	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);
	gdl = text0f153780(gdl);

	return gdl;
}
