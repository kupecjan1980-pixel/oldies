/*
 * Oldies viewer: subtitles by listening (see subs.h).
 *
 * The sound of a clip comes out of the game's ADPCM decoder the same every time, so the game's output holds
 * the clip's samples exactly (plus whatever else plays). Finding a clip's start:
 *   1. coarse: the stream and each clip's first 93 ms, low-passed and at a quarter of the rate, are
 *      correlated (by FFT) each 93 ms;
 *   2. the likely ones are checked sample by sample at the full rate, around where the coarse step put them:
 *      at once over those 93 ms, and over 370 ms when that much has come.
 * A clip that's playing is checked on as it goes, and dropped when the game cut it short.
 * Only the clips the game has read from its files are looked for (it reads the beginning of each clip of a
 * scene when the scene loads): tgspy.so tells the reads too.
 */

#include "subs.h"
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <complex>
#include <map>
#include <vector>

namespace {

enum {
	kRate = 22050,
	kDec = 4,          // the coarse step's rate: a quarter
	kCoarse = 512,     // the coarse template, in its samples (93 ms)
	kFft = 1024,
	kHop = 512 * kDec, // a step, in full-rate samples
	kFine = 8192,      // the fine check's template (370 ms; less for a shorter clip)
	kRing = 1 << 18,   // the stream kept (12 s)
	kLoud = 500,       // a clip's first sample this loud is where its template starts
	kHeader = 28
};

struct Line { float start, end; std::string text[kSubsLangs]; };

struct Clip {
	std::string file;      // the data file
	long offset, size;     // the clip in it (with its header)
	int tplStart;          // the template's first sample in the clip
	int tplLen;            // how many full-rate samples the fine template has (<= kFine)
	std::vector<std::complex<float> > spec; // the coarse template's spectrum, conjugated, over its energy's root
	std::vector<int16_t> tpl;               // the fine template: tplLen samples
	bool active;                            // looked for: the game has read it
	std::vector<Line> lines;
	Clip() : offset(0), size(0), tplStart(0), tplLen(0), active(false) {}
};

struct Playing {
	int clip;
	int64_t onset;                  // the stream's sample at which the clip's sample 0 played
	std::vector<int16_t> pcm;       // the whole clip
	int misses;
	bool heard;                     // checked over enough samples: its lines are shown
};

struct Pending { int clip; int64_t tplAt; bool placed; }; // a coarse find: where its template began (placed: to the sample)

const int kStep[89] = { 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80,
	88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
	876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
	5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086,
	29794, 32767 };
const int kIndex[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

std::string g_dir;
std::vector<Clip> g_clips;
std::map<std::string, std::string> g_realName; // upper-case name -> the file's name in the folder
pthread_t g_thread;
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
volatile bool g_run;
int g_sock = -1;
std::string g_sockPath;
bool g_verbose;
bool g_anyRead;                      // tgspy.so tells the reads: only clips read are looked for
std::vector<std::complex<float> > g_twiddle(kFft / 2);

std::vector<int16_t> g_ring(kRing);
int64_t g_now;                       // samples received
std::vector<float> g_dec(kFft);      // the last kFft coarse samples
int16_t g_hist[8];                   // the low-pass's memory
int g_decPhase;
int g_sinceHop;
std::vector<Playing> g_playing;
std::vector<Pending> g_pending;

// IMA ADPCM, the low nibble first, from zero; at most `want` samples
void decode(const uint8_t *data, size_t n, size_t want, std::vector<int16_t> &out) {
	int pred = 0, idx = 0;
	out.clear();
	out.reserve(want < 2 * n ? want : 2 * n);
	for (size_t i = 0; i < n && out.size() < want; ++i) {
		for (int half = 0; half < 2; ++half) {
			const int nib = half ? data[i] >> 4 : data[i] & 15;
			const int step = kStep[idx];
			int d = step >> 3;
			if (nib & 1) d += step >> 2;
			if (nib & 2) d += step >> 1;
			if (nib & 4) d += step;
			pred += (nib & 8) ? -d : d;
			if (pred > 32767) pred = 32767; else if (pred < -32768) pred = -32768;
			idx += kIndex[nib & 7];
			if (idx < 0) idx = 0; else if (idx > 88) idx = 88;
			out.push_back((int16_t)pred);
		}
	}
}

bool readClip(const Clip &c, size_t wantSamples, std::vector<int16_t> &pcm) {
	std::map<std::string, std::string>::const_iterator it = g_realName.find(c.file);
	if (it == g_realName.end()) return false;
	FILE *f = fopen((g_dir + "/" + it->second).c_str(), "rb");
	if (!f) return false;
	size_t bytes = c.size > kHeader ? c.size - kHeader : 0;
	if (bytes > (wantSamples + 1) / 2) bytes = (wantSamples + 1) / 2;
	std::vector<uint8_t> buf(bytes);
	bool ok = fseek(f, c.offset + kHeader, SEEK_SET) == 0 && fread(buf.data(), 1, bytes, f) == bytes;
	fclose(f);
	if (ok) decode(buf.data(), bytes, wantSamples, pcm);
	return ok;
}

void fft(std::complex<float> *a, bool inverse) { // (kFft points)
	const int n = kFft;
	for (int i = 1, j = 0; i < n; ++i) {
		int bit = n >> 1;
		for (; j & bit; bit >>= 1) j ^= bit;
		j ^= bit;
		if (i < j) std::swap(a[i], a[j]);
	}
	for (int len = 2; len <= n; len <<= 1) {
		const int stride = n / len;
		for (int i = 0; i < n; i += len) {
			for (int k = 0; k < len / 2; ++k) {
				const std::complex<float> w = inverse ? std::conj(g_twiddle[k * stride]) : g_twiddle[k * stride];
				const float vr = a[i + k + len / 2].real() * w.real() - a[i + k + len / 2].imag() * w.imag();
				const float vi = a[i + k + len / 2].real() * w.imag() + a[i + k + len / 2].imag() * w.real();
				const std::complex<float> u = a[i + k];
				a[i + k] = std::complex<float>(u.real() + vr, u.imag() + vi);
				a[i + k + len / 2] = std::complex<float>(u.real() - vr, u.imag() - vi);
			}
		}
	}
}

// the low-pass before the quarter rate: a triangle over 8 samples
inline float lowpass(const int16_t *x) { // x[0..7], the oldest first
	return (x[0] + 2 * x[1] + 3 * x[2] + 4 * x[3] + 4 * x[4] + 3 * x[5] + 2 * x[6] + x[7]) * (1 / 20.f);
}

std::string upper(std::string s) {
	for (size_t i = 0; i < s.size(); ++i) if (s[i] >= 'a' && s[i] <= 'z') s[i] -= 32;
	return s;
}

bool loadPack(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) return false;
	std::map<std::pair<std::string, long>, int> at;
	char *line = 0;
	size_t cap = 0;
	while (getline(&line, &cap, f) > 0) {
		std::vector<std::string> col;
		for (char *p = line;;) {
			char *e = strpbrk(p, "\t\n");
			col.push_back(e ? std::string(p, e - p) : std::string(p));
			if (!e || *e == '\n') break;
			p = e + 1;
		}
		if (col.size() < 5 + kSubsLangs || col[0].empty() || col[0][0] == '#') continue;
		const std::pair<std::string, long> key(upper(col[0]), atol(col[1].c_str()));
		if (!at.count(key)) {
			at[key] = (int)g_clips.size();
			Clip c;
			c.file = key.first;
			c.offset = key.second;
			c.size = atol(col[2].c_str());
			g_clips.push_back(c);
		}
		Line l;
		l.start = (float)atof(col[3].c_str());
		l.end = (float)atof(col[4].c_str());
		for (int k = 0; k < kSubsLangs; ++k) l.text[k] = col[5 + k];
		g_clips[at[key]].lines.push_back(l);
	}
	free(line);
	fclose(f);
	return !g_clips.empty();
}

// each clip's templates: from its first loud sample on
void makeTemplates() {
	std::vector<int16_t> pcm;
	std::vector<std::complex<float> > buf(kFft);
	int made = 0;
	for (int k = 0; k < kFft / 2; ++k) g_twiddle[k] = std::complex<float>(cosf(-2 * (float)M_PI * k / kFft), sinf(-2 * (float)M_PI * k / kFft));
	for (size_t i = 0; i < g_clips.size(); ++i) {
		Clip &c = g_clips[i];
		if (!readClip(c, 8 * kRate, pcm)) continue;
		int s = -1;
		for (size_t k = 0; k < pcm.size(); ++k) if (abs(pcm[k]) > kLoud) { s = (int)k; break; }
		if (s < 0 || (int)pcm.size() - s < kCoarse * kDec) continue;
		c.tplStart = s;
		c.tplLen = (int)pcm.size() - s < kFine ? (int)pcm.size() - s : kFine;
		c.tpl.assign(pcm.begin() + s, pcm.begin() + s + c.tplLen);
		double energy = 0;
		for (int k = 0; k < kFft; ++k) {
			float v = 0;
			if (k < kCoarse) {
				int16_t x[8];
				for (int j = 0; j < 8; ++j) { const int p = s + k * kDec + j - 7; x[j] = p >= 0 ? pcm[p] : 0; }
				v = lowpass(x);
				energy += v * v;
			}
			buf[k] = v;
		}
		if (energy < 1) continue;
		fft(buf.data(), false);
		c.spec.resize(kFft);
		const float scale = 1 / (float)sqrt(energy);
		for (int k = 0; k < kFft; ++k) c.spec[k] = std::conj(buf[k]) * scale;
		++made;
	}
	if (g_verbose) fprintf(stderr, "subs: %d clips, %d with a template\n", (int)g_clips.size(), made);
}

inline int16_t ringAt(int64_t pos) { return g_ring[pos & (kRing - 1)]; }

// how alike the stream from `at` on and the samples are: -1..1 (0 when either is silent)
float alike(int64_t at, const int16_t *x, int n) {
	double dot = 0, es = 0, ex = 0;
	for (int i = 0; i < n; ++i) {
		const double s = ringAt(at + i);
		dot += s * x[i];
		es += s * s;
		ex += (double)x[i] * x[i];
	}
	if (es < n * 400.0 || ex < n * 400.0) return 0; // (quieter than 20 of 32768 on average: nothing to tell by)
	return (float)(dot / sqrt(es * ex));
}

bool isPlaying(int clip, int64_t onset) {
	for (size_t i = 0; i < g_playing.size(); ++i) {
		if (g_playing[i].clip == clip && llabs(g_playing[i].onset - onset) < kRate / 2) return true;
	}
	return false;
}

// a coarse find checked at the full rate; true when it's settled (heard or not), false to look again later
bool checkPending(Pending &p) {
	const Clip &c = g_clips[p.clip];
	if (!p.placed) { // at once, over the 93 ms the coarse step had: where to the sample, or nowhere
		const int n = kCoarse * kDec;
		float best = 0;
		int64_t bestAt = 0;
		for (int d = -8; d <= 8; ++d) {
			const float a = alike(p.tplAt + d, c.tpl.data(), n);
			if (a > best) { best = a; bestAt = p.tplAt + d; }
		}
		if (best < 0.6f) return true;
		p.tplAt = bestAt;
		p.placed = true;
	}
	if (g_now - p.tplAt < c.tplLen) return false;
	const float score = alike(p.tplAt, c.tpl.data(), c.tplLen);
	if (score < 0.7f) return true;
	const int64_t onset = p.tplAt - c.tplStart;
	if (isPlaying(p.clip, onset)) return true;
	Playing pl;
	pl.clip = p.clip;
	pl.onset = onset;
	pl.misses = 0;
	pl.heard = true;
	readClip(c, (size_t)-1 / 4, pl.pcm);
	if (g_verbose) {
		fprintf(stderr, "subs: %6.2f s  %s+%ld starts (%.2f)%s%s\n", onset / (double)kRate, c.file.c_str(), c.offset, score,
			c.lines.empty() ? "" : "  ", c.lines.empty() ? "" : c.lines[0].text[0].c_str());
	}
	pthread_mutex_lock(&g_lock);
	// (the same sound under another name, in the scene's file and the common one: one is enough)
	for (size_t i = 0; i < g_playing.size(); ++i) {
		if (llabs(g_playing[i].onset - onset) < 64 && g_playing[i].pcm.size() == pl.pcm.size()) { pthread_mutex_unlock(&g_lock); return true; }
	}
	g_playing.push_back(pl);
	pthread_mutex_unlock(&g_lock);
	return true;
}

// one step: the clips playing are checked on, new ones looked for
void hop() {
	// 1. those playing: still there?
	pthread_mutex_lock(&g_lock);
	for (size_t i = 0; i < g_playing.size();) {
		Playing &p = g_playing[i];
		const int64_t pos = g_now - p.onset; // the clip's sample now
		bool gone = pos - kHop >= (int64_t)p.pcm.size();
		if (!gone && pos >= 2 * kHop && pos <= (int64_t)p.pcm.size()) {
			const int16_t *x = &p.pcm[pos - kHop];
			double ex = 0;
			for (int k = 0; k < kHop; ++k) ex += (double)x[k] * x[k];
			if (ex > kHop * 150.0 * 150.0) {
				if (alike(g_now - kHop, x, kHop) < 0.3f) ++p.misses; else p.misses = 0;
				if (p.misses >= 4) {
					gone = true;
					if (g_verbose) fprintf(stderr, "subs: %6.2f s  %s+%ld cut short\n", g_now / (double)kRate, g_clips[p.clip].file.c_str(), g_clips[p.clip].offset);
				}
			}
		}
		if (gone) g_playing.erase(g_playing.begin() + i); else ++i;
	}
	pthread_mutex_unlock(&g_lock);

	// 2. finds waiting for more samples
	for (size_t i = 0; i < g_pending.size();) {
		if (checkPending(g_pending[i])) g_pending.erase(g_pending.begin() + i); else ++i;
	}

	// 3. the coarse search: template starts at the coarse samples [0, kFft - kCoarse] of the window
	static std::vector<std::complex<float> > spec(kFft), prod(kFft);
	static std::vector<float> energy(kFft - kCoarse + 1);
	double e = 0;
	for (int k = 0; k < kCoarse; ++k) e += g_dec[k] * g_dec[k];
	double loudest = e;
	for (int k = 0;; ++k) {
		energy[k] = (float)e;
		if (e > loudest) loudest = e;
		if (k == kFft - kCoarse) break;
		e += g_dec[k + kCoarse] * g_dec[k + kCoarse] - g_dec[k] * g_dec[k];
		if (e < 0) e = 0;
	}
	if (loudest < kCoarse * 100.0) return; // (silence)
	for (int k = 0; k < kFft; ++k) spec[k] = g_dec[k];
	fft(spec.data(), false);
	const int64_t windowStart = g_now - (int64_t)kFft * kDec; // the full-rate sample of the window's coarse sample 0, about
	for (size_t i = 0; i < g_clips.size(); ++i) {
		const Clip &c = g_clips[i];
		if (c.spec.empty() || (g_anyRead && !c.active)) continue;
		for (int k = 0; k < kFft; ++k) prod[k] = spec[k] * c.spec[k];
		fft(prod.data(), true);
		float best = 0.5f;
		int bestLag = -1;
		// (the lags of the last step only: the earlier ones were this window's a step ago)
		for (int lag = kFft - kCoarse - kHop / kDec; lag <= kFft - kCoarse; ++lag) {
			if (energy[lag] < kCoarse * 100.0f) continue;
			const float v = prod[lag].real() / kFft / sqrtf(energy[lag]);
			if (v > best) { best = v; bestLag = lag; }
		}
		if (bestLag < 0) continue;
		Pending p;
		p.clip = (int)i;
		p.placed = false;
		p.tplAt = windowStart + (int64_t)bestLag * kDec + 3; // (a coarse sample is the low-pass ending at its 4th sample)
		if (isPlaying(p.clip, p.tplAt - c.tplStart)) continue;
		bool known = false;
		for (size_t k = 0; k < g_pending.size(); ++k) if (g_pending[k].clip == p.clip && llabs(g_pending[k].tplAt - p.tplAt) < 64) known = true;
		if (!known && !checkPending(p) && g_pending.size() < 400) g_pending.push_back(p);
	}
}

void feed(const int16_t *x, size_t n) {
	for (size_t i = 0; i < n; ++i) {
		g_ring[g_now & (kRing - 1)] = x[i];
		++g_now;
		memmove(g_hist, g_hist + 1, 7 * sizeof(int16_t));
		g_hist[7] = x[i];
		if (++g_decPhase == kDec) {
			g_decPhase = 0;
			memmove(&g_dec[0], &g_dec[1], (kFft - 1) * sizeof(float));
			g_dec[kFft - 1] = lowpass(g_hist);
		}
		if (++g_sinceHop == kHop) {
			g_sinceHop = 0;
			hop();
		}
	}
}

// the game read a part of a data file: the clips there are looked for from now on
void wasRead(const std::string &file, long offset, long size) {
	g_anyRead = true;
	for (size_t i = 0; i < g_clips.size(); ++i) {
		Clip &c = g_clips[i];
		if (!c.active && offset < c.offset + c.size && offset + size > c.offset && c.file == file) {
			c.active = true;
			if (g_verbose) fprintf(stderr, "subs: %6.2f s  %s+%ld read\n", g_now / (double)kRate, c.file.c_str(), c.offset);
		}
	}
}

// a packet of tgspy.so: "A rate channels format bytes\n" and the samples, or "R file offset size\n"
void packet(const char *msg, size_t len) {
	if (len > 4 && len < 200 && msg[0] == 'R') {
		char name[64];
		long offset = 0, size = 0;
		const std::string line(msg, len);
		if (sscanf(line.c_str(), "R %63s %ld %ld", name, &offset, &size) == 3) wasRead(upper(name), offset, size);
		return;
	}
	if (len < 4 || msg[0] != 'A') return;
	const char *nl = (const char *)memchr(msg, '\n', len < 64 ? len : 64);
	unsigned rate = 0, channels = 0, bytes = 0;
	int format = 0;
	if (!nl || sscanf(msg, "A %u %u %d %u", &rate, &channels, &format, &bytes) != 4) return;
	const char *data = nl + 1;
	if (bytes > len - (data - msg) || format != 3 || channels < 1 || channels > 2) return; // (3: 16-bit, little endian)
	static bool told;
	if (rate != kRate) {
		if (!told) fprintf(stderr, "subs: the game plays at %u Hz, not %d: no subtitles\n", rate, kRate);
		told = true;
		return;
	}
	const size_t frames = bytes / (2 * channels);
	static std::vector<int16_t> mono;
	mono.resize(frames);
	const int16_t *s = (const int16_t *)data;
	for (size_t i = 0; i < frames; ++i) mono[i] = channels == 1 ? s[i] : (int16_t)((s[2 * i] + s[2 * i + 1]) / 2);
	feed(mono.data(), frames);
}

void *listenThread(void *) {
	makeTemplates();
	std::vector<char> buf(1 << 17);
	while (g_run) {
		const ssize_t n = recv(g_sock, buf.data(), buf.size(), 0);
		if (n > 0) packet(buf.data(), (size_t)n);
		else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) break;
	}
	return 0;
}

bool prepare(const char *gameDir, const char *pack) {
	g_verbose = getenv("OLDIES_SUBS_LOG") != 0;
	g_dir = gameDir;
	if (DIR *d = opendir(gameDir)) {
		while (struct dirent *e = readdir(d)) g_realName[upper(e->d_name)] = e->d_name;
		closedir(d);
	}
	return loadPack(pack);
}

} // namespace

bool Subs_start(const char *gameDir, const char *pack, const char *sock) {
	if (!prepare(gameDir, pack)) return false;
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	if (strlen(sock) >= sizeof(addr.sun_path)) return false;
	strcpy(addr.sun_path, sock);
	unlink(sock);
	g_sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (g_sock < 0 || bind(g_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) return false;
	int size = 8 << 20;
	setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
	struct timeval tv = { 0, 200000 };
	setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	g_sockPath = sock;
	g_run = true;
	return pthread_create(&g_thread, 0, listenThread, 0) == 0;
}

void Subs_stop() {
	if (!g_run) return;
	g_run = false;
	pthread_join(g_thread, 0);
	close(g_sock);
	unlink(g_sockPath.c_str());
}

std::string Subs_current(int lang) {
	std::string out;
	if (lang < 0 || lang >= kSubsLangs) return out;
	pthread_mutex_lock(&g_lock);
	for (size_t i = 0; i < g_playing.size(); ++i) {
		const Playing &p = g_playing[i];
		if (!p.heard) continue;
		const float t = (g_now - p.onset) / (float)kRate;
		const std::vector<Line> &lines = g_clips[p.clip].lines;
		for (size_t k = 0; k < lines.size(); ++k) {
			// (a line stays until the next one when that follows within a second and a half)
			float end = lines[k].end + 0.35f;
			if (k + 1 < lines.size() && lines[k + 1].start - lines[k].end < 1.5f) end = lines[k + 1].start - 0.1f;
			if (t >= lines[k].start - 0.1f && t < end && !lines[k].text[lang].empty()) {
				if (!out.empty()) out += "\n";
				out += lines[k].text[lang];
			}
		}
	}
	pthread_mutex_unlock(&g_lock);
	return out;
}

void Subs_feedFile(const char *path) {
	g_verbose = true;
	makeTemplates();
	FILE *f = fopen(path, "rb");
	if (!f) return;
	std::vector<char> buf(1 << 17);
	std::string last;
	struct timeval t0, t1;
	gettimeofday(&t0, 0);
	while (fgets(buf.data(), 200, f)) { // (tgspy.so's log: the packets one after another)
		size_t len = strlen(buf.data());
		unsigned bytes = 0;
		if (buf[0] == 'A' && sscanf(buf.data(), "A %*u %*u %*d %u", &bytes) == 1 && bytes < buf.size() - 200) {
			if (fread(buf.data() + len, 1, bytes, f) != bytes) break;
			len += bytes;
		}
		packet(buf.data(), len);
		const std::string now = Subs_current(0);
		if (now != last) {
			fprintf(stderr, "subs: %6.2f s  [%s]\n", g_now / (double)kRate, now.c_str());
			last = now;
		}
	}
	fclose(f);
	gettimeofday(&t1, 0);
	fprintf(stderr, "subs: %.1f s of sound in %.2f s\n", g_now / (double)kRate, (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6);
}

#ifdef SUBS_TEST
// subs-test GAME_FOLDER PACK TGSPY.log
int main(int argc, char **argv) {
	if (argc < 4 || !prepare(argv[1], argv[2])) { fprintf(stderr, "usage: subs-test GAME_FOLDER PACK TGSPY.log\n"); return 2; }
	Subs_feedFile(argv[3]);
	return 0;
}
#endif
