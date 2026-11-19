// On-screen additions for network games: the lobby's player list and chat window,
// chat while playing, the score and timer bar, the kill feed and the hold-Tab scoreboard.
//
// Everything is drawn after the game and its menus, in the console's 320-wide space.
//
// Keys: T types a chat line (Enter sends, Esc cancels), R marks you ready in the lobby,
// Tab held shows the scoreboard during a match.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ultra64.h>
#include "constants.h"
#include "bss.h"
#include "data.h"
#include "types.h"
#include "game/game_1531a0.h"
#include "game/hudmsg.h"
#include "game/lv.h"
#include "game/mplayer/mplayer.h"
#include "lib/vi.h"
#include "net/net.h"
#include "net/netbuf.h"
#include "net/netmsg.h"
#include "net/netlag.h"
#include "net/nethud.h"
#include "net/netjoin.h"
#include "console.h"
#include "input.h"
#include "system.h"

extern u32 g_TeamColours[];
extern s32 g_MpTimeLimit60;
extern s32 g_StageTimeElapsed60;
extern s32 g_MenuKeyboardPlayer;

s32 g_NetHudScoreBar = 1;
s32 g_NetHudKillFeed = 1;

#define VK_TAB_KEY 43
#define VK_T_KEY (VK_A + ('t' - 'a'))
#define VK_R_KEY (VK_A + ('r' - 'a'))

#define COL_TEXT   0xffffffe0
#define COL_DIM    0xa0a0a0c0
#define COL_GOOD   0x40ff40e0
#define COL_WARN   0xffe040e0
#define COL_BAD    0xff4040e0
#define COL_YOU    0xffe060ff
#define COL_BORDER 0x00a0ffa0

#define ROW_H 8
#define PANEL_W 142
#define CHAT_W 112        // the chat window, at the right edge of the screen
#define CHAT_LOBBY_ROWS 5 // its height in the lobby: four lines and the one being typed

/* roster: who is connected */

#define ROSTER_READY  0x01
#define ROSTER_INGAME 0x02
#define ROSTER_HOST   0x04

struct rosterentry {
	u8 used;
	u8 flags;
	u16 ping;
	u8 loss;
	char name[NET_MAX_NAME + 1];
};

static struct rosterentry g_Roster[NET_MAX_CLIENTS];
static f32 g_RosterNextSend = 0.f;

/* chat */

#define CHAT_LINES 7
#define CHAT_COLS 38
#define CHAT_SHOW_SECS 10.f
#define CHAT_INPUT_MAX 100

static char g_ChatLines[CHAT_LINES][CHAT_COLS + 1];
static f32 g_ChatTimes[CHAT_LINES];
static s32 g_ChatNum = 0; // lines in use, oldest first
static char g_ChatInput[CHAT_INPUT_MAX + 1];
static s32 g_ChatInputCol = 0;
static s32 g_ChatTyping = false;

/* kill feed */

#define FEED_LINES 4
#define FEED_SHOW_SECS 6.f

static char g_FeedLines[FEED_LINES][48];
static f32 g_FeedTimes[FEED_LINES];
static s32 g_FeedNum = 0;

static s32 g_ScoreboardHeld = false;
// time (sysGetSeconds) until which the keys that closed the chat are still ignored. Real time,
// not g_NetTick: the tick restarts at 0 with every new session, which left the menus dead
// for as long as the last session had run
static f32 g_ChatBlockUntil = 0.f;

static void nethudCopyName(char *dst, size_t dstlen, const char *src)
{
	snprintf(dst, dstlen, "%s", src ? src : "");
	char *nl = strchr(dst, '\n');
	if (nl) {
		*nl = '\0';
	}
}

static s32 nethudTextWidth(const char *text);

static bool nethudFontReady(void)
{
	return g_CharsHandelGothicXs && g_FontHandelGothicXs;
}

void nethudReset(void)
{
	memset(g_Roster, 0, sizeof(g_Roster));
	g_RosterNextSend = 0.f;
	g_FeedNum = 0;
	g_ScoreboardHeld = false;
}

s32 nethudIsTyping(void)
{
	return g_ChatTyping;
}

// typing a chat line, or only just finished: the Enter that sent it (and an E still held)
// must not reach the game or the menus
s32 nethudBlocksInput(void)
{
	if (!g_NetMode) {
		return false;
	}
	if (g_ChatTyping) {
		return true;
	}
	const f32 now = sysGetSeconds();
	return now < g_ChatBlockUntil && now > g_ChatBlockUntil - 1.f;
}

// in a Combat Simulator match the keyboard's Tab is the scoreboard key and nothing else
s32 nethudTabReserved(void)
{
	return g_NetMode && g_NetLocalClient && g_NetLocalClient->state >= CLSTATE_GAME
		&& g_Vars.normmplayerisrunning && !g_ChatTyping;
}

void nethudChatAdd(const char *text)
{
	if (!text || !text[0]) {
		return;
	}

	// long lines wrap onto more rows: as many words as fit the chat window's width
	char src[256];
	nethudCopyName(src, sizeof(src), text);
	const s32 maxwidth = CHAT_W - 8;
	const size_t len = strlen(src);
	size_t ofs = 0;

	while (ofs < len) {
		char line[CHAT_COLS + 1];
		size_t n = 0;       // characters taken
		size_t lastspace = 0;

		while (ofs + n < len && n < CHAT_COLS) {
			line[n] = src[ofs + n];
			line[n + 1] = '\0';
			if (nethudFontReady() ? nethudTextWidth(line) > maxwidth : n >= 24) {
				break;
			}
			if (src[ofs + n] == ' ') {
				lastspace = n;
			}
			++n;
		}

		if (ofs + n < len && lastspace > 0) {
			n = lastspace; // break at the last space, which is dropped
			line[n] = '\0';
			ofs += n + 1;
		} else {
			if (n == 0) {
				n = 1; // one character wider than the window: take it anyway
			}
			line[n] = '\0';
			ofs += n;
		}

		if (g_ChatNum == CHAT_LINES) {
			memmove(g_ChatLines[0], g_ChatLines[1], sizeof(g_ChatLines[0]) * (CHAT_LINES - 1));
			memmove(&g_ChatTimes[0], &g_ChatTimes[1], sizeof(g_ChatTimes[0]) * (CHAT_LINES - 1));
			--g_ChatNum;
		}
		snprintf(g_ChatLines[g_ChatNum], sizeof(g_ChatLines[0]), "%s", line);
		g_ChatTimes[g_ChatNum] = sysGetSeconds();
		++g_ChatNum;
	}
}

void nethudKill(const struct mpchrconfig *attacker, const struct mpchrconfig *victim)
{
	if (!g_NetMode || !victim) {
		return;
	}

	char a[MAX_PLAYERNAME + 1];
	char v[MAX_PLAYERNAME + 1];
	nethudCopyName(v, sizeof(v), victim->name);

	if (g_FeedNum == FEED_LINES) {
		memmove(g_FeedLines[0], g_FeedLines[1], sizeof(g_FeedLines[0]) * (FEED_LINES - 1));
		memmove(&g_FeedTimes[0], &g_FeedTimes[1], sizeof(g_FeedTimes[0]) * (FEED_LINES - 1));
		--g_FeedNum;
	}

	if (!attacker || attacker == victim) {
		snprintf(g_FeedLines[g_FeedNum], sizeof(g_FeedLines[0]), "%s died", v);
	} else {
		nethudCopyName(a, sizeof(a), attacker->name);
		snprintf(g_FeedLines[g_FeedNum], sizeof(g_FeedLines[0]), "%s killed %s", a, v);
	}

	g_FeedTimes[g_FeedNum] = sysGetSeconds();
	++g_FeedNum;
}

/* messages */

u32 nethudRosterWrite(struct netbuf *dst)
{
	netbufWriteU8(dst, SVC_NETINFO);
	const u32 countpos = dst->wp;
	netbufWriteU8(dst, 0);
	u8 count = 0;
	for (s32 i = 0; i < g_NetMaxClients; ++i) {
		const struct netclient *cl = &g_NetClients[i];
		if (cl->state < CLSTATE_LOBBY) {
			continue;
		}
		u8 flags = 0;
		if (cl->ready) flags |= ROSTER_READY;
		if (cl->state >= CLSTATE_GAME) flags |= ROSTER_INGAME;
		if (cl == g_NetLocalClient) flags |= ROSTER_HOST;
		char name[NET_MAX_NAME + 1];
		nethudCopyName(name, sizeof(name), cl->settings.name);
		netbufWriteU8(dst, cl->id);
		netbufWriteU8(dst, flags);
		netbufWriteU16(dst, cl->ping);
		netbufWriteU8(dst, cl->loss);
		netbufWriteStr(dst, name);
		++count;
	}
	if (!dst->error) {
		dst->data[countpos] = count;
	}
	return dst->error;
}

u32 nethudRosterRead(struct netbuf *src, struct netclient *srccl)
{
	struct rosterentry fresh[NET_MAX_CLIENTS];
	memset(fresh, 0, sizeof(fresh));

	const u8 count = netbufReadU8(src);
	for (u8 n = 0; n < count && !src->error; ++n) {
		const u8 id = netbufReadU8(src);
		const u8 flags = netbufReadU8(src);
		const u16 ping = netbufReadU16(src);
		const u8 loss = netbufReadU8(src);
		const char *name = netbufReadStr(src);
		if (!src->error && id < NET_MAX_CLIENTS) {
			fresh[id].used = 1;
			fresh[id].flags = flags;
			fresh[id].ping = ping;
			fresh[id].loss = loss;
			nethudCopyName(fresh[id].name, sizeof(fresh[id].name), name);
			g_NetClients[id].ping = ping;
			g_NetClients[id].loss = loss;
		}
	}

	if (!src->error) {
		memcpy(g_Roster, fresh, sizeof(g_Roster));
	}

	return src->error;
}

u32 nethudReadyWrite(struct netbuf *dst, s32 ready)
{
	netbufWriteU8(dst, CLC_READY);
	netbufWriteU8(dst, ready ? 1 : 0);
	return dst->error;
}

u32 nethudReadyRead(struct netbuf *src, struct netclient *srccl)
{
	const u8 ready = netbufReadU8(src);
	if (!src->error) {
		srccl->ready = ready ? 1 : 0;
		g_RosterNextSend = 0.f; // show it right away
	}
	return src->error;
}

// host: build our own copy of the roster and send it to everyone, about once a second
static void nethudServerRoster(void)
{
	const f32 now = sysGetSeconds();
	if (now < g_RosterNextSend && now > g_RosterNextSend - 5.f) {
		return;
	}
	g_RosterNextSend = now + 1.f;

	memset(g_Roster, 0, sizeof(g_Roster));
	for (s32 i = 0; i < g_NetMaxClients && i < NET_MAX_CLIENTS; ++i) {
		struct netclient *cl = &g_NetClients[i];
		if (cl->state < CLSTATE_LOBBY) {
			cl->ready = 0;
			continue;
		}
		if (cl->state >= CLSTATE_GAME) {
			cl->ready = 0; // the mark is for the next match
		}
		struct rosterentry *e = &g_Roster[i];
		e->used = 1;
		e->flags = (cl->ready ? ROSTER_READY : 0) | (cl->state >= CLSTATE_GAME ? ROSTER_INGAME : 0) | (cl == g_NetLocalClient ? ROSTER_HOST : 0);
		e->ping = cl->ping;
		e->loss = cl->loss;
		nethudCopyName(e->name, sizeof(e->name), cl->settings.name);
	}

	u8 data[64 + NET_MAX_CLIENTS * (NET_MAX_NAME + 8)];
	struct netbuf buf = { NULL };
	buf.data = data;
	buf.size = sizeof(data);
	if (nethudRosterWrite(&buf) == 0) {
		netSend(NULL, &buf, false, NETCHAN_CONTROL);
	}
}

/* keys */

static void nethudStopTyping(void)
{
	g_ChatBlockUntil = sysGetSeconds() + 0.2f;
	g_ChatTyping = false;
	g_ChatInput[0] = '\0';
	g_ChatInputCol = 0;
	inputStopTextInput();
	inputClearLastKey();
}

void nethudTick(void)
{
	if (!g_NetMode || !g_NetLocalClient || g_NetLocalClient->state < CLSTATE_LOBBY) {
		if (g_ChatTyping) {
			nethudStopTyping();
		}
		g_ChatNum = 0;
		g_ScoreboardHeld = false;
		return;
	}

	// our name or character may have been changed in a menu since we connected
	static f32 nextsettings = 0.f;
	const f32 nowsecs = sysGetSeconds();
	if (g_NetLocalClient->state == CLSTATE_LOBBY && (nowsecs >= nextsettings || nowsecs < nextsettings - 5.f)) {
		nextsettings = nowsecs + 1.f;
		netLobbyRefreshSettings();
	}

	if (g_NetMode == NETMODE_SERVER) {
		nethudServerRoster();
	}

	if (conIsOpen()) {
		return;
	}

	if (g_ChatTyping) {
		const s32 key = inputGetLastKey();
		if (key == VK_RETURN) {
			if (g_ChatInput[0]) {
				netChat(NULL, g_ChatInput);
			}
			nethudStopTyping();
		} else if (inputTextHandler(g_ChatInput, CHAT_INPUT_MAX, &g_ChatInputCol, false) < 0) {
			nethudStopTyping();
		}
		g_ScoreboardHeld = false;
		return;
	}

	// a name or setup title is being typed in a menu: T and R are letters there
	const bool menutyping = (g_MenuKeyboardPlayer >= 0);

	if (!menutyping && inputKeyJustPressed(VK_T_KEY)) {
		g_ChatTyping = true;
		g_ChatInput[0] = '\0';
		g_ChatInputCol = 0;
		g_MenuKeyboardPlayer = -1;
		inputClearLastKey();
		inputClearLastTextChar();
		inputStartTextInput();
		return;
	}

	const bool inlobby = (g_NetLocalClient->state == CLSTATE_LOBBY);

	if (inlobby && !menutyping && g_NetMode == NETMODE_CLIENT && inputKeyJustPressed(VK_R_KEY)) {
		const s32 id = g_NetLocalClient->id;
		const s32 ready = (id >= 0 && id < NET_MAX_CLIENTS) ? !(g_Roster[id].flags & ROSTER_READY) : 1;
		u8 data[8];
		struct netbuf buf = { NULL };
		buf.data = data;
		buf.size = sizeof(data);
		nethudReadyWrite(&buf, ready);
		netSend(g_NetLocalClient, &buf, true, NETCHAN_CONTROL);
		if (id >= 0 && id < NET_MAX_CLIENTS) {
			// show it at once; the host's next roster confirms it
			g_Roster[id].flags = (g_Roster[id].flags & ~ROSTER_READY) | (ready ? ROSTER_READY : 0);
		}
	}

	g_ScoreboardHeld = !inlobby && inputKeyPressed(VK_TAB_KEY);
}

/* drawing */

static Gfx *nethudText(Gfx *gdl, s32 x, s32 y, const char *text, u32 colour)
{
	char buf[160];
	snprintf(buf, sizeof(buf), "%s", text);
	return textRenderProjected(gdl, &x, &y, buf, g_CharsHandelGothicXs, g_FontHandelGothicXs, colour, viGetWidth(), viGetHeight(), 0, 0);
}

static s32 nethudTextWidth(const char *text)
{
	s32 textheight, textwidth;
	char buf[160];
	snprintf(buf, sizeof(buf), "%s", text);
	textMeasure(&textheight, &textwidth, buf, g_CharsHandelGothicXs, g_FontHandelGothicXs, 0);
	return textwidth;
}

static u32 nethudPingColour(u32 ping, u32 loss)
{
	if (ping >= 220 || loss >= 5) {
		return COL_BAD;
	} else if (ping >= 120 || loss >= 1) {
		return COL_WARN;
	}
	return COL_GOOD;
}

static Gfx *nethudRenderChat(Gfx *gdl, s32 x, s32 bottom, bool always)
{
	const f32 now = sysGetSeconds();
	s32 first = 0;

	if (!always && !g_ChatTyping) {
		// while playing, only what was said lately
		while (first < g_ChatNum && now - g_ChatTimes[first] > CHAT_SHOW_SECS) {
			++first;
		}
	}

	const s32 rows = (g_ChatNum - first) + (g_ChatTyping ? 1 : 0);
	if (rows <= 0 && !always) {
		return gdl;
	}

	// in a match at most as many lines as the lobby window holds, so it stays clear of
	// the radar above and the ammo display below
	s32 boxrows = always ? CHAT_LOBBY_ROWS : rows;
	if (boxrows > CHAT_LOBBY_ROWS) {
		boxrows = CHAT_LOBBY_ROWS;
	}
	const s32 top = bottom - boxrows * ROW_H - 4;

	if (always || g_ChatTyping) {
		gdl = hudmsgRenderBox(gdl, x, top, x + CHAT_W, bottom, 1.f, COL_BORDER, 0.7f);
	}

	s32 y = bottom - 2 - ROW_H;

	if (g_ChatTyping) {
		char line[CHAT_INPUT_MAX + 8];
		// show the end of a long line: the font is proportional, so measure it
		s32 skip = 0;
		do {
			snprintf(line, sizeof(line), "> %s_", g_ChatInput + skip);
		} while (nethudTextWidth(line) > CHAT_W - 8 && g_ChatInput[skip++]);
		gdl = nethudText(gdl, x + 3, y, line, COL_YOU);
		y -= ROW_H;
	} else if (always) {
		gdl = nethudText(gdl, x + 3, y, "T: chat", COL_DIM);
		y -= ROW_H;
	}

	for (s32 i = g_ChatNum - 1; i >= first && y >= top + 2; --i) {
		gdl = nethudText(gdl, x + 3, y, g_ChatLines[i], COL_TEXT);
		y -= ROW_H;
	}

	return gdl;
}

static Gfx *nethudRenderLobby(Gfx *gdl)
{
	s32 count = 0;
	for (s32 i = 0; i < NET_MAX_CLIENTS; ++i) {
		count += g_Roster[i].used;
	}

	const s32 x = 4;
	s32 y = 4;
	const s32 w = PANEL_W;

	gdl = hudmsgRenderBox(gdl, x, y, x + w, y + (count + 2) * ROW_H + 6, 1.f, COL_BORDER, 0.7f);

	char line[96];
	snprintf(line, sizeof(line), "PLAYERS %d/%d", count, g_NetMaxClients);
	gdl = nethudText(gdl, x + 3, y + 2, line, COL_TEXT);
	y += ROW_H + 3;

	for (s32 i = 0; i < NET_MAX_CLIENTS; ++i) {
		const struct rosterentry *e = &g_Roster[i];
		if (!e->used) {
			continue;
		}

		const bool me = (g_NetLocalClient && (s32)g_NetLocalClient->id == i);
		gdl = nethudText(gdl, x + 3, y, e->name[0] ? e->name : "Player", me ? COL_YOU : COL_TEXT);

		const char *mark;
		u32 colour;
		if (e->flags & ROSTER_HOST) {
			mark = "HOST";
			colour = COL_TEXT;
		} else if (e->flags & ROSTER_INGAME) {
			mark = "IN MATCH";
			colour = COL_DIM;
		} else if (e->flags & ROSTER_READY) {
			mark = "READY";
			colour = COL_GOOD;
		} else {
			mark = "NOT READY";
			colour = COL_DIM;
		}
		gdl = nethudText(gdl, x + 64, y, mark, colour);

		if (!(e->flags & ROSTER_HOST) && e->ping) {
			snprintf(line, sizeof(line), "%u", e->ping);
			gdl = nethudText(gdl, x + w - 3 - nethudTextWidth(line), y, line, nethudPingColour(e->ping, e->loss));
		}

		y += ROW_H;
	}

	gdl = nethudText(gdl, x + 3, y + 1, g_NetMode == NETMODE_CLIENT ? "R: ready / not ready" : "Start when all are ready", COL_DIM);

	// chat window, bottom left
	// chat window, bottom right
	gSPExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT, G_ASPECT_RIGHT_EXT);
	gdl = nethudRenderChat(gdl, SCREEN_WIDTH_LO - 4 - CHAT_W, SCREEN_HEIGHT_LO - 6, true);

	return gdl;
}

// the player rankings without the empty player slots kept for late joiners
static s32 nethudPlayerRankings(struct ranking *rankings)
{
	const s32 num = mpGetPlayerRankings(rankings);
	s32 out = 0;
	for (s32 i = 0; i < num; ++i) {
		if (!netjoinIsVacantConfig(rankings[i].mpchr)) {
			if (out != i) {
				rankings[out] = rankings[i];
			}
			++out;
		}
	}
	return out;
}

static const struct mpchrconfig *nethudMyMpChr(void)
{
	if (g_Vars.players[0] && g_Vars.playerstats[0].mpindex >= 0) {
		return MPCHR(g_Vars.playerstats[0].mpindex);
	}
	return NULL;
}

static void nethudTimeLeft(char *out, size_t outlen)
{
	out[0] = '\0';
	if (g_MpTimeLimit60 > 0) {
		s32 left = (TICKS(g_MpTimeLimit60) - g_StageTimeElapsed60) / TICKS(60);
		if (left < 0) {
			left = 0;
		}
		snprintf(out, outlen, "%d:%02d", left / 60, left % 60);
	}
}

// the top of the screen: how the match stands and how long is left
static Gfx *nethudRenderScoreBar(Gfx *gdl)
{
	struct ranking rankings[MAX_MPCHRS];
	char left[48] = "";
	char right[48] = "";
	char mid[24] = "";
	u32 leftcol = COL_TEXT;
	u32 rightcol = COL_TEXT;
	bool close = false;

	nethudTimeLeft(mid, sizeof(mid));

	if (g_MpSetup.options & MPOPTION_TEAMSENABLED) {
		const s32 num = mpGetTeamRankings(rankings);
		if (num <= 0) {
			return gdl;
		}
		const s32 limit = g_MpSetup.teamscorelimit < 400 ? g_MpSetup.teamscorelimit + 1 : 0;
		const struct mpchrconfig *me = nethudMyMpChr();
		// leading team on the left; on the right ours, or the runner-up if we lead
		s32 other = num > 1 ? 1 : -1;
		for (s32 i = 1; i < num; ++i) {
			if (me && rankings[i].teamnum == me->team) {
				other = i;
			}
		}
		char name[16];
		nethudCopyName(name, sizeof(name), g_BossFile.teamnames[rankings[0].teamnum % MAX_TEAMS]);
		snprintf(left, sizeof(left), "%s %d", name, rankings[0].score);
		leftcol = (g_TeamColours[rankings[0].teamnum % MAX_TEAMS] & 0xffffff00) | 0xff;
		if (other > 0) {
			nethudCopyName(name, sizeof(name), g_BossFile.teamnames[rankings[other].teamnum % MAX_TEAMS]);
			snprintf(right, sizeof(right), "%s %d", name, rankings[other].score);
			rightcol = (g_TeamColours[rankings[other].teamnum % MAX_TEAMS] & 0xffffff00) | 0xff;
		}
		if (limit) {
			char tmp[24];
			snprintf(tmp, sizeof(tmp), mid[0] ? "%s  to %d" : "%sto %d", mid, limit);
			snprintf(mid, sizeof(mid), "%s", tmp);
			close = rankings[0].score >= limit - 2;
		}
	} else {
		const s32 num = nethudPlayerRankings(rankings);
		if (num <= 0) {
			return gdl;
		}
		const s32 limit = g_MpSetup.scorelimit < 100 ? g_MpSetup.scorelimit + 1 : 0;
		const struct mpchrconfig *me = nethudMyMpChr();
		s32 mine = -1;
		for (s32 i = 0; i < num; ++i) {
			if (me && rankings[i].mpchr == me) {
				mine = i;
			}
		}
		char name[MAX_PLAYERNAME + 1];
		// the leader on the left; on the right you, or the runner-up if you lead
		nethudCopyName(name, sizeof(name), rankings[0].mpchr->name);
		snprintf(left, sizeof(left), "1st %s %d", name, rankings[0].score);
		leftcol = mine == 0 ? COL_YOU : COL_TEXT;
		const s32 other = mine > 0 ? mine : (num > 1 ? 1 : -1);
		if (other > 0) {
			nethudCopyName(name, sizeof(name), rankings[other].mpchr->name);
			static const char *places[] = { "1st", "2nd", "3rd" };
			char place[8];
			if (other < 3) {
				snprintf(place, sizeof(place), "%s", places[other]);
			} else {
				snprintf(place, sizeof(place), "%dth", other + 1);
			}
			snprintf(right, sizeof(right), "%s %s %d", place, name, rankings[other].score);
			rightcol = other == mine ? COL_YOU : COL_TEXT;
		}
		if (limit) {
			char tmp[24];
			snprintf(tmp, sizeof(tmp), mid[0] ? "%s  to %d" : "%sto %d", mid, limit);
			snprintf(mid, sizeof(mid), "%s", tmp);
			close = rankings[0].score >= limit - 2;
		}
	}

	// the last minute counts as close too
	if (g_MpTimeLimit60 > 0 && TICKS(g_MpTimeLimit60) - g_StageTimeElapsed60 < TICKS(3600)) {
		close = true;
	}

	const s32 wl = nethudTextWidth(left);
	const s32 wm = mid[0] ? nethudTextWidth(mid) : 0;
	const s32 wr = right[0] ? nethudTextWidth(right) : 0;
	const s32 gap = 10;
	const s32 total = wl + (wm ? gap + wm : 0) + (wr ? gap + wr : 0);
	s32 x = SCREEN_WIDTH_LO / 2 - total / 2;
	const s32 y = 3;

	gdl = hudmsgRenderBox(gdl, x - 5, y - 2, x + total + 5, y + ROW_H + 2, 1.f, close ? 0xff6040c0 : COL_BORDER, 0.6f);

	gdl = nethudText(gdl, x, y, left, leftcol);
	x += wl + gap;
	if (wm) {
		gdl = nethudText(gdl, x, y, mid, close ? COL_WARN : COL_DIM);
		x += wm + gap;
	}
	if (wr) {
		gdl = nethudText(gdl, x, y, right, rightcol);
	}

	return gdl;
}

static Gfx *nethudRenderFeed(Gfx *gdl)
{
	const f32 now = sysGetSeconds();
	s32 y = 16;
	for (s32 i = 0; i < g_FeedNum; ++i) {
		if (now - g_FeedTimes[i] > FEED_SHOW_SECS) {
			continue;
		}
		gdl = nethudText(gdl, 4, y, g_FeedLines[i], COL_TEXT);
		y += ROW_H;
	}
	return gdl;
}

static const struct netclient *nethudClientForMpChr(const struct mpchrconfig *mpchr)
{
	for (s32 i = 0; i < g_NetMaxClients; ++i) {
		const struct netclient *cl = &g_NetClients[i];
		if (cl->state >= CLSTATE_GAME && cl->config && &cl->config->base == mpchr) {
			return cl;
		}
	}
	return NULL;
}

static Gfx *nethudRenderScoreboard(Gfx *gdl)
{
	struct ranking rankings[MAX_MPCHRS];
	const s32 num = nethudPlayerRankings(rankings);
	if (num <= 0) {
		return gdl;
	}

	const bool teams = (g_MpSetup.options & MPOPTION_TEAMSENABLED) != 0;
	const s32 w = 200;
	const s32 x = SCREEN_WIDTH_LO / 2 - w / 2;
	s32 y = 28;
	const struct mpchrconfig *me = nethudMyMpChr();

	gdl = hudmsgRenderBox(gdl, x, y, x + w, y + (num + 1) * ROW_H + 8, 1.f, COL_BORDER, 0.85f);
	y += 3;

	gdl = nethudText(gdl, x + 4, y, "PLAYER", COL_DIM);
	gdl = nethudText(gdl, x + 104, y, "SCORE", COL_DIM);
	gdl = nethudText(gdl, x + 136, y, "DEATHS", COL_DIM);
	gdl = nethudText(gdl, x + 174, y, "PING", COL_DIM);
	y += ROW_H + 2;

	for (s32 i = 0; i < num; ++i) {
		const struct mpchrconfig *mpchr = rankings[i].mpchr;
		char name[MAX_PLAYERNAME + 1];
		char tmp[16];
		nethudCopyName(name, sizeof(name), mpchr->name);

		u32 colour = mpchr == me ? COL_YOU : COL_TEXT;
		if (teams && mpchr != me) {
			colour = (g_TeamColours[mpchr->team % MAX_TEAMS] & 0xffffff00) | 0xe0;
		}

		gdl = nethudText(gdl, x + 4, y, name, colour);
		snprintf(tmp, sizeof(tmp), "%d", rankings[i].score);
		gdl = nethudText(gdl, x + 104, y, tmp, COL_TEXT);
		snprintf(tmp, sizeof(tmp), "%d", mpchr->numdeaths);
		gdl = nethudText(gdl, x + 136, y, tmp, COL_TEXT);

		const struct netclient *cl = nethudClientForMpChr(mpchr);
		if (cl && cl->id != 0) {
			snprintf(tmp, sizeof(tmp), "%u", cl->ping);
			gdl = nethudText(gdl, x + 174, y, tmp, nethudPingColour(cl->ping, cl->loss));
		} else if (cl) {
			gdl = nethudText(gdl, x + 174, y, "host", COL_DIM);
		}

		y += ROW_H;
	}

	return gdl;
}

Gfx *nethudRender(Gfx *gdl)
{
	if (!g_NetMode || !g_NetLocalClient || g_NetLocalClient->state < CLSTATE_LOBBY) {
		return gdl;
	}

	if (!g_CharsHandelGothicXs || !g_FontHandelGothicXs || conIsOpen()) {
		return gdl;
	}

	gdl = text0f153628(gdl);

	if (g_NetLocalClient->state == CLSTATE_LOBBY) {
		gSPExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT, G_ASPECT_LEFT_EXT);
		gdl = nethudRenderLobby(gdl);
	} else {
		const bool match = g_Vars.normmplayerisrunning && g_Vars.players[0];

		// chat at the right edge, between the radar and the ammo display
		gSPExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT, G_ASPECT_RIGHT_EXT);
		gdl = nethudRenderChat(gdl, SCREEN_WIDTH_LO - 4 - CHAT_W, 100, false);
		gSPExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT, G_ASPECT_LEFT_EXT);

		if (match && g_NetHudKillFeed) {
			// (top left: the radar has the top right)
			gdl = nethudRenderFeed(gdl);
		}

		if (match && (g_NetHudScoreBar || g_ScoreboardHeld)) {
			gSPExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT, G_ASPECT_CENTER_EXT);
			if (g_NetHudScoreBar) {
				gdl = nethudRenderScoreBar(gdl);
			}
			if (g_ScoreboardHeld) {
				gdl = nethudRenderScoreboard(gdl);
			}
		}
	}

	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT);
	gdl = text0f153780(gdl);

	return gdl;
}
