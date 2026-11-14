#ifndef _IN_TEXPACK_H
#define _IN_TEXPACK_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

// HD texture packs.
//
// A pack is a folder of images named after the game's texture numbers: four hex
// digits and .png or .jpg (0a3f.png), the layout community packs for the PC port
// use. It goes in its own folder inside texture-packs/, next to the game data
// (the --basedir folder), the save folder or the exe. The image replaces the
// texture when the renderer uploads it; it can be any size, since texture
// coordinates are relative to the original's size.
//
// Not handled yet: the textures stored inside model files (the pack's folders
// named after models) and the font folders.

#define TEXPACK_MAX_NAME 128

extern s32 g_TexpackEnabled;                  // Video.TexturePacks
extern char g_TexpackName[TEXPACK_MAX_NAME];  // Video.TexturePack: folder name, "" = the first one found

// whether a pack is on and has textures
s32 texpackActive(void);
s32 texpackGetNumTextures(void);

// look for the pack again (after changing the setting)
void texpackRescan(void);

// The renderer only sees a texture as the address of its texels. texLoad() knows the
// texture number that address belongs to and records it here.
void texpackRegisterTexture(const void *data, s32 texnum);
void texpackForgetAll(void);
s32 texpackGetTextureNum(const void *data);

// The pack's image for the texture at data as RGBA32, in the game's row order (bottom
// row first), or NULL if it has none. Free with texpackFreeReplacement().
u8 *texpackLoadReplacement(const void *data, s32 *outWidth, s32 *outHeight);
void texpackFreeReplacement(u8 *rgba);

#ifdef __cplusplus
}
#endif

#endif
