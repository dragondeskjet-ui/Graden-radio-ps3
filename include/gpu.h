#ifndef GPU_H
#define GPU_H
#include "api.h"

/* textures */
enum { TEX_ATLAS = 0, TEX_UI = 1, TEX_HUD = 2, TEX_TOAST = 3, TEX_STARS = 4, TEX_COUNT = 5 };

/* sprite: pixel rect, uv rect, premultiplied colour (0..1), rotation (radians, about centre) */
struct Sprite { float x, y, w, h, u0, v0, u1, v1, r, g, b, a, rot; int tex; };

struct GlobeView { float lon, lat, R, cx, cy, dot_scale; bool show_globe; };

bool gpu_init(int w, int h);
bool gpu_load_earth(const char *path);           /* RGB565 BE 4096x2048 */
/* upload premultiplied ARGB pixels (w x h) to a texture slot; double-buffered for dynamic ones */
void gpu_upload(int tex, const u32 *argb, int w, int h);
/* draw order: background sprites, globe, dot sprites, foreground sprites (all sprites use the proven 2D shader) */
void gpu_frame(const GlobeView &g, const Sprite *bg, int nbg, const Sprite *dots, int ndots, const Sprite *fg, int nfg);
#endif
