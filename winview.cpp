/*
 * Oldies: a viewer for a game that runs in an X server of its own (a Windows game under Wine in a small
 * hidden Xwayland). It takes that server's screen many times a second, takes the 256-colour dithering out
 * of it, and shows it through the same picture layer as the DOS games (hard pixels .. FSR 1 .. neural nets,
 * to the screen's resolution). The mouse, the keyboard and a controller are passed on to that server.
 *
 *   oldies-view DISPLAY [TITLE] [--subs PACK --game FOLDER --spy SOCKET]      e.g. oldies-view :77 "Ace Ventura"
 *
 * Subtitles (subs.cpp), for a game that only speaks: PACK holds the lines of the game's speech clips, FOLDER
 * the game's data files, and at SOCKET tgspy.so (preloaded into Wine) tells the game's sound.
 *
 * F7 next upscaling (Shift+F7 back), F8 dithering removal on / off, F9 subtitles (none, English, German,
 * Czech), F10 marks the things that can be used (once more: marks away), F11 full screen, F12 picture as PNG.
 * Controller: left stick the pointer, A left button, B right button, X Esc, Y F4, RB upscaling, LB dithering,
 * Back subtitles, a stick pressed marks the things that can be used.
 */

#include <math.h>
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
#include "subs.h"
#include "ui.h"
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
static bool g_haveSubs;      // a subtitle pack is there and the game's sound is listened to
static int g_subs = 1;       // subtitles: 0 none, 1 English, 2 German, 3 Czech
static const char *const kSubsName[1 + kSubsLangs] = { "off", "English", "Deutsch", "Česky" };
static Canvas g_overlay;
// Things that can be used: the game draws its own pointer into the picture, an arrow, and a hand over a thing
// that can be used. F10 takes the pointer over the whole picture (the picture shown stands still meanwhile),
// sees where it turned into the hand, and marks those places for a while.
enum { kScanStep = 24, kScanCols = 640 / kScanStep + 1, kScanRows = 480 / kScanStep + 1 };
static bool g_scan;
static int g_scanAt, g_scanBackX, g_scanBackY;
static Uint32 g_scanSent;
static std::vector<char> g_scanHit;
// The game writes the name of the thing under the pointer small and faint into the bottom left corner, in one
// colour: taken from there as a shape (no reading), it's shown big at the top, and beside each mark.
enum { kLabelX = 78, kLabelY = 449, kLabelW = 340, kLabelH = 21 };
typedef std::vector<uint8_t> Label; // kLabelW x kLabelH, 1 where the letters are; empty: none
struct Mark { int x, y; Label label; };
static std::vector<Mark> g_marks; // in the game's picture
static Label g_hover;
static std::vector<Label> g_scanLabel;
static Uint32 g_marksUntil;
static int g_marksSerial;
static std::string g_note;   // a short note in the corner (what a key just changed), until g_noteUntil
static Uint32 g_noteUntil;

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
		else if (!strcmp(k, "subs")) g_subs = v;
	}
	fclose(f);
	if (g_mode < 0 || g_mode >= kScaleCount) g_mode = kScaleNeuralM;
	if (g_threshold < 4 || g_threshold > 160) g_threshold = 72;
	if (g_passes < 1 || g_passes > 4) g_passes = 2;
	if (g_subs < 0 || g_subs > kSubsLangs) g_subs = 1;
}

static void saveConfig() {
	FILE *f = fopen(configPath().c_str(), "w");
	if (!f) return;
	fprintf(f, "scale=%d\nfullhd=%d\ndedither=%d\nfullscreen=%d\nthreshold=%d\npasses=%d\nsubs=%d\n", g_mode, g_fullHd ? 1 : 0, g_dedither ? 1 : 0, g_fullscreen ? 1 : 0, g_threshold, g_passes, g_subs);
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

// the game's pointer at (x, y) of a frame (BGRX): 1 the arrow, 2 the hand, 0 neither seen
static int pointerAt(const uint32_t *px, int w, int h, int x, int y) {
	int red = 0, skin = 0;
	for (int j = y - 14; j <= y + 24; ++j) {
		if (j < 0 || j >= h) continue;
		for (int i = x - 14; i <= x + 21; ++i) {
			if (i < 0 || i >= w) continue;
			const uint32_t c = px[j * w + i];
			const int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
			if (i >= x - 1 && j >= y - 1 && abs(r - 255) < 14 && abs(g - 50) < 14 && abs(b - 10) < 14) ++red;
			if (i <= x + 14 && j <= y + 13 && abs(r - 246) < 14 && abs(g - 141) < 14 && abs(b - 106) < 14) ++skin;
		}
	}
	// (at the picture's edge only a part of either shows)
	const bool edge = x < 14 || y < 14 || x > w - 22 || y > h - 25;
	if (skin >= (edge ? 50 : 120)) return 2;
	if (red >= (edge ? 25 : 70)) return 1;
	return 0;
}

static void labelOf(const uint32_t *px, int w, int h, Label &out) {
	out.clear();
	if (w < kLabelX + kLabelW || h < kLabelY + kLabelH) return;
	Label m(kLabelW * kLabelH, 0);
	int n = 0, left = kLabelW;
	for (int j = 0; j < kLabelH; ++j) {
		for (int i = 0; i < kLabelW; ++i) {
			if ((px[(kLabelY + j) * w + kLabelX + i] & 0xFFFFFF) == 0xD17655) { m[j * kLabelW + i] = 1; ++n; if (i < left) left = i; }
		}
	}
	if (n >= 40 && left >= 1 && left <= 5) out.swap(m); // (the writing starts at the same place every time)
}

// a label drawn into the overlay, its left top at (x, y), a point of the game's `scale` of the screen's
static void drawLabel(const Label &m, float x, float y, float scale) {
	int right = 0;
	for (int j = 0; j < kLabelH; ++j) for (int i = right; i < kLabelW; ++i) if (m[j * kLabelW + i]) right = i;
	const float pad = 5 * scale, tw = (right + 2) * scale, th = kLabelH * scale;
	if (x + tw + 2 * pad > g_overlay.w) x = g_overlay.w - tw - 2 * pad;
	if (x < 0) x = 0;
	if (y + th + 2 * pad > g_overlay.h) y = g_overlay.h - th - 2 * pad;
	if (y < 0) y = 0;
	g_overlay.rect(x, y, tw + 2 * pad, th + 2 * pad, 0xC8000000u, pad);
	const int x0 = (int)(x + pad), y0 = (int)(y + pad);
	for (int j = 0; j < (int)th; ++j) {
		const int py = y0 + j;
		if (py < 0 || py >= g_overlay.h) continue;
		for (int i = 0; i < (int)tw; ++i) {
			const int px = x0 + i;
			if (px < 0 || px >= g_overlay.w) continue;
			// (between the shape's points: smooth edges)
			const float fx = (i + 0.5f) / scale - 0.5f, fy = (j + 0.5f) / scale - 0.5f;
			const int ix = (int)floorf(fx), iy = (int)floorf(fy);
			const float ax = fx - ix, ay = fy - iy;
			float a = 0;
			for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) {
				const int sx = ix + dx, sy = iy + dy;
				if (sx < 0 || sy < 0 || sx >= kLabelW || sy >= kLabelH || !m[sy * kLabelW + sx]) continue;
				a += (dx ? ax : 1 - ax) * (dy ? ay : 1 - ay);
			}
			a = (a - 0.3f) / 0.4f;
			if (a <= 0) continue;
			if (a > 1) a = 1;
			uint8_t *p = (uint8_t *)&g_overlay.px[py * g_overlay.w + px]; // (premultiplied RGBA)
			const float keep = 1 - a;
			p[0] = (uint8_t)(255 * a + p[0] * keep);
			p[1] = (uint8_t)(236 * a + p[1] * keep);
			p[2] = (uint8_t)(150 * a + p[2] * keep);
			p[3] = (uint8_t)(255 * a + p[3] * keep);
		}
	}
}

static void scanPoint(int at, int *x, int *y) {
	*x = (at % kScanCols) * kScanStep + kScanStep / 2;
	*y = (at / kScanCols) * kScanStep + kScanStep / 2;
	if (*x > g_w - 2) *x = g_w - 2;
	if (*y > g_h - 2) *y = g_h - 2;
}

static void scanStart() {
	Window r, c; int rx = g_w / 2, ry = g_h / 2, wx, wy; unsigned mask;
	XQueryPointer(g_dpy, g_root, &r, &c, &rx, &ry, &wx, &wy, &mask);
	g_scanBackX = rx; g_scanBackY = ry;
	g_scan = true;
	g_scanAt = 0;
	g_scanHit.assign(kScanCols * kScanRows, 0);
	g_scanLabel.assign(kScanCols * kScanRows, Label());
	g_marks.clear();
	++g_marksSerial;
	int x, y;
	scanPoint(0, &x, &y);
	XTestFakeMotionEvent(g_dpy, -1, x, y, CurrentTime);
	g_scanSent = SDL_GetTicks();
}

// the places where the hand showed, one mark a thing: neighbouring places with the same name are one thing
static void scanFinish() {
	g_scan = false;
	XTestFakeMotionEvent(g_dpy, -1, g_scanBackX, g_scanBackY, CurrentTime);
	// (a place whose name didn't show in time takes a neighbour's: most likely the same thing)
	for (int pass = 0; pass < 3; ++pass) {
		for (int at = 0; at < (int)g_scanHit.size(); ++at) {
			if (!g_scanHit[at] || !g_scanLabel[at].empty()) continue;
			const int cx = at % kScanCols, cy = at / kScanCols;
			for (int dy = -1; dy <= 1 && g_scanLabel[at].empty(); ++dy) for (int dx = -1; dx <= 1; ++dx) {
				const int nx = cx + dx, ny = cy + dy;
				if (nx < 0 || ny < 0 || nx >= kScanCols || ny >= kScanRows) continue;
				if (g_scanHit[ny * kScanCols + nx] && !g_scanLabel[ny * kScanCols + nx].empty()) { g_scanLabel[at] = g_scanLabel[ny * kScanCols + nx]; break; }
			}
		}
	}
	std::vector<char> seen(g_scanHit.size(), 0);
	for (int start = 0; start < (int)g_scanHit.size(); ++start) {
		if (!g_scanHit[start] || seen[start]) continue;
		std::vector<int> group(1, start);
		seen[start] = 1;
		for (size_t k = 0; k < group.size(); ++k) {
			const int cx = group[k] % kScanCols, cy = group[k] / kScanCols;
			for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
				const int nx = cx + dx, ny = cy + dy;
				if (nx < 0 || ny < 0 || nx >= kScanCols || ny >= kScanRows) continue;
				const int n = ny * kScanCols + nx;
				if (g_scanHit[n] && !seen[n] && g_scanLabel[n] == g_scanLabel[start]) { seen[n] = 1; group.push_back(n); }
			}
		}
		double mx = 0, my = 0;
		for (size_t k = 0; k < group.size(); ++k) { mx += group[k] % kScanCols; my += group[k] / kScanCols; }
		mx /= group.size(); my /= group.size();
		int best = group[0];
		double bestD = 1e9;
		for (size_t k = 0; k < group.size(); ++k) { // (the mark on a place of the thing itself, the nearest to its middle)
			const double d = (group[k] % kScanCols - mx) * (group[k] % kScanCols - mx) + (group[k] / kScanCols - my) * (group[k] / kScanCols - my);
			if (d < bestD) { bestD = d; best = group[k]; }
		}
		Mark p;
		scanPoint(best, &p.x, &p.y);
		p.label = g_scanLabel[start];
		g_marks.push_back(p);
	}
	g_marksUntil = SDL_GetTicks() + 15000;
	++g_marksSerial;
}

static void note(const std::string &text) {
	g_note = text;
	g_noteUntil = SDL_GetTicks() + 2500;
}

// the texts over the picture: the subtitle at the bottom of the game's picture, the note in its top left corner
static void renderOverlay(int dw, int dh, const std::string &subtitle, const std::string &corner) {
	g_overlay.resize(dw, dh);
	g_overlay.clear();
	int gx, gy, gw, gh;
	g_gfx->gameRect(&gx, &gy, &gw, &gh);
	if (!subtitle.empty()) {
		const float size = gh * 0.042f, pad = size * 0.45f;
		std::vector<std::string> lines;
		for (size_t at = 0; at <= subtitle.size();) { // (one speaker a paragraph)
			size_t nl = subtitle.find('\n', at);
			if (nl == std::string::npos) nl = subtitle.size();
			const std::vector<std::string> part = Font_wrap(subtitle.substr(at, nl - at), size, true, gw * 0.84f);
			lines.insert(lines.end(), part.begin(), part.end());
			at = nl + 1;
		}
		if (lines.size() > 5) lines.erase(lines.begin(), lines.end() - 5);
		float widest = 0;
		for (size_t i = 0; i < lines.size(); ++i) { const float w = Font_width(lines[i], size, true); if (w > widest) widest = w; }
		const float lineH = size * 1.3f, boxH = lines.size() * lineH + 2 * pad;
		const float top = gy + gh - gh * 0.035f - boxH;
		g_overlay.rect(gx + (gw - widest) / 2 - 2 * pad, top, widest + 4 * pad, boxH, 0xB8000000u, pad);
		for (size_t i = 0; i < lines.size(); ++i) {
			Font_draw(g_overlay, gx + (gw - Font_width(lines[i], size, true)) / 2, top + pad + i * lineH, lines[i], size, 0xFFFFFFFFu, true);
		}
	}
	if (!g_scan) {
		for (size_t i = 0; i < g_marks.size(); ++i) {
			const float cx = gx + (g_marks[i].x + 0.5f) * gw / g_w, cy = gy + (g_marks[i].y + 0.5f) * gh / g_h, r = gh * 0.022f;
			g_overlay.ring(cx, cy, r, r * 0.42f, 0xC0000000u);
			g_overlay.ring(cx, cy, r, r * 0.22f, 0xFFFFE040u);
			g_overlay.circle(cx, cy, r * 0.28f, 0xFFFFE040u);
		}
		for (size_t i = 0; i < g_marks.size(); ++i) { // (the names over the rings)
			if (g_marks[i].label.empty()) continue;
			const float cx = gx + (g_marks[i].x + 0.5f) * gw / g_w, cy = gy + (g_marks[i].y + 0.5f) * gh / g_h, r = gh * 0.022f;
			drawLabel(g_marks[i].label, cx + r * 1.5f, cy - r * 1.2f, gh / 480.f * 0.8f);
		}
		if (!g_hover.empty() && g_marks.empty()) {
			const float scale = gh / 480.f * 1.6f;
			int right = 0;
			for (int j = 0; j < kLabelH; ++j) for (int i = right; i < kLabelW; ++i) if (g_hover[j * kLabelW + i]) right = i;
			drawLabel(g_hover, gx + (gw - (right + 12) * scale) / 2, gy + gh * 0.03f, scale);
		}
	}
	if (!corner.empty()) {
		const float size = gh * 0.03f, pad = size * 0.5f;
		const float w = Font_width(corner, size, false);
		g_overlay.rect(gx + size, gy + size, w + 2 * pad, size * 1.3f + 2 * pad, 0xB8000000u, pad);
		Font_draw(g_overlay, gx + size + pad, gy + size + pad, corner, size, 0xFFFFFFFFu, false);
	}
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
	note(std::string(Gfx_modeName(g_mode)) + ((g_fullHd && g_mode >= kScaleFsr && dh > 1080) ? " (Full HD)" : "") + (g_dedither ? "  ·  dithering removed" : "  ·  dithering as it is"));
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
		fprintf(stderr, "usage: oldies-view DISPLAY [TITLE] [--subs PACK --game FOLDER --spy SOCKET]\n");
		return 2;
	}
	const char *title = argc > 2 && argv[2][0] != '-' ? argv[2] : "Oldies";
	const char *subsPack = 0, *gameDir = 0, *spySock = 0;
	for (int i = 2; i + 1 < argc; ++i) {
		if (!strcmp(argv[i], "--subs")) subsPack = argv[++i];
		else if (!strcmp(argv[i], "--game")) gameDir = argv[++i];
		else if (!strcmp(argv[i], "--spy")) spySock = argv[++i];
	}
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
	g_note.clear();
	SDL_ShowCursor(SDL_DISABLE);
	Font_load((dir + "/assets").c_str());
	if (subsPack && gameDir && spySock) g_haveSubs = Subs_start(gameDir, subsPack, spySock);
	if (subsPack && !g_haveSubs) fprintf(stderr, "oldies-view: no subtitles (%s)\n", subsPack);
	std::string shownSub, shownNote;
	bool overlayShown = false;
	int shownMarks = 0;
	const char *scanTest = getenv("OLDIES_VIEW_SCAN"); // (for testing: the things looked for after so many seconds)

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
			case SDL_MOUSEMOTION: if (!g_scan) sendPointer(ev.motion.x, ev.motion.y); break;
			case SDL_MOUSEBUTTONDOWN:
			case SDL_MOUSEBUTTONUP: {
					if (g_scan) break;
					if (!g_marks.empty()) { g_marks.clear(); ++g_marksSerial; }
					sendPointer(ev.button.x, ev.button.y);
					const int b = ev.button.button == SDL_BUTTON_LEFT ? 1 : ev.button.button == SDL_BUTTON_MIDDLE ? 2 : ev.button.button == SDL_BUTTON_RIGHT ? 3 : 0;
					if (b) XTestFakeButtonEvent(g_dpy, b, ev.type == SDL_MOUSEBUTTONDOWN ? True : False, CurrentTime);
				}
				break;
			case SDL_KEYDOWN:
				if (ev.key.repeat) break;
				if (ev.key.keysym.sym == SDLK_F7) { cycleMode((ev.key.keysym.mod & KMOD_SHIFT) ? -1 : 1); applyMode(title); dirty = true; break; }
				if (ev.key.keysym.sym == SDLK_F8) { g_dedither = !g_dedither; applyMode(title); break; }
				if (ev.key.keysym.sym == SDLK_F9) {
					if (g_haveSubs) {
						g_subs = (g_subs + ((ev.key.keysym.mod & KMOD_SHIFT) ? kSubsLangs : 1)) % (kSubsLangs + 1);
						note(std::string("Subtitles: ") + kSubsName[g_subs]);
						saveConfig();
					} else {
						note("No subtitles for this game here");
					}
					break;
				}
				if (ev.key.keysym.sym == SDLK_F10) {
					if (!g_scan && !g_marks.empty()) { g_marks.clear(); ++g_marksSerial; }
					else if (!g_scan) scanStart();
					break;
				}
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
					case SDL_CONTROLLER_BUTTON_LEFTSTICK:
					case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
						if (down && !g_scan) { if (!g_marks.empty()) { g_marks.clear(); ++g_marksSerial; } else scanStart(); }
						break;
					case SDL_CONTROLLER_BUTTON_BACK:
						if (down && g_haveSubs) {
							g_subs = (g_subs + 1) % (kSubsLangs + 1);
							note(std::string("Subtitles: ") + kSubsName[g_subs]);
							saveConfig();
						}
						break;
					}
				}
				break;
			}
		}
		if (pad && !g_scan) { // (the left stick moves the pointer: slowly near the middle, faster further out)
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
		const uint32_t *px = (const uint32_t *)g_img->data;
		if (g_scan) { // the pointer at the next place once the game has drawn it at this one (or doesn't, in time)
			int x, y;
			scanPoint(g_scanAt, &x, &y);
			const int what = pointerAt(px, g_w, g_h, x, y);
			Label seenLabel;
			if (what == 2) labelOf(px, g_w, g_h, seenLabel);
			// (the hand: its name is waited for a little too)
			if ((what == 2 && (!seenLabel.empty() || SDL_GetTicks() - g_scanSent > 120)) || what == 1 || SDL_GetTicks() - g_scanSent > 40) {
				if (what == 2) { g_scanHit[g_scanAt] = 1; g_scanLabel[g_scanAt] = seenLabel; }
				if (getenv("OLDIES_VIEW_SCAN")) fprintf(stderr, "%c%s", ".-#"[what == 2 ? 2 : what == 1 ? 0 : 1], (g_scanAt + 1) % kScanCols ? "" : "\n");
				if (++g_scanAt >= kScanCols * kScanRows) {
					scanFinish();
					char buf[80];
					snprintf(buf, sizeof(buf), g_marks.empty() ? "Nothing to use found here" : "%d things to use", (int)g_marks.size());
					note(buf);
				} else {
					scanPoint(g_scanAt, &x, &y);
					XTestFakeMotionEvent(g_dpy, -1, x, y, CurrentTime);
					g_scanSent = SDL_GetTicks();
					if (g_scanAt % kScanCols == 0) {
						char buf[80];
						snprintf(buf, sizeof(buf), "Looking for things to use…  %d %%", g_scanAt * 100 / (kScanCols * kScanRows));
						note(buf);
					}
				}
			}
			XFlush(g_dpy);
		}
		uint32_t hash = 2166136261u;
		for (int i = 0; i < g_w * g_h; i += 3) hash = (hash ^ px[i]) * 16777619u;
		XFixesCursorImage *cur = XFixesGetCursorImage(g_dpy);
		const unsigned long serial = cur ? cur->cursor_serial : 0;
		const int cx = cur ? cur->x : -1, cy = cur ? cur->y : -1;
		if (cur) XFree(cur);
		if (!g_scan) { // the name of the thing under the pointer
			Label now;
			labelOf(px, g_w, g_h, now);
			if (now != g_hover) { g_hover.swap(now); ++g_marksSerial; }
		}
		if (g_scan) {
			// (the picture stands still while the pointer runs over it)
		} else if (hash != lastHash || serial != lastCursor || cx != lastCx || cy != lastCy || g_dedither != lastDedither) {
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
		{ // the texts over the picture, when they change
			if (!g_note.empty() && SDL_GetTicks() > g_noteUntil) g_note.clear();
			const std::string sub = (g_haveSubs && g_subs) ? Subs_current(g_subs - 1) : std::string();
			if (!g_marks.empty() && !g_scan && SDL_GetTicks() > g_marksUntil) { g_marks.clear(); ++g_marksSerial; }
			if (sub != shownSub || g_note != shownNote || g_marksSerial != shownMarks || (overlayShown && (g_overlay.w != dw || g_overlay.h != dh))) {
				shownSub = sub;
				shownNote = g_note;
				shownMarks = g_marksSerial;
				overlayShown = !sub.empty() || !g_note.empty() || !g_marks.empty() || !g_hover.empty();
				if (overlayShown) {
					renderOverlay(dw, dh, sub, g_note);
					g_gfx->setOverlay(&g_overlay.px[0], g_overlay.w, g_overlay.h);
				} else {
					g_gfx->setOverlay(0, 0, 0);
				}
				dirty = true;
			}
		}
		if (scanTest && SDL_GetTicks() - start > (Uint32)(atof(scanTest) * 1000)) {
			scanTest = 0;
			scanStart();
			fprintf(stderr, "scan: started at %u ms\n", SDL_GetTicks() - start);
		}
		static bool wasScan;
		if (wasScan && !g_scan) fprintf(stderr, "scan: done at %u ms, %d things\n", SDL_GetTicks() - start, (int)g_marks.size());
		wasScan = g_scan;
		// (with subtitles, the test's picture waits for one to be showing, a minute at most)
		const Uint32 shotAt = script ? (Uint32)(atof(script) * 1000) : 0;
		const bool shotNow = script && SDL_GetTicks() - start > shotAt && (!g_haveSubs || getenv("OLDIES_VIEW_SCAN") || !g_subs || !shownSub.empty() || SDL_GetTicks() - start > shotAt + 60000);
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
			SDL_Delay(g_scan ? 2 : 8);
		}
	}
	saveConfig();
	Subs_stop();
	return 0;
}
