/*
 * Oldies: old DOS games on Linux desktops and handhelds, each with the same things around it: controllers,
 * a pause menu (states, the controller's layout, hints), English / Czech / Slovak menus, the picture upscaled
 * to the screen (FSR 1, neural nets) and frames in between up to the screen's refresh.
 *
 * The games run in an emulator core (a libretro core: DOSBox Pure for DOS games), loaded from cores/. This
 * file is the shell around it: the window, the sound, the keys and buttons handed to the game, the menus.
 * games.h says what's known of each game: how its folder is recognised, what to start, which button is which
 * key, the hints. The games' own files are never part of this: the player picks the folder that holds them.
 */

#include <SDL.h>
#include <dirent.h>
#include <dlfcn.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include "gfx.h"
#include "ui.h"
#include "third_party/libretro.h"
#include "games.h"

#define OLDIES_VERSION "0.1.0"

// --- small helpers -------------------------------------------------------------------------------------------

static bool isDir(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
static bool isFile(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode); }
static void makeDirs(const std::string &p) {
	for (size_t i = 1; i <= p.size(); ++i) {
		if (i == p.size() || p[i] == '/') mkdir(p.substr(0, i).c_str(), 0755);
	}
}
static std::string envOr(const char *n, const std::string &f) { const char *v = getenv(n); return (v && *v) ? v : f; }
static std::string home() { return envOr("HOME", "/"); }
static std::string configDir() { return envOr("XDG_CONFIG_HOME", home() + "/.config") + "/oldies"; }
static std::string dataHome() { return envOr("XDG_DATA_HOME", home() + "/.local/share") + "/oldies"; }
static std::string parentOf(const std::string &p) { const size_t s = p.find_last_of('/'); return (s == std::string::npos || s == 0) ? "/" : p.substr(0, s); }
static std::string readFile(const std::string &p) {
	std::string out;
	if (FILE *f = fopen(p.c_str(), "rb")) {
		char buf[65536];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
		fclose(f);
	}
	return out;
}
static bool writeFile(const std::string &p, const std::string &d) {
	FILE *f = fopen(p.c_str(), "wb");
	if (!f) return false;
	const bool ok = fwrite(d.data(), 1, d.size(), f) == d.size();
	fclose(f);
	return ok;
}
static std::vector<std::string> listDirs(const std::string &p) {
	std::vector<std::string> out;
	if (DIR *d = opendir(p.c_str())) {
		while (struct dirent *e = readdir(d)) {
			if (e->d_name[0] != '.' && isDir(p + "/" + e->d_name)) out.push_back(e->d_name);
		}
		closedir(d);
	}
	std::sort(out.begin(), out.end(), [](const std::string &a, const std::string &b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });
	return out;
}
static std::string findNoCase(const std::string &dir, const char *name) {
	std::string found;
	if (DIR *d = opendir(dir.c_str())) {
		while (struct dirent *e = readdir(d)) {
			if (strcasecmp(e->d_name, name) == 0) { found = dir + "/" + e->d_name; break; }
		}
		closedir(d);
	}
	return found;
}
static std::string resourceDir() {
	char exe[4096];
	const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	std::string dir = ".";
	if (n > 0) { exe[n] = 0; dir = parentOf(exe); }
	return dir;
}

// --- settings ------------------------------------------------------------------------------------------------

static std::string g_lang;          // "en", "cs", "sk"
static int g_scale = kScaleNeuralM;
static bool g_fullHd = false, g_frameGen = true, g_fullscreen = true;
static int g_slot = 1;
static std::map<std::string, std::string> g_dirs; // a game's id -> where its files are
static std::map<std::string, int> g_gameLang;     // a game's id -> which of its own languages

static const char *T(const char *en, const char *cs, const char *sk) { return g_lang == "cs" ? cs : g_lang == "sk" ? sk : en; }
static int li() { return g_lang == "cs" ? 1 : g_lang == "sk" ? 2 : 0; }

static void loadConfig() {
	const std::string text = readFile(configDir() + "/config.ini");
	size_t pos = 0;
	while (pos < text.size()) {
		size_t e = text.find('\n', pos);
		if (e == std::string::npos) e = text.size();
		const std::string line = text.substr(pos, e - pos);
		pos = e + 1;
		const size_t eq = line.find('=');
		if (eq == std::string::npos) continue;
		const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
		if (k == "lang") g_lang = v;
		else if (k == "scale") g_scale = atoi(v.c_str());
		else if (k == "fullhd") g_fullHd = v == "1";
		else if (k == "framegen") g_frameGen = v != "0";
		else if (k == "fullscreen") g_fullscreen = v != "0";
		else if (k == "slot") g_slot = atoi(v.c_str());
		else if (k.compare(0, 4, "dir.") == 0) g_dirs[k.substr(4)] = v;
		else if (k.compare(0, 9, "gamelang.") == 0) g_gameLang[k.substr(9)] = atoi(v.c_str());
	}
	if (g_lang != "en" && g_lang != "cs" && g_lang != "sk") g_lang = "";
	if (g_scale < 0 || g_scale >= kScaleCount) g_scale = kScaleNeuralM;
	if (g_slot < 1 || g_slot > 99) g_slot = 1;
}

static void saveConfig() {
	makeDirs(configDir());
	char buf[200];
	snprintf(buf, sizeof(buf), "lang=%s\nscale=%d\nfullhd=%d\nframegen=%d\nfullscreen=%d\nslot=%d\n", g_lang.c_str(), g_scale,
		g_fullHd ? 1 : 0, g_frameGen ? 1 : 0, g_fullscreen ? 1 : 0, g_slot);
	std::string out = buf;
	for (std::map<std::string, std::string>::iterator it = g_dirs.begin(); it != g_dirs.end(); ++it) {
		out += "dir." + it->first + "=" + it->second + "\n";
	}
	for (std::map<std::string, int>::iterator it = g_gameLang.begin(); it != g_gameLang.end(); ++it) {
		snprintf(buf, sizeof(buf), "gamelang.%s=%d\n", it->first.c_str(), it->second);
		out += buf;
	}
	writeFile(configDir() + "/config.ini", out);
}

// the folder that holds a game: path itself, or a folder one or two levels inside it (the file the game is
// started with is looked for, whatever the letters' case); "" when it isn't there
static std::string findGame(const Game &g, const std::string &path, int depth = 2) {
	if (!findNoCase(path, g.exe).empty()) {
		for (int i = 0; g.marks[i]; ++i) {
			if (findNoCase(path, g.marks[i]).empty()) return "";
		}
		return path;
	}
	if (depth > 0) {
		const std::vector<std::string> subs = listDirs(path);
		for (size_t i = 0; i < subs.size() && i < 60; ++i) {
			const std::string found = findGame(g, path + "/" + subs[i], depth - 1);
			if (!found.empty()) return found;
		}
	}
	return "";
}

// --- the core ------------------------------------------------------------------------------------------------

struct Core {
	void *h;
	void (*init)(void);
	void (*deinit)(void);
	void (*get_system_av_info)(struct retro_system_av_info *);
	void (*set_environment)(retro_environment_t);
	void (*set_video_refresh)(retro_video_refresh_t);
	void (*set_audio_sample)(retro_audio_sample_t);
	void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
	void (*set_input_poll)(retro_input_poll_t);
	void (*set_input_state)(retro_input_state_t);
	void (*set_controller_port_device)(unsigned, unsigned);
	bool (*load_game)(const struct retro_game_info *);
	void (*unload_game)(void);
	void (*run)(void);
	void (*reset)(void);
	size_t (*serialize_size)(void);
	bool (*serialize)(void *, size_t);
	bool (*unserialize)(const void *, size_t);
};

static Core g_core;
static const Game *g_game;
static std::string g_gameDir;
static void toast(const std::string &text, float seconds);
static enum retro_pixel_format g_pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static retro_keyboard_event_t g_keyboardCb;
static struct retro_system_av_info g_av;
static bool g_avChanged, g_coreQuit;
static std::vector<uint32_t> g_frame;
static int g_frameW, g_frameH;
static bool g_frameNew;
static uint32_t g_frameHash;
static SDL_AudioDeviceID g_audio;
static int g_audioRate;
static bool g_keysDown[RETROK_LAST];
static bool g_padDown[16];       // (RETRO_DEVICE_ID_JOYPAD_*: for cores that read the pad themselves)
static std::map<std::string, std::string> g_options;
static std::string g_systemDir, g_saveDir;

static void RETRO_CALLCONV coreLog(enum retro_log_level level, const char *fmt, ...) {
	if (level < RETRO_LOG_WARN && !getenv("OLDIES_VERBOSE")) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static bool RETRO_CALLCONV environment(unsigned cmd, void *data) {
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool *)data = true; return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: g_pixfmt = *(const enum retro_pixel_format *)data; return true;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY: *(const char **)data = g_systemDir.c_str(); return true;
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char **)data = g_saveDir.c_str(); return true;
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((struct retro_log_callback *)data)->log = coreLog; return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
			struct retro_variable *v = (struct retro_variable *)data;
			std::map<std::string, std::string>::iterator it = g_options.find(v->key ? v->key : "");
			if (it == g_options.end()) { v->value = 0; return false; }
			v->value = it->second.c_str();
			return true;
		}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *(bool *)data = false; return true;
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *(unsigned *)data = 0; return true;
	case RETRO_ENVIRONMENT_SET_VARIABLES:
	case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
	case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
	case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
	case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
	case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
	case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
		return true;
	case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK: g_keyboardCb = ((const struct retro_keyboard_callback *)data)->callback; return true;
	case RETRO_ENVIRONMENT_SET_GEOMETRY: g_av.geometry = *(const struct retro_game_geometry *)data; g_avChanged = true; return true;
	case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: g_av = *(const struct retro_system_av_info *)data; g_avChanged = true; return true;
	case RETRO_ENVIRONMENT_SET_MESSAGE: fprintf(stderr, "core: %s\n", ((const struct retro_message *)data)->msg); return true;
	case RETRO_ENVIRONMENT_SET_MESSAGE_EXT: fprintf(stderr, "core: %s\n", ((const struct retro_message_ext *)data)->msg); return true;
	case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: return false;
	case RETRO_ENVIRONMENT_GET_LANGUAGE: *(unsigned *)data = RETRO_LANGUAGE_ENGLISH; return true;
	case RETRO_ENVIRONMENT_GET_FASTFORWARDING: *(bool *)data = false; return true;
	case RETRO_ENVIRONMENT_SHUTDOWN: g_coreQuit = true; return true;
	}
	return false;
}

// the core's picture: made RGBA; a picture the same as the one before isn't a new one (many DOS games draw
// 10 to 35 a second into a screen refreshed 70 times: the frames in between are made from the real ones)
static void RETRO_CALLCONV videoRefresh(const void *data, unsigned w, unsigned h, size_t pitch) {
	if (!data || !w || !h) {
		return;
	}
	if ((int)w != g_frameW || (int)h != g_frameH) {
		g_frameW = w;
		g_frameH = h;
		g_frame.assign(w * h, 0xFF000000);
		g_frameHash = 0;
	}
	uint32_t hash = 2166136261u;
	for (unsigned y = 0; y < h; ++y) {
		uint32_t *dst = &g_frame[y * w];
		const uint8_t *row = (const uint8_t *)data + y * pitch;
		if (g_pixfmt == RETRO_PIXEL_FORMAT_XRGB8888) {
			const uint32_t *src = (const uint32_t *)row;
			for (unsigned x = 0; x < w; ++x) {
				const uint32_t c = src[x];
				dst[x] = 0xFF000000u | ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
			}
		} else if (g_pixfmt == RETRO_PIXEL_FORMAT_RGB565) {
			const uint16_t *src = (const uint16_t *)row;
			for (unsigned x = 0; x < w; ++x) {
				const uint16_t c = src[x];
				const uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
				dst[x] = 0xFF000000u | ((b * 255 / 31) << 16) | ((g * 255 / 63) << 8) | (r * 255 / 31);
			}
		} else {
			const uint16_t *src = (const uint16_t *)row;
			for (unsigned x = 0; x < w; ++x) {
				const uint16_t c = src[x];
				const uint32_t r = (c >> 10) & 31, g = (c >> 5) & 31, b = c & 31;
				dst[x] = 0xFF000000u | ((b * 255 / 31) << 16) | ((g * 255 / 31) << 8) | (r * 255 / 31);
			}
		}
		for (unsigned x = (y & 3); x < w; x += 4) { // (a quarter of the points: enough to tell)
			hash = (hash ^ dst[x]) * 16777619u;
		}
	}
	if (hash != g_frameHash) {
		g_frameHash = hash;
		g_frameNew = true;
	}
}

static size_t RETRO_CALLCONV audioBatch(const int16_t *data, size_t frames) {
	if (g_audio) {
		if (SDL_GetQueuedAudioSize(g_audio) < (Uint32)(g_audioRate * 4 / 4)) { // (no more than a quarter second waiting)
			SDL_QueueAudio(g_audio, data, (Uint32)(frames * 4));
		}
	}
	return frames;
}
static void RETRO_CALLCONV audioSample(int16_t l, int16_t r) { const int16_t s[2] = { l, r }; audioBatch(s, 1); }
static void RETRO_CALLCONV inputPoll(void) {}
static int16_t RETRO_CALLCONV inputState(unsigned port, unsigned device, unsigned, unsigned id) {
	if (port != 0) return 0;
	if ((device & RETRO_DEVICE_MASK) == RETRO_DEVICE_KEYBOARD) return (id < RETROK_LAST && g_keysDown[id]) ? 1 : 0;
	if ((device & RETRO_DEVICE_MASK) == RETRO_DEVICE_JOYPAD) return (id < 16 && g_padDown[id]) ? 1 : 0;
	return 0;
}

static void sendKey(unsigned key, bool down) {
	if (key == 0 || key >= RETROK_LAST || g_keysDown[key] == down) {
		return;
	}
	g_keysDown[key] = down;
	if (g_keyboardCb) {
		g_keyboardCb(down, key, 0, 0);
	}
}

static bool g_dirHeld[4];        // up, down, left, right
static unsigned g_dirKeys[2];    // the keys down for them now

static void releaseAll() {
	memset(g_dirHeld, 0, sizeof(g_dirHeld));
	g_dirKeys[0] = g_dirKeys[1] = 0;
	for (unsigned k = 1; k < RETROK_LAST; ++k) {
		sendKey(k, false);
	}
	memset(g_padDown, 0, sizeof(g_padDown));
}

// where a core for a game is looked for; "" when it isn't there
static std::string corePath(const char *name) {
	const std::string file = std::string(name) + "_libretro.so";
	const std::string places[] = { envOr("OLDIES_CORES", ""), resourceDir() + "/cores", dataHome() + "/cores",
		"/usr/lib/libretro", "/usr/lib64/libretro", "/usr/lib/x86_64-linux-gnu/libretro",
		home() + "/.config/retroarch/cores", home() + "/.var/app/org.libretro.RetroArch/config/retroarch/cores" };
	for (size_t i = 0; i < sizeof(places) / sizeof(places[0]); ++i) {
		if (!places[i].empty() && isFile(places[i] + "/" + file)) return places[i] + "/" + file;
	}
	return "";
}

static bool loadCore(const std::string &path, std::string &why) {
	memset(&g_core, 0, sizeof(g_core));
	g_core.h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
	if (!g_core.h) {
		why = dlerror();
		return false;
	}
#define SYM(field, name) *(void **)&g_core.field = dlsym(g_core.h, name); if (!g_core.field) { why = "no " name " in the core"; return false; }
	SYM(init, "retro_init") SYM(deinit, "retro_deinit") SYM(get_system_av_info, "retro_get_system_av_info")
	SYM(set_environment, "retro_set_environment") SYM(set_video_refresh, "retro_set_video_refresh")
	SYM(set_audio_sample, "retro_set_audio_sample") SYM(set_audio_sample_batch, "retro_set_audio_sample_batch")
	SYM(set_input_poll, "retro_set_input_poll") SYM(set_input_state, "retro_set_input_state")
	SYM(set_controller_port_device, "retro_set_controller_port_device") SYM(load_game, "retro_load_game")
	SYM(unload_game, "retro_unload_game") SYM(run, "retro_run") SYM(reset, "retro_reset")
	SYM(serialize_size, "retro_serialize_size") SYM(serialize, "retro_serialize") SYM(unserialize, "retro_unserialize")
#undef SYM
	return true;
}

static std::string statePath(int slot) {
	char buf[64];
	snprintf(buf, sizeof(buf), "/%s-%02d.state", g_game->id, slot);
	makeDirs(dataHome() + "/states");
	return dataHome() + "/states" + buf;
}

static bool saveState(int slot) {
	const size_t n = g_core.serialize_size();
	if (!n) return false;
	std::string buf(n, 0);
	return g_core.serialize(&buf[0], n) && writeFile(statePath(slot), buf);
}

static bool loadState(int slot) {
	const std::string buf = readFile(statePath(slot));
	return !buf.empty() && g_core.unserialize(buf.data(), buf.size());
}

// --- the window, the picture, the menus ----------------------------------------------------------------------

enum { kNone, kGames, kFolder, kLanguage, kPause, kControls, kHint, kMessage };
// a game whose texts are one file per language (Metal Mutant's TEXTE.EN / .ES / .FR): the picked one is
// copied over the file the game reads. True when the folder has those files.
static bool gameLangs(const Game &game, const std::string &dir) {
	return game.gameLang[0][0] && !readFile(dir + "/" + game.gameLangFile[0]).empty();
}

static void applyGameLang(const Game &game, const std::string &dir) {
	if (!gameLangs(game, dir)) return;
	int which = g_gameLang.count(game.id) ? g_gameLang[game.id] : 0;
	if (which < 0 || which > 2 || !game.gameLang[which][0]) which = 0;
	const std::string text = readFile(dir + "/" + game.gameLangFile[which]);
	if (!text.empty() && text != readFile(dir + "/" + game.gameLangFile[3])) writeFile(dir + "/" + game.gameLangFile[3], text);
}

enum { kPResume, kPSave, kPLoad, kPHint, kPControls, kPScale, kPFrames, kPLang, kPGameLang, kPFull, kPFolder, kPReset, kPGames, kPQuit, kPCount };

static SDL_Window *g_window;
static Gfx *g_gfx;
static SDL_GameController *g_pad;
static Canvas g_overlay;
static bool g_overlayShown, g_dirty = true, g_quit, g_padUsed, g_first;
static int g_screen = kNone, g_sel, g_scroll, g_hint;
static const Game *g_pick; // the game the folder is being picked for
static std::string g_browse, g_message, g_toast, g_pendingShot;
static std::vector<std::string> g_browseDirs;
static double g_toastUntil;
static uint32_t g_frameAt, g_frameTick = 30;
static bool g_wantStart, g_wantBackToGames;
static int g_shots = 1;

struct Info {
	int w, h, refresh, gx, gy, gw, gh, nw, nh;
	float scaleMs, motionMs;
	unsigned modesOk;
};

static double nowSec() { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

static void toast(const std::string &text, float secs = 2.5f) {
	g_toast = text;
	g_toastUntil = nowSec() + secs;
	g_dirty = true;
}

static void getInfo(Info &i) {
	SDL_GL_GetDrawableSize(g_window, &i.w, &i.h);
	g_gfx->resize(i.w, i.h);
	SDL_DisplayMode dm;
	const int d = SDL_GetWindowDisplayIndex(g_window);
	i.refresh = (d >= 0 && SDL_GetCurrentDisplayMode(d, &dm) == 0 && dm.refresh_rate > 0) ? dm.refresh_rate : 60;
	g_gfx->gameRect(&i.gx, &i.gy, &i.gw, &i.gh);
	g_gfx->neuralSize(&i.nw, &i.nh);
	i.scaleMs = g_gfx->lastScaleMs();
	i.motionMs = g_gfx->lastMotionMs();
	i.modesOk = 0;
	for (int m = 0; m < kScaleCount; ++m) i.modesOk |= g_gfx->modeAvailable(m) ? (1u << m) : 0;
}

static std::string scaleName(int mode, const Info &i) {
	std::string s;
	switch (mode) {
	case kScaleOriginal: s = T("Original size", "Původní velikost", "Pôvodná veľkosť"); break;
	case kScalePixels: s = T("Sharp pixels", "Ostré pixely", "Ostré pixely"); break;
	case kScaleSmooth: s = T("Smooth", "Hladké", "Hladké"); break;
	case kScaleSharp: s = T("Sharp (bicubic)", "Ostré (bikubické)", "Ostré (bikubické)"); break;
	case kScaleFsr: s = "AMD FSR 1"; break;
	case kScaleNeuralS: s = T("AI upscaling: fast", "AI zvětšení: rychlé", "AI zväčšenie: rýchle"); break;
	case kScaleNeuralM: s = T("AI upscaling: balanced", "AI zvětšení: vyvážené", "AI zväčšenie: vyvážené"); break;
	default: s = T("AI upscaling: best", "AI zvětšení: nejlepší", "AI zväčšenie: najlepšie"); break;
	}
	if (mode >= kScaleFsr && i.h > 1080) {
		char buf[48];
		snprintf(buf, sizeof(buf), " → %d×%d", i.w, i.h);
		s += g_fullHd ? " → Full HD" : buf;
	}
	return s;
}

// the picture's ways in a ring: original, pixels, smooth, sharp, then FSR and each net to Full HD and to the
// screen's own resolution (once where the screen is Full HD or smaller)
static void cycleScale(int dir) {
	Info i;
	getInfo(i);
	std::vector<std::pair<int, bool> > ways;
	for (int m = 0; m < kScaleCount; ++m) {
		if (!(i.modesOk & (1u << m))) continue;
		if (m >= kScaleFsr && i.h > 1080) ways.push_back(std::make_pair(m, true));
		ways.push_back(std::make_pair(m, false));
	}
	int at = 0;
	for (size_t k = 0; k < ways.size(); ++k) {
		if (ways[k].first == g_gfx->mode() && (ways[k].second == g_fullHd || ways[k].first < kScaleFsr || i.h <= 1080)) { at = (int)k; break; }
	}
	at = (at + dir + (int)ways.size()) % (int)ways.size();
	g_gfx->setMode(ways[at].first);
	g_fullHd = ways[at].second;
	g_gfx->setTargetLines(g_fullHd ? 1080 : 0);
	g_scale = g_gfx->mode();
	saveConfig();
	g_dirty = true;
}

static void toggleFrameGen() {
	g_gfx->setFrameGen(!g_gfx->frameGen());
	g_frameGen = g_gfx->frameGen();
	saveConfig();
	g_dirty = true;
}

static void openScreen(int s) {
	g_screen = s;
	g_sel = g_scroll = 0;
	g_dirty = true;
	releaseAll();
	if (s == kLanguage) g_sel = li();
	if (s == kHint) g_hint = 0;
}

static void browseTo(const std::string &path) {
	g_browse = path.empty() ? "/" : path;
	g_browseDirs = listDirs(g_browse);
	g_sel = (g_pick && !findGame(*g_pick, g_browse, 1).empty()) ? 0 : (g_browseDirs.empty() ? 1 : 2);
	g_scroll = 0;
	g_dirty = true;
}

static int itemCount() {
	switch (g_screen) {
	case kGames: return kGameCount + 1;
	case kFolder: return 2 + (int)g_browseDirs.size();
	case kLanguage: return 3;
	case kPause: return kPCount;
	}
	return 1;
}

static int hintCount() {
	int n = 0;
	while (g_game && n < kMaxHints && g_game->hints[n].title[0]) ++n;
	return n;
}

static void adjust(int dir) {
	if (g_screen == kPause) {
		switch (g_sel) {
		case kPSave:
		case kPLoad: g_slot = g_slot + dir < 1 ? 99 : g_slot + dir > 99 ? 1 : g_slot + dir; saveConfig(); break;
		case kPScale: cycleScale(dir); break;
		case kPFrames: toggleFrameGen(); break;
		case kPLang: { static const char *o[] = { "en", "cs", "sk" }; g_lang = o[(li() + dir + 3) % 3]; saveConfig(); } break;
		case kPGameLang:
			if (g_game && gameLangs(*g_game, g_gameDir)) {
				int n = 0;
				while (n < 3 && g_game->gameLang[n][0]) ++n;
				g_gameLang[g_game->id] = ((g_gameLang.count(g_game->id) ? g_gameLang[g_game->id] : 0) + dir + n) % n;
				saveConfig();
				applyGameLang(*g_game, g_gameDir);
				toast(T("The game's language changes when it's started anew", "Jazyk hry se změní po novém spuštění hry", "Jazyk hry sa zmení po novom spustení hry"), 3.0f);
			}
			break;
		case kPFull:
			g_fullscreen = !g_fullscreen;
			SDL_SetWindowFullscreen(g_window, g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
			saveConfig();
			break;
		}
	} else if (g_screen == kHint && hintCount()) {
		g_hint = (g_hint + dir + hintCount()) % hintCount();
	}
}

static void accept() {
	switch (g_screen) {
	case kGames:
		if (g_sel >= kGameCount) { g_quit = true; break; }
		g_pick = &kGames_[g_sel];
		if (g_dirs.count(g_pick->id) && !findGame(*g_pick, g_dirs[g_pick->id]).empty()) {
			g_wantStart = true;
			g_screen = kNone;
		} else {
			g_first = true;
			g_browse = "";
			openScreen(kFolder);
			browseTo(home());
		}
		break;
	case kFolder:
		if (g_sel == 0) {
			const std::string found = g_pick ? findGame(*g_pick, g_browse, 1) : "";
			if (!found.empty()) {
				g_dirs[g_pick->id] = found;
				saveConfig();
				g_wantStart = true;
				g_screen = kNone;
			}
		} else if (g_sel == 1) {
			browseTo(parentOf(g_browse));
		} else if (g_sel - 2 < (int)g_browseDirs.size()) {
			browseTo((g_browse == "/" ? "" : g_browse) + "/" + g_browseDirs[g_sel - 2]);
		}
		break;
	case kLanguage: { static const char *o[] = { "en", "cs", "sk" }; g_lang = o[g_sel]; saveConfig(); g_screen = kNone; } break;
	case kPause:
		switch (g_sel) {
		case kPResume: g_screen = kNone; break;
		case kPSave: toast(saveState(g_slot) ? T("Saved", "Uloženo", "Uložené") : T("This game's state can't be saved", "Stav této hry uložit nejde", "Stav tejto hry sa uložiť nedá")); g_screen = kNone; break;
		case kPLoad: if (!loadState(g_slot)) toast(T("Nothing saved in this slot", "V této pozici nic uloženo není", "V tejto pozícii nie je nič uložené")); g_screen = kNone; break;
		case kPHint: openScreen(kHint); break;
		case kPControls: openScreen(kControls); break;
		case kPScale: case kPFrames: case kPLang: case kPGameLang: case kPFull: adjust(1); break;
		case kPFolder: g_pick = g_game; g_first = false; openScreen(kFolder); browseTo(parentOf(g_gameDir)); break;
		case kPReset: g_core.reset(); g_screen = kNone; break;
		case kPGames: g_wantBackToGames = true; g_screen = kNone; break;
		case kPQuit: g_quit = true; break;
		}
		break;
	case kControls: g_screen = kPause; g_sel = kPControls; break;
	case kHint: g_screen = kPause; g_sel = kPHint; break;
	case kMessage: g_screen = g_game ? kNone : kGames; break;
	}
	g_dirty = true;
}

enum { kUp, kDown, kLeft, kRight, kAccept, kBack, kMenu };

static void uiKey(int ev) {
	const int n = itemCount();
	g_dirty = true;
	switch (ev) {
	case kUp: g_sel = (g_sel + n - 1) % n; break;
	case kDown: g_sel = (g_sel + 1) % n; break;
	case kLeft: adjust(-1); break;
	case kRight: adjust(1); break;
	case kAccept: accept(); break;
	case kBack:
	case kMenu:
		if (g_screen == kFolder) {
			if (ev == kBack && g_browse != "/") browseTo(parentOf(g_browse));
			else if (g_game && !g_first) { g_screen = kPause; g_sel = kPFolder; }
			else if (ev == kMenu) { g_screen = kGames; g_sel = 0; }
		} else if (g_screen == kControls) { g_screen = kPause; g_sel = kPControls; }
		else if (g_screen == kHint) { g_screen = kPause; g_sel = kPHint; }
		else if (g_screen == kLanguage) { if (g_lang.empty()) g_lang = "en"; g_screen = kNone; }
		else if (g_screen == kGames) { if (ev == kMenu) g_quit = true; }
		else if (g_screen == kMessage) { g_screen = g_game ? kNone : kGames; }
		else g_screen = kNone;
		break;
	}
}

static const uint32_t kPanel = 0xE6101820, kText = 0xFFF2F2F2, kDim = 0xFFA8B0BC, kHot = 0xFFFFD27A, kHotBg = 0x40FFD27A,
	kOff = 0xFF6A7280, kLine = 0x50FFFFFF, kGood = 0xFF9BE38F;

struct Draw {
	Canvas &c;
	float u;
	Draw(Canvas &cv) : c(cv), u(cv.h / 720.f) {}
	float text(float x, float y, const std::string &s, float size, uint32_t color, bool bold = false) { return Font_draw(c, x, y, s, size * u, color, bold); }
	float width(const std::string &s, float size, bool bold = false) { return Font_width(s, size * u, bold); }
	void centered(float cx, float y, const std::string &s, float size, uint32_t color, bool bold = false) { text(cx - width(s, size, bold) / 2, y, s, size, color, bold); }
	float para(float x, float y, float w, const std::string &s, float size, uint32_t color, float lead = 1.38f) {
		const std::vector<std::string> lines = Font_wrap(s, size * u, false, w);
		for (size_t i = 0; i < lines.size(); ++i) { text(x, y, lines[i], size, color); y += size * u * lead; }
		return y;
	}
	void panel(float w, float h, float *x, float *y) {
		*x = (c.w - w) / 2;
		*y = (c.h - h) / 2;
		c.rect(0, 0, (float)c.w, (float)c.h, 0x90000000);
		c.rect(*x, *y, w, h, kPanel, 18 * u);
	}
};

static const char *padName(int b) {
	switch (b) {
	case SDL_CONTROLLER_BUTTON_A: return "A";
	case SDL_CONTROLLER_BUTTON_B: return "B";
	case SDL_CONTROLLER_BUTTON_X: return "X";
	case SDL_CONTROLLER_BUTTON_Y: return "Y";
	case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "LB";
	case SDL_CONTROLLER_BUTTON_BACK: return "View / Back";
	case SDL_CONTROLLER_BUTTON_DPAD_UP: return "D-pad ↑";
	case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "D-pad ↓";
	case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "D-pad ←";
	case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "D-pad →";
	case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "L3";
	case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "R3";
	}
	return "";
}

static void render(Canvas &c, const Info &info) {
	Draw d(c);
	const float u = d.u;
	char buf[256];
	if (!g_toast.empty() && nowSec() < g_toastUntil) {
		const float tw = d.width(g_toast, 20, true), ph = 44 * u, pw = tw + 48 * u;
		c.rect((c.w - pw) / 2, c.h - ph - 40 * u, pw, ph, 0xD8101820, ph / 2);
		d.text((c.w - pw) / 2 + 24 * u, c.h - ph - 40 * u + 9 * u, g_toast, 20, kText, true);
	}
	if (g_screen == kNone) return;
	float x, y;
	const std::string keysMove = g_padUsed ? T("D-pad: move   A: pick   B: back", "Křížový ovladač: pohyb   A: vybrat   B: zpět", "Krížový ovládač: pohyb   A: vybrať   B: späť")
		: T("Arrows: move   Enter: pick   Esc: back", "Šipky: pohyb   Enter: vybrat   Esc: zpět", "Šípky: pohyb   Enter: vybrať   Esc: späť");
	if (g_screen == kGames) {
		const float w = 760 * u, h = (190 + (kGameCount + 1) * 64) * u;
		d.panel(w, h, &x, &y);
		d.text(x + 40 * u, y + 26 * u, "Oldies", 34, kText, true);
		d.text(x + w - 40 * u - d.width(OLDIES_VERSION, 13), y + 30 * u, OLDIES_VERSION, 13, kOff);
		d.text(x + 40 * u, y + 74 * u, T("Old DOS games, with your own game files", "Staré hry pro DOS, s vlastními soubory her", "Staré hry pre DOS, s vlastnými súbormi hier"), 16, kDim);
		for (int i = 0; i <= kGameCount; ++i) {
			const float ry = y + 120 * u + i * 64 * u;
			if (i == g_sel) c.rect(x + 24 * u, ry - 6 * u, w - 48 * u, 56 * u, kHotBg, 10 * u);
			if (i == kGameCount) {
				d.text(x + 44 * u, ry + 6 * u, T("Quit", "Ukončit", "Ukončiť"), 22, i == g_sel ? kHot : kText, i == g_sel);
				continue;
			}
			const Game &g = kGames_[i];
			const bool have = g_dirs.count(g.id) && !findGame(g, g_dirs[g.id]).empty();
			d.text(x + 44 * u, ry, g.name, 24, i == g_sel ? kHot : kText, true);
			d.text(x + 44 * u, ry + 30 * u, have ? g_dirs[g.id] : std::string(T("its files' folder will be asked for", "zeptá se na složku se soubory hry", "spýta sa na priečinok so súbormi hry")), 13, have ? kGood : kDim);
			d.text(x + w - 44 * u - d.width(g.year, 15), ry + 8 * u, g.year, 15, kDim);
		}
		d.centered(x + w / 2, y + h - 38 * u, keysMove, 15, kDim);
	} else if (g_screen == kFolder) {
		const float w = 880 * u, h = 620 * u;
		d.panel(w, h, &x, &y);
		snprintf(buf, sizeof(buf), T("Where are the files of %s?", "Kde jsou soubory hry %s?", "Kde sú súbory hry %s?"), g_pick ? g_pick->name : "");
		d.text(x + 36 * u, y + 26 * u, buf, 28, kText, true);
		snprintf(buf, sizeof(buf), T("The game's own files aren't part of this program. Pick the folder that holds them (the one with %s, or a folder above it).",
			"Soubory hry nejsou součástí tohoto programu. Vyber složku, kde jsou (tu, kde je %s, nebo složku nad ní).",
			"Súbory hry nie sú súčasťou tohto programu. Vyber priečinok, kde sú (ten, kde je %s, alebo priečinok nad ním)."), g_pick ? g_pick->exe : "");
		float ty = d.para(x + 36 * u, y + 70 * u, w - 72 * u, buf, 16, kDim);
		d.text(x + 36 * u, ty + 6 * u, g_browse, 17, kHot, true);
		const bool here = g_pick && !findGame(*g_pick, g_browse, 1).empty();
		const float top = ty + 40 * u, rowH = 34 * u;
		const int rows = (int)((y + h - 60 * u - top) / rowH);
		if (g_sel < g_scroll) g_scroll = g_sel;
		if (g_sel >= g_scroll + rows) g_scroll = g_sel - rows + 1;
		for (int i = g_scroll; i < itemCount() && i < g_scroll + rows; ++i) {
			const float ry = top + (i - g_scroll) * rowH;
			std::string label;
			uint32_t color = kText;
			if (i == 0) {
				label = here ? T("✔  The game is here: use this folder", "✔  Hra je tady: použít tuto složku", "✔  Hra je tu: použiť tento priečinok")
					: T("(the game isn't in this folder)", "(v této složce hra není)", "(v tomto priečinku hra nie je)");
				color = here ? kGood : kOff;
			} else if (i == 1) {
				label = T("↑  One folder up", "↑  O složku výš", "↑  O priečinok vyššie");
			} else {
				label = g_browseDirs[i - 2];
				if (g_pick && !findGame(*g_pick, g_browse + "/" + label, 1).empty()) color = kGood;
			}
			if (i == g_sel) c.rect(x + 24 * u, ry - 2 * u, w - 48 * u, rowH - 2 * u, kHotBg, 8 * u);
			d.text(x + 40 * u, ry + 3 * u, label, 19, i == g_sel && color == kText ? kHot : color, i == g_sel);
		}
		d.centered(x + w / 2, y + h - 40 * u, g_padUsed ? T("D-pad: move   A: open / pick   B: one folder up", "Křížový ovladač: pohyb   A: otevřít / vybrat   B: o složku výš", "Krížový ovládač: pohyb   A: otvoriť / vybrať   B: o priečinok vyššie")
			: T("Arrows: move   Enter: open / pick   Backspace: one folder up", "Šipky: pohyb   Enter: otevřít / vybrat   Backspace: o složku výš", "Šípky: pohyb   Enter: otvoriť / vybrať   Backspace: o priečinok vyššie"), 15, kDim);
	} else if (g_screen == kLanguage) {
		const float w = 560 * u, h = 340 * u;
		d.panel(w, h, &x, &y);
		d.centered(x + w / 2, y + 28 * u, "Language  ·  Jazyk", 30, kText, true);
		static const char *names[] = { "English", "Čeština", "Slovenčina" };
		for (int i = 0; i < 3; ++i) {
			const float ry = y + 100 * u + i * 60 * u;
			if (i == g_sel) c.rect(x + 30 * u, ry - 6 * u, w - 60 * u, 50 * u, kHotBg, 10 * u);
			d.text(x + 50 * u, ry, names[i], 24, i == g_sel ? kHot : kText, true);
		}
	} else if (g_screen == kPause) {
		const float w = 740 * u, rowH = 40 * u, h = 110 * u + kPCount * rowH + 52 * u;
		d.panel(w, h, &x, &y);
		d.centered(x + w / 2, y + 22 * u, g_game ? g_game->name : "", 30, kText, true);
		d.text(x + w - 40 * u - d.width("Oldies " OLDIES_VERSION, 13), y + 14 * u, "Oldies " OLDIES_VERSION, 13, kOff);
		for (int i = 0; i < kPCount; ++i) {
			const float ry = y + 84 * u + i * rowH;
			std::string label, value;
			switch (i) {
			case kPResume: label = T("Back to the game", "Zpět do hry", "Späť do hry"); break;
			case kPSave: label = T("Save where you are", "Uložit, kde jsi", "Uložiť, kde si"); snprintf(buf, sizeof(buf), "‹ %s %d ›", T("slot", "pozice", "pozícia"), g_slot); value = buf; break;
			case kPLoad: label = T("Go back to a saved place", "Vrátit se na uložené místo", "Vrátiť sa na uložené miesto"); snprintf(buf, sizeof(buf), "‹ %s %d ›", T("slot", "pozice", "pozícia"), g_slot); value = buf; break;
			case kPHint: label = T("Hints", "Nápověda", "Nápoveda"); break;
			case kPControls: label = T("Controls", "Ovládání", "Ovládanie"); break;
			case kPScale: label = T("Picture", "Obraz", "Obraz"); value = "‹ " + scaleName(g_gfx->mode(), info) + " ›"; break;
			case kPFrames:
				label = T("Frames in between", "Snímky navíc", "Snímky navyše");
				if (g_gfx->frameGen()) { snprintf(buf, sizeof(buf), "‹ %s %d %s ›", T("up to", "až", "až"), info.refresh, T("a second", "za sekundu", "za sekundu")); value = buf; }
				else value = T("‹ off ›", "‹ vypnuto ›", "‹ vypnuté ›");
				break;
			case kPLang: label = T("Menus' language", "Jazyk nabídek", "Jazyk ponúk"); value = std::string("‹ ") + (li() == 1 ? "Čeština" : li() == 2 ? "Slovenčina" : "English") + " ›"; break;
			case kPGameLang: label = T("The game's language", "Jazyk hry", "Jazyk hry");
				if (g_game && gameLangs(*g_game, g_gameDir)) value = std::string("‹ ") + g_game->gameLang[g_gameLang.count(g_game->id) ? g_gameLang[g_game->id] : 0] + " ›";
				else value = T("only the one it has", "jen ten, který má", "len ten, ktorý má");
				break;
			case kPFull: label = T("Full screen", "Celá obrazovka", "Celá obrazovka"); value = g_fullscreen ? T("on", "zapnuto", "zapnuté") : T("off", "vypnuto", "vypnuté"); break;
			case kPFolder: label = T("Game files folder…", "Složka se soubory hry…", "Priečinok so súbormi hry…"); break;
			case kPReset: label = T("Start the game anew", "Spustit hru znovu", "Spustiť hru znova"); break;
			case kPGames: label = T("Another game", "Jiná hra", "Iná hra"); break;
			case kPQuit: label = T("Quit", "Ukončit", "Ukončiť"); break;
			}
			if (i == g_sel) c.rect(x + 24 * u, ry - 4 * u, w - 48 * u, rowH - 4 * u, kHotBg, 9 * u);
			d.text(x + 44 * u, ry + 1 * u, label, 21, i == g_sel ? kHot : kText, i == g_sel);
			if (!value.empty()) d.text(x + w - 44 * u - d.width(value, 18), ry + 3 * u, value, 18, i == g_sel ? kHot : kDim);
		}
		std::string note;
		if (g_sel == kPScale) {
			if (g_gfx->mode() >= kScaleFsr && info.nw) snprintf(buf, sizeof(buf), "%d×%d → %d×%d (%.1f ms) → %d×%d, %s %d×%d @ %d Hz", g_frameW, g_frameH, info.nw, info.nh, info.scaleMs, info.gw, info.gh, T("screen", "obrazovka", "obrazovka"), info.w, info.h, info.refresh);
			else snprintf(buf, sizeof(buf), "%d×%d → %d×%d, %s %d×%d @ %d Hz", g_frameW, g_frameH, info.gw, info.gh, T("screen", "obrazovka", "obrazovka"), info.w, info.h, info.refresh);
			note = buf;
		} else if (g_sel == kPFrames) {
			snprintf(buf, sizeof(buf), T("The game draws a new picture every %d ms; the ones between are made from what moved. That much later on the screen.",
				"Hra kreslí nový obrázek každých %d ms; ty mezi nimi se dopočítají z toho, co se pohnulo. O tolik později na obrazovce.",
				"Hra kreslí nový obrázok každých %d ms; tie medzi nimi sa dopočítajú z toho, čo sa pohlo. O toľko neskôr na obrazovke."), (int)g_frameTick);
			note = buf;
		} else if (g_sel == kPSave || g_sel == kPLoad) {
			note = T("The whole computer's state is kept: it works anywhere in the game", "Uloží se stav celého počítače: funguje kdekoli ve hře", "Uloží sa stav celého počítača: funguje kdekoľvek v hre");
		}
		if (!note.empty()) d.centered(x + w / 2, y + h - 62 * u, note, 14, kDim);
		d.centered(x + w / 2, y + h - 36 * u, g_padUsed ? T("D-pad: move   ◄ ►: change   A: pick   B / Start: back", "Křížový ovladač: pohyb   ◄ ►: změnit   A: vybrat   B / Start: zpět", "Krížový ovládač: pohyb   ◄ ►: zmeniť   A: vybrať   B / Start: späť")
			: T("Arrows: move, ◄ ► change   Enter: pick   Esc: back", "Šipky: pohyb, ◄ ► změnit   Enter: vybrat   Esc: zpět", "Šípky: pohyb, ◄ ► zmeniť   Enter: vybrať   Esc: späť"), 15, kDim);
	} else if (g_screen == kHint) {
		const float w = 900 * u, h = 500 * u;
		d.panel(w, h, &x, &y);
		const int n = hintCount();
		if (!n) {
			d.text(x + 40 * u, y + 40 * u, T("No hints for this game yet", "Pro tuto hru zatím nápověda není", "Pre túto hru zatiaľ nápoveda nie je"), 22, kText);
		} else {
			snprintf(buf, sizeof(buf), "%s %d / %d", T("Hint", "Nápověda", "Nápoveda"), g_hint + 1, n);
			d.text(x + 40 * u, y + 26 * u, buf, 16, kDim, true);
			d.text(x + 40 * u, y + 52 * u, g_game->hints[g_hint].title[li()], 30, kHot, true);
			d.para(x + 40 * u, y + 108 * u, w - 80 * u, g_game->hints[g_hint].text[li()], 20, kText, 1.42f);
		}
		d.centered(x + w / 2, y + h - 40 * u, T("◄ ►: other hints   B / Esc: back", "◄ ►: další nápovědy   B / Esc: zpět", "◄ ►: ďalšie nápovedy   B / Esc: späť"), 15, kDim);
	} else if (g_screen == kControls) {
		const float w = 900 * u, h = 640 * u;
		d.panel(w, h, &x, &y);
		d.text(x + 40 * u, y + 24 * u, T("Controls", "Ovládání", "Ovládanie"), 30, kText, true);
		const float c1 = x + 470 * u, c2 = x + 660 * u;
		float ty = y + 100 * u;
		d.text(c1, ty - 28 * u, T("Controller", "Ovladač", "Ovládač"), 15, kDim, true);
		d.text(c2, ty - 28 * u, T("Keyboard", "Klávesnice", "Klávesnica"), 15, kDim, true);
		struct Row { std::string what, pad, keys; bool hot; };
		std::vector<Row> rows;
		for (int i = 0; g_game && i < kMaxPadKeys && g_game->keys[i].what[0]; ++i) {
			const PadKey &k = g_game->keys[i];
			Row r = { k.what[li()], k.pad, k.keyName, false };
			rows.push_back(r);
		}
		const Row extra[] = {
			{ T("Picture: next upscaling", "Obraz: další zvětšení", "Obraz: ďalšie zväčšenie"), "RB", "F7", true },
			{ T("Frames in between on / off", "Snímky navíc zap. / vyp.", "Snímky navyše zap. / vyp."), "RT", "F8", true },
			{ T("Pause menu", "Nabídka pauzy", "Ponuka pauzy"), "Menu / Start", "F6", false },
			{ T("Quick save / load", "Rychlé uložení / nahrání", "Rýchle uloženie / nahranie"), "L3 / R3", "F5 / F9", false },
			{ T("Full screen", "Celá obrazovka", "Celá obrazovka"), "", "F11", false },
			{ T("Save the picture (PNG)", "Uložit snímek (PNG)", "Uložiť snímku (PNG)"), "", "F12", false },
		};
		for (size_t i = 0; i < sizeof(extra) / sizeof(extra[0]); ++i) rows.push_back(extra[i]);
		const float step = fminf(36 * u, (h - 170 * u) / rows.size());
		for (size_t i = 0; i < rows.size(); ++i) {
			if (i) c.rect(x + 40 * u, ty - 5 * u, w - 80 * u, 1, kLine);
			d.text(x + 40 * u, ty, rows[i].what, 18, rows[i].hot ? kHot : kText, rows[i].hot);
			d.text(c1, ty, rows[i].pad, 18, rows[i].hot ? kHot : kText, true);
			d.text(c2, ty, rows[i].keys, 18, rows[i].hot ? kHot : kText, true);
			ty += step;
		}
		d.centered(x + w / 2, y + h - 40 * u, std::string(g_pad ? T("A controller is connected", "Ovladač je připojen", "Ovládač je pripojený") : T("No controller connected: the keyboard works too", "Ovladač není připojen: jde to i klávesnicí", "Ovládač nie je pripojený: ide to aj klávesnicou"))
			+ "     " + T("B / Esc: back", "B / Esc: zpět", "B / Esc: späť"), 15, kDim);
	} else if (g_screen == kMessage) {
		const float w = 820 * u, h = 360 * u;
		d.panel(w, h, &x, &y);
		d.para(x + 40 * u, y + 40 * u, w - 80 * u, g_message, 20, kText);
		d.centered(x + w / 2, y + h - 40 * u, "OK", 15, kDim);
	}
}

static void present() {
	Info info;
	getInfo(info);
	if (!g_toast.empty() && nowSec() >= g_toastUntil) { g_toast.clear(); g_dirty = true; }
	const bool want = g_screen != kNone || !g_toast.empty();
	if (want) {
		if (g_dirty || !g_overlayShown || g_overlay.w != info.w || g_overlay.h != info.h) {
			g_overlay.resize(info.w, info.h);
			g_overlay.clear();
			render(g_overlay, info);
			g_dirty = false;
			g_gfx->setOverlay(&g_overlay.px[0], g_overlay.w, g_overlay.h);
			g_overlayShown = true;
		}
	} else if (g_overlayShown) {
		g_gfx->setOverlay(0, 0, 0);
		g_overlayShown = false;
	}
	float phase = 1.0f;
	if (g_gfx->frameGen() && g_screen == kNone) {
		phase = (SDL_GetTicks() - g_frameAt) / (float)g_frameTick;
		phase = phase > 1.0f ? 1.0f : phase;
	}
	g_gfx->draw(phase);
	if (!g_pendingShot.empty()) {
		int nw = 0, nh = 0;
		g_gfx->neuralSize(&nw, &nh);
		fprintf(stderr, "shot %s: picture '%s', upscaling %.1f ms to %dx%d, frames in between %s (motion %.1f ms, a new picture every %u ms), game %dx%d shown %dx%d\n",
			g_pendingShot.c_str(), Gfx_modeName(g_gfx->mode()), g_gfx->lastScaleMs(), nw, nh, g_gfx->frameGen() ? "on" : "off", g_gfx->lastMotionMs(), g_frameTick, g_frameW, g_frameH, info.gw, info.gh);
		g_gfx->screenshot(g_pendingShot.c_str());
		g_pendingShot.clear();
	}
	SDL_GL_SwapWindow(g_window);
}

// --- keys and buttons ----------------------------------------------------------------------------------------

static unsigned retroKey(SDL_Keycode k) {
	if (k >= SDLK_a && k <= SDLK_z) return RETROK_a + (k - SDLK_a);
	if (k >= SDLK_0 && k <= SDLK_9) return RETROK_0 + (k - SDLK_0);
	if (k >= SDLK_F1 && k <= SDLK_F12) return RETROK_F1 + (k - SDLK_F1);
	switch (k) {
	case SDLK_UP: return RETROK_UP; case SDLK_DOWN: return RETROK_DOWN; case SDLK_LEFT: return RETROK_LEFT; case SDLK_RIGHT: return RETROK_RIGHT;
	case SDLK_RETURN: return RETROK_RETURN; case SDLK_KP_ENTER: return RETROK_KP_ENTER; case SDLK_SPACE: return RETROK_SPACE;
	case SDLK_ESCAPE: return RETROK_ESCAPE; case SDLK_TAB: return RETROK_TAB; case SDLK_BACKSPACE: return RETROK_BACKSPACE;
	case SDLK_LCTRL: return RETROK_LCTRL; case SDLK_RCTRL: return RETROK_RCTRL; case SDLK_LALT: return RETROK_LALT; case SDLK_RALT: return RETROK_RALT;
	case SDLK_LSHIFT: return RETROK_LSHIFT; case SDLK_RSHIFT: return RETROK_RSHIFT;
	case SDLK_INSERT: return RETROK_INSERT; case SDLK_DELETE: return RETROK_DELETE; case SDLK_HOME: return RETROK_HOME; case SDLK_END: return RETROK_END;
	case SDLK_PAGEUP: return RETROK_PAGEUP; case SDLK_PAGEDOWN: return RETROK_PAGEDOWN;
	case SDLK_MINUS: return RETROK_MINUS; case SDLK_EQUALS: return RETROK_EQUALS; case SDLK_COMMA: return RETROK_COMMA; case SDLK_PERIOD: return RETROK_PERIOD;
	case SDLK_SLASH: return RETROK_SLASH; case SDLK_SEMICOLON: return RETROK_SEMICOLON; case SDLK_QUOTE: return RETROK_QUOTE;
	case SDLK_LEFTBRACKET: return RETROK_LEFTBRACKET; case SDLK_RIGHTBRACKET: return RETROK_RIGHTBRACKET; case SDLK_BACKSLASH: return RETROK_BACKSLASH;
	case SDLK_KP_0: return RETROK_KP0; case SDLK_KP_1: return RETROK_KP1; case SDLK_KP_2: return RETROK_KP2; case SDLK_KP_3: return RETROK_KP3; case SDLK_KP_4: return RETROK_KP4;
	case SDLK_KP_5: return RETROK_KP5; case SDLK_KP_6: return RETROK_KP6; case SDLK_KP_7: return RETROK_KP7; case SDLK_KP_8: return RETROK_KP8; case SDLK_KP_9: return RETROK_KP9;
	}
	return 0;
}

static void openPad() {
	for (int i = 0; !g_pad && i < SDL_NumJoysticks(); ++i) {
		if (SDL_IsGameController(i)) g_pad = SDL_GameControllerOpen(i);
	}
}

// a controller's button for the game: the key the game's own controls have for it (games.h). The four
// directions go together: two of them held are the game's own key for that diagonal, where it has one
// (DOS games read the keypad's 7 9 1 3 for those, not two keys at once)

static unsigned padKeyOf(int button) {
	for (int i = 0; g_game && i < kMaxPadKeys && g_game->keys[i].what[0]; ++i) {
		if (g_game->keys[i].button == button) return g_game->keys[i].key;
	}
	return 0;
}

static void applyDirs() {
	const bool u = g_dirHeld[0] && !g_dirHeld[1], d = g_dirHeld[1] && !g_dirHeld[0];
	const bool l = g_dirHeld[2] && !g_dirHeld[3], r = g_dirHeld[3] && !g_dirHeld[2];
	unsigned want[2] = { 0, 0 };
	const int corner = (u && r) ? 0 : (d && r) ? 1 : (d && l) ? 2 : (u && l) ? 3 : -1;
	if (corner >= 0 && g_game && g_game->diag[corner]) {
		want[0] = g_game->diag[corner];
	} else {
		want[0] = u ? padKeyOf(SDL_CONTROLLER_BUTTON_DPAD_UP) : d ? padKeyOf(SDL_CONTROLLER_BUTTON_DPAD_DOWN) : 0;
		want[1] = l ? padKeyOf(SDL_CONTROLLER_BUTTON_DPAD_LEFT) : r ? padKeyOf(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) : 0;
	}
	for (int i = 0; i < 2; ++i) {
		if (g_dirKeys[i] && g_dirKeys[i] != want[0] && g_dirKeys[i] != want[1]) sendKey(g_dirKeys[i], false);
	}
	for (int i = 0; i < 2; ++i) {
		if (want[i]) sendKey(want[i], true);
		g_dirKeys[i] = want[i];
	}
}

static void padButton(int button, bool down) {
	if (button >= SDL_CONTROLLER_BUTTON_DPAD_UP && button <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
		g_dirHeld[button - SDL_CONTROLLER_BUTTON_DPAD_UP] = down;
		applyDirs();
		return;
	}
	sendKey(padKeyOf(button), down);
}

// the keyboard's arrows are the directions too (a laptop has no keypad for a game that wants one)
static int arrowButton(SDL_Keycode sym) {
	switch (sym) {
	case SDLK_UP: return SDL_CONTROLLER_BUTTON_DPAD_UP;
	case SDLK_DOWN: return SDL_CONTROLLER_BUTTON_DPAD_DOWN;
	case SDLK_LEFT: return SDL_CONTROLLER_BUTTON_DPAD_LEFT;
	case SDLK_RIGHT: return SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
	}
	return -1;
}

static void quickSave() { toast(saveState(g_slot) ? T("Saved", "Uloženo", "Uložené") : T("This game's state can't be saved", "Stav této hry uložit nejde", "Stav tejto hry sa uložiť nedá")); }

static int g_axisX, g_axisY;
static bool g_triggerDown;

static void handleEvent(const SDL_Event &ev) {
	const bool menu = g_screen != kNone;
	switch (ev.type) {
	case SDL_QUIT: g_quit = true; break;
	case SDL_WINDOWEVENT:
		if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || ev.window.event == SDL_WINDOWEVENT_EXPOSED) g_dirty = true;
		if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) releaseAll();
		break;
	case SDL_CONTROLLERDEVICEADDED: openPad(); g_dirty = true; break;
	case SDL_CONTROLLERDEVICEREMOVED:
		if (g_pad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad)) == ev.cdevice.which) {
			SDL_GameControllerClose(g_pad);
			g_pad = 0;
			openPad();
			releaseAll();
			g_dirty = true;
		}
		break;
	case SDL_CONTROLLERAXISMOTION: {
			if (ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
				const bool down = ev.caxis.value > 16384;
				if (down && !g_triggerDown && !menu && g_game) {
					toggleFrameGen();
					Info i;
					getInfo(i);
					char buf[160];
					if (g_gfx->frameGen()) snprintf(buf, sizeof(buf), T("Frames in between: on, up to %d a second", "Snímky navíc: zapnuto, až %d za sekundu", "Snímky navyše: zapnuté, až %d za sekundu"), i.refresh);
					else snprintf(buf, sizeof(buf), "%s", T("Frames in between: off", "Snímky navíc: vypnuto", "Snímky navyše: vypnuté"));
					toast(buf, 3.0f);
				}
				g_triggerDown = down;
				break;
			}
			if (ev.caxis.axis != SDL_CONTROLLER_AXIS_LEFTX && ev.caxis.axis != SDL_CONTROLLER_AXIS_LEFTY) break;
			const bool horizontal = ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX;
			const int dir = ev.caxis.value < -16384 ? -1 : ev.caxis.value > 16384 ? 1 : 0;
			int &was = horizontal ? g_axisX : g_axisY;
			if (dir == was) break;
			was = dir;
			if (menu) {
				if (dir) uiKey(horizontal ? (dir < 0 ? kLeft : kRight) : (dir < 0 ? kUp : kDown));
			} else if (horizontal) {
				padButton(SDL_CONTROLLER_BUTTON_DPAD_LEFT, dir < 0);
				padButton(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, dir > 0);
			} else {
				padButton(SDL_CONTROLLER_BUTTON_DPAD_UP, dir < 0);
				padButton(SDL_CONTROLLER_BUTTON_DPAD_DOWN, dir > 0);
			}
		}
		break;
	case SDL_CONTROLLERBUTTONDOWN:
	case SDL_CONTROLLERBUTTONUP: {
			const bool down = ev.cbutton.state == SDL_PRESSED;
			if (!g_padUsed) { g_padUsed = true; g_dirty = true; }
			if (menu) {
				if (!down) break;
				switch (ev.cbutton.button) {
				case SDL_CONTROLLER_BUTTON_A: uiKey(kAccept); break;
				case SDL_CONTROLLER_BUTTON_B: uiKey(kBack); break;
				case SDL_CONTROLLER_BUTTON_START: uiKey(kMenu); break;
				case SDL_CONTROLLER_BUTTON_DPAD_UP: uiKey(kUp); break;
				case SDL_CONTROLLER_BUTTON_DPAD_DOWN: uiKey(kDown); break;
				case SDL_CONTROLLER_BUTTON_DPAD_LEFT: uiKey(kLeft); break;
				case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: uiKey(kRight); break;
				}
				break;
			}
			switch (ev.cbutton.button) {
			case SDL_CONTROLLER_BUTTON_START:
			case SDL_CONTROLLER_BUTTON_GUIDE: if (down && g_game) openScreen(kPause); break;
			case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
				if (down && g_game) { cycleScale(1); Info i; getInfo(i); toast(scaleName(g_gfx->mode(), i), 3.0f); }
				break;
			case SDL_CONTROLLER_BUTTON_LEFTSTICK: if (down && g_game) quickSave(); break;
			case SDL_CONTROLLER_BUTTON_RIGHTSTICK: if (down && g_game) loadState(g_slot); break;
			default: padButton(ev.cbutton.button, down); break;
			}
		}
		break;
	case SDL_KEYDOWN:
		if (ev.key.repeat && !menu) break;
		if (g_padUsed) { g_padUsed = false; g_dirty = true; }
		if (ev.key.keysym.sym == SDLK_F11 || (ev.key.keysym.sym == SDLK_RETURN && (ev.key.keysym.mod & KMOD_ALT))) {
			g_fullscreen = !g_fullscreen;
			SDL_SetWindowFullscreen(g_window, g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
			saveConfig();
			g_dirty = true;
			break;
		}
		if (ev.key.keysym.sym == SDLK_F12) {
			char name[64];
			snprintf(name, sizeof(name), "/oldies-%03d.png", g_shots++);
			makeDirs(dataHome());
			g_pendingShot = dataHome() + name;
			toast(g_pendingShot, 3.0f);
			break;
		}
		if (menu) {
			switch (ev.key.keysym.sym) {
			case SDLK_UP: uiKey(kUp); break;
			case SDLK_DOWN: uiKey(kDown); break;
			case SDLK_LEFT: uiKey(kLeft); break;
			case SDLK_RIGHT: uiKey(kRight); break;
			case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: uiKey(kAccept); break;
			case SDLK_BACKSPACE: uiKey(kBack); break;
			case SDLK_ESCAPE: case SDLK_F6: uiKey(kMenu); break;
			}
			break;
		}
		// the shell's own keys (F5-F12: the function keys the games here don't use), the rest to the game
		switch (ev.key.keysym.sym) {
		case SDLK_F6: if (g_game) openScreen(kPause); break;
		case SDLK_F7: if (g_game) { cycleScale((ev.key.keysym.mod & KMOD_SHIFT) ? -1 : 1); Info i; getInfo(i); toast(scaleName(g_gfx->mode(), i), 3.0f); } break;
		case SDLK_F8: if (g_game) { toggleFrameGen(); toast(g_gfx->frameGen() ? T("Frames in between: on", "Snímky navíc: zapnuto", "Snímky navyše: zapnuté") : T("Frames in between: off", "Snímky navíc: vypnuto", "Snímky navyše: vypnuté"), 3.0f); } break;
		case SDLK_F5: if (g_game) quickSave(); break;
		case SDLK_F9: if (g_game) loadState(g_slot); break;
		default:
			if (g_game && arrowButton(ev.key.keysym.sym) >= 0) padButton(arrowButton(ev.key.keysym.sym), true);
			else sendKey(retroKey(ev.key.keysym.sym), true);
			break;
		}
		break;
	case SDL_KEYUP:
		if (!menu) {
			if (g_game && arrowButton(ev.key.keysym.sym) >= 0) padButton(arrowButton(ev.key.keysym.sym), false);
			else sendKey(retroKey(ev.key.keysym.sym), false);
		}
		break;
	}
}

// (for testing without hands: OLDIES_SCRIPT=file, lines "milliseconds key NAME | shot PATH | quit")
struct ScriptEvent { uint32_t at; std::string what, arg; };
static std::vector<ScriptEvent> g_script;
static uint32_t g_start;

static void runScript() {
	const uint32_t now = SDL_GetTicks() - g_start;
	while (!g_script.empty() && g_script[0].at <= now) {
		const ScriptEvent e = g_script[0];
		g_script.erase(g_script.begin());
		SDL_Event ev;
		memset(&ev, 0, sizeof(ev));
		if (e.what == "key" || e.what == "release") {
			ev.key.keysym.sym = SDL_GetKeyFromName(e.arg.c_str());
			ev.type = e.what == "key" ? SDL_KEYDOWN : SDL_KEYUP;
			handleEvent(ev);
			if (e.what == "key") {
				ScriptEvent up = { now + 150, "release", e.arg };
				size_t at = 0;
				while (at < g_script.size() && g_script[at].at <= up.at) ++at;
				g_script.insert(g_script.begin() + at, up);
			}
		} else if (e.what == "shot") {
			g_pendingShot = e.arg;
			g_dirty = true;
		} else if (e.what == "quit") {
			g_quit = true;
		}
	}
}

static void pump() {
	SDL_Event ev;
	while (SDL_PollEvent(&ev)) handleEvent(ev);
	runScript();
}

static void message(const std::string &text) {
	g_message = text;
	openScreen(kMessage);
}

// --- a game, started and run ---------------------------------------------------------------------------------

static bool startGame(const Game &game) {
	const std::string dir = findGame(game, g_dirs[game.id]);
	if (dir.empty()) return false;
	const std::string core = corePath(getenv("OLDIES_CORE") ? getenv("OLDIES_CORE") : game.core);
	if (core.empty()) {
		message(std::string(T("The emulator core is missing: ", "Chybí jádro emulátoru: ", "Chýba jadro emulátora: ")) + game.core + "_libretro.so\n\n"
			+ T("Run ./install.sh in the program's folder: it fetches the core. Or put the file into the 'cores' folder there yourself.",
			    "Spusť ./install.sh ve složce programu: jádro stáhne. Nebo dej soubor do tamní složky „cores“ sám.",
			    "Spusti ./install.sh v priečinku programu: jadro stiahne. Alebo daj súbor do tamojšieho priečinka „cores“ sám."));
		return false;
	}
	std::string why;
	if (!loadCore(core, why)) {
		message(std::string(T("The emulator core could not be loaded: ", "Jádro emulátoru se nepodařilo načíst: ", "Jadro emulátora sa nepodarilo načítať: ")) + why);
		return false;
	}
	applyGameLang(game, dir);
	g_game = &game;
	g_gameDir = dir;
	g_systemDir = dataHome() + "/system";
	g_saveDir = dataHome() + "/saves/" + game.id;
	makeDirs(g_systemDir);
	makeDirs(g_saveDir);
	g_options.clear();
	for (int i = 0; game.options[i][0]; ++i) g_options[game.options[i][0]] = game.options[i][1];
	g_keyboardCb = 0;
	g_coreQuit = false;
	memset(g_keysDown, 0, sizeof(g_keysDown));
	g_core.set_environment(environment);
	g_core.init();
	g_core.set_video_refresh(videoRefresh);
	g_core.set_audio_sample(audioSample);
	g_core.set_audio_sample_batch(audioBatch);
	g_core.set_input_poll(inputPoll);
	g_core.set_input_state(inputState);
	const std::string exe = findNoCase(dir, game.exe);
	struct retro_game_info info;
	memset(&info, 0, sizeof(info));
	info.path = exe.c_str();
	if (!g_core.load_game(&info)) {
		g_core.deinit();
		dlclose(g_core.h);
		g_game = 0;
		message(T("The game could not be started by the emulator core.", "Jádro emulátoru hru nespustilo.", "Jadro emulátora hru nespustilo."));
		return false;
	}
	g_core.set_controller_port_device(0, RETRO_DEVICE_KEYBOARD); // (the shell hands the game keys, not a pad)
	g_core.get_system_av_info(&g_av);
	g_avChanged = true;
	g_frameW = g_frameH = 0;
	return true;
}

static void stopGame() {
	if (!g_game) return;
	if (g_audio) { SDL_CloseAudioDevice(g_audio); g_audio = 0; }
	g_core.unload_game();
	g_core.deinit();
	dlclose(g_core.h);
	g_game = 0;
	g_frameW = g_frameH = 0;
}

static void applyAv() {
	g_avChanged = false;
	const int rate = (int)(g_av.timing.sample_rate + 0.5);
	if (rate > 0 && rate != g_audioRate) {
		if (g_audio) SDL_CloseAudioDevice(g_audio);
		SDL_AudioSpec want, got;
		memset(&want, 0, sizeof(want));
		want.freq = rate;
		want.format = AUDIO_S16SYS;
		want.channels = 2;
		want.samples = 1024;
		g_audio = SDL_OpenAudioDevice(0, 0, &want, &got, 0);
		g_audioRate = rate;
		if (g_audio) SDL_PauseAudioDevice(g_audio, 0);
	}
	g_gfx->setAspect(4.f / 3); // (a DOS game's 320x200 or 640x480 filled a 4:3 screen, whatever its pixels' count says)
}

int main(int argc, char *argv[]) {
	std::string wantGame, wantDir;
	int fullscreen = -1;
	for (int i = 1; i < argc; ++i) {
		const std::string a = argv[i];
		if (a == "--window") fullscreen = 0;
		else if (a == "--fullscreen") fullscreen = 1;
		else if (a.compare(0, 7, "--lang=") == 0) g_lang = a.substr(7);
		else if (a == "--help") { printf("Oldies " OLDIES_VERSION "\nusage: oldies [GAME [FOLDER WITH ITS FILES]] [--window] [--fullscreen] [--lang=en|cs|sk]\ngames:"); for (int g = 0; g < kGameCount; ++g) printf(" %s", kGames_[g].id); printf("\n"); return 0; }
		else if (wantGame.empty()) wantGame = a;
		else wantDir = a;
	}
	const std::string cliLang = g_lang;
	loadConfig();
	if (cliLang == "en" || cliLang == "cs" || cliLang == "sk") g_lang = cliLang;
	if (fullscreen >= 0) g_fullscreen = fullscreen != 0;

	setenv("SDL_VIDEO_WAYLAND_WMCLASS", "oldies", 0);
	SDL_SetHint("SDL_APP_ID", "oldies");
	SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
		fprintf(stderr, "SDL: %s\n", SDL_GetError());
		return 1;
	}
	SDL_ShowCursor(SDL_DISABLE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	int winW = 1280, winH = 960;
	if (const char *size = getenv("OLDIES_WINDOW")) sscanf(size, "%dx%d", &winW, &winH);
	g_window = SDL_CreateWindow("Oldies", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, winW, winH, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
	SDL_GLContext gl = g_window ? SDL_GL_CreateContext(g_window) : 0;
	if (!gl) {
		fprintf(stderr, "OpenGL ES 3 is needed and could not be started: %s\n", SDL_GetError());
		return 1;
	}
	SDL_GL_SetSwapInterval(1);
	const std::string res = resourceDir();
	g_gfx = Gfx_create();
	if (!g_gfx->init((res + "/shaders").c_str())) return 1;
	Font_load((res + "/assets").c_str());
	if (SDL_Surface *icon = SDL_LoadBMP((res + "/assets/icon.bmp").c_str())) { SDL_SetWindowIcon(g_window, icon); SDL_FreeSurface(icon); }
	g_gfx->setMode(g_scale);
	g_gfx->setTargetLines(g_fullHd ? 1080 : 0);
	g_gfx->setFrameGen(g_frameGen);
	if (g_fullscreen) SDL_SetWindowFullscreen(g_window, SDL_WINDOW_FULLSCREEN_DESKTOP);
	openPad();
	g_start = SDL_GetTicks();
	if (const char *path = getenv("OLDIES_SCRIPT")) {
		if (FILE *f = fopen(path, "r")) {
			char line[512], what[32], arg[400];
			unsigned at;
			while (fgets(line, sizeof(line), f)) {
				arg[0] = 0;
				if (sscanf(line, "%u %31s %399[^\n]", &at, what, arg) >= 2) { ScriptEvent e = { at, what, arg }; g_script.push_back(e); }
			}
			fclose(f);
		}
	}

	if (g_lang.empty()) {
		openScreen(kLanguage);
		while (g_screen != kNone && !g_quit) { pump(); present(); SDL_Delay(5); }
	}
	saveConfig();
	// the game named on the command line, else the list
	for (int g = 0; g < kGameCount && !wantGame.empty(); ++g) {
		if (wantGame == kGames_[g].id) {
			g_pick = &kGames_[g];
			if (!wantDir.empty() && !findGame(*g_pick, wantDir).empty()) { g_dirs[g_pick->id] = findGame(*g_pick, wantDir); saveConfig(); }
			g_sel = g;
			g_screen = kGames;
			accept();
		}
	}
	if (!g_wantStart && g_screen == kNone) openScreen(kGames);

	double acc = 0;
	uint32_t last = SDL_GetTicks();
	while (!g_quit) {
		pump();
		if (g_wantStart) {
			g_wantStart = false;
			if (g_game) stopGame();
			if (startGame(*g_pick)) { acc = 0; last = SDL_GetTicks(); }
			else if (g_screen == kNone) openScreen(kGames);
		}
		if (g_wantBackToGames) {
			g_wantBackToGames = false;
			stopGame();
			openScreen(kGames);
		}
		const uint32_t now = SDL_GetTicks();
		if (g_game && g_screen == kNone) {
			if (g_avChanged) applyAv();
			const double fps = g_av.timing.fps > 1 ? g_av.timing.fps : 60.0;
			acc += (now - last) * fps / 1000.0;
			if (acc > 4) acc = 4; // (never more than a few frames behind: no rushing after a hitch)
			while (acc >= 1.0) {
				acc -= 1.0;
				g_frameNew = false;
				g_core.run();
				if (g_frameNew && g_frameW) {
					g_gfx->setGame(&g_frame[0], g_frameW, g_frameH);
					const uint32_t t = SDL_GetTicks(), gap = t - g_frameAt;
					g_frameTick = gap < 12 ? 12 : gap > 120 ? 120 : gap;
					g_frameAt = t;
				}
				if (g_coreQuit) { g_wantBackToGames = true; break; }
			}
		} else {
			acc = 0;
			if (g_audio) SDL_ClearQueuedAudio(g_audio);
		}
		last = now;
		const uint32_t t0 = SDL_GetTicks();
		present();
		Info info;
		getInfo(info);
		const uint32_t each = 1000 / (info.refresh > 0 ? info.refresh : 60), took = SDL_GetTicks() - t0;
		if (took + 1 < each) SDL_Delay(each - took - 1); // (where the swap doesn't wait for the screen)
	}
	stopGame();
	saveConfig();
	SDL_Quit();
	return 0;
}
