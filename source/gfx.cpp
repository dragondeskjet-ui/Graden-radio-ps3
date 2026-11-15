#include <stdlib.h>
#include <string.h>
#include "gfx.h"
#include "font8x16.h"

static u32 *fb = NULL;
static int W = 0, H = 0;

void gfx_init(u32 w, u32 h) { W = w; H = h; fb = (u32 *)malloc((size_t)w * h * 4); }

void gfx_clear(u32 rgb) { for (int i = 0; i < W * H; i++) fb[i] = rgb; }

void gfx_pixel(int x, int y, u32 rgb) { if (x >= 0 && y >= 0 && x < W && y < H) fb[y * W + x] = rgb; }

void gfx_rect(int x, int y, int w, int h, u32 rgb)
{
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > W) w = W - x;
	if (y + h > H) h = H - y;
	for (int j = 0; j < h; j++) {
		u32 *p = fb + (y + j) * W + x;
		for (int i = 0; i < w; i++) p[i] = rgb;
	}
}

/* Decode one UTF-8 char, map to Latin-1 (others -> '?'). Returns bytes consumed. */
static int next_char(const unsigned char *s, int *out)
{
	if (s[0] < 0x80) { *out = s[0]; return 1; }
	if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
		int cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
		*out = cp < 256 ? cp : '?';
		return 2;
	}
	if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) { *out = '?'; return 3; }
	if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) { *out = '?'; return 4; }
	*out = '?';
	return 1;
}

void gfx_text(int x, int y, const char *utf8, u32 rgb, int scale)
{
	const unsigned char *s = (const unsigned char *)utf8;
	while (*s) {
		int c;
		s += next_char(s, &c);
		if (c < 32) c = '?';
		const unsigned char *g = font8x16[c - 32];
		for (int r = 0; r < 16; r++)
			for (int b = 0; b < 8; b++)
				if (g[r] & (0x80 >> b))
					gfx_rect(x + b * scale, y + r * scale, scale, scale, rgb);
		x += 8 * scale;
	}
}

int gfx_text_width(const char *utf8, int scale)
{
	const unsigned char *s = (const unsigned char *)utf8;
	int n = 0, c;
	while (*s) { s += next_char(s, &c); n++; }
	return n * 8 * scale;
}

void gfx_present(u32 *vram) { memcpy(vram, fb, (size_t)W * H * 4); }
