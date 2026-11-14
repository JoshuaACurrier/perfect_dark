// Chr (guard / simulant) synchronization.
//
// The server runs all AI. It sends a compact snapshot of each chr that changed
// (SVC_CHR_MOVE): position, facing, animation, aim, held weapons and gunfire.
// Clients don't run AI for chrs at all; instead each chr becomes a "puppet".
// Shots are sent separately (SVC_CHR_SHOT) so clients can show tracers, sparks
// and play sounds. Damage and disarming already go through SVC_CHR_DAMAGE /
// SVC_CHR_DISARM. Guards letting go of things (death, disarm) and being removed
// are SVC_CHR_DROP / SVC_CHR_REMOVE (netmsg.c), since clients never run the code
// that does that.
//
// Timing: snapshots carry the server's clock in 60 Hz ticks of real time (not
// frames: either end may run at any frame rate). The client keeps a short
// history per chr and shows each one a little in the past, interpolating between
// the two snapshots around that moment. How far in the past adapts to how
// unevenly packets arrive, so movement stays smooth over the internet instead of
// guessing ahead and snapping back.

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "platform.h"
#include "types.h"
#include "constants.h"
#include "data.h"
#include "bss.h"
#include "game/chr.h"
#include "game/chraction.h"
#include "game/bot.h"
#include "game/bondgun.h"
#include "game/playermgr.h"
#include "game/prop.h"
#include "game/propobj.h"
#include "game/sparks.h"
#include "game/footstep.h"
#include "lib/model.h"
#include "lib/anim.h"
#include "system.h"
#include "net/net.h"
#include "net/netbuf.h"
#include "net/netmsg.h"
#include "net/netchr.h"

// snapshot flags
#define NCF_DEAD      (1 << 0)
#define NCF_HIDDEN    (1 << 1)
#define NCF_FLIP      (1 << 2)
#define NCF_FIRE_R    (1 << 3)
#define NCF_FIRE_L    (1 << 4)
#define NCF_AIM       (1 << 5)
#define NCF_LOOPING   (1 << 6)
#define NCF_ENDFRAME  (1 << 7)

// two snapshots further apart than this (world units) are a teleport, not movement
#define NETCHR_SNAP_DIST 250.f
// how far (in 60 Hz ticks) we're willing to extrapolate past the newest snapshot
#define NETCHR_MAX_EXTRAP 6.f
// if the anim frame is further off than this, jump to the right frame
#define NETCHR_FRAME_TOLERANCE 6.f
// snapshots kept per chr (at 60 per second this is half a second)
#define NETCHR_HIST 32
// limits for the display delay, in 60 Hz ticks
#define NETCHR_DELAY_MIN 1.5f
#define NETCHR_DELAY_MAX 15.f

#define ANGLE_TO_U16(a) ((u16)((s32)((a) * (65536.f / M_BADTAU)) & 0xffff))
#define U16_TO_ANGLE(v) ((f32)(v) * (M_BADTAU / 65536.f))

struct netchrsnap {
	f32 t;              // server time, 60 Hz ticks
	struct coord pos;
	f32 manground;
	f32 rot;            // model facing
	f32 look;           // look angle (simulants aim separately from their body)
	RoomNum rooms[8];
	f32 frame;
	f32 speed;
	f32 loopframe;
	f32 endframe;
	s16 animnum;
	s8 actiontype;
	u8 flags;
};

struct netchr {
	// client: snapshot history, newest at hist[head]
	struct netchrsnap hist[NETCHR_HIST];
	u8 head;
	u8 count;
	f32 rxtime;         // local time when the newest snapshot arrived, 0 if none yet
	struct coord disp;  // position we're displaying
	f32 dispground;
	f32 aimend[4];      // these follow the newest snapshot
	s8 weapons[2];
	u8 latestflags;
	u8 life;
	u8 locallife;       // last life number we applied
	u8 applied;         // the puppet has been placed at least once
	u8 removeok;        // the server said to remove this chr
	u8 removewarned;

	// server: what we last sent
	struct netchrsnap sent;
	struct coord sentprevpos;
	f32 sentaim[4];
	s8 sentweapons[2];
	u8 sentany;
};

// per chr slot state
static struct netchr *g_NetChrs = NULL;
static s32 g_NetChrsNum = 0;

// server side: life counters (incremented every time a simulant respawns)
static u8 *g_NetChrLives = NULL;
static u32 g_NetChrSendCursor = 0;
static f32 g_NetChrLastSend = -1000.f;

// both: our clock for this stage
static u64 g_NetChrEpoch = 0;

// client side: our estimate of the server clock and of the delay to show chrs at
static s32 g_NetChrSynced = false;
static f32 g_NetChrOffset;      // server time - local time, for the fastest packets
static f32 g_NetChrJitter;      // how much later than the fastest packets they tend to come (decaying peak)
static f32 g_NetChrInterval;    // time between snapshot packets
static f32 g_NetChrLastSrvTime;
static f32 g_NetChrLastRx;
static f32 g_NetChrRenderOfs;   // offset - delay, smoothed
static f32 g_NetChrRenderTime;  // server time we show chrs at this frame
static u32 g_NetChrRenderTick;  // g_NetTick g_NetChrRenderTime was worked out for
static f32 g_NetChrNextLog;

static u8 g_NetChrBufData[NET_BUFSIZE];
static struct netbuf g_NetChrBuf = { .data = g_NetChrBufData, .size = sizeof(g_NetChrBufData) };

static inline f32 netchrNow(void)
{
	return (f32)((f64)(sysGetMicroseconds() - g_NetChrEpoch) * (60.0 / 1000000.0));
}

static inline struct netchr *netchrFor(struct chrdata *chr)
{
	const s32 idx = chr - g_ChrSlots;
	if (!g_NetChrs || idx < 0 || idx >= g_NetChrsNum) {
		return NULL;
	}
	return &g_NetChrs[idx];
}

void netchrReset(void)
{
	free(g_NetChrs);
	free(g_NetChrLives);
	g_NetChrs = NULL;
	g_NetChrLives = NULL;
	g_NetChrsNum = 0;
	g_NetChrSendCursor = 0;
	g_NetChrLastSend = -1000.f;
	g_NetChrEpoch = sysGetMicroseconds();
	g_NetChrSynced = false;
	g_NetChrRenderTick = 0;
	g_NetChrNextLog = 0.f;

	if (g_NumChrSlots > 0) {
		g_NetChrsNum = g_NumChrSlots;
		g_NetChrs = calloc(g_NetChrsNum, sizeof(*g_NetChrs));
		g_NetChrLives = calloc(g_NetChrsNum, 1);
	}
}

void netchrAllowRemove(struct chrdata *chr)
{
	struct netchr *nc = netchrFor(chr);
	if (nc) {
		nc->removeok = true;
	}
}

s32 netchrClientMayRemove(struct chrdata *chr)
{
	if (g_NetMode != NETMODE_CLIENT || !chr->prop || chr->prop->type != PROPTYPE_CHR || chr->aibot || !chr->prop->syncid) {
		return true;
	}
	struct netchr *nc = netchrFor(chr);
	if (!nc || nc->removeok) {
		return true;
	}
	if (!nc->removewarned) {
		nc->removewarned = true;
		sysLogPrintf(LOG_NOTE, "NET: kept chr %d (syncid %u) that would have been removed locally", chr->chrnum, chr->prop->syncid);
	}
	return false;
}

void netchrOnClientSpawn(struct chrdata *chr)
{
	struct netchr *nc = netchrFor(chr);
	if (nc) {
		// forget whatever chr had this slot before; wait for the first snapshot
		memset(nc, 0, sizeof(*nc));
	}
}

void netchrOnRespawn(struct chrdata *chr)
{
	if (g_NetMode != NETMODE_SERVER || !g_NetChrLives) {
		return;
	}
	const s32 idx = chr - g_ChrSlots;
	if (idx >= 0 && idx < g_NetChrsNum) {
		++g_NetChrLives[idx];
	}
}

static inline struct prop *netchrFindProp(u16 syncid)
{
	if (!syncid) {
		return NULL;
	}
	// props that exist from the start of the level have syncid == index + 1
	if (syncid <= g_Vars.maxprops && g_Vars.props[syncid - 1].syncid == syncid) {
		return &g_Vars.props[syncid - 1];
	}
	for (s32 i = 0; i < g_Vars.maxprops; ++i) {
		if (g_Vars.props[i].syncid == syncid) {
			return &g_Vars.props[i];
		}
	}
	return NULL;
}

static inline s8 netchrHeldWeaponNum(struct chrdata *chr, s32 hand)
{
	struct prop *prop = chr->weapons_held[hand];
	if (prop && prop->type == PROPTYPE_WEAPON && prop->weapon) {
		return prop->weapon->weaponnum;
	}
	return -1;
}

// frame numbers from the network get used as loop bounds in the anim code,
// so anything out of range (or NaN) would hang the game
static inline f32 netchrClampFrame(f32 v, s32 numframes)
{
	const f32 max = (numframes > 1) ? (f32)(numframes - 1) : 0.f;
	if (!(v >= 0.f)) {
		return 0.f;
	}
	return (v > max) ? max : v;
}

static inline s16 quantAim(f32 v)
{
	f32 q = v * 8192.f;
	if (q > 32767.f) q = 32767.f;
	if (q < -32768.f) q = -32768.f;
	return (s16)q;
}

static inline f32 netchrAbs(f32 v)
{
	return v < 0.f ? -v : v;
}

static inline f32 netchrClampf(f32 v, f32 lo, f32 hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

/* server -> client: SVC_CHR_MOVE */

static inline u8 netchrFlags(struct chrdata *chr)
{
	struct anim *anim = chr->model->anim;
	u8 flags = 0;
	if (chr->actiontype == ACT_DIE || chr->actiontype == ACT_DEAD) flags |= NCF_DEAD;
	if (chr->chrflags & CHRCFLAG_HIDDEN) flags |= NCF_HIDDEN;
	if (anim->flip) flags |= NCF_FLIP;
	if (chr->weapons_held[HAND_RIGHT] && weaponIsGunfireVisible(chr->weapons_held[HAND_RIGHT])) flags |= NCF_FIRE_R;
	if (chr->weapons_held[HAND_LEFT] && weaponIsGunfireVisible(chr->weapons_held[HAND_LEFT])) flags |= NCF_FIRE_L;
	if (chr->aimendlshoulder != 0.f || chr->aimendrshoulder != 0.f || chr->aimendback != 0.f || chr->aimendsideback != 0.f) {
		flags |= NCF_AIM;
	}
	if (anim->looping) flags |= NCF_LOOPING;
	if (anim->endframe >= 0.f) flags |= NCF_ENDFRAME;
	return flags;
}

// On the server g_NetChrs[].sent holds what we last sent for each chr. Chrs whose
// state hasn't changed in a way the client can't predict are skipped, except for a
// periodic refresh so lost packets get corrected. Missions have dozens of guards
// standing around, so this saves most of the bandwidth.
#define NETCHR_REFRESH_TICKS 30.f

static inline s32 netchrShouldSend(struct chrdata *chr, f32 now)
{
	struct prop *prop = chr->prop;
	if (chr->chrnum < 0 || !prop || !prop->syncid || prop->type != PROPTYPE_CHR || !chr->model || !chr->model->anim) {
		return false;
	}
	if (chr->hidden & CHRHFLAG_DELETING) {
		return false;
	}

	struct netchr *nc = netchrFor(chr);
	if (!nc || !nc->sentany) {
		return true;
	}
	const struct netchrsnap *last = &nc->sent;

	const f32 since = now - last->t;
	if (since >= NETCHR_REFRESH_TICKS) {
		return true;
	}

	// it was moving when we last sent it: send at least once more so the
	// client knows where it stopped
	if (last->pos.x != nc->sentprevpos.x || last->pos.y != nc->sentprevpos.y || last->pos.z != nc->sentprevpos.z) {
		return true;
	}

	struct anim *anim = chr->model->anim;
	const s8 wr = netchrHeldWeaponNum(chr, HAND_RIGHT);
	const s8 wl = netchrHeldWeaponNum(chr, HAND_LEFT);

	if (netchrFlags(chr) != last->flags
			|| chr->actiontype != last->actiontype
			|| anim->animnum != last->animnum
			|| anim->speed != last->speed
			|| wr != nc->sentweapons[HAND_RIGHT] || wl != nc->sentweapons[HAND_LEFT]
			|| netchrAbs(prop->pos.x - last->pos.x) > 0.5f
			|| netchrAbs(prop->pos.y - last->pos.y) > 0.5f
			|| netchrAbs(prop->pos.z - last->pos.z) > 0.5f
			|| netchrAbs(modelGetChrRotY(chr->model) - last->rot) > 0.01f
			|| netchrAbs(chrGetInverseTheta(chr) - last->look) > 0.01f
			|| netchrAbs(chr->aimendlshoulder - nc->sentaim[0]) > 0.01f
			|| netchrAbs(chr->aimendrshoulder - nc->sentaim[1]) > 0.01f
			|| netchrAbs(chr->aimendback - nc->sentaim[2]) > 0.01f
			|| netchrAbs(chr->aimendsideback - nc->sentaim[3]) > 0.01f) {
		return true;
	}

	// the client advances the frame by itself; only resend when it's off (e.g. a loop wrapped)
	const f32 predicted = last->frame + last->speed * since;
	if (netchrAbs(anim->frame - predicted) > 2.f) {
		return true;
	}

	return false;
}

static void netchrRecordSent(struct chrdata *chr, u8 flags, f32 now)
{
	struct netchr *nc = netchrFor(chr);
	if (!nc) {
		return;
	}
	struct netchrsnap *last = &nc->sent;
	struct anim *anim = chr->model->anim;
	nc->sentprevpos = nc->sentany ? last->pos : chr->prop->pos;
	last->pos = chr->prop->pos;
	last->rot = modelGetChrRotY(chr->model);
	last->look = chrGetInverseTheta(chr);
	nc->sentaim[0] = chr->aimendlshoulder;
	nc->sentaim[1] = chr->aimendrshoulder;
	nc->sentaim[2] = chr->aimendback;
	nc->sentaim[3] = chr->aimendsideback;
	last->animnum = anim->animnum;
	last->frame = anim->frame;
	last->speed = anim->speed;
	last->flags = flags;
	last->actiontype = chr->actiontype;
	nc->sentweapons[HAND_RIGHT] = netchrHeldWeaponNum(chr, HAND_RIGHT);
	nc->sentweapons[HAND_LEFT] = netchrHeldWeaponNum(chr, HAND_LEFT);
	last->t = now;
	nc->sentany = true;
}

static u32 netchrWriteOne(struct netbuf *dst, struct chrdata *chr, f32 now)
{
	struct prop *prop = chr->prop;
	struct model *model = chr->model;
	struct anim *anim = model->anim;
	const s32 idx = chr - g_ChrSlots;

	const u8 flags = netchrFlags(chr);

	netbufWriteU16(dst, prop->syncid);
	netbufWriteU8(dst, flags);
	netbufWriteU8(dst, g_NetChrLives ? g_NetChrLives[idx] : 0);
	netbufWriteS8(dst, chr->actiontype);
	netbufWriteCoord(dst, &prop->pos);
	netbufWriteF32(dst, chr->manground);
	netbufWriteU16(dst, ANGLE_TO_U16(modelGetChrRotY(model)));
	netbufWriteU16(dst, ANGLE_TO_U16(chrGetInverseTheta(chr)));
	for (s32 i = 0; i < ARRAYCOUNT(prop->rooms); ++i) {
		netbufWriteS16(dst, prop->rooms[i]);
		if (prop->rooms[i] < 0) {
			break;
		}
	}
	netbufWriteS16(dst, anim->animnum);
	netbufWriteF32(dst, anim->frame);
	netbufWriteF32(dst, anim->speed);
	if (flags & NCF_LOOPING) {
		netbufWriteF32(dst, anim->loopframe);
	}
	if (flags & NCF_ENDFRAME) {
		netbufWriteF32(dst, anim->endframe);
	}
	netbufWriteS8(dst, netchrHeldWeaponNum(chr, HAND_RIGHT));
	netbufWriteS8(dst, netchrHeldWeaponNum(chr, HAND_LEFT));
	if (flags & NCF_AIM) {
		netbufWriteS16(dst, quantAim(chr->aimendlshoulder));
		netbufWriteS16(dst, quantAim(chr->aimendrshoulder));
		netbufWriteS16(dst, quantAim(chr->aimendback));
		netbufWriteS16(dst, quantAim(chr->aimendsideback));
	}

	netchrRecordSent(chr, flags, now);

	return dst->error;
}

void netchrServerSend(void)
{
	if (g_NetMode != NETMODE_SERVER || !g_NetChrs || g_NetLocalClient->state < CLSTATE_GAME) {
		return;
	}

	// at most 60 rounds a second (or fewer with Net.Server.UpdateFrames), however fast the host
	// renders; the clients interpolate between them
	const f32 now = netchrNow();
	const f32 mininterval = (f32)(g_NetServerUpdateRate > 1 ? g_NetServerUpdateRate : 1) - 0.1f;
	if (now - g_NetChrLastSend < mininterval) {
		return;
	}
	g_NetChrLastSend = now;

	// worst case size of one chr entry
	const u32 maxentry = 2 + 1 + 1 + 1 + 12 + 4 + 2 + 2 + 2 * 8 + 2 + 4 + 4 + 4 + 4 + 2 + 8;
	// at most this many packets per round; anything left over goes out next round
	s32 packetsleft = 4;
	s32 checked = 0;

	while (checked < g_NumChrSlots && packetsleft > 0) {
		netbufStartWrite(&g_NetChrBuf);
		netbufWriteU8(&g_NetChrBuf, SVC_CHR_MOVE);
		netbufWriteF32(&g_NetChrBuf, now);
		const u32 countpos = g_NetChrBuf.wp;
		netbufWriteU8(&g_NetChrBuf, 0);
		u8 count = 0;

		while (checked < g_NumChrSlots && count < 255 && netbufWriteLeft(&g_NetChrBuf) > (s32)maxentry) {
			struct chrdata *chr = &g_ChrSlots[g_NetChrSendCursor % g_NumChrSlots];
			g_NetChrSendCursor = (g_NetChrSendCursor + 1) % g_NumChrSlots;
			++checked;
			if (netchrShouldSend(chr, now)) {
				netchrWriteOne(&g_NetChrBuf, chr, now);
				++count;
			}
		}

		if (!count) {
			break;
		}

		g_NetChrBuf.data[countpos] = count;
		netSend(NULL, &g_NetChrBuf, false, NETCHAN_DEFAULT);
		--packetsleft;
	}
}

// client: a snapshot packet stamped srvtime arrived now; update our idea of the server clock
// and of how far behind it to show chrs so that there's (nearly) always a snapshot on each side
static void netchrClientClockSample(f32 srvtime)
{
	const f32 now = netchrNow();
	const f32 sample = srvtime - now;

	if (!g_NetChrSynced) {
		g_NetChrSynced = true;
		g_NetChrOffset = sample;
		g_NetChrJitter = 1.f;
		g_NetChrInterval = 1.f;
		g_NetChrLastSrvTime = srvtime;
		g_NetChrLastRx = now;
		g_NetChrRenderOfs = sample - (NETCHR_DELAY_MIN + 2.f);
		return;
	}

	const f32 elapsed = netchrClampf(now - g_NetChrLastRx, 0.f, 60.f);
	g_NetChrLastRx = now;

	// track the fastest packets; drift down slowly so a clock running at a slightly
	// different rate (or a route that got slower for good) gets picked up again
	g_NetChrOffset -= elapsed * 0.001f;
	if (sample > g_NetChrOffset) {
		g_NetChrOffset = sample;
	}

	// how late the others come: a peak that decays over a few seconds
	const f32 late = g_NetChrOffset - sample;
	g_NetChrJitter -= elapsed * 0.01f;
	if (late > g_NetChrJitter) {
		g_NetChrJitter = late;
	}
	if (g_NetChrJitter < 0.f) {
		g_NetChrJitter = 0.f;
	}

	// gap between snapshot rounds (several packets of one round share a stamp)
	if (srvtime > g_NetChrLastSrvTime) {
		const f32 gap = netchrClampf(srvtime - g_NetChrLastSrvTime, 0.f, 6.f);
		g_NetChrInterval += (gap - g_NetChrInterval) * 0.05f;
		g_NetChrLastSrvTime = srvtime;
	}
}

static f32 netchrDelay(void)
{
	return netchrClampf(g_NetChrInterval + g_NetChrJitter + 0.75f, NETCHR_DELAY_MIN, NETCHR_DELAY_MAX);
}

// the server time chrs are shown at this frame
static f32 netchrRenderTime(void)
{
	if (g_NetChrRenderTick == g_NetTick && g_NetChrRenderTick) {
		return g_NetChrRenderTime;
	}

	const f32 now = netchrNow();
	const f32 target = g_NetChrOffset - netchrDelay();
	const f32 elapsed = (g_NetChrRenderTick && g_NetChrRenderTime != 0.f) ? netchrClampf(now + g_NetChrRenderOfs - g_NetChrRenderTime, 0.f, 10.f) : 0.f;
	const f32 diff = target - g_NetChrRenderOfs;

	if (diff > 30.f || diff < -30.f) {
		// way off (a hitch, or the first frame): jump
		g_NetChrRenderOfs = target;
	} else {
		// otherwise run up to 5% fast or slow until we're there, which nobody notices
		const f32 maxstep = 0.05f * (elapsed > 0.f ? elapsed : 1.f);
		g_NetChrRenderOfs += netchrClampf(diff, -maxstep, maxstep);
	}

	g_NetChrRenderTick = g_NetTick;
	g_NetChrRenderTime = now + g_NetChrRenderOfs;

	if (now >= g_NetChrNextLog) {
		g_NetChrNextLog = now + 60.f * 30.f;
		sysLogPrintf(LOG_NOTE, "NET: chr display delay %.1f ticks (packet spread %.1f, interval %.1f)",
			-(g_NetChrRenderOfs - g_NetChrOffset), g_NetChrJitter, g_NetChrInterval);
	}

	return g_NetChrRenderTime;
}

u32 netmsgSvcChrMoveRead(struct netbuf *src, struct netclient *srccl)
{
	const f32 srvtime = netbufReadF32(src);
	const u8 count = netbufReadU8(src);

	if (!src->error && srccl->state >= CLSTATE_GAME && srvtime == srvtime) {
		netchrClientClockSample(srvtime);
	}

	for (u8 n = 0; n < count && !src->error; ++n) {
		struct netchrsnap snap;
		f32 aimend[4] = { 0.f, 0.f, 0.f, 0.f };
		s8 weapons[2];
		memset(&snap, 0, sizeof(snap));
		snap.t = srvtime;

		const u16 syncid = netbufReadU16(src);
		snap.flags = netbufReadU8(src);
		const u8 life = netbufReadU8(src);
		snap.actiontype = netbufReadS8(src);
		netbufReadCoord(src, &snap.pos);
		snap.manground = netbufReadF32(src);
		snap.rot = U16_TO_ANGLE(netbufReadU16(src));
		snap.look = U16_TO_ANGLE(netbufReadU16(src));
		for (s32 i = 0; i < ARRAYCOUNT(snap.rooms); ++i) {
			snap.rooms[i] = netbufReadS16(src);
			if (snap.rooms[i] < 0) {
				break;
			}
		}
		snap.rooms[ARRAYCOUNT(snap.rooms) - 1] = -1;
		snap.animnum = netbufReadS16(src);
		snap.frame = netbufReadF32(src);
		snap.speed = netbufReadF32(src);
		snap.loopframe = (snap.flags & NCF_LOOPING) ? netbufReadF32(src) : 0.f;
		snap.endframe = (snap.flags & NCF_ENDFRAME) ? netbufReadF32(src) : -1.f;
		weapons[HAND_RIGHT] = netbufReadS8(src);
		weapons[HAND_LEFT] = netbufReadS8(src);
		if (snap.flags & NCF_AIM) {
			for (s32 i = 0; i < 4; ++i) {
				aimend[i] = netbufReadS16(src) / 8192.f;
			}
		}

		if (src->error || srccl->state < CLSTATE_GAME) {
			break;
		}

		if (snap.animnum > 0) {
			const s32 numframes = animGetNumFrames(snap.animnum);
			snap.frame = netchrClampFrame(snap.frame, numframes);
			snap.loopframe = netchrClampFrame(snap.loopframe, numframes);
			snap.endframe = (snap.endframe >= 0.f) ? netchrClampFrame(snap.endframe, numframes) : -1.f;
		}
		if (!(snap.speed >= -10.f && snap.speed <= 10.f)) {
			snap.speed = 0.f;
		}

		struct prop *prop = netchrFindProp(syncid);
		if (!prop || prop->type != PROPTYPE_CHR || !prop->chr) {
			continue;
		}

		struct netchr *nc = netchrFor(prop->chr);
		if (!nc) {
			continue;
		}

		if (nc->count) {
			if (!(srvtime > nc->hist[nc->head].t)) {
				// stale or duplicate
				continue;
			}
			if (life != nc->life) {
				// a simulant respawned: don't slide from where it died
				nc->count = 0;
			}
		}

		nc->head = (nc->head + 1) % NETCHR_HIST;
		nc->hist[nc->head] = snap;
		if (nc->count < NETCHR_HIST) {
			++nc->count;
		}

		nc->life = life;
		nc->latestflags = snap.flags;
		nc->weapons[HAND_RIGHT] = weapons[HAND_RIGHT];
		nc->weapons[HAND_LEFT] = weapons[HAND_LEFT];
		memcpy(nc->aimend, aimend, sizeof(aimend));
		const f32 now = netchrNow();
		nc->rxtime = (now > 0.f) ? now : 0.001f;

		// hidden state matters even when the chr isn't ticking
		if (snap.flags & NCF_HIDDEN) {
			prop->chr->chrflags |= CHRCFLAG_HIDDEN;
		} else {
			prop->chr->chrflags &= ~CHRCFLAG_HIDDEN;
		}
	}

	return src->error;
}

/* client side puppeteering */

// replaces the normal root motion callback (chr0f01f378) for puppets:
// the position comes from the server, the animation only moves the limbs
static bool netchrRootCallback(struct model *model, struct coord *oldpos, struct coord *newpos, f32 *ground)
{
	struct chrdata *chr = model->chr;
	struct netchr *nc = chr ? netchrFor(chr) : NULL;

	if (!nc || !nc->applied || !chr->prop) {
		return chr0f01f378(model, oldpos, newpos, ground);
	}

	*ground = nc->dispground;
	newpos->x = nc->disp.x;
	newpos->y = nc->disp.y - nc->dispground;
	newpos->z = nc->disp.z;
	chr->prop->pos = nc->disp;

	return true;
}

static void netchrApplyRooms(struct chrdata *chr, const RoomNum *rooms)
{
	struct prop *prop = chr->prop;
	bool same = true;

	for (s32 i = 0; i < ARRAYCOUNT(prop->rooms); ++i) {
		if (prop->rooms[i] != rooms[i]) {
			same = false;
			break;
		}
		if (rooms[i] < 0) {
			break;
		}
	}

	if (!same && rooms[0] >= 0) {
		propDeregisterRooms(prop);
		roomsCopy((RoomNum *)rooms, prop->rooms);
		propRegisterRooms(prop);
	}
}

static void netchrApplyWeapons(struct chrdata *chr, struct netchr *nc)
{
	for (s32 hand = 0; hand < 2; ++hand) {
		const s8 want = nc->weapons[hand];
		const s8 have = netchrHeldWeaponNum(chr, hand);
		if (want == have) {
			continue;
		}
		if (want < 0 && chr->weapons_held[hand] && chr->weapons_held[hand]->syncid) {
			// a synced prop (e.g. a guard's gun from the level setup) is about to be
			// dropped by the death/disarm code; deleting it here would lose it
			continue;
		}
		if (chr->weapons_held[hand]) {
			if (chr->weapons_held[hand]->obj) {
				chr->weapons_held[hand]->obj->hidden |= OBJHFLAG_DELETING;
			}
			chr->weapons_held[hand] = NULL;
		}
		if (want > WEAPON_UNARMED) {
			const s32 modelnum = playermgrGetModelOfWeapon(want);
			if (modelnum >= 0) {
				chrGiveWeapon(chr, modelnum, want, hand == HAND_LEFT ? OBJFLAG_WEAPON_LEFTHANDED : 0);
			}
		}
		if (chr->aibot && hand == HAND_RIGHT) {
			chr->aibot->weaponnum = (want >= 0) ? want : WEAPON_UNARMED;
		}
	}
}

static void netchrApplyAnim(struct chrdata *chr, const struct netchrsnap *s, f32 elapsed)
{
	struct model *model = chr->model;
	struct anim *anim = model->anim;

	if (!anim || s->animnum <= 0) {
		return;
	}

	const s32 flip = (s->flags & NCF_FLIP) != 0;
	const s32 numframes = animGetNumFrames(s->animnum);
	const f32 last = (numframes > 1) ? (f32)(numframes - 1) : 0.f;

	// where the server's model would be by now
	f32 frame = s->frame + s->speed * (elapsed > 0.f ? elapsed : 0.f);
	if (s->endframe >= 0.f && frame > s->endframe) {
		frame = s->endframe;
	}
	if (frame > last) {
		if ((s->flags & NCF_LOOPING) && last - s->loopframe > 1.f) {
			frame = s->loopframe + fmodf(frame - s->loopframe, last - s->loopframe);
		} else {
			frame = last;
		}
	}
	frame = netchrClampFrame(frame, numframes);

	if (anim->animnum != s->animnum || anim->flip != flip) {
		modelSetAnimation(model, s->animnum, flip, frame, s->speed, 8.f);
	} else {
		const f32 framediff = anim->frame - frame;
		if (framediff > NETCHR_FRAME_TOLERANCE || framediff < -NETCHR_FRAME_TOLERANCE) {
			modelSetAnimFrame(model, frame);
		}
		if (anim->speed != s->speed) {
			modelSetAnimSpeed(model, s->speed, 0);
		}
	}

	if (s->flags & NCF_LOOPING) {
		anim->looping = true;
		anim->loopframe = s->loopframe;
	} else {
		anim->looping = false;
	}

	anim->endframe = s->endframe;
}

static inline f32 netchrLerpAngle(f32 from, f32 to, f32 t)
{
	f32 diff = to - from;
	while (diff > M_PI) diff -= M_BADTAU;
	while (diff < -M_PI) diff += M_BADTAU;
	f32 out = from + diff * t;
	while (out >= M_BADTAU) out -= M_BADTAU;
	while (out < 0.f) out += M_BADTAU;
	return out;
}

static inline f32 netchrDist2(const struct coord *a, const struct coord *b)
{
	const f32 dx = a->x - b->x, dy = a->y - b->y, dz = a->z - b->z;
	return dx * dx + dy * dy + dz * dz;
}

static inline const struct netchrsnap *netchrHist(const struct netchr *nc, s32 back)
{
	return &nc->hist[(nc->head + NETCHR_HIST - back) % NETCHR_HIST];
}

// works out the chr's state at server time rt into out (position, facing and the
// rooms; anim fields from the snapshot that was current then, whose pointer is returned)
static const struct netchrsnap *netchrSample(const struct netchr *nc, f32 rt, struct netchrsnap *out)
{
	const struct netchrsnap *newest = netchrHist(nc, 0);

	if (rt >= newest->t) {
		// past the newest one: carry on a little in the direction it was going
		*out = *newest;
		if (nc->count > 1 && !(newest->flags & NCF_DEAD)) {
			const struct netchrsnap *prev = netchrHist(nc, 1);
			const f32 dt = newest->t - prev->t;
			if (dt > 0.f && dt <= 10.f && netchrDist2(&newest->pos, &prev->pos) < NETCHR_SNAP_DIST * NETCHR_SNAP_DIST) {
				const f32 ahead = (rt - newest->t < NETCHR_MAX_EXTRAP ? rt - newest->t : NETCHR_MAX_EXTRAP) / dt;
				out->pos.x += (newest->pos.x - prev->pos.x) * ahead;
				out->pos.y += (newest->pos.y - prev->pos.y) * ahead;
				out->pos.z += (newest->pos.z - prev->pos.z) * ahead;
			}
		}
		return newest;
	}

	for (s32 back = 1; back < nc->count; ++back) {
		const struct netchrsnap *a = netchrHist(nc, back);
		if (a->t > rt) {
			continue;
		}
		const struct netchrsnap *b = netchrHist(nc, back - 1);
		*out = *a;
		if (netchrDist2(&a->pos, &b->pos) < NETCHR_SNAP_DIST * NETCHR_SNAP_DIST && b->t > a->t) {
			const f32 f = (rt - a->t) / (b->t - a->t);
			out->pos.x = a->pos.x + (b->pos.x - a->pos.x) * f;
			out->pos.y = a->pos.y + (b->pos.y - a->pos.y) * f;
			out->pos.z = a->pos.z + (b->pos.z - a->pos.z) * f;
			out->manground = a->manground + (b->manground - a->manground) * f;
			out->rot = netchrLerpAngle(a->rot, b->rot, f);
			out->look = netchrLerpAngle(a->look, b->look, f);
			if (f >= 0.5f) {
				memcpy(out->rooms, b->rooms, sizeof(out->rooms));
			}
		}
		return a;
	}

	// older than anything we have (just appeared, or respawned): show the oldest
	const struct netchrsnap *oldest = netchrHist(nc, nc->count - 1);
	*out = *oldest;
	return oldest;
}

void netchrPuppetTick(struct chrdata *chr)
{
	struct netchr *nc = netchrFor(chr);
	struct prop *prop = chr->prop;

	if (chr->actiontype == ACT_INIT) {
		// same as chraTick: put the chr into a valid standing animation,
		// otherwise its model would tick an uninitialized anim until the first snapshot
		chr->chrflags |= CHRCFLAG_FORCETOGROUND;
		func0f02e9a0(chr, 0);
		chr->sleep = 0;
	}

	if (chr->model && chr->model->anim && chr->model->anim->animnum <= 0) {
		// simulants get their animations from their movement code, which we don't run;
		// give them an idle animation until the server tells us otherwise
		modelSetAnimation(chr->model, ANIM_006A, 0, 0, 0.25f, 0);
		modelSetAnimLooping(chr->model, 0, 16);
	}

	if (!nc || !nc->count || !nc->rxtime || !prop || !chr->model) {
		// nothing from the server yet; stay frozen
		return;
	}

	// a simulant respawned on the server
	if (nc->life != nc->locallife) {
		nc->locallife = nc->life;
		if (chr->aibot && nc->applied && !(nc->latestflags & NCF_DEAD)) {
			botSpawn(chr, true);
			nc->applied = false; // snap to the new position
		}
	}

	const f32 rt = netchrRenderTime();
	struct netchrsnap cur;
	const struct netchrsnap *animsnap = netchrSample(nc, rt, &cur);

	if (!nc->applied || netchrDist2(&cur.pos, &nc->disp) > NETCHR_SNAP_DIST * NETCHR_SNAP_DIST) {
		// first update or a teleport
		nc->disp = cur.pos;
		RoomNum rooms[8];
		memcpy(rooms, cur.rooms, sizeof(rooms));
		chrSetPos(chr, &nc->disp, rooms, 360.f - BADRAD2DEG(cur.rot), false);
		modelSetAnim70(chr->model, netchrRootCallback);
		nc->applied = true;
	}

	nc->disp = cur.pos;
	nc->dispground = cur.manground;

	propSetPerimEnabled(prop, false);
	prop->pos = nc->disp;
	chr->manground = cur.manground;
	chr->ground = cur.manground;
	modelSetRootPosition(chr->model, &nc->disp);
	propSetPerimEnabled(prop, true);

	netchrApplyRooms(chr, cur.rooms);

	// facing
	modelSetChrRotY(chr->model, cur.rot);
	if (chr->aibot) {
		chr->aibot->lookangle = cur.look;
		chr->aibot->roty = cur.rot;
	}

	// keep the client's own idea of what the chr is doing simple and safe:
	// the animation comes from the server anyway
	if (cur.flags & NCF_DEAD) {
		if (chr->actiontype != ACT_DIE && chr->actiontype != ACT_DEAD) {
			memset(&chr->act_dead, 0, sizeof(chr->act_dead));
			chr->actiontype = ACT_DEAD;
		}
	} else if (chr->actiontype != ACT_STAND) {
		memset(&chr->act_stand, 0, sizeof(chr->act_stand));
		chr->actiontype = ACT_STAND;
	}

	netchrApplyAnim(chr, animsnap, rt - animsnap->t);
	netchrApplyWeapons(chr, nc);

	// footsteps come from the walk and run animations
	if (!(cur.flags & (NCF_DEAD | NCF_HIDDEN))) {
		footstepCheckDefault(chr);
	}

	// aim
	chr->aimendlshoulder = nc->aimend[0];
	chr->aimendrshoulder = nc->aimend[1];
	chr->aimendback = nc->aimend[2];
	chr->aimendsideback = nc->aimend[3];
	if (chr->aimendcount < 2) {
		chr->aimendcount = 3;
	}

	// muzzle flashes
	const f32 sincerx = netchrNow() - nc->rxtime;
	chrSetFiring(chr, HAND_RIGHT, (nc->latestflags & NCF_FIRE_R) != 0 && sincerx < 4.f);
	chrSetFiring(chr, HAND_LEFT, (nc->latestflags & NCF_FIRE_L) != 0 && sincerx < 4.f);
}

/* server -> client: SVC_CHR_SHOT */

#define NCSHOT_LEFT   (1 << 0)
#define NCSHOT_BEAM   (1 << 1)
#define NCSHOT_HITBG  (1 << 2)
#define NCSHOT_PAINT  (1 << 3)

u32 netmsgSvcChrShotWrite(struct netbuf *dst, struct chrdata *chr, s32 handnum, bool beam, struct coord *from, struct coord *to, bool hitbg, bool paint)
{
	if (!chr->prop || !chr->prop->syncid || chr->prop->type != PROPTYPE_CHR) {
		return dst->error;
	}

	const u8 flags = (handnum == HAND_LEFT ? NCSHOT_LEFT : 0) | (beam ? NCSHOT_BEAM : 0)
		| (hitbg ? NCSHOT_HITBG : 0) | (paint ? NCSHOT_PAINT : 0);

	netbufWriteU8(dst, SVC_CHR_SHOT);
	netbufWriteU16(dst, chr->prop->syncid);
	netbufWriteU8(dst, flags);
	netbufWriteCoord(dst, from);
	netbufWriteCoord(dst, to);

	return dst->error;
}

u32 netmsgSvcChrShotRead(struct netbuf *src, struct netclient *srccl)
{
	const u16 syncid = netbufReadU16(src);
	const u8 flags = netbufReadU8(src);
	struct coord from, to;
	netbufReadCoord(src, &from);
	netbufReadCoord(src, &to);

	if (src->error || srccl->state < CLSTATE_GAME) {
		return src->error;
	}

	struct prop *prop = netchrFindProp(syncid);
	if (!prop || prop->type != PROPTYPE_CHR || !prop->chr) {
		return src->error;
	}

	struct chrdata *chr = prop->chr;
	const s32 hand = (flags & NCSHOT_LEFT) ? HAND_LEFT : HAND_RIGHT;
	struct prop *gunprop = chrGetHeldProp(chr, hand);

	if (gunprop) {
		chr->hidden2 &= ~CHRH2FLAG_FIRESOUNDDONE;
		chrUpdateFireslot(chr, hand, true, (flags & NCSHOT_BEAM) != 0, &from, &to);
		chrSetFiring(chr, hand, true);

		if (flags & NCSHOT_HITBG) {
			RoomNum fromrooms[8], hitrooms[8];
			roomsCopy(prop->rooms, fromrooms);
			func0f065e74(&from, fromrooms, &to, hitrooms);
			struct gset gset = gunprop->weapon->gset;
			bgunPlayBgHitSound(&gset, &to, -1, hitrooms);
			sparksCreate(hitrooms[0], NULL, &to, NULL, NULL, (flags & NCSHOT_PAINT) ? SPARKTYPE_PAINT : SPARKTYPE_DEFAULT);
		}
	}

	return src->error;
}
