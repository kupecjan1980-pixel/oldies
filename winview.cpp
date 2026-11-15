/*
 * Oldies: a viewer for a game that runs in an X server of its own (a Windows game under Wine in a small
 * hidden Xwayland). It takes that server's screen many times a second, takes the 256-colour dithering out
 * of it, and shows it through the same picture layer as the DOS games (hard pixels .. FSR 1 .. neural nets,
 * to the screen's resolution). The mouse, the keyboard and a controller are passed on to that server.
 *
 *   oldies-view DISPLAY [TITLE]         e.g. oldies-view :77 "Ace Ventura"
 *
 * F7 next upscaling (Shift+F7 back), F8 dithering removal on / off, F11 full screen, F12 picture as PNG.
 * Controller: left stick the pointer, A left button, B right button, X Esc, Y F4, RB upscaling, LB dithering.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>
#include <SDL.h>
#include "gfx.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xfixes.h>

static Display *g_dpy;
static Window g_root;
static int g_w, g_h;
static XImage *g_img;
static XShmSegmentInfo g_shm;
static SDL_Window *g_window;
static Gfx *g_gfx;
static int g_mode = kScaleNeuralM;
static bool g_fullHd, g_dedither = true, g_fullscreen = true, g_quit;
static int g_threshold = 72; // (how far apart two colours may be and still be taken for a dither pair: 0..255)
static int g_passes = 2;     // (how many times over: each pass reaches a point further)

static std::string configPath() {
	const char *x = getenv("XDG_CONFIG_HOME");
	const std::string dir = (x && *x) ? std::string(x) + "/oldies" : std::string(getenv("HOME") ? getenv("HOME") : ".") + "/.config/oldies";
	mkdir(dir.c_str(), 0755);
	return dir + "/view.ini";
}

static void loadConfig() {
	FILE *f = fopen(configPath().c_str(), "r");
	if (!f) return;
	char k[64];
	int v;
	while (fscanf(f, " %63[^=]=%d", k, &v) == 2) {
		if (!strcmp(k, "scale")) g_mode = v;
		else if (!strcmp(k, "fullhd")) g_fullHd = v != 0;
		else if (!strcmp(k, "dedither")) g_dedither = v != 0;
		else if (!strcmp(k, "fullscreen")) g_fullscreen = v != 0;
		else if (!strcmp(k, "threshold")) g_threshold = v;
		else if (!strcmp(k, "passes")) g_passes = v;
	}
	fclose(f);
	if (g_mode < 0 || g_mode >= kScaleCount) g_mode = kScaleNeuralM;
	if (g_threshold < 4 || g_threshold > 160) g_threshold = 72;
	if (g_passes < 1 || g_passes > 4) g_passes = 2;
}

static void saveConfig() {
	FILE *f = fopen(configPath().c_str(), "w");
	if (!f) return;
	fprintf(f, "scale=%d\nfullhd=%d\ndedither=%d\nfullscreen=%d\nthreshold=%d\npasses=%d\n", g_mode, g_fullHd ? 1 : 0, g_dedither ? 1 : 0, g_fullscreen ? 1 : 0, g_threshold, g_passes);
	fclose(f);
}

static int xError(Display *, XErrorEvent *) { return 0; }
static int xIoError(Display *) { g_quit = true; _exit(0); return 0; }

static bool openCapture(const char *name) {
	for (int tries = 0; tries < 100 && !g_dpy; ++tries) { // (the server may still be starting)
		g_dpy = XOpenDisplay(name);
		if (!g_dpy) usleep(100000);
	}
	if (!g_dpy) return false;
	XSetErrorHandler(xError);
	XSetIOErrorHandler(xIoError);
	g_root = DefaultRootWindow(g_dpy);
	XWindowAttributes a;
	XGetWindowAttributes(g_dpy, g_root, &a);
	g_w = a.width;
	g_h = a.height;
	g_img = XShmCreateImage(g_dpy, a.visual, a.depth, ZPixmap, 0, &g_shm, g_w, g_h);
	if (!g_img || g_img->bits_per_pixel != 32) return false;
	g_shm.shmid = shmget(IPC_PRIVATE, g_img->bytes_per_line * g_img->height, IPC_CREAT | 0600);
	g_shm.shmaddr = g_img->data = (char *)shmat(g_shm.shmid, 0, 0);
	g_shm.readOnly = False;
	XShmAttach(g_dpy, &g_shm);
	XSync(g_dpy, False);
	shmctl(g_shm.shmid, IPC_RMID, 0);
	return true;
}

// 256-colour pictures are dithered: two palette colours in a checkerboard stand for the one between them.
// A point becomes the weighted mean of itself and those of its eight neighbours whose colour is near its
// own; a drawn outline or an edge between two areas is further away than that and stays as sharp as it was.
static void dedither(const uint32_t *src, uint32_t *dst, int w, int h, int threshold) {
	static const int kWeight[9] = { 1, 2, 1, 2, 4, 2, 1, 2, 1 };
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const uint32_t c = src[y * w + x];
			const int cr = c & 255, cg = (c >> 8) & 255, cb = (c >> 16) & 255;
			int r = 0, g = 0, b = 0, sum = 0, k = 0;
			for (int dy = -1; dy <= 1; ++dy) {
				for (int dx = -1; dx <= 1; ++dx, ++k) {
					const int xx = x + dx, yy = y + dy;
					if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
					const uint32_t n = src[yy * w + xx];
					const int nr = n & 255, ng = (n >> 8) & 255, nb = (n >> 16) & 255;
					if (abs(nr - cr) > threshold || abs(ng - cg) > threshold || abs(nb - cb) > threshold) continue;
					r += nr * kWeight[k]; g += ng * kWeight[k]; b += nb * kWeight[k];
					sum += kWeight[k];
				}
			}
			dst[y * w + x] = 0xFF000000u | ((b / sum) << 16) | ((g / sum) << 8) | (r / sum);
		}
	}
}

// the server's pointer picture, drawn into the frame (a screen grab doesn't have it)
static void drawCursor(uint32_t *frame, int w, int h) {
	XFixesCursorImage *c = XFixesGetCursorImage(g_dpy);
	if (!c) return;
	for (int y = 0; y < (int)c->height; ++y) {
		for (int x = 0; x < (int)c->width; ++x) {
			const int px = c->x - c->xhot + x, py = c->y - c->yhot + y;
			if (px < 0 || py < 0 || px >= w || py >= h) continue;
			const uint32_t p = (uint32_t)c->pixels[y * c->width + x]; // (ARGB, premultiplied)
			const int a = p >> 24;
			if (!a) continue;
			uint32_t &d = frame[py * w + px];
			const int r = ((p >> 16) & 255) + (d & 255) * (255 - a) / 255;
			const int g = ((p >> 8) & 255) + ((d >> 8) & 255) * (255 - a) / 255;
			const int b = (p & 255) + ((d >> 16) & 255) * (255 - a) / 255;
			d = 0xFF000000u | ((b > 255 ? 255 : b) << 16) | ((g > 255 ? 255 : g) << 8) | (r > 255 ? 255 : r);
		}
	}
	XFree(c);
}

static KeySym keysymOf(SDL_Keycode k) {
	if (k >= 32 && k < 127) return (KeySym)k;
	switch (k) {
	case SDLK_RETURN: return XK_Return;
	case SDLK_KP_ENTER: return XK_KP_Enter;
	case SDLK_ESCAPE: return XK_Escape;
	case SDLK_BACKSPACE: return XK_BackSpace;
	case SDLK_TAB: return XK_Tab;
	case SDLK_DELETE: return XK_Delete;
	case SDLK_INSERT: return XK_Insert;
	case SDLK_HOME: return XK_Home;
	case SDLK_END: return XK_End;
	case SDLK_PAGEUP: return XK_Page_Up;
	case SDLK_PAGEDOWN: return XK_Page_Down;
	case SDLK_UP: return XK_Up;
	case SDLK_DOWN: return XK_Down;
	case SDLK_LEFT: return XK_Left;
	case SDLK_RIGHT: return XK_Right;
	case SDLK_LSHIFT: return XK_Shift_L;
	case SDLK_RSHIFT: return XK_Shift_R;
	case SDLK_LCTRL: return XK_Control_L;
	case SDLK_RCTRL: return XK_Control_R;
	case SDLK_LALT: return XK_Alt_L;
	case SDLK_RALT: return XK_Alt_R;
	}
	if (k >= SDLK_F1 && k <= SDLK_F12) return XK_F1 + (k - SDLK_F1);
	return NoSymbol;
}

static void sendKey(KeySym sym, bool down) {
	const KeyCode code = sym != NoSymbol ? XKeysymToKeycode(g_dpy, sym) : 0;
	if (code) XTestFakeKeyEvent(g_dpy, code, down ? True : False, CurrentTime);
}

static void sendPointer(int winX, int winY) {
	int ww, wh, dw, dh, rx, ry, rw, rh;
	SDL_GetWindowSize(g_window, &ww, &wh);
	SDL_GL_GetDrawableSize(g_window, &dw, &dh);
	g_gfx->gameRect(&rx, &ry, &rw, &rh);
	if (ww <= 0 || wh <= 0 || rw <= 0 || rh <= 0) return;
	int x = (int)(((double)winX * dw / ww - rx) * g_w / rw), y = (int)(((double)winY * dh / wh - ry) * g_h / rh);
	x = x < 0 ? 0 : x >= g_w ? g_w - 1 : x;
	y = y < 0 ? 0 : y >= g_h ? g_h - 1 : y;
	XTestFakeMotionEvent(g_dpy, -1, x, y, CurrentTime);
}

static void applyMode(const char *title) {
	int dw, dh;
	SDL_GL_GetDrawableSize(g_window, &dw, &dh);
	while (!g_gfx->modeAvailable(g_mode) && g_mode > 0) --g_mode;
	g_gfx->setMode(g_mode);
	g_gfx->setTargetLines((g_fullHd && g_mode >= kScaleFsr && dh > 1080) ? 1080 : 0);
	char buf[200];
	snprintf(buf, sizeof(buf), "%s  ·  %s%s%s", title, Gfx_modeName(g_mode), (g_fullHd && g_mode >= kScaleFsr && dh > 1080) ? " (Full HD)" : "",
		g_dedither ? "  ·  dithering removed" : "");
	SDL_SetWindowTitle(g_window, buf);
	saveConfig();
}

// the next way of upscaling; on a screen bigger than Full HD, FSR and the nets come twice (to Full HD, to the screen)
static void cycleMode(int dir) {
	int dw, dh;
	SDL_GL_GetDrawableSize(g_window, &dw, &dh);
	std::vector<std::pair<int, bool> > ways;
	for (int m = 0; m < kScaleCount; ++m) {
		if (!g_gfx->modeAvailable(m)) continue;
		if (m >= kScaleFsr && dh > 1080) ways.push_back(std::make_pair(m, true));
		ways.push_back(std::make_pair(m, false));
	}
	int at = 0;
	for (size_t k = 0; k < ways.size(); ++k) {
		if (ways[k].first == g_mode && (ways[k].second == g_fullHd || g_mode < kScaleFsr || dh <= 1080)) { at = (int)k; break; }
	}
	at = (at + dir + (int)ways.size()) % (int)ways.size();
	g_mode = ways[at].first;
	g_fullHd = ways[at].second;
}

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: oldies-view DISPLAY [TITLE]\n");
		return 2;
	}
	const char *title = argc > 2 ? argv[2] : "Oldies";
	loadConfig();
	if (!openCapture(argv[1])) {
		fprintf(stderr, "oldies-view: can't take the screen of %s\n", argv[1]);
		return 1;
	}
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) return 1;
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	int winW = 1280, winH = 960;
	if (const char *size = getenv("OLDIES_WINDOW")) { sscanf(size, "%dx%d", &winW, &winH); g_fullscreen = false; }
	g_window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, winW, winH,
		SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
	SDL_GLContext gl = g_window ? SDL_GL_CreateContext(g_window) : 0;
	if (!gl) { fprintf(stderr, "oldies-view: no OpenGL ES 3: %s\n", SDL_GetError()); return 1; }
	SDL_GL_SetSwapInterval(1);
	char exe[4096];
	const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	std::string dir = ".";
	if (n > 0) { exe[n] = 0; dir = exe; dir = dir.substr(0, dir.rfind('/')); }
	g_gfx = Gfx_create();
	if (!g_gfx->init((dir + "/shaders").c_str())) return 1;
	g_gfx->setAspect((float)g_w / g_h);
	int dw, dh;
	SDL_GL_GetDrawableSize(g_window, &dw, &dh);
	g_gfx->resize(dw, dh);
	applyMode(title);
	SDL_ShowCursor(SDL_DISABLE);

	SDL_GameController *pad = 0;
	std::vector<uint32_t> raw(g_w * g_h), shown(g_w * g_h);
	uint32_t lastHash = 0;
	unsigned long lastCursor = 0;
	int lastCx = -1, lastCy = -1;
	bool dirty = true, lastDedither = g_dedither;
	double padX = g_w / 2, padY = g_h / 2;
	const char *script = getenv("OLDIES_VIEW_SHOT"); // (for testing: "SECONDS:FILE.png", then quit)
	const Uint32 start = SDL_GetTicks();

	while (!g_quit) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			switch (ev.type) {
			case SDL_QUIT: g_quit = true; break;
			case SDL_WINDOWEVENT:
				if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || ev.window.event == SDL_WINDOWEVENT_EXPOSED) {
					SDL_GL_GetDrawableSize(g_window, &dw, &dh);
					g_gfx->resize(dw, dh);
					applyMode(title);
					dirty = true;
				}
				break;
			case SDL_MOUSEMOTION: sendPointer(ev.motion.x, ev.motion.y); break;
			case SDL_MOUSEBUTTONDOWN:
			case SDL_MOUSEBUTTONUP: {
					sendPointer(ev.button.x, ev.button.y);
					const int b = ev.button.button == SDL_BUTTON_LEFT ? 1 : ev.button.button == SDL_BUTTON_MIDDLE ? 2 : ev.button.button == SDL_BUTTON_RIGHT ? 3 : 0;
					if (b) XTestFakeButtonEvent(g_dpy, b, ev.type == SDL_MOUSEBUTTONDOWN ? True : False, CurrentTime);
				}
				break;
			case SDL_KEYDOWN:
				if (ev.key.repeat) break;
				if (ev.key.keysym.sym == SDLK_F7) { cycleMode((ev.key.keysym.mod & KMOD_SHIFT) ? -1 : 1); applyMode(title); dirty = true; break; }
				if (ev.key.keysym.sym == SDLK_F8) { g_dedither = !g_dedither; applyMode(title); break; }
				if (ev.key.keysym.sym == SDLK_F11) {
					g_fullscreen = !g_fullscreen;
					SDL_SetWindowFullscreen(g_window, g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
					saveConfig();
					break;
				}
				if (ev.key.keysym.sym == SDLK_F12) {
					char path[300];
					snprintf(path, sizeof(path), "%s/oldies-%u.png", getenv("HOME") ? getenv("HOME") : ".", (unsigned)time(0));
					g_gfx->screenshot(path);
					break;
				}
				sendKey(keysymOf(ev.key.keysym.sym), true);
				break;
			case SDL_KEYUP: sendKey(keysymOf(ev.key.keysym.sym), false); break;
			case SDL_CONTROLLERDEVICEADDED:
				if (!pad) pad = SDL_GameControllerOpen(ev.cdevice.which);
				break;
			case SDL_CONTROLLERDEVICEREMOVED:
				if (pad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)) == ev.cdevice.which) { SDL_GameControllerClose(pad); pad = 0; }
				break;
			case SDL_CONTROLLERBUTTONDOWN:
			case SDL_CONTROLLERBUTTONUP: {
					const bool down = ev.type == SDL_CONTROLLERBUTTONDOWN;
					switch (ev.cbutton.button) {
					case SDL_CONTROLLER_BUTTON_A: XTestFakeButtonEvent(g_dpy, 1, down ? True : False, CurrentTime); break;
					case SDL_CONTROLLER_BUTTON_B: XTestFakeButtonEvent(g_dpy, 3, down ? True : False, CurrentTime); break;
					case SDL_CONTROLLER_BUTTON_X: sendKey(XK_Escape, down); break;
					case SDL_CONTROLLER_BUTTON_Y:
					case SDL_CONTROLLER_BUTTON_START: sendKey(XK_F4, down); break;
					case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: if (down) { cycleMode(1); applyMode(title); dirty = true; } break;
					case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: if (down) { g_dedither = !g_dedither; applyMode(title); } break;
					}
				}
				break;
			}
		}
		if (pad) { // (the left stick moves the pointer: slowly near the middle, faster further out)
			const double ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX) / 32768.0, ay = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY) / 32768.0;
			if (ax * ax + ay * ay > 0.03) {
				Window r, c; int rx, ry, wx, wy; unsigned mask;
				if (XQueryPointer(g_dpy, g_root, &r, &c, &rx, &ry, &wx, &wy, &mask) && (abs(rx - (int)padX) > 2 || abs(ry - (int)padY) > 2)) { padX = rx; padY = ry; }
				padX += ax * (2 + 7 * (ax < 0 ? -ax : ax));
				padY += ay * (2 + 7 * (ay < 0 ? -ay : ay));
				padX = padX < 0 ? 0 : padX > g_w - 1 ? g_w - 1 : padX;
				padY = padY < 0 ? 0 : padY > g_h - 1 ? g_h - 1 : padY;
				XTestFakeMotionEvent(g_dpy, -1, (int)padX, (int)padY, CurrentTime);
			}
		}
		XFlush(g_dpy);

		if (!XShmGetImage(g_dpy, g_root, g_img, 0, 0, AllPlanes)) { SDL_Delay(50); continue; }
		uint32_t hash = 2166136261u;
		const uint32_t *px = (const uint32_t *)g_img->data;
		for (int i = 0; i < g_w * g_h; i += 3) hash = (hash ^ px[i]) * 16777619u;
		XFixesCursorImage *cur = XFixesGetCursorImage(g_dpy);
		const unsigned long serial = cur ? cur->cursor_serial : 0;
		const int cx = cur ? cur->x : -1, cy = cur ? cur->y : -1;
		if (cur) XFree(cur);
		if (hash != lastHash || serial != lastCursor || cx != lastCx || cy != lastCy || g_dedither != lastDedither) {
			lastHash = hash; lastCursor = serial; lastCx = cx; lastCy = cy; lastDedither = g_dedither;
			for (int i = 0; i < g_w * g_h; ++i) { // (BGRX -> RGBA)
				const uint32_t c = px[i];
				raw[i] = 0xFF000000u | ((c & 255) << 16) | (c & 0xFF00) | ((c >> 16) & 255);
			}
			if (g_dedither) {
				for (int pass = 0; pass < g_passes; ++pass) {
					dedither(&raw[0], &shown[0], g_w, g_h, g_threshold);
					if (pass + 1 < g_passes) raw.swap(shown);
				}
			} else {
				shown = raw;
			}
			drawCursor(&shown[0], g_w, g_h);
			g_gfx->setGame(&shown[0], g_w, g_h);
			dirty = true;
		}
		const bool shotNow = script && SDL_GetTicks() - start > (Uint32)(atof(script) * 1000);
		if (dirty || shotNow) {
			g_gfx->draw();
			if (shotNow) {
				const char *file = strchr(script, ':');
				if (file) g_gfx->screenshot(file + 1);
				g_quit = true;
			}
			SDL_GL_SwapWindow(g_window);
			dirty = false;
		} else {
			SDL_Delay(8);
		}
	}
	saveConfig();
	return 0;
}
