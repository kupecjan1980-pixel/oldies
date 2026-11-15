/*
 * Bermuda Syndrome Linux port: drawing for the menus and texts over the game (at the screen's resolution, so
 * they're as sharp on a 4K screen as on a handheld's): a picture to draw into, shapes, and text in a real
 * font (Noto Sans, with Czech and Slovak letters).
 */

#ifndef UI_H__
#define UI_H__

#include <stdint.h>
#include <string>
#include <vector>

// colours: 0xAARRGGBB, not premultiplied (the canvas keeps premultiplied RGBA bytes)
struct Canvas {
	std::vector<uint32_t> px;
	int w, h;
	Canvas() : w(0), h(0) {}
	void resize(int nw, int nh);
	void clear();
	void rect(float x, float y, float rw, float rh, uint32_t color, float radius = 0);
	void circle(float cx, float cy, float r, uint32_t color);
	void ring(float cx, float cy, float r, float thick, uint32_t color);
	void line(float x0, float y0, float x1, float y1, float thick, uint32_t color);
};

bool Font_load(const char *dir); // NotoSans-Regular.ttf and NotoSans-Bold.ttf in dir
float Font_width(const std::string &utf8, float size, bool bold);
// draws at (x, y = the line's top); returns the width
float Font_draw(Canvas &c, float x, float y, const std::string &utf8, float size, uint32_t color, bool bold);
// text broken into lines no wider than maxWidth
std::vector<std::string> Font_wrap(const std::string &utf8, float size, bool bold, float maxWidth);

// a Windows code page's bytes as UTF-8 (1250: Czech, Slovak; 1252: English, German)
std::string textToUtf8(const char *s, int codepage);

#endif
