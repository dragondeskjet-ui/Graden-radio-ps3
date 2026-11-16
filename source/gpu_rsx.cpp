/* RSX renderer: the GPU draws stars, atmosphere, the textured globe, station dots, the cached UI layer and sprites.
 * The PPU only builds small vertex lists and re-uploads UI textures when they change. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <malloc.h>
#include <ppu-types.h>
#include <rsx/rsx.h>
#include "rsxutil.h"
#include "gpu.h"

#include "vs2d_vpo.h"
#include "fs2d_fpo.h"
#include "vsglobe_vpo.h"
#include "fsglobe_fpo.h"
#include "vsdots_vpo.h"

struct Prog { rsxVertexProgram *vp; void *vucode; rsxFragmentProgram *fp; void *fucode; u32 foff; };
static Prog P2D, PGLOBE, PDOTS;
static rsxProgramConst *c2d_scr, *cg_east, *cg_north, *cg_view, *cg_scr, *cd_east, *cd_north, *cd_view, *cd_scr, *cd_scale;
static int u2d_tex, ug_tex;

struct Tex { u32 *mem[2]; u32 off[2]; int w, h, cur, fmt; bool dbl; };
static Tex T[TEX_COUNT];
static u16 *earth_hi, *earth_lo; static u32 earth_hi_off, earth_lo_off;
static int W, H;

/* globe mesh */
struct GV { float x, y, z, u, v; };
static GV *gverts; static u16 *gidx; static u32 gverts_off, gidx_off; static int gnidx;
/* dots */
struct DV { float x, y, z, u, v, cx, cy, size, r, g, b, a; };
static DV *dverts; static u32 dverts_off; static int ndverts;
/* sprites (double buffered) */
struct SV { float x, y, u, v, r, g, b, a; };
#define MAX_SPRITES 512
static SV *sverts[2]; static u32 sverts_off[2]; static int sframe = 0, scursor = 0;

static void load_prog(Prog &p, const void *vpo, const void *fpo)
{
	u32 sz;
	p.vp = (rsxVertexProgram *)vpo;
	rsxVertexProgramGetUCode(p.vp, &p.vucode, &sz);
	p.fp = NULL;
	if (fpo) {
		p.fp = (rsxFragmentProgram *)fpo;
		void *uc; rsxFragmentProgramGetUCode(p.fp, &uc, &sz);
		p.fucode = rsxMemalign(64, sz); memcpy(p.fucode, uc, sz);
		rsxAddressToOffset(p.fucode, &p.foff);
	}
}

static void tex_alloc(int slot, int w, int h, bool dbl)
{
	Tex &t = T[slot]; t.w = w; t.h = h; t.dbl = dbl; t.cur = 0;
	for (int i = 0; i < (dbl ? 2 : 1); i++) {
		t.mem[i] = (u32 *)rsxMemalign(128, (size_t)w * h * 4);
		memset(t.mem[i], 0, (size_t)w * h * 4);
		rsxAddressToOffset(t.mem[i], &t.off[i]);
	}
	if (!dbl) { t.mem[1] = t.mem[0]; t.off[1] = t.off[0]; }
}

void gpu_upload(int slot, const u32 *argb, int w, int h)
{
	Tex &t = T[slot];
	if (!t.mem[0] || w != t.w || h != t.h) {
		if (t.mem[0]) { rsxFree(t.mem[0]); if (t.dbl) rsxFree(t.mem[1]); t.mem[0] = NULL; }
		tex_alloc(slot, w, h, slot == TEX_UI || slot == TEX_HUD || slot == TEX_TOAST);
	}
	int dst = t.dbl ? t.cur ^ 1 : 0;
	memcpy(t.mem[dst], argb, (size_t)w * h * 4);
	t.cur = dst;
}

static void bind_tex(int unit, u32 offset, int w, int h, bool rgb565, bool linear_filter, bool repeat_u)
{
	gcmTexture tx;
	memset(&tx, 0, sizeof tx);
	tx.format = (rgb565 ? GCM_TEXTURE_FORMAT_R5G6B5 : GCM_TEXTURE_FORMAT_A8R8G8B8) | GCM_TEXTURE_FORMAT_LIN;
	tx.mipmap = 1;
	tx.dimension = GCM_TEXTURE_DIMS_2D;
	tx.cubemap = GCM_FALSE;
	tx.remap = ((GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT) |
	            (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT) |
	            (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT) |
	            (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT) |
	            (GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_B_SHIFT) |
	            (GCM_TEXTURE_REMAP_COLOR_G << GCM_TEXTURE_REMAP_COLOR_G_SHIFT) |
	            (GCM_TEXTURE_REMAP_COLOR_R << GCM_TEXTURE_REMAP_COLOR_R_SHIFT) |
	            (GCM_TEXTURE_REMAP_COLOR_A << GCM_TEXTURE_REMAP_COLOR_A_SHIFT));
	tx.width = w; tx.height = h; tx.depth = 1;
	tx.location = GCM_LOCATION_RSX;
	tx.pitch = w * (rgb565 ? 2 : 4);
	tx.offset = offset;
	rsxLoadTexture(context, unit, &tx);
	rsxTextureControl(context, unit, GCM_TRUE, 0 << 8, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
	u32 f = linear_filter ? GCM_TEXTURE_LINEAR : GCM_TEXTURE_NEAREST;
	rsxTextureFilter(context, unit, 0, f, f, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	rsxTextureWrapMode(context, unit, repeat_u ? GCM_TEXTURE_REPEAT : GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
}

static void attr(int a, u32 off, int stride, int n) { rsxBindVertexArrayAttrib(context, a, 0, off, stride, n, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX); }
static void attr_off(int a) { rsxBindVertexArrayAttrib(context, a, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX); }
static void attrs_clear() { for (int a = 0; a < 16; a++) attr_off(a); }

static void build_globe_mesh()
{
	const int LAT = 64, LON = 128;
	int nv = (LAT + 1) * (LON + 1);
	gverts = (GV *)rsxMemalign(128, nv * sizeof(GV));
	gnidx = LAT * LON * 6;
	gidx = (u16 *)rsxMemalign(128, gnidx * 2);
	const float d = 3.14159265f / 180.f;
	for (int i = 0; i <= LAT; i++) {
		float lat = 90.f - 180.f * i / LAT;
		for (int j = 0; j <= LON; j++) {
			float lon = -180.f + 360.f * j / LON;
			GV &v = gverts[i * (LON + 1) + j];
			v.x = cosf(lat * d) * cosf(lon * d); v.y = cosf(lat * d) * sinf(lon * d); v.z = sinf(lat * d);
			v.u = (float)j / LON; v.v = (float)i / LAT;
		}
	}
	int k = 0;
	for (int i = 0; i < LAT; i++) for (int j = 0; j < LON; j++) {
		u16 a = i * (LON + 1) + j, b = a + 1, c = a + (LON + 1), e = c + 1;
		gidx[k++] = a; gidx[k++] = c; gidx[k++] = b; gidx[k++] = b; gidx[k++] = c; gidx[k++] = e;
	}
	rsxAddressToOffset(gverts, &gverts_off); rsxAddressToOffset(gidx, &gidx_off);
}

bool gpu_init(int w, int h)
{
	W = w; H = h;
	load_prog(P2D, vs2d_vpo, fs2d_fpo);
	load_prog(PGLOBE, vsglobe_vpo, fsglobe_fpo);
	load_prog(PDOTS, vsdots_vpo, NULL);
	c2d_scr = rsxVertexProgramGetConst(P2D.vp, "scr");
	u2d_tex = rsxFragmentProgramGetAttrib(P2D.fp, "tex")->index;
	cg_east = rsxVertexProgramGetConst(PGLOBE.vp, "east"); cg_north = rsxVertexProgramGetConst(PGLOBE.vp, "north");
	cg_view = rsxVertexProgramGetConst(PGLOBE.vp, "view"); cg_scr = rsxVertexProgramGetConst(PGLOBE.vp, "scr");
	ug_tex = rsxFragmentProgramGetAttrib(PGLOBE.fp, "tex")->index;
	cd_east = rsxVertexProgramGetConst(PDOTS.vp, "east"); cd_north = rsxVertexProgramGetConst(PDOTS.vp, "north");
	cd_view = rsxVertexProgramGetConst(PDOTS.vp, "view"); cd_scr = rsxVertexProgramGetConst(PDOTS.vp, "scr");
	cd_scale = rsxVertexProgramGetConst(PDOTS.vp, "dot_scale");
	for (int i = 0; i < 2; i++) { sverts[i] = (SV *)rsxMemalign(128, MAX_SPRITES * 4 * sizeof(SV)); rsxAddressToOffset(sverts[i], &sverts_off[i]); }
	build_globe_mesh();
	earth_lo = (u16 *)rsxMemalign(128, 8 * 8 * 2);
	for (int i = 0; i < 64; i++) earth_lo[i] = 0x0947;
	rsxAddressToOffset(earth_lo, &earth_lo_off);
	return true;
}

bool gpu_load_earth(const char *path)
{
	FILE *f = fopen(path, "rb"); if (!f) return false;
	const int w = 4096, h = 2048;
	u16 *buf = (u16 *)malloc((size_t)w * h * 2);
	bool ok = buf && fread(buf, 2, (size_t)w * h, f) == (size_t)w * h;   /* file is big-endian = native */
	fclose(f);
	if (!ok) { free(buf); return false; }
	earth_hi = (u16 *)rsxMemalign(128, (size_t)w * h * 2);
	memcpy(earth_hi, buf, (size_t)w * h * 2);
	/* 1024x512 version for zoomed-out views (avoids shimmering without mipmaps) */
	const int lw = 1024, lh = 512;
	u16 *lo = (u16 *)malloc(lw * lh * 2);
	for (int y = 0; y < lh; y++) for (int x = 0; x < lw; x++) {
		u32 r = 0, g = 0, b = 0;
		for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) { u16 p = buf[(y * 4 + j) * w + x * 4 + i]; r += p >> 11; g += (p >> 5) & 63; b += p & 31; }
		lo[y * lw + x] = (u16)(((r / 16) << 11) | ((g / 16) << 5) | (b / 16));
	}
	earth_lo = (u16 *)rsxMemalign(128, lw * lh * 2);   /* (8x8 fallback is leaked; tiny) */
	memcpy(earth_lo, lo, lw * lh * 2);
	free(lo); free(buf);
	rsxAddressToOffset(earth_hi, &earth_hi_off); rsxAddressToOffset(earth_lo, &earth_lo_off);
	return true;
}

void gpu_set_places(const Place *p, int n, const float *uvd, const float *uvg)
{
	/* glow quad for big cities + core quad for every city */
	int nglow = 0; for (int i = 0; i < n; i++) if (p[i].size >= 12) nglow++;
	int nq = n + nglow;
	DV *v = (DV *)rsxMemalign(128, (size_t)nq * 4 * sizeof(DV));
	if (!v) return;
	const float d = 3.14159265f / 180.f;
	const float cxs[4] = { -1, 1, 1, -1 }, cys[4] = { -1, -1, 1, 1 };
	int k = 0;
	for (int pass = 0; pass < 2; pass++) for (int i = 0; i < n; i++) {
		float t = p[i].size >= 40 ? 1.f : p[i].size / 40.f;
		if (pass == 0 && p[i].size < 12) continue;
		float x = cosf(p[i].lat * d) * cosf(p[i].lon * d), y = cosf(p[i].lat * d) * sinf(p[i].lon * d), z = sinf(p[i].lat * d);
		float rad = 0.9f + 1.4f * sqrtf(t);
		float size = pass == 0 ? rad * 2.4f + 1.f : rad + 1.f;            /* quad half-size in px */
		const float *uv = pass == 0 ? uvg : uvd;                          /* u0 v0 u1 v1 */
		float a = pass == 0 ? 0.16f : 0.65f + 0.35f * t;
		float cr = (0.18f + (0.61f - 0.18f) * t), cg = (0.83f + (1.0f - 0.83f) * t), cb = (0.60f + (0.84f - 0.60f) * t);
		for (int c = 0; c < 4; c++) {
			DV &q = v[k * 4 + c];
			q.x = x; q.y = y; q.z = z;
			q.u = cxs[c] < 0 ? uv[0] : uv[2]; q.v = cys[c] < 0 ? uv[1] : uv[3];
			q.cx = cxs[c]; q.cy = cys[c]; q.size = size;
			q.r = cr * a; q.g = cg * a; q.b = cb * a; q.a = a;
		}
		k++;
	}
	if (dverts) rsxFree(dverts);
	dverts = v; ndverts = k * 4;
	rsxAddressToOffset(dverts, &dverts_off);
}

static void set_env()
{
	rsxSetColorMask(context, GCM_COLOR_MASK_B | GCM_COLOR_MASK_G | GCM_COLOR_MASK_R | GCM_COLOR_MASK_A);
	rsxSetColorMaskMrt(context, 0);
	f32 scale[4] = { W * 0.5f, H * -0.5f, 0.5f, 0.f }, offset[4] = { W * 0.5f, H * 0.5f, 0.5f, 0.f };
	rsxSetViewport(context, 0, 0, W, H, 0.f, 1.f, scale, offset);
	rsxSetScissor(context, 0, 0, W, H);
	for (int i = 0; i < 8; i++) rsxSetViewportClip(context, i, W, H);
	rsxSetZMinMaxControl(context, GCM_FALSE, GCM_TRUE, GCM_FALSE);
	rsxSetShadeModel(context, GCM_SHADE_MODEL_SMOOTH);
	rsxSetFrontFace(context, GCM_FRONTFACE_CCW);
	rsxSetCullFaceEnable(context, GCM_FALSE);
	rsxSetUserClipPlaneControl(context, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
	                           GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE);
}
static void blend_premul(bool on)
{
	rsxSetBlendEnable(context, on ? GCM_TRUE : GCM_FALSE);
	if (on) { rsxSetBlendFunc(context, GCM_ONE, GCM_ONE_MINUS_SRC_ALPHA, GCM_ONE, GCM_ONE_MINUS_SRC_ALPHA); rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD); }
}

/* sprites batched per texture, in order */
static void draw_sprites(const Sprite *s, int ns)
{
	if (ns <= 0) return;
	if (scursor + ns > MAX_SPRITES) ns = MAX_SPRITES - scursor;
	if (ns <= 0) return;
	SV *v = sverts[sframe] + scursor * 4; u32 voff = sverts_off[sframe] + scursor * 4 * sizeof(SV);
	scursor += ns;
	for (int i = 0; i < ns; i++) {
		const Sprite &q = s[i];
		float cx = q.x + q.w / 2, cy = q.y + q.h / 2, hw = q.w / 2, hh = q.h / 2;
		float cs = cosf(q.rot), sn = sinf(q.rot);
		const float ox[4] = { -hw, hw, hw, -hw }, oy[4] = { -hh, -hh, hh, hh };
		const float us[4] = { q.u0, q.u1, q.u1, q.u0 }, vs[4] = { q.v0, q.v0, q.v1, q.v1 };
		for (int c = 0; c < 4; c++) {
			SV &o = v[i * 4 + c];
			o.x = cx + ox[c] * cs - oy[c] * sn; o.y = cy + ox[c] * sn + oy[c] * cs;
			o.u = us[c]; o.v = vs[c]; o.r = q.r; o.g = q.g; o.b = q.b; o.a = q.a;
		}
	}
	rsxLoadVertexProgram(context, P2D.vp, P2D.vucode);
	float scr[4] = { 2.f / W, 2.f / H, 0, 0 };
	rsxSetVertexProgramParameter(context, P2D.vp, c2d_scr, scr);
	rsxLoadFragmentProgramLocation(context, P2D.fp, P2D.foff, GCM_LOCATION_RSX);
	attrs_clear();
	attr(GCM_VERTEX_ATTRIB_POS, voff, sizeof(SV), 2);
	attr(GCM_VERTEX_ATTRIB_TEX0, voff + 8, sizeof(SV), 2);
	attr(GCM_VERTEX_ATTRIB_COLOR0, voff + 16, sizeof(SV), 4);
	int start = 0;
	while (start < ns) {
		int tex = s[start].tex, end = start + 1;
		while (end < ns && s[end].tex == tex) end++;
		Tex &t = T[tex];
		if (t.mem[0]) {
			rsxInvalidateTextureCache(context, GCM_INVALIDATE_TEXTURE);
			bind_tex(u2d_tex, t.off[t.cur], t.w, t.h, false, tex == TEX_ATLAS, false);
			rsxDrawVertexArray(context, GCM_TYPE_QUADS, start * 4, (end - start) * 4);
		}
		start = end;
	}
}

void gpu_frame(const GlobeView &g, const Sprite *bg, int nbg, const Sprite *s, int ns)
{
	sframe ^= 1; scursor = 0;
	set_env();
	rsxSetClearColor(context, 0x03060D);
	rsxSetClearDepthStencil(context, 0xffff);
	rsxClearSurface(context, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A | GCM_CLEAR_S | GCM_CLEAR_Z);
	rsxSetDepthTestEnable(context, GCM_FALSE);
	rsxSetDepthWriteEnable(context, GCM_FALSE);
	blend_premul(true);
	rsxInvalidateVertexCache(context);

	/* 1. background: stars + atmosphere glow */
	draw_sprites(bg, nbg);

	/* 2. globe */
	const float d = 3.14159265f / 180.f;
	float sl = sinf(g.lon * d), cl = cosf(g.lon * d), sp = sinf(g.lat * d), cp = cosf(g.lat * d);
	float east[4] = { -sl, cl, 0, g.R }, north[4] = { -sp * cl, -sp * sl, cp, 0 }, view[4] = { cp * cl, cp * sl, sp, 0 };
	float scr[4] = { g.cx, g.cy, 2.f / W, 2.f / H };
	if (g.show_globe) {
		blend_premul(false);
		rsxSetDepthTestEnable(context, GCM_TRUE); rsxSetDepthFunc(context, GCM_LESS); rsxSetDepthWriteEnable(context, GCM_TRUE);
		rsxLoadVertexProgram(context, PGLOBE.vp, PGLOBE.vucode);
		rsxSetVertexProgramParameter(context, PGLOBE.vp, cg_east, east);
		rsxSetVertexProgramParameter(context, PGLOBE.vp, cg_north, north);
		rsxSetVertexProgramParameter(context, PGLOBE.vp, cg_view, view);
		rsxSetVertexProgramParameter(context, PGLOBE.vp, cg_scr, scr);
		rsxLoadFragmentProgramLocation(context, PGLOBE.fp, PGLOBE.foff, GCM_LOCATION_RSX);
		rsxInvalidateTextureCache(context, GCM_INVALIDATE_TEXTURE);
		bool hi = g.R * 2.f > 900.f || !earth_lo;
		if (earth_hi) bind_tex(ug_tex, hi ? earth_hi_off : earth_lo_off, hi ? 4096 : 1024, hi ? 2048 : 512, true, true, true);
		else bind_tex(ug_tex, earth_lo_off, 8, 8, true, true, true);    /* plain ocean fallback */
		attrs_clear();
		attr(GCM_VERTEX_ATTRIB_POS, gverts_off, sizeof(GV), 3);
		attr(GCM_VERTEX_ATTRIB_TEX0, gverts_off + 12, sizeof(GV), 2);
		rsxDrawIndexArray(context, GCM_TYPE_TRIANGLES, gidx_off, gnidx, GCM_INDEX_TYPE_16B, GCM_LOCATION_RSX);
		rsxSetDepthTestEnable(context, GCM_FALSE); rsxSetDepthWriteEnable(context, GCM_FALSE);
		blend_premul(true);

		/* 3. station dots (atlas texture) */
		if (dverts && T[TEX_ATLAS].mem[0]) {
			rsxLoadVertexProgram(context, PDOTS.vp, PDOTS.vucode);
			rsxSetVertexProgramParameter(context, PDOTS.vp, cd_east, east);
			rsxSetVertexProgramParameter(context, PDOTS.vp, cd_north, north);
			rsxSetVertexProgramParameter(context, PDOTS.vp, cd_view, view);
			rsxSetVertexProgramParameter(context, PDOTS.vp, cd_scr, scr);
			float ds[4] = { g.dot_scale, 0, 0, 0 };
			rsxSetVertexProgramParameter(context, PDOTS.vp, cd_scale, ds);
			rsxLoadFragmentProgramLocation(context, P2D.fp, P2D.foff, GCM_LOCATION_RSX);
			rsxInvalidateTextureCache(context, GCM_INVALIDATE_TEXTURE);
			bind_tex(u2d_tex, T[TEX_ATLAS].off[0], T[TEX_ATLAS].w, T[TEX_ATLAS].h, false, true, false);
			attrs_clear();
			attr(GCM_VERTEX_ATTRIB_POS, dverts_off, sizeof(DV), 3);
			attr(GCM_VERTEX_ATTRIB_TEX0, dverts_off + 12, sizeof(DV), 2);
			attr(GCM_VERTEX_ATTRIB_TEX1, dverts_off + 20, sizeof(DV), 3);
			attr(GCM_VERTEX_ATTRIB_COLOR0, dverts_off + 32, sizeof(DV), 4);
			rsxDrawVertexArray(context, GCM_TYPE_QUADS, 0, ndverts);
		}
	}

	/* 4. UI layer + dynamic sprites */
	draw_sprites(s, ns);
}
