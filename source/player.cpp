#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/thread.h>
#include <audio/audio.h>
#include <mad.h>
#include "player.h"
#include "http.h"

#define OUT_RATE   48000.0
#define NET_RING   (512 * 1024)
#define PCM_FRAMES 65536               /* power of two, ~1.36 s @48k */
#define PREBUFFER  (48000 / 2)

static volatile int s_state = P_IDLE;
static char s_msg[128] = "";
static volatile int s_stop = 0;
static volatile int s_net_done = 0;
static volatile float s_volume = 0.8f;
static volatile int s_quit = 0;

/* ---- byte ring: net thread -> decoder thread ---- */
static unsigned char *net_ring = NULL;
static volatile unsigned long net_w = 0, net_r = 0;

static size_t net_avail() { return net_w - net_r; }
static void net_barrier() { __sync_synchronize(); }

/* ---- pcm ring: decoder thread -> audio thread (stereo float) ---- */
static float *pcm = NULL;
static volatile unsigned long pcm_w = 0, pcm_r = 0;

static sys_ppu_thread_t th_net, th_dec, th_audio;
static int net_started = 0, dec_started = 0;

static void set_msg(const char *m) { snprintf(s_msg, sizeof s_msg, "%s", m); }

static void fail(const char *m)
{
	if (s_state != P_ERROR && !s_stop) { set_msg(m); s_state = P_ERROR; }
}

/* ================= network thread ================= */
static char s_url[512];
static char s_ctype[96];
static int s_unsupported = 0;

static size_t hdr_cb(char *p, size_t sz, size_t n, void *u)
{
	size_t len = sz * n;
	if (len >= 5 && !strncasecmp(p, "HTTP/", 5)) s_ctype[0] = 0;
	else if (len > 13 && !strncasecmp(p, "content-type:", 13)) {
		size_t l = len - 13; if (l >= sizeof s_ctype) l = sizeof s_ctype - 1;
		memcpy(s_ctype, p + 13, l); s_ctype[l] = 0;
		for (char *q = s_ctype; *q; q++) if (*q == '\r' || *q == '\n') *q = 0;
	}
	return len;
}

static size_t net_write(char *p, size_t sz, size_t n, void *u)
{
	size_t len = sz * n;
	if (s_stop) return 0;
	if (!s_unsupported && s_ctype[0]) {
		if (strcasestr(s_ctype, "aac") || strcasestr(s_ctype, "mp4") || strcasestr(s_ctype, "ogg") ||
		    strcasestr(s_ctype, "mpegurl") || strcasestr(s_ctype, "text/html")) {
			s_unsupported = 1;
			return 0;
		}
	}
	size_t done = 0;
	while (done < len) {
		if (s_stop) return 0;
		size_t free_b = NET_RING - net_avail();
		if (free_b == 0) { usleep(5000); continue; }
		size_t c = len - done < free_b ? len - done : free_b;
		for (size_t i = 0; i < c; i++) net_ring[(net_w + i) % NET_RING] = (unsigned char)p[done + i];
		net_barrier();
		net_w += c;
		done += c;
	}
	return len;
}

static int net_progress(void *u, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) { return s_stop ? 1 : 0; }

static void net_thread(void *arg)
{
	CURL *c = curl_easy_init();
	if (c) {
		http_setup(c);
		curl_easy_setopt(c, CURLOPT_URL, s_url);
		curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, net_write);
		curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, hdr_cb);
		curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, net_progress);
		curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
		curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 20L);
		char cerr[CURL_ERROR_SIZE] = {0};
		curl_easy_setopt(c, CURLOPT_ERRORBUFFER, cerr);
		CURLcode rc = curl_easy_perform(c);
		long code = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
		if (s_unsupported) { char m[128]; snprintf(m, sizeof m, "Formato no soportado (%s)", s_ctype); fail(m); }
		else if (!s_stop && rc != CURLE_OK) { char m[128]; snprintf(m, sizeof m, "%s", cerr[0] ? cerr : curl_easy_strerror(rc)); fail(m); }
		else if (!s_stop && code >= 400) { char m[64]; snprintf(m, sizeof m, "HTTP %ld", code); fail(m); }
		curl_easy_cleanup(c);
	} else fail("curl init");
	s_net_done = 1;
	sysThreadExit(0);
}

/* ================= decoder thread ================= */
static mad_stream g_stream; static mad_frame g_frame; static mad_synth g_synth;

static double rs_phase, rs_prev_l, rs_prev_r;

static inline void pcm_push(float l, float r)
{
	while (!s_stop && (pcm_w - pcm_r) >= PCM_FRAMES) usleep(4000);
	if (s_stop) return;
	size_t i = (pcm_w & (PCM_FRAMES - 1)) * 2;
	pcm[i] = l; pcm[i + 1] = r;
	net_barrier();
	pcm_w++;
}

static inline float clampf(float x) { return x > 1.f ? 1.f : (x < -1.f ? -1.f : x); }

static void emit(mad_synth *s, unsigned rate)
{
	const double step = (double)rate / OUT_RATE;
	const float k = 1.0f / (float)(1 << MAD_F_FRACBITS);
	for (unsigned i = 0; i < s->pcm.length && !s_stop; i++) {
		double l = s->pcm.samples[0][i] * (double)k;
		double r = s->pcm.channels > 1 ? s->pcm.samples[1][i] * (double)k : l;
		while (rs_phase < 1.0) {
			float ol = (float)(rs_prev_l + (l - rs_prev_l) * rs_phase);
			float orr = (float)(rs_prev_r + (r - rs_prev_r) * rs_phase);
			pcm_push(clampf(ol), clampf(orr));
			rs_phase += step;
		}
		rs_phase -= 1.0;
		rs_prev_l = l; rs_prev_r = r;
	}
}

#define INBUF 16384

static void dec_thread(void *arg)
{
	unsigned char *in = (unsigned char *)malloc(INBUF + MAD_BUFFER_GUARD);
	size_t have = 0;
	rs_phase = 0; rs_prev_l = rs_prev_r = 0;
	mad_stream_init(&g_stream); mad_frame_init(&g_frame); mad_synth_init(&g_synth);
	int errs = 0;

	while (in && !s_stop) {
		size_t got = 0;
		size_t av = net_avail();
		if (av > 0 && have < INBUF) {
			got = INBUF - have < av ? INBUF - have : av;
			for (size_t i = 0; i < got; i++) in[have + i] = net_ring[(net_r + i) % NET_RING];
			net_barrier();
			net_r += got; have += got;
		}
		if (have < 1024) {
			if (s_net_done && av == 0) break;
			usleep(5000);
			if (got == 0) continue;
		}
		mad_stream_buffer(&g_stream, in, have);
		g_stream.error = MAD_ERROR_NONE;
		while (!s_stop) {
			if (mad_frame_decode(&g_frame, &g_stream)) {
				if (MAD_RECOVERABLE(g_stream.error)) { if (++errs > 200000) s_stop = 1; continue; }
				break;
			}
			mad_synth_frame(&g_synth, &g_frame);
			emit(&g_synth, g_frame.header.samplerate);
			if (s_state == P_BUFFERING && (pcm_w - pcm_r) >= PREBUFFER) { s_state = P_PLAYING; }
		}
		size_t rem = 0;
		if (g_stream.next_frame) {
			rem = g_stream.bufend - g_stream.next_frame;
			memmove(in, g_stream.next_frame, rem);
		}
		if (rem == have && got == 0) { /* no progress: drop a byte so we resync */
			if (rem > 0) { memmove(in, in + 1, rem - 1); rem--; }
		}
		have = rem;
	}
	if (!s_stop && s_state != P_ERROR) fail("Stream terminado");
	mad_synth_finish(&g_synth); mad_frame_finish(&g_frame); mad_stream_finish(&g_stream);
	free(in);
	sysThreadExit(0);
}

/* ================= audio output thread (persistent) ================= */
static audioPortConfig acfg;
static u32 aport;

static void audio_thread(void *arg)
{
	u32 idx = 1;
	while (!s_quit) {
		u64 cur = *(vu32 *)(u64)acfg.readIndex;
		if (idx == (u32)cur) { usleep(1500); continue; }
		float *buf = (float *)(u64)acfg.audioDataStart + 2 * AUDIO_BLOCK_SAMPLES * idx;
		float vol = s_volume;
		int playing = (s_state == P_PLAYING || s_state == P_BUFFERING);
		unsigned long have = pcm_w - pcm_r;
		if (playing && s_state == P_PLAYING && have >= AUDIO_BLOCK_SAMPLES) {
			for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
				size_t p = ((pcm_r + i) & (PCM_FRAMES - 1)) * 2;
				buf[i * 2] = pcm[p] * vol; buf[i * 2 + 1] = pcm[p + 1] * vol;
			}
			net_barrier();
			pcm_r += AUDIO_BLOCK_SAMPLES;
		} else {
			memset(buf, 0, sizeof(float) * 2 * AUDIO_BLOCK_SAMPLES);
			if (s_state == P_PLAYING && have < AUDIO_BLOCK_SAMPLES) s_state = P_BUFFERING; /* underrun */
		}
		idx = (idx + 1) % AUDIO_BLOCK_8;
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
	prm.numChannels = AUDIO_PORT_2CH;
	prm.numBlocks = AUDIO_BLOCK_8;
	prm.attr = 0;
	prm.level = 1;
	if (audioPortOpen(&prm, &aport) != 0) return false;
	if (audioGetPortConfig(aport, &acfg) != 0) return false;
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
	s_ctype[0] = 0; s_unsupported = 0; s_net_done = 0; s_stop = 0;
	net_r = net_w = 0; pcm_r = pcm_w = 0;
	set_msg("Conectando...");
	s_state = P_CONNECTING;
	if (sysThreadCreate(&th_net, net_thread, NULL, 1000, 0x80000, THREAD_JOINABLE, (char *)"rg_net") != 0) { fail("thread red"); return; }
	net_started = 1;
	s_state = P_BUFFERING;
	set_msg("Cargando buffer...");
	if (sysThreadCreate(&th_dec, dec_thread, NULL, 950, 0x40000, THREAD_JOINABLE, (char *)"rg_dec") != 0) { fail("thread audio"); return; }
	dec_started = 1;
}

PState player_state() { return (PState)s_state; }
const char *player_message() { return s_msg; }
void player_set_volume(float v) { s_volume = v < 0 ? 0 : (v > 1 ? 1 : v); }
float player_get_volume() { return s_volume; }

void player_shutdown()
{
	player_stop();
	s_quit = 1;
	u64 r; sysThreadJoin(th_audio, &r);
	audioPortStop(aport); audioPortClose(aport); audioQuit();
}
