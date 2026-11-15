#ifndef GFX_H
#define GFX_H
#include <ppu-types.h>

void gfx_init(u32 w, u32 h);
void gfx_clear(u32 rgb);
void gfx_rect(int x, int y, int w, int h, u32 rgb);
void gfx_pixel(int x, int y, u32 rgb);
void gfx_text(int x, int y, const char *utf8, u32 rgb, int scale);
int  gfx_text_width(const char *utf8, int scale);
void gfx_present(u32 *vram); /* copy back buffer to a VRAM framebuffer */
#endif
