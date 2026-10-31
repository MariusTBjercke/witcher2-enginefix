// 64x64 item icons: DXT1/3/5 or 32-bit BGRA DDS in, 32-bit BGRA DDS (no mipmaps) out.
// Port of the Dds class in ScriptPatches.ps1 (git tag v0.5.1); pixels are 0xAARRGGBB,
// row by row, and the same rounding is used so the bytes match the reference.
#ifndef SP_DDS_H
#define SP_DDS_H

#include "sp_common.h"

#define SP_DDS_PIXELS (64 * 64)

// 1 = ok, px receives 64*64 pixels. 0 for anything that is not a 64x64 DXT1/3/5 or
// 32-bit icon.
int sp_dds_decode64(const sp_buf* dds, unsigned* px);

// top drawn over base with the given opacity (the inventory shows its overlay at 0.9);
// the result replaces base.
void sp_dds_over(unsigned* base, const unsigned* top, double opacity);

// Uncompressed A8R8G8B8 with the layout of the game's own 32-bit item icons. 1 = ok.
int sp_dds_encode64(const unsigned* px, sp_buf* out);

#endif
