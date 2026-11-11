// Chr (guard / simulant) synchronization.
//
// The server runs all AI. Every frame it sends a compact snapshot of each chr
// (SVC_CHR_MOVE): position, facing, animation, aim, held weapons and gunfire.
// Clients don't run AI for chrs at all; instead each chr becomes a "puppet"
// that smoothly follows the latest snapshot. Shots are sent separately
// (SVC_CHR_SHOT) so clients can show tracers, sparks and play sounds.
// Damage and disarming already go through SVC_CHR_DAMAGE / SVC_CHR_DISARM. Guards
// letting go of things (death, disarm) and being removed are SVC_CHR_DROP /
// SVC_CHR_REMOVE (netmsg.c), since clients never run the code that does that.

#include <stdlib.h>
#include <string.h>
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

// how far (in world units) a puppet may drift from where it should be before it snaps
#define NETCHR_SNAP_DIST 250.f
// how many ticks we're willing to extrapolate past the last snapshot
#define NETCHR_MAX_EXTRAP 8
// if the anim frame is further off than this, jump to the right frame
#define NETCHR_FRAME_TOLERANCE 6.f

#define ANGLE_TO_U16(a) ((u16)((s32)((a) * (65536.f / M_BADTAU)) & 0xffff))
#define U16_TO_ANGLE(v) ((f32)(v) * (M_BADTAU / 65536.f))

struct netchr {
	u32 rxtick;         // local g_NetTick when the latest snapshot arrived, 0 if none yet
	u32 srvtick;        // server tick of the latest snapshot
	u32 prevsrvtick;    // server tick of the snapshot before that
	struct coord pos;   // latest snapshot position
	struct coord prevpos;
	struct coord disp;  // position we're displaying
	f32 manground;
	f32 rot;            // model facing
	f32 look;           // look angle (simulants aim separately from their body)
	RoomNum rooms[8];
	f32 aimend[4];
	f32 frame;
	f32 speed;
	f32 loopframe;
	f32 endframe;
	s16 animnum;
	s8 actiontype;
	s8 weapons[2];
	u8 flags;
	u8 life;
	u8 locallife;       // last life number we applied
	u8 applied;         // the puppet has been placed at least once
	u8 removeok;        // the server said to remove this chr
	u8 removewarned;
};

// per chr slot state on the client; on the server only the send bookkeeping is used
static struct netchr *g_NetChrs = NULL;
static s32 g_NetChrsNum = 0;

// server side: life counters (incremented every time a simulant respawns)
static u8 *g_NetChrLives = NULL;
static u32 g_NetChrSendCursor = 0;

static u8 g_NetChrBufData[NET_BUFSIZE];
static struct netbuf g_NetChrBuf = { .data = g_NetChrBufData, .size = sizeof(g_NetChrBufData) };

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

static inline f32 netchrAbs(f32 v)
{
	return v < 0.f ? -v : v;
}

// On the server g_NetChrs[] holds what we last sent for each chr. Chrs whose state
// hasn't changed in a way the client can't predict are skipped, except for a
// periodic refresh so lost packets get corrected. Missions have dozens of guards
// standing around, so this saves most of the bandwidth.
#define NETCHR_REFRESH_TICKS 30

static inline s32 netchrShouldSend(struct chrdata *chr)
{
	struct prop *prop = chr->prop;
	if (chr->chrnum < 0 || !prop || !prop->syncid || prop->type != PROPTYPE_CHR || !chr->model || !chr->model->anim) {
		return false;
	}
	if (chr->hidden & CHRHFLAG_DELETING) {
		return false;
	}

	struct netchr *last = netchrFor(chr);
	if (!last || !last->srvtick) {
		return true;
	}

	const u32 since = g_NetTick - last->srvtick;
	if (since >= NETCHR_REFRESH_TICKS) {
		return true;
	}

	// it was moving when we last sent it: send at least once more so the
	// client stops extrapolating when it stops
	if (last->pos.x != last->prevpos.x || last->pos.y != last->prevpos.y || last->pos.z != last->prevpos.z) {
		return true;
	}

	struct anim *anim = chr->model->anim;
	const s8 wr = netchrHeldWeaponNum(chr, HAND_RIGHT);
	const s8 wl = netchrHeldWeaponNum(chr, HAND_LEFT);

	if (netchrFlags(chr) != last->flags
			|| chr->actiontype != last->actiontype
			|| anim->animnum != last->animnum
			|| anim->speed != last->speed
			|| wr != last->weapons[HAND_RIGHT] || wl != last->weapons[HAND_LEFT]
			|| netchrAbs(prop->pos.x - last->pos.x) > 0.5f
			|| netchrAbs(prop->pos.y - last->pos.y) > 0.5f
			|| netchrAbs(prop->pos.z - last->pos.z) > 0.5f
			|| netchrAbs(modelGetChrRotY(chr->model) - last->rot) > 0.01f
			|| netchrAbs(chrGetInverseTheta(chr) - last->look) > 0.01f
			|| netchrAbs(chr->aimendlshoulder - last->aimend[0]) > 0.01f
			|| netchrAbs(chr->aimendrshoulder - last->aimend[1]) > 0.01f
			|| netchrAbs(chr->aimendback - last->aimend[2]) > 0.01f
			|| netchrAbs(chr->aimendsideback - last->aimend[3]) > 0.01f) {
		return true;
	}

	// the client advances the frame by itself; only resend when it's off (e.g. a loop wrapped)
	const f32 predicted = last->frame + last->speed * since;
	if (netchrAbs(anim->frame - predicted) > 2.f) {
		return true;
	}

	return false;
}

static void netchrRecordSent(struct chrdata *chr, u8 flags)
{
	struct netchr *last = netchrFor(chr);
	if (!last) {
		return;
	}
	struct anim *anim = chr->model->anim;
	last->prevpos = last->srvtick ? last->pos : chr->prop->pos;
	last->pos = chr->prop->pos;
	last->rot = modelGetChrRotY(chr->model);
	last->look = chrGetInverseTheta(chr);
	last->aimend[0] = chr->aimendlshoulder;
	last->aimend[1] = chr->aimendrshoulder;
	last->aimend[2] = chr->aimendback;
	last->aimend[3] = chr->aimendsideback;
	last->animnum = anim->animnum;
	last->frame = anim->frame;
	last->speed = anim->speed;
	last->flags = flags;
	last->actiontype = chr->actiontype;
	last->weapons[HAND_RIGHT] = netchrHeldWeaponNum(chr, HAND_RIGHT);
	last->weapons[HAND_LEFT] = netchrHeldWeaponNum(chr, HAND_LEFT);
	last->srvtick = g_NetTick ? g_NetTick : 1;
}

static u32 netchrWriteOne(struct netbuf *dst, struct chrdata *chr)
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

	netchrRecordSent(chr, flags);

	return dst->error;
}

void netchrServerSend(void)
{
	if (g_NetMode != NETMODE_SERVER || !g_NetChrs || g_NetLocalClient->state < CLSTATE_GAME) {
		return;
	}

	// worst case size of one chr entry
	const u32 maxentry = 2 + 1 + 1 + 1 + 12 + 4 + 2 + 2 + 2 * 8 + 2 + 4 + 4 + 4 + 4 + 2 + 8;
	// at most this many packets per frame; anything left over goes out next frame
	s32 packetsleft = 4;
	s32 checked = 0;

	while (checked < g_NumChrSlots && packetsleft > 0) {
		netbufStartWrite(&g_NetChrBuf);
		netbufWriteU8(&g_NetChrBuf, SVC_CHR_MOVE);
		netbufWriteU32(&g_NetChrBuf, g_NetTick);
		const u32 countpos = g_NetChrBuf.wp;
		netbufWriteU8(&g_NetChrBuf, 0);
		u8 count = 0;

		while (checked < g_NumChrSlots && count < 255 && netbufWriteLeft(&g_NetChrBuf) > (s32)maxentry) {
			struct chrdata *chr = &g_ChrSlots[g_NetChrSendCursor % g_NumChrSlots];
			g_NetChrSendCursor = (g_NetChrSendCursor + 1) % g_NumChrSlots;
			++checked;
			if (netchrShouldSend(chr)) {
				netchrWriteOne(&g_NetChrBuf, chr);
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

u32 netmsgSvcChrMoveRead(struct netbuf *src, struct netclient *srccl)
{
	const u32 srvtick = netbufReadU32(src);
	const u8 count = netbufReadU8(src);

	for (u8 n = 0; n < count && !src->error; ++n) {
		struct netchr tmp;
		memset(&tmp, 0, sizeof(tmp));

		const u16 syncid = netbufReadU16(src);
		tmp.flags = netbufReadU8(src);
		tmp.life = netbufReadU8(src);
		tmp.actiontype = netbufReadS8(src);
		netbufReadCoord(src, &tmp.pos);
		tmp.manground = netbufReadF32(src);
		tmp.rot = U16_TO_ANGLE(netbufReadU16(src));
		tmp.look = U16_TO_ANGLE(netbufReadU16(src));
		for (s32 i = 0; i < ARRAYCOUNT(tmp.rooms); ++i) {
			tmp.rooms[i] = netbufReadS16(src);
			if (tmp.rooms[i] < 0) {
				break;
			}
		}
		tmp.animnum = netbufReadS16(src);
		tmp.frame = netbufReadF32(src);
		tmp.speed = netbufReadF32(src);
		tmp.loopframe = (tmp.flags & NCF_LOOPING) ? netbufReadF32(src) : 0.f;
		tmp.endframe = (tmp.flags & NCF_ENDFRAME) ? netbufReadF32(src) : -1.f;
		tmp.weapons[HAND_RIGHT] = netbufReadS8(src);
		tmp.weapons[HAND_LEFT] = netbufReadS8(src);
		if (tmp.flags & NCF_AIM) {
			for (s32 i = 0; i < 4; ++i) {
				tmp.aimend[i] = netbufReadS16(src) / 8192.f;
			}
		}

		if (src->error || srccl->state < CLSTATE_GAME) {
			break;
		}

		if (tmp.animnum > 0) {
			const s32 numframes = animGetNumFrames(tmp.animnum);
			tmp.frame = netchrClampFrame(tmp.frame, numframes);
			tmp.loopframe = netchrClampFrame(tmp.loopframe, numframes);
			tmp.endframe = (tmp.endframe >= 0.f) ? netchrClampFrame(tmp.endframe, numframes) : -1.f;
		}
		if (!(tmp.speed >= -10.f && tmp.speed <= 10.f)) {
			tmp.speed = 0.f;
		}

		struct prop *prop = netchrFindProp(syncid);
		if (!prop || prop->type != PROPTYPE_CHR || !prop->chr) {
			continue;
		}

		struct netchr *nc = netchrFor(prop->chr);
		if (!nc) {
			continue;
		}

		if (nc->rxtick && srvtick <= nc->srvtick) {
			// stale or duplicate
			continue;
		}

		// keep the interpolation history
		if (nc->rxtick) {
			tmp.prevpos = nc->pos;
			tmp.prevsrvtick = nc->srvtick;
		} else {
			tmp.prevpos = tmp.pos;
			tmp.prevsrvtick = srvtick;
		}

		tmp.disp = nc->disp;
		tmp.locallife = nc->locallife;
		tmp.applied = nc->applied;
		tmp.rxtick = g_NetTick ? g_NetTick : 1;
		tmp.srvtick = srvtick;

		*nc = tmp;

		// hidden state matters even when the chr isn't ticking
		if (nc->flags & NCF_HIDDEN) {
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

	*ground = nc->manground;
	newpos->x = nc->disp.x;
	newpos->y = nc->disp.y - nc->manground;
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

static void netchrApplyAnim(struct chrdata *chr, struct netchr *nc, u32 ticksince)
{
	struct model *model = chr->model;
	struct anim *anim = model->anim;

	if (!anim || nc->animnum <= 0) {
		return;
	}

	const s32 flip = (nc->flags & NCF_FLIP) != 0;
	const s32 numframes = animGetNumFrames(nc->animnum);
	f32 frame = nc->frame + nc->speed * (ticksince < NETCHR_MAX_EXTRAP ? ticksince : NETCHR_MAX_EXTRAP);
	if (nc->endframe >= 0.f && frame > nc->endframe) {
		frame = nc->endframe;
	}
	if (frame >= numframes - 1 || frame < 0.f) {
		frame = (nc->flags & NCF_LOOPING) ? nc->loopframe : nc->frame;
	}
	frame = netchrClampFrame(frame, numframes);

	if (anim->animnum != nc->animnum || anim->flip != flip) {
		modelSetAnimation(model, nc->animnum, flip, frame, nc->speed, 8.f);
	} else {
		const f32 framediff = anim->frame - frame;
		if (framediff > NETCHR_FRAME_TOLERANCE || framediff < -NETCHR_FRAME_TOLERANCE) {
			modelSetAnimFrame(model, frame);
		}
		if (anim->speed != nc->speed) {
			modelSetAnimSpeed(model, nc->speed, 0);
		}
	}

	if (nc->flags & NCF_LOOPING) {
		anim->looping = true;
		anim->loopframe = nc->loopframe;
	} else {
		anim->looping = false;
	}

	anim->endframe = nc->endframe;
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

	if (!nc || !nc->rxtick || !prop || !chr->model) {
		// nothing from the server yet; stay frozen
		return;
	}

	const u32 ticksince = (g_NetTick > nc->rxtick) ? (g_NetTick - nc->rxtick) : 0;

	// a simulant respawned on the server
	if (nc->life != nc->locallife) {
		nc->locallife = nc->life;
		if (chr->aibot && nc->applied && !(nc->flags & NCF_DEAD)) {
			botSpawn(chr, true);
			nc->applied = false; // snap to the new position
		}
	}

	// where should the chr be right now? extrapolate a little past the last snapshot
	struct coord target = nc->pos;
	if (nc->srvtick > nc->prevsrvtick && !(nc->flags & NCF_DEAD)) {
		const f32 dt = (f32)(nc->srvtick - nc->prevsrvtick);
		const f32 ahead = (f32)(ticksince < NETCHR_MAX_EXTRAP ? ticksince : NETCHR_MAX_EXTRAP);
		target.x += (nc->pos.x - nc->prevpos.x) / dt * ahead;
		target.y += (nc->pos.y - nc->prevpos.y) / dt * ahead;
		target.z += (nc->pos.z - nc->prevpos.z) / dt * ahead;
	}

	const f32 dx = target.x - nc->disp.x;
	const f32 dy = target.y - nc->disp.y;
	const f32 dz = target.z - nc->disp.z;

	if (!nc->applied || dx * dx + dy * dy + dz * dz > NETCHR_SNAP_DIST * NETCHR_SNAP_DIST) {
		// first update or way off: teleport
		nc->disp = target;
		RoomNum rooms[8];
		memcpy(rooms, nc->rooms, sizeof(rooms));
		chrSetPos(chr, &nc->disp, rooms, 360.f - BADRAD2DEG(nc->rot), false);
		modelSetAnim70(chr->model, netchrRootCallback);
		nc->applied = true;
	} else {
		const f32 t = 0.5f;
		nc->disp.x += dx * t;
		nc->disp.y += dy * t;
		nc->disp.z += dz * t;
	}

	propSetPerimEnabled(prop, false);
	prop->pos = nc->disp;
	chr->manground = nc->manground;
	chr->ground = nc->manground;
	modelSetRootPosition(chr->model, &nc->disp);
	propSetPerimEnabled(prop, true);

	netchrApplyRooms(chr, nc->rooms);

	// facing
	const f32 rot = netchrLerpAngle(modelGetChrRotY(chr->model), nc->rot, 0.5f);
	modelSetChrRotY(chr->model, rot);
	if (chr->aibot) {
		chr->aibot->lookangle = netchrLerpAngle(chr->aibot->lookangle, nc->look, 0.5f);
		chr->aibot->roty = rot;
	}

	// keep the client's own idea of what the chr is doing simple and safe:
	// the animation comes from the server anyway
	if (nc->flags & NCF_DEAD) {
		if (chr->actiontype != ACT_DIE && chr->actiontype != ACT_DEAD) {
			memset(&chr->act_dead, 0, sizeof(chr->act_dead));
			chr->actiontype = ACT_DEAD;
		}
	} else if (chr->actiontype != ACT_STAND) {
		memset(&chr->act_stand, 0, sizeof(chr->act_stand));
		chr->actiontype = ACT_STAND;
	}

	netchrApplyAnim(chr, nc, ticksince);
	netchrApplyWeapons(chr, nc);

	// aim
	chr->aimendlshoulder = nc->aimend[0];
	chr->aimendrshoulder = nc->aimend[1];
	chr->aimendback = nc->aimend[2];
	chr->aimendsideback = nc->aimend[3];
	if (chr->aimendcount < 2) {
		chr->aimendcount = 3;
	}

	// muzzle flashes
	chrSetFiring(chr, HAND_RIGHT, (nc->flags & NCF_FIRE_R) != 0 && ticksince < 4);
	chrSetFiring(chr, HAND_LEFT, (nc->flags & NCF_FIRE_L) != 0 && ticksince < 4);
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
