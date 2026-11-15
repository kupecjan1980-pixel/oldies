/*
 * Bermuda Syndrome Linux port: drawing for the menus and texts over the game.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include "ui.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "third_party/stb_truetype.h"

void Canvas::resize(int nw, int nh) {
	if (nw != w || nh != h) {
		w = nw;
		h = nh;
		px.assign((size_t)w * h, 0);
	}
}

void Canvas::clear() {
	if (!px.empty()) {
		memset(&px[0], 0, px.size() * sizeof(uint32_t));
	}
}

// one pixel, `cover` of it (0..255), in a colour 0xAARRGGBB: over what's there
static inline void blend(uint32_t &dst, uint32_t color, int cover) {
	const int a = ((color >> 24) & 255) * cover / 255;
	if (a <= 0) {
		return;
	}
	const int r = ((color >> 16) & 255) * a / 255, g = ((color >> 8) & 255) * a / 255, b = (color & 255) * a / 255;
	const int ia = 255 - a;
	const int dr = dst & 255, dg = (dst >> 8) & 255, db = (dst >> 16) & 255, da = (dst >> 24) & 255;
	dst = (uint32_t)(r + dr * ia / 255) | ((uint32_t)(g + dg * ia / 255) << 8) | ((uint32_t)(b + db * ia / 255) << 16)
		| ((uint32_t)(a + da * ia / 255) << 24);
}

void Canvas::rect(float x, float y, float rw, float rh, uint32_t color, float radius) {
	const int x0 = (int)floorf(x), y0 = (int)floorf(y), x1 = (int)ceilf(x + rw), y1 = (int)ceilf(y + rh);
	if (radius > rw / 2) radius = rw / 2;
	if (radius > rh / 2) radius = rh / 2;
	for (int py = y0 < 0 ? 0 : y0; py < y1 && py < h; ++py) {
		for (int qx = x0 < 0 ? 0 : x0; qx < x1 && qx < w; ++qx) {
			float cover = 1;
			const float cx = qx + 0.5f, cy = py + 0.5f;
			// the edges
			float ex = fminf(cx - x, x + rw - cx), ey = fminf(cy - y, y + rh - cy);
			if (radius > 0 && ex < radius && ey < radius) {
				const float dx = radius - ex, dy = radius - ey;
				cover = radius - sqrtf(dx * dx + dy * dy) + 0.5f;
			} else {
				cover = fminf(ex, ey) + 0.5f;
			}
			if (cover <= 0) {
				continue;
			}
			blend(px[(size_t)py * w + qx], color, cover >= 1 ? 255 : (int)(cover * 255));
		}
	}
}

void Canvas::circle(float cx, float cy, float r, uint32_t color) {
	rect(cx - r, cy - r, 2 * r, 2 * r, color, r);
}

void Canvas::ring(float cx, float cy, float r, float thick, uint32_t color) {
	const int x0 = (int)floorf(cx - r - 1), y0 = (int)floorf(cy - r - 1), x1 = (int)ceilf(cx + r + 1), y1 = (int)ceilf(cy + r + 1);
	for (int py = y0 < 0 ? 0 : y0; py < y1 && py < h; ++py) {
		for (int qx = x0 < 0 ? 0 : x0; qx < x1 && qx < w; ++qx) {
			const float d = sqrtf((qx + 0.5f - cx) * (qx + 0.5f - cx) + (py + 0.5f - cy) * (py + 0.5f - cy));
			const float cover = fminf(r - d, d - (r - thick)) + 0.5f;
			if (cover > 0) {
				blend(px[(size_t)py * w + qx], color, cover >= 1 ? 255 : (int)(cover * 255));
			}
		}
	}
}

void Canvas::line(float ax, float ay, float bx, float by, float thick, uint32_t color) {
	const float minx = fminf(ax, bx) - thick, maxx = fmaxf(ax, bx) + thick, miny = fminf(ay, by) - thick, maxy = fmaxf(ay, by) + thick;
	const float dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
	for (int py = miny < 0 ? 0 : (int)miny; py <= (int)maxy && py < h; ++py) {
		for (int qx = minx < 0 ? 0 : (int)minx; qx <= (int)maxx && qx < w; ++qx) {
			const float fx = qx + 0.5f - ax, fy = py + 0.5f - ay;
			float t = len2 > 0 ? (fx * dx + fy * dy) / len2 : 0;
			t = t < 0 ? 0 : t > 1 ? 1 : t;
			const float ex = fx - t * dx, ey = fy - t * dy;
			const float cover = thick / 2 - sqrtf(ex * ex + ey * ey) + 0.5f;
			if (cover > 0) {
				blend(px[(size_t)py * w + qx], color, cover >= 1 ? 255 : (int)(cover * 255));
			}
		}
	}
}

// --- the font ----------------------------------------------------------------------------------------------

struct Glyph {
	std::vector<uint8_t> bits;
	int w, h, xoff, yoff;
	float advance;
};

struct Face {
	std::vector<uint8_t> data;
	stbtt_fontinfo info;
	bool ok;
	std::map<uint64_t, Glyph> cache;
	Face() : ok(false) {}
};

static Face g_faces[2];

static bool loadFace(Face &f, const char *path) {
	FILE *fp = fopen(path, "rb");
	if (!fp) {
		return false;
	}
	fseek(fp, 0, SEEK_END);
	const long n = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	f.data.resize(n);
	const size_t got = fread(&f.data[0], 1, n, fp);
	fclose(fp);
	f.ok = got == (size_t)n && stbtt_InitFont(&f.info, &f.data[0], stbtt_GetFontOffsetForIndex(&f.data[0], 0)) != 0;
	return f.ok;
}

bool Font_load(const char *dir) {
	const std::string d = dir;
	const bool a = loadFace(g_faces[0], (d + "/NotoSans-Regular.ttf").c_str());
	const bool b = loadFace(g_faces[1], (d + "/NotoSans-Bold.ttf").c_str());
	return a && b;
}

static int nextCode(const char *&s) {
	const unsigned char c = (unsigned char)*s;
	int code = 0, extra = 0;
	if (c < 0x80) { code = c; }
	else if ((c & 0xE0) == 0xC0) { code = c & 0x1F; extra = 1; }
	else if ((c & 0xF0) == 0xE0) { code = c & 0x0F; extra = 2; }
	else if ((c & 0xF8) == 0xF0) { code = c & 0x07; extra = 3; }
	else { code = '?'; }
	++s;
	while (extra-- > 0 && (*s & 0xC0) == 0x80) {
		code = (code << 6) | (*s & 0x3F);
		++s;
	}
	return code;
}

static const Glyph &glyph(Face &f, int code, float size) {
	const int q = (int)(size * 4 + 0.5f); // (sizes to a quarter of a pixel)
	const uint64_t key = ((uint64_t)q << 32) | (uint32_t)code;
	std::map<uint64_t, Glyph>::iterator it = f.cache.find(key);
	if (it != f.cache.end()) {
		return it->second;
	}
	if (f.cache.size() > 6000) {
		f.cache.clear();
	}
	Glyph g;
	const float scale = stbtt_ScaleForPixelHeight(&f.info, q / 4.f);
	int adv = 0, lsb = 0;
	stbtt_GetCodepointHMetrics(&f.info, code, &adv, &lsb);
	g.advance = adv * scale;
	g.w = g.h = g.xoff = g.yoff = 0;
	unsigned char *bits = stbtt_GetCodepointBitmap(&f.info, scale, scale, code, &g.w, &g.h, &g.xoff, &g.yoff);
	if (bits) {
		g.bits.assign(bits, bits + g.w * g.h);
		stbtt_FreeBitmap(bits, 0);
	}
	return f.cache[key] = g;
}

// Signs the font doesn't have (arrows, a tick) are drawn here from lines, as wide as they're tall.
static bool isSign(int code) {
	return code == 0x2191 || code == 0x2193 || code == 0x2190 || code == 0x2192 || code == 0x25C4 || code == 0x25BA || code == 0x2714;
}

static void drawSign(Canvas &c, int code, float x, float y, float size, uint32_t color) {
	const float m = size * 0.5f, cx = x + m * 0.9f, cy = y + size * 0.68f, r = size * 0.30f, t = size * 0.11f;
	switch (code) {
	case 0x2191: // up
		c.line(cx, cy + r, cx, cy - r, t, color);
		c.line(cx, cy - r, cx - r * 0.7f, cy - r * 0.3f, t, color);
		c.line(cx, cy - r, cx + r * 0.7f, cy - r * 0.3f, t, color);
		break;
	case 0x2193: // down
		c.line(cx, cy - r, cx, cy + r, t, color);
		c.line(cx, cy + r, cx - r * 0.7f, cy + r * 0.3f, t, color);
		c.line(cx, cy + r, cx + r * 0.7f, cy + r * 0.3f, t, color);
		break;
	case 0x2190: case 0x25C4: // left
		c.line(cx + r, cy, cx - r, cy, t, color);
		c.line(cx - r, cy, cx - r * 0.3f, cy - r * 0.7f, t, color);
		c.line(cx - r, cy, cx - r * 0.3f, cy + r * 0.7f, t, color);
		break;
	case 0x2192: case 0x25BA: // right
		c.line(cx - r, cy, cx + r, cy, t, color);
		c.line(cx + r, cy, cx + r * 0.3f, cy - r * 0.7f, t, color);
		c.line(cx + r, cy, cx + r * 0.3f, cy + r * 0.7f, t, color);
		break;
	case 0x2714: // a tick
		c.line(cx - r, cy, cx - r * 0.3f, cy + r * 0.7f, t * 1.2f, color);
		c.line(cx - r * 0.3f, cy + r * 0.7f, cx + r, cy - r * 0.8f, t * 1.2f, color);
		break;
	}
}

float Font_width(const std::string &utf8, float size, bool bold) {
	Face &f = g_faces[bold ? 1 : 0];
	if (!f.ok) {
		return 0;
	}
	float x = 0;
	for (const char *s = utf8.c_str(); *s; ) {
		const int code = nextCode(s);
		x += isSign(code) ? size * 0.9f : glyph(f, code, size).advance;
	}
	return x;
}

float Font_draw(Canvas &c, float x, float y, const std::string &utf8, float size, uint32_t color, bool bold) {
	Face &f = g_faces[bold ? 1 : 0];
	if (!f.ok) {
		return 0;
	}
	const float scale = stbtt_ScaleForPixelHeight(&f.info, (int)(size * 4 + 0.5f) / 4.f);
	int ascent = 0, descent = 0, gap = 0;
	stbtt_GetFontVMetrics(&f.info, &ascent, &descent, &gap);
	const float base = y + ascent * scale;
	const float x0 = x;
	for (const char *s = utf8.c_str(); *s; ) {
		const int code = nextCode(s);
		if (isSign(code)) {
			drawSign(c, code, x, y, size, color);
			x += size * 0.9f;
			continue;
		}
		const Glyph &g = glyph(f, code, size);
		const int gx = (int)floorf(x + 0.5f) + g.xoff, gy = (int)floorf(base + 0.5f) + g.yoff;
		for (int j = 0; j < g.h; ++j) {
			const int py = gy + j;
			if (py < 0 || py >= c.h) {
				continue;
			}
			for (int i = 0; i < g.w; ++i) {
				const int qx = gx + i;
				const int cover = g.bits[j * g.w + i];
				if (cover && qx >= 0 && qx < c.w) {
					blend(c.px[(size_t)py * c.w + qx], color, cover);
				}
			}
		}
		x += g.advance;
	}
	return x - x0;
}

std::vector<std::string> Font_wrap(const std::string &utf8, float size, bool bold, float maxWidth) {
	std::vector<std::string> lines;
	size_t pos = 0;
	while (pos <= utf8.size()) {
		size_t nl = utf8.find('\n', pos);
		if (nl == std::string::npos) {
			nl = utf8.size();
		}
		const std::string para = utf8.substr(pos, nl - pos);
		pos = nl + 1;
		std::string line;
		size_t p = 0;
		while (p < para.size()) {
			size_t sp = para.find(' ', p);
			if (sp == std::string::npos) {
				sp = para.size();
			}
			const std::string word = para.substr(p, sp - p);
			p = sp + 1;
			const std::string tryLine = line.empty() ? word : line + " " + word;
			if (!line.empty() && Font_width(tryLine, size, bold) > maxWidth) {
				lines.push_back(line);
				line = word;
			} else {
				line = tryLine;
			}
		}
		lines.push_back(line);
		if (nl == utf8.size()) {
			break;
		}
	}
	return lines;
}

// Windows-1250 and Windows-1252, 0x80..0xFF, as Unicode
static const uint16_t kCp1250[128] = {
	0x20AC, 0x81, 0x201A, 0x83, 0x201E, 0x2026, 0x2020, 0x2021, 0x88, 0x2030, 0x160, 0x2039, 0x15A, 0x164, 0x17D, 0x179,
	0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x98, 0x2122, 0x161, 0x203A, 0x15B, 0x165, 0x17E, 0x17A,
	0xA0, 0x2C7, 0x2D8, 0x141, 0xA4, 0x104, 0xA6, 0xA7, 0xA8, 0xA9, 0x15E, 0xAB, 0xAC, 0xAD, 0xAE, 0x17B,
	0xB0, 0xB1, 0x2DB, 0x142, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0x105, 0x15F, 0xBB, 0x13D, 0x2DD, 0x13E, 0x17C,
	0x154, 0xC1, 0xC2, 0x102, 0xC4, 0x139, 0x106, 0xC7, 0x10C, 0xC9, 0x118, 0xCB, 0x11A, 0xCD, 0xCE, 0x10E,
	0x110, 0x143, 0x147, 0xD3, 0xD4, 0x150, 0xD6, 0xD7, 0x158, 0x16E, 0xDA, 0x170, 0xDC, 0xDD, 0x162, 0xDF,
	0x155, 0xE1, 0xE2, 0x103, 0xE4, 0x13A, 0x107, 0xE7, 0x10D, 0xE9, 0x119, 0xEB, 0x11B, 0xED, 0xEE, 0x10F,
	0x111, 0x144, 0x148, 0xF3, 0xF4, 0x151, 0xF6, 0xF7, 0x159, 0x16F, 0xFA, 0x171, 0xFC, 0xFD, 0x163, 0x2D9
};
static const uint16_t kCp1252Hi[32] = {
	0x20AC, 0x81, 0x201A, 0x192, 0x201E, 0x2026, 0x2020, 0x2021, 0x2C6, 0x2030, 0x160, 0x2039, 0x152, 0x8D, 0x17D, 0x8F,
	0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x2DC, 0x2122, 0x161, 0x203A, 0x153, 0x9D, 0x17E, 0x178
};

std::string textToUtf8(const char *s, int codepage) {
	std::string out;
	for (; *s; ++s) {
		const unsigned char c = (unsigned char)*s;
		int code = c;
		if (c >= 0x80) {
			code = codepage == 1250 ? kCp1250[c - 0x80] : (c < 0xA0 ? kCp1252Hi[c - 0x80] : c);
		}
		if (code < 0x80) {
			out += (char)code;
		} else if (code < 0x800) {
			out += (char)(0xC0 | (code >> 6));
			out += (char)(0x80 | (code & 0x3F));
		} else {
			out += (char)(0xE0 | (code >> 12));
			out += (char)(0x80 | ((code >> 6) & 0x3F));
			out += (char)(0x80 | (code & 0x3F));
		}
	}
	return out;
}
