#include <os_internal.h>
#include <ultraerror.h>
#include "n_synthInternals.h"

void n_alSynSetPan(N_ALVoice *v, u8 pan)
{
	ALParam *update;

	if (v->pvoice) {
		/*
		 * get new update struct from the free list
		 */
		update = __n_allocParam();
		ALFailIf(update == 0, ERR_ALSYN_NO_UPDATE);

		/*
		 * set offset and pan data
		 */
		update->delta = n_syn->paramSamples + v->pvoice->offset;
		update->type = AL_FILTER_SET_PAN;
		update->data.i = pan;
		update->next = 0;

		n_alEnvmixerParam(v->pvoice, AL_FILTER_ADD_UPDATE, update);
	}
}

#ifndef PLATFORM_N64
/**
 * Give the voice a direction for binaural rendering (0..127 = 0..360 degrees
 * clockwise from straight ahead), or -1 for none. Takes effect right away; the
 * mixer smooths the change.
 */
void n_alSynSetAzimuth(N_ALVoice *v, s16 azimuth)
{
	if (v->pvoice) {
		v->pvoice->em_az = azimuth;
	}
}

/**
 * How muffled the voice is: 0 (clear) to 255 (behind thick walls). Takes effect
 * right away; the mixer smooths the change.
 */
void n_alSynSetMuffle(N_ALVoice *v, s16 muffle)
{
	if (v->pvoice) {
		v->pvoice->em_muffle = muffle;
	}
}
#endif
