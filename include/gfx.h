#ifndef GFX_H
#define GFX_H
#include <ppu-types.h>

struct FontDef;
extern const FontDef *F_SMALL, *F_BODY, *F_BOLD, *F_TITLE;

void gfx_init(u32 w, u32 h);
u32 *gfx_buffer();
void gfx_set_target(u32 *buf);          /* NULL = default back buffer */
void gfx_clear(u32 rgb);
void gfx_rect(int x, int y, int w, int h, u32 rgb);
void gfx_rect_a(int x, int y, int w, int h, u32 rgb, int a);
void gfx_rrect(int x, int y, int w, int h, int r, u32 rgb, int a);
void gfx_circle(float cx, float cy, float r, u32 rgb, int a);
void gfx_ring(float cx, float cy, float r, float th, u32 rgb, int a);
void gfx_arc(float cx, float cy, float r, float th, float a0, float a1, u32 rgb, int a);
void gfx_line(float x0, float y0, float x1, float y1, float th, u32 rgb, int a);
void gfx_star(float cx, float cy, float r, u32 rgb, int a);
void gfx_tri(float x0, float y0, float x1, float y1, float x2, float y2, u32 rgb, int a);

int  gfx_text(const FontDef *f, int x, int y, const char *utf8, u32 rgb, int a = 255);  /* y = top of line; returns width */
int  gfx_text_w(const FontDef *f, const char *utf8);
int  gfx_text_fit(const FontDef *f, int x, int y, const char *utf8, int maxw, u32 rgb, int a = 255);
int  gfx_line_h(const FontDef *f);

/* PlayStation-style face/shoulder button glyphs. kind: 'X','O','T','S' or a label like "L1" */
int  gfx_button(const char *kind, int x, int cy, int size);

void gfx_present(u32 *vram);
void gfx_clip(int x, int y, int w, int h);
void gfx_noclip();
void gfx_rrect_v(int x, int y, int w, int h, int r, u32 c0, int a0, u32 c1, int a1);
void gfx_rrect_stroke(int x, int y, int w, int h, int r, float th, u32 c, int a);
void gfx_target_size(int w, int h);
extern u32 GFX_BTN_BG, GFX_BTN_FG;
#endif
