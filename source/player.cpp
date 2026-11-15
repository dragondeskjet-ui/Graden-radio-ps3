/* Streaming player: network thread -> byte ring -> decoder thread -> PCM ring -> audio thread.
 * Formats: MP3 (libmad), AAC/HE-AAC ADTS (faad2), Ogg Vorbis (vorbisfile),
 * HLS (m3u8 with MPEG-TS or packed ADTS/MP3 segments), PLS/M3U playlists. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/thread.h>
#include <audio/audio.h>
#include <mad.h>
#include <neaacdec.h>
#include <vorbis/vorbisfile.h>
#include "player.h"
#include "http.h"

#define OUT_RATE   48000.0
#define NET_RING   (1024 * 1024)
#define PCM_FRAMES 131072              /* power of two, ~2.7 s @48k */
#define PREBUFFER  (48000)             /* 1 s before starting */
#define AHEAD      3                   /* audio blocks written ahead of hardware */

static volatile int s_state = P_IDLE;
static char s_msg[128] = "";
static char s_fmt[16] = "";
static volatile int s_stop = 0;
static volatile int s_net_done = 0;
static volatile float s_volume = 0.8f;
static volatile int s_quit = 0;

static void mb() { __sync_synchronize(); }
static void set_msg(const char *m) { snprintf(s_msg, sizeof s_msg, "%s", m); }
static void fail(const char *m) { if (s_state != P_ERROR && !s_stop) { set_msg(m); s_state = P_ERROR; } }

/* ================= byte ring ================= */
static unsigned char *net_ring = NULL;
static volatile unsigned long net_w = 0, net_r = 0;

static int ring_put(const unsigned char *p, size_t len)
{
	size_t done = 0;
	while (done < len) {
		if (s_stop) return 0;
		size_t fr = NET_RING - (net_w - net_r);
		if (!fr) { usleep(5000); continue; }
		size_t c = len - done < fr ? len - done : fr;
		for (size_t i = 0; i < c; i++) net_ring[(net_w + i) % NET_RING] = p[done + i];
		mb(); net_w += c; done += c;
	}
	return 1;
}

/* Blocking read: waits until at least 1 byte or stream end. Returns 0 at end/stop. */
static size_t ring_read(unsigned char *dst, size_t n)
{
	for (;;) {
		if (s_stop) return 0;
		size_t av = net_w - net_r;
		if (av) {
			size_t c = n < av ? n : av;
			for (size_t i = 0; i < c; i++) dst[i] = net_ring[(net_r + i) % NET_RING];
			mb(); net_r += c;
			return c;
		}
		if (s_net_done) return 0;
		usleep(5000);
	}
}

/* Fill buf up to want bytes (keeps existing have). Returns new have. */
static size_t fill(unsigned char *buf, size_t have, size_t want)
{
	while (have < want && !s_stop) {
		size_t got = ring_read(buf + have, want - have);
		if (!got) break;
		have += got;
		if (have >= 4096 && (net_w - net_r) == 0) break; /* don't stall on partial fill */
	}
	return have;
}

/* ================= PCM ring + resampler ================= */
static float *pcm = NULL;
static volatile unsigned long pcm_w = 0, pcm_r = 0;
static double rs_phase, rs_l, rs_r;

static inline float clampf(float x) { return x > 1.f ? 1.f : (x < -1.f ? -1.f : x); }

static inline void pcm_push(float l, float r)
{
	while (!s_stop && (pcm_w - pcm_r) >= PCM_FRAMES) usleep(4000);
	if (s_stop) return;
	size_t i = (pcm_w & (PCM_FRAMES - 1)) * 2;
	pcm[i] = clampf(l); pcm[i + 1] = clampf(r);
	mb(); pcm_w++;
	if (s_state == P_BUFFERING && (pcm_w - pcm_r) >= PREBUFFER) s_state = P_PLAYING;
}

static inline void emit_frame(double l, double r, unsigned rate)
{
	const double step = (double)rate / OUT_RATE;
	while (rs_phase < 1.0) {
		pcm_push((float)(rs_l + (l - rs_l) * rs_phase), (float)(rs_r + (r - rs_r) * rs_phase));
		rs_phase += step;
	}
	rs_phase -= 1.0; rs_l = l; rs_r = r;
}

static void set_playing_msg() { char m[64]; snprintf(m, sizeof m, "Reproduciendo (%s)", s_fmt); set_msg(m); }

/* ================= decoders ================= */
#define INBUF 32768

static size_t skip_id3(unsigned char *in, size_t have)
{
	while (have >= 10 && !memcmp(in, "ID3", 3)) {
		size_t sz = ((in[6] & 0x7F) << 21) | ((in[7] & 0x7F) << 14) | ((in[8] & 0x7F) << 7) | (in[9] & 0x7F);
		sz += 10 + ((in[5] & 0x10) ? 10 : 0);
		if (sz <= have) { memmove(in, in + sz, have - sz); have -= sz; continue; }
		size_t left = sz - have; have = 0;
		unsigned char tmp[4096];
		while (left && !s_stop) { size_t g = ring_read(tmp, left < sizeof tmp ? left : sizeof tmp); if (!g) break; left -= g; }
		have = fill(in, 0, INBUF);
	}
	return have;
}

static void dec_mp3(unsigned char *in, size_t have)
{
	mad_stream st; mad_frame fr; mad_synth sy;
	mad_stream_init(&st); mad_frame_init(&fr); mad_synth_init(&sy);
	const double k = 1.0 / (double)(1L << MAD_F_FRACBITS);
	int ok_frames = 0, errs = 0;
	while (!s_stop) {
		have = fill(in, have, INBUF);
		if (have == 0) break;
		mad_stream_buffer(&st, in, have);
		st.error = MAD_ERROR_NONE;
		for (;;) {
			if (mad_frame_decode(&fr, &st)) {
				if (MAD_RECOVERABLE(st.error)) { errs++; continue; }
				break;
			}
			mad_synth_frame(&sy, &fr);
			if (ok_frames++ == 0) set_playing_msg();
			for (unsigned i = 0; i < sy.pcm.length && !s_stop; i++) {
				double l = sy.pcm.samples[0][i] * k;
				double r = sy.pcm.channels > 1 ? sy.pcm.samples[1][i] * k : l;
				emit_frame(l, r, fr.header.samplerate);
			}
		}
		size_t rem = st.next_frame ? (size_t)(st.bufend - st.next_frame) : 0;
		if (rem == have) { if (s_net_done && (net_w - net_r) == 0) break; rem = rem > 1 ? rem - 1 : 0; memmove(in, in + (have - rem), rem); }
		else memmove(in, st.next_frame, rem);
		have = rem;
		if (ok_frames == 0 && errs > 2000) { fail("No es un stream MP3 valido"); break; }
	}
	mad_synth_finish(&sy); mad_frame_finish(&fr); mad_stream_finish(&st);
}

static size_t adts_sync(unsigned char *in, size_t have)
{
	size_t i = 0;
	while (i + 1 < have && !(in[i] == 0xFF && (in[i + 1] & 0xF6) == 0xF0)) i++;
	if (i) { memmove(in, in + i, have - i); have -= i; }
	return have;
}

static void dec_aac(unsigned char *in, size_t have)
{
	NeAACDecHandle h = NeAACDecOpen();
	NeAACDecConfigurationPtr cfg = NeAACDecGetCurrentConfiguration(h);
	cfg->outputFormat = FAAD_FMT_FLOAT;
	cfg->downMatrix = 1;
	NeAACDecSetConfiguration(h, cfg);
	have = adts_sync(in, fill(in, have, INBUF));
	unsigned long sr = 0; unsigned char ch = 0;
	long used = NeAACDecInit(h, in, have, &sr, &ch);
	if (used < 0) { fail("AAC: no se pudo iniciar el decodificador"); NeAACDecClose(h); return; }
	memmove(in, in + used, have - used); have -= used;
	int ok_frames = 0, errs = 0;
	while (!s_stop) {
		if (have < 8192) have = fill(in, have, INBUF);
		if (have < 8) break;
		NeAACDecFrameInfo fi;
		float *out = (float *)NeAACDecDecode(h, &fi, in, have);
		if (fi.error) {
			if (++errs > 500 && ok_frames == 0) { char m[96]; snprintf(m, sizeof m, "AAC: %s", NeAACDecGetErrorMessage(fi.error)); fail(m); break; }
			memmove(in, in + 1, have - 1); have = adts_sync(in, have - 1);
			continue;
		}
		size_t c = fi.bytesconsumed > have ? have : fi.bytesconsumed;
		if (c == 0 && fi.samples == 0) { if (s_net_done && (net_w - net_r) == 0) break; memmove(in, in + 1, have - 1); have--; continue; }
		memmove(in, in + c, have - c); have -= c;
		if (out && fi.samples > 0 && fi.channels > 0) {
			if (ok_frames++ == 0) set_playing_msg();
			unsigned frames = fi.samples / fi.channels;
			for (unsigned i = 0; i < frames && !s_stop; i++) {
				double l = out[i * fi.channels];
				double r = fi.channels > 1 ? out[i * fi.channels + 1] : l;
				emit_frame(l, r, fi.samplerate);
			}
		}
	}
	NeAACDecClose(h);
}

/* vorbisfile streaming callbacks: prebuffered bytes first, then the ring */
static unsigned char *ov_pre; static size_t ov_pre_n, ov_pre_pos;
static size_t ov_readcb(void *ptr, size_t size, size_t nmemb, void *ds)
{
	size_t want = size * nmemb;
	if (ov_pre_pos < ov_pre_n) {
		size_t c = ov_pre_n - ov_pre_pos < want ? ov_pre_n - ov_pre_pos : want;
		memcpy(ptr, ov_pre + ov_pre_pos, c); ov_pre_pos += c; return c / size;
	}
	return ring_read((unsigned char *)ptr, want) / size;
}

static void dec_ogg(unsigned char *in, size_t have)
{
	have = fill(in, have, INBUF);
	if (have >= 36 && memmem(in, have, "OpusHead", 8)) { fail("Ogg Opus no soportado"); return; }
	ov_pre = in; ov_pre_n = have; ov_pre_pos = 0;
	ov_callbacks cb = { ov_readcb, NULL, NULL, NULL };
	OggVorbis_File vf;
	if (ov_open_callbacks(NULL, &vf, NULL, 0, cb) < 0) { fail("Ogg: no es Vorbis valido"); return; }
	int bs = 0, ok = 0;
	while (!s_stop) {
		float **p;
		long n = ov_read_float(&vf, &p, 2048, &bs);
		if (n == 0) break;
		if (n < 0) { if (n == OV_EINVAL || n == OV_EBADLINK) break; continue; }
		vorbis_info *vi = ov_info(&vf, -1);
		if (!vi) break;
		if (ok++ == 0) set_playing_msg();
		for (long i = 0; i < n && !s_stop; i++) {
			double l = p[0][i], r = vi->channels > 1 ? p[1][i] : l;
			emit_frame(l, r, vi->rate);
		}
	}
	ov_clear(&vf);
}

static void dec_thread(void *arg)
{
	unsigned char *in = (unsigned char *)malloc(INBUF + MAD_BUFFER_GUARD + 16);
	rs_phase = 0; rs_l = rs_r = 0;
	size_t have = in ? fill(in, 0, 8192) : 0;
	if (in && have) have = skip_id3(in, have);
	if (!in || !have) { fail(s_net_done ? "El stream no envio datos" : "Sin memoria"); }
	else if (have >= 4 && !memcmp(in, "OggS", 4)) { strcpy(s_fmt, "OGG"); dec_ogg(in, have); }
	else {
		/* find first frame sync and decide MP3 vs ADTS AAC */
		int kind = 0;
		for (size_t i = 0; i + 1 < have; i++) {
			if (in[i] == 0xFF && (in[i + 1] & 0xE0) == 0xE0) {
				if ((in[i + 1] & 0x06) == 0) { if ((in[i + 1] & 0xF0) == 0xF0) { kind = 2; break; } }
				else { kind = 1; break; }
			}
		}
		if (kind == 0) {
			if (!memcmp(in, "<", 1) || !strncasecmp((char *)in, "#EXTM3U", 7)) fail("Respuesta no es audio");
			else if (have > 8 && !memcmp(in + 4, "ftyp", 4)) fail("AAC en MP4 no soportado");
			else fail("Formato de audio desconocido");
		} else if (kind == 2) { if (!s_fmt[0]) strcpy(s_fmt, "AAC"); dec_aac(in, have); }
		else { if (!s_fmt[0]) strcpy(s_fmt, "MP3"); dec_mp3(in, have); }
	}
	if (!s_stop && s_state != P_ERROR) fail("Stream terminado");
	free(in);
	sysThreadExit(0);
}

/* ================= network ================= */
static char s_url[1024];

/* MPEG-TS demuxer -> elementary audio stream (ADTS AAC or MP3) */
struct Ts { unsigned char pkt[188]; int n; int pmt_pid; int apid; int pes_skip; bool pes_hdr; unsigned char hdr[32]; int hn; };
static void ts_reset(Ts *t) { memset(t, 0, sizeof *t); t->pmt_pid = -1; t->apid = -1; }

static void ts_packet(Ts *t, const unsigned char *p)
{
	if (p[0] != 0x47) return;
	int pusi = p[1] & 0x40, pid = ((p[1] & 0x1F) << 8) | p[2], afc = (p[3] >> 4) & 3;
	int off = 4;
	if (afc == 0 || afc == 2) return;
	if (afc == 3) off += 1 + p[4];
	if (off >= 188) return;
	if (pid == 0 && pusi) {                       /* PAT */
		int o = off + 1 + p[off];
		int len = ((p[o + 1] & 0x0F) << 8) | p[o + 2];
		for (int i = o + 8; i + 4 <= o + 3 + len - 4 && i + 4 <= 188; i += 4) {
			int prog = (p[i] << 8) | p[i + 1];
			if (prog) { t->pmt_pid = ((p[i + 2] & 0x1F) << 8) | p[i + 3]; break; }
		}
		return;
	}
	if (pid == t->pmt_pid && pusi && t->apid < 0) { /* PMT */
		int o = off + 1 + p[off];
		int len = ((p[o + 1] & 0x0F) << 8) | p[o + 2];
		int pil = ((p[o + 10] & 0x0F) << 8) | p[o + 11];
		int end = o + 3 + len - 4; if (end > 188) end = 188;
		for (int i = o + 12 + pil; i + 5 <= end;) {
			int type = p[i], epid = ((p[i + 1] & 0x1F) << 8) | p[i + 2], eil = ((p[i + 3] & 0x0F) << 8) | p[i + 4];
			if (type == 0x0F || type == 0x03 || type == 0x04) { t->apid = epid; break; }
			i += 5 + eil;
		}
		return;
	}
	if (pid != t->apid) return;
	const unsigned char *d = p + off; int n = 188 - off;
	if (pusi) { t->pes_hdr = true; t->hn = 0; }
	if (t->pes_hdr) {                             /* collect 9-byte PES header, then skip optional part */
		while (n && t->hn < 9) { t->hdr[t->hn++] = *d++; n--; }
		if (t->hn < 9) return;
		t->pes_skip = t->hdr[8]; t->pes_hdr = false;
	}
	if (t->pes_skip) { int s = t->pes_skip < n ? t->pes_skip : n; d += s; n -= s; t->pes_skip -= s; }
	if (n > 0) ring_put(d, n);
}

/* Per-request sink. mode: 0 = raw stream, 1 = capture (playlist), 2 = segment (TS or packed audio) */
struct Sink { int mode; Buf cap; bool decided; bool ts; Ts tsd; long id3_skip; unsigned char head[10]; int hn; char ctype[96]; };

static bool is_playlist_ctype(const char *c)
{
	return strcasestr(c, "mpegurl") || strcasestr(c, "scpls") || strcasestr(c, "x-pls") || strcasestr(c, "audio/x-mpegurl");
}

static size_t sink_hdr(char *p, size_t sz, size_t n, void *u)
{
	Sink *s = (Sink *)u; size_t len = sz * n;
	if (len >= 5 && (!strncasecmp(p, "HTTP/", 5) || !strncasecmp(p, "ICY ", 4))) s->ctype[0] = 0;
	else if (len > 13 && !strncasecmp(p, "content-type:", 13)) {
		size_t l = len - 13; if (l >= sizeof s->ctype) l = sizeof s->ctype - 1;
		memcpy(s->ctype, p + 13, l); s->ctype[l] = 0;
		for (char *q = s->ctype; *q; q++) if (*q == '\r' || *q == '\n') *q = 0;
	}
	return len;
}

static int cap_add(Buf *b, const char *p, size_t len)
{
	if (b->n + len + 1 > (256u << 10)) return 0;
	if (b->n + len + 1 > b->cap) {
		size_t nc = b->cap ? b->cap * 2 : 16384; while (nc < b->n + len + 1) nc *= 2;
		char *nd = (char *)realloc(b->d, nc); if (!nd) return 0; b->d = nd; b->cap = nc;
	}
	memcpy(b->d + b->n, p, len); b->n += len; b->d[b->n] = 0; return 1;
}

static size_t sink_write(char *p, size_t sz, size_t n, void *u)
{
	Sink *s = (Sink *)u; size_t len = sz * n;
	if (s_stop) return 0;
	if (s->mode == 0 && !s->decided) {
		s->decided = true;
		if (is_playlist_ctype(s->ctype) || (len >= 7 && !strncasecmp(p, "#EXTM3U", 7)) || (len >= 10 && !strncasecmp(p, "[playlist]", 10)) || strcasestr(s->ctype, "text/"))
			s->mode = 1;
		else if (len > 188 * 2 && (unsigned char)p[0] == 0x47 && (unsigned char)p[188] == 0x47) { s->ts = true; ts_reset(&s->tsd); }
	}
	if (s->mode == 1) return cap_add(&s->cap, p, len) ? len : 0;
	const unsigned char *d = (const unsigned char *)p;
	if (s->mode == 2 && !s->decided) {
		/* strip leading ID3 (HLS packed audio timestamps) and detect TS */
		while (len && s->hn < 10) { s->head[s->hn++] = *d++; len--; }
		if (s->hn < 10) return sz * n;
		s->decided = true;
		if (!memcmp(s->head, "ID3", 3)) {
			s->id3_skip = ((s->head[6] & 0x7F) << 21) | ((s->head[7] & 0x7F) << 14) | ((s->head[8] & 0x7F) << 7) | (s->head[9] & 0x7F);
		} else if (s->head[0] == 0x47) {
			s->ts = true; ts_reset(&s->tsd);
			memcpy(s->tsd.pkt, s->head, 10); s->tsd.n = 10;
		} else ring_put(s->head, 10);
	}
	if (s->id3_skip) { long k = s->id3_skip < (long)len ? s->id3_skip : (long)len; d += k; len -= k; s->id3_skip -= k; if (!len) return sz * n; }
	if (s->ts) {
		while (len) {
			int need = 188 - s->tsd.n, c = (int)len < need ? (int)len : need;
			memcpy(s->tsd.pkt + s->tsd.n, d, c); s->tsd.n += c; d += c; len -= c;
			if (s->tsd.n == 188) { ts_packet(&s->tsd, s->tsd.pkt); s->tsd.n = 0; }
			if (s_stop) return 0;
		}
	} else if (len && !ring_put(d, len)) return 0;
	return sz * n;
}

static int net_progress(void *, curl_off_t, curl_off_t, curl_off_t, curl_off_t) { return s_stop ? 1 : 0; }

/* Performs a GET into the sink. Returns curl code; final URL copied to eff. */
static CURLcode do_get(const char *url, Sink *s, char *eff, size_t efflen, long *code, char *cerr)
{
	CURL *c = curl_easy_init();
	if (!c) return CURLE_FAILED_INIT;
	http_setup(c);
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink_write);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, s);
	curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, sink_hdr);
	curl_easy_setopt(c, CURLOPT_HEADERDATA, s);
	curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, net_progress);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 20L);
	curl_easy_setopt(c, CURLOPT_ERRORBUFFER, cerr);
	cerr[0] = 0;
	CURLcode rc = curl_easy_perform(c);
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, code);
	char *e = NULL; curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &e);
	if (eff) snprintf(eff, efflen, "%s", e ? e : url);
	curl_easy_cleanup(c);
	return rc;
}

static void resolve(const char *base, const char *rel, char *out, size_t n)
{
	if (strstr(rel, "://")) { snprintf(out, n, "%s", rel); return; }
	const char *sch = strstr(base, "://");
	if (!sch) { snprintf(out, n, "%s", rel); return; }
	if (rel[0] == '/' && rel[1] == '/') { snprintf(out, n, "%.*s%s", (int)(sch - base + 1), base, rel); return; }
	const char *host_end = strchr(sch + 3, '/');
	if (rel[0] == '/') { int hl = host_end ? (int)(host_end - base) : (int)strlen(base); snprintf(out, n, "%.*s%s", hl, base, rel); return; }
	const char *q = strchr(base, '?'); int bl = q ? (int)(q - base) : (int)strlen(base);
	int dl = bl; while (dl > 0 && base[dl - 1] != '/') dl--;
	if (host_end == NULL || dl <= (int)(sch - base) + 3) snprintf(out, n, "%.*s/%s", bl, base, rel);
	else snprintf(out, n, "%.*s%s", dl, base, rel);
}

static char *trim(char *l) { while (*l == ' ' || *l == '\t') l++; char *e = l + strlen(l); while (e > l && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0; return l; }

/* First stream URL in a PLS or plain M3U, or HLS detection. Returns 1 = got url, 2 = HLS media, 3 = HLS master, 0 = none */
static int parse_playlist(char *txt, const char *base, char *out, size_t n)
{
	if (strstr(txt, "#EXT-X-STREAM-INF")) {
		/* master: choose lowest BANDWIDTH variant (radio usually audio-only) */
		long best = -1; char *line = strtok(txt, "\n"); int want = 0; long bw = 0;
		for (; line; line = strtok(NULL, "\n")) {
			char *l = trim(line);
			if (!strncmp(l, "#EXT-X-STREAM-INF", 17)) { want = 1; char *b = strstr(l, "BANDWIDTH="); bw = b ? atol(b + 10) : 0; continue; }
			if (want && *l && *l != '#') { if (best < 0 || bw < best) { best = bw; resolve(base, l, out, n); } want = 0; }
		}
		return best >= 0 ? 3 : 0;
	}
	if (strstr(txt, "#EXTINF") && (strstr(txt, "#EXT-X-TARGETDURATION") || strstr(txt, "#EXT-X-MEDIA-SEQUENCE"))) { snprintf(out, n, "%s", base); return 2; }
	for (char *line = strtok(txt, "\n"); line; line = strtok(NULL, "\n")) {
		char *l = trim(line);
		if (!strncasecmp(l, "File", 4)) { char *eq = strchr(l, '='); if (eq) { resolve(base, trim(eq + 1), out, n); return 1; } }
		if (*l && *l != '#' && *l != '[' && strstr(l, "://")) { snprintf(out, n, "%s", l); return 1; }
		if (*l && *l != '#' && *l != '[' && !strchr(l, '=')) { resolve(base, l, out, n); return 1; }
	}
	return 0;
}

static bool sleep_check(int ms) { while (ms > 0 && !s_stop) { usleep(50000); ms -= 50; } return !s_stop; }

static void run_hls(const char *media_url)
{
	strcpy(s_fmt, "HLS");
	long long last_seq = -1; int empty_rounds = 0;
	char cerr[CURL_ERROR_SIZE];
	while (!s_stop) {
		Sink pl; memset(&pl, 0, sizeof pl); pl.mode = 1; pl.decided = true;
		long code = 0; char eff[1024];
		CURLcode rc = do_get(media_url, &pl, eff, sizeof eff, &code, cerr);
		if (s_stop) { buf_free(&pl.cap); break; }
		if (rc != CURLE_OK || code >= 400 || !pl.cap.d) { buf_free(&pl.cap); if (++empty_rounds > 5) { fail("HLS: no se pudo leer la lista"); break; } sleep_check(2000); continue; }
		char *txt = pl.cap.d;
		if (strstr(txt, "#EXT-X-KEY") && !strstr(txt, "METHOD=NONE")) { buf_free(&pl.cap); fail("HLS cifrado no soportado"); break; }
		if (strstr(txt, "#EXT-X-MAP")) { buf_free(&pl.cap); fail("HLS fMP4 no soportado"); break; }
		long long seq = 0; int target = 6; bool endlist = strstr(txt, "#EXT-X-ENDLIST") != NULL;
		char *m = strstr(txt, "#EXT-X-MEDIA-SEQUENCE:"); if (m) seq = atoll(m + 22);
		char *t = strstr(txt, "#EXT-X-TARGETDURATION:"); if (t) target = atoi(t + 22);
		if (target < 1) target = 1; if (target > 30) target = 30;
		/* collect segment URIs */
		char *segs[256]; int ns = 0;
		for (char *line = strtok(txt, "\n"); line && ns < 256; line = strtok(NULL, "\n")) { char *l = trim(line); if (*l && *l != '#') segs[ns++] = l; }
		int start = 0;
		if (last_seq < 0) start = ns > 3 ? ns - 3 : 0;           /* begin near live edge */
		int fetched = 0;
		for (int i = start; i < ns && !s_stop; i++) {
			long long sq = seq + i;
			if (sq <= last_seq) continue;
			char su[1024]; resolve(eff, segs[i], su, sizeof su);
			Sink sg; memset(&sg, 0, sizeof sg); sg.mode = 2;
			long sc = 0; char se[1024];
			CURLcode r2 = do_get(su, &sg, se, sizeof se, &sc, cerr);
			if (sg.ts && sg.tsd.apid < 0 && !s_stop) { buf_free(&pl.cap); fail("HLS: sin pista de audio compatible"); return; }
			last_seq = sq; fetched++;
			if (r2 != CURLE_OK && !s_stop) break;
		}
		buf_free(&pl.cap);
		if (endlist) break;
		empty_rounds = fetched ? 0 : empty_rounds + 1;
		if (empty_rounds > 10) { fail("HLS: la lista no avanza"); break; }
		sleep_check(fetched ? target * 500 : target * 1000);
	}
}

static void net_thread(void *arg)
{
	char url[1024]; snprintf(url, sizeof url, "%s", s_url);
	char cerr[CURL_ERROR_SIZE];
	for (int hop = 0; hop < 6 && !s_stop; hop++) {
		Sink s; memset(&s, 0, sizeof s);
		long code = 0; char eff[1024];
		CURLcode rc = do_get(url, &s, eff, sizeof eff, &code, cerr);
		if (s_stop) { buf_free(&s.cap); break; }
		if (s.mode == 1) {
			char next[1024];
			int k = s.cap.d ? parse_playlist(s.cap.d, eff, next, sizeof next) : 0;
			buf_free(&s.cap);
			if (k == 1 || k == 3) { snprintf(url, sizeof url, "%s", next); continue; }
			if (k == 2) { run_hls(eff); break; }
			fail("Lista de reproduccion vacia o no soportada");
			break;
		}
		if (rc != CURLE_OK) { char m[128]; snprintf(m, sizeof m, "%s", cerr[0] ? cerr : curl_easy_strerror(rc)); fail(m); }
		else if (code >= 400) { char m[64]; snprintf(m, sizeof m, "HTTP %ld", code); fail(m); }
		break;
	}
	s_net_done = 1;
	sysThreadExit(0);
}

/* ================= audio output thread (persistent) ================= */
static audioPortConfig acfg;
static u32 aport;
static sys_ppu_thread_t th_net, th_dec, th_audio;
static int net_started = 0, dec_started = 0;

static void fill_block(u32 blk)
{
	float *buf = (float *)(u64)acfg.audioDataStart + 2 * AUDIO_BLOCK_SAMPLES * blk;
	float vol = s_volume;
	unsigned long have = pcm_w - pcm_r;
	if (s_state == P_PLAYING && have >= AUDIO_BLOCK_SAMPLES) {
		for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
			size_t p = ((pcm_r + i) & (PCM_FRAMES - 1)) * 2;
			buf[i * 2] = pcm[p] * vol; buf[i * 2 + 1] = pcm[p + 1] * vol;
		}
		mb(); pcm_r += AUDIO_BLOCK_SAMPLES;
	} else {
		memset(buf, 0, sizeof(float) * 2 * AUDIO_BLOCK_SAMPLES);
		if (s_state == P_PLAYING) { s_state = P_BUFFERING; set_msg("Cargando buffer..."); }
	}
}

static void audio_thread(void *arg)
{
	/* readIndex holds the address of a u64 block counter (must be read as 64-bit) */
	volatile u64 *ridx = (volatile u64 *)(u64)acfg.readIndex;
	u32 nb = (u32)acfg.numBlocks;
	u32 wr = ((u32)*ridx + 1) % nb;
	while (!s_quit) {
		u32 cur = (u32)(*ridx % nb);
		u32 target = (cur + AHEAD) % nb;
		bool wrote = false;
		while (wr != target) {
			if (wr == cur) { wr = (wr + 1) % nb; continue; } /* never touch the block being played */
			fill_block(wr); wr = (wr + 1) % nb; wrote = true;
		}
		if (!wrote) usleep(1000);
	}
	sysThreadExit(0);
}

/* ================= public API ================= */
bool player_init()
{
	net_ring = (unsigned char *)malloc(NET_RING);
	pcm = (float *)malloc(sizeof(float) * 2 * PCM_FRAMES);
	if (!net_ring || !pcm) return false;
	if (audioInit() != 0) return false;
	audioPortParam prm;
	memset(&prm, 0, sizeof prm);
	prm.numChannels = AUDIO_PORT_2CH;
	prm.numBlocks = AUDIO_BLOCK_8;
	prm.attrib = 0;
	prm.level = 1.0f;
	if (audioPortOpen(&prm, &aport) != 0) return false;
	if (audioGetPortConfig(aport, &acfg) != 0) return false;
	memset((void *)(u64)acfg.audioDataStart, 0, acfg.portSize);
	if (audioPortStart(aport) != 0) return false;
	return sysThreadCreate(&th_audio, audio_thread, NULL, 900, 0x10000, THREAD_JOINABLE, (char *)"rg_audio") == 0;
}

void player_stop()
{
	s_stop = 1;
	if (net_started) { u64 r; sysThreadJoin(th_net, &r); net_started = 0; }
	if (dec_started) { u64 r; sysThreadJoin(th_dec, &r); dec_started = 0; }
	s_state = P_IDLE;
	pcm_r = pcm_w; net_r = net_w;
}

void player_play(const char *url)
{
	player_stop();
	snprintf(s_url, sizeof s_url, "%s", url);
	s_fmt[0] = 0; s_net_done = 0; s_stop = 0;
	net_r = net_w = 0; pcm_r = pcm_w = 0;
	set_msg("Conectando...");
	s_state = P_BUFFERING;
	if (sysThreadCreate(&th_net, net_thread, NULL, 1000, 0x80000, THREAD_JOINABLE, (char *)"rg_net") != 0) { fail("thread red"); return; }
	net_started = 1;
	if (sysThreadCreate(&th_dec, dec_thread, NULL, 950, 0x80000, THREAD_JOINABLE, (char *)"rg_dec") != 0) { fail("thread decodificador"); return; }
	dec_started = 1;
}

PState player_state() { return (PState)s_state; }
const char *player_message() { return s_msg; }
const char *player_format() { return s_fmt; }
void player_set_volume(float v) { s_volume = v < 0 ? 0 : (v > 1 ? 1 : v); }
float player_get_volume() { return s_volume; }

void player_shutdown()
{
	player_stop();
	s_quit = 1;
	u64 r; sysThreadJoin(th_audio, &r);
	audioPortStop(aport); audioPortClose(aport); audioQuit();
}
