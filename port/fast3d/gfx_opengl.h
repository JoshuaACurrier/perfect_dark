#ifndef GFX_OPENGL_H
#define GFX_OPENGL_H

#include "gfx_rendering_api.h"

extern struct GfxRenderingAPI gfx_opengl_api;

// post-processing (gfx_postfx.inc)
extern int gfx_postfx_ssao;
extern float gfx_postfx_ssao_strength;
extern int gfx_postfx_bloom;
extern float gfx_postfx_bloom_strength;
bool gfx_opengl_postfx_wanted(void);
void gfx_opengl_postfx(const float P[4][4], int vx, int vy, int vw, int vh);

#endif
