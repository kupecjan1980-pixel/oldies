/*
 * Bermuda Syndrome Linux port: the picture on the screen (OpenGL ES 3).
 *
 * The game draws a 640x480 frame; this puts it on the screen at the screen's own resolution, in one of the
 * ways below (switched while playing), with the menus and texts drawn over it at the screen's resolution.
 */

#ifndef GFX_H__
#define GFX_H__

#include <stdint.h>

enum {
	kScaleOriginal = 0, // 640x480 as it is, in the middle of the screen
	kScalePixels,       // whole multiples, hard pixels
	kScaleSmooth,       // to the screen's height, bilinear
	kScaleSharp,        // to the screen's height, bicubic (Catmull-Rom)
	kScaleFsr,          // AMD FSR 1: edge-directed upscaling and sharpening (any maker's card)
	kScaleNeuralS,      // a neural net doubles it (Anime4K CNN, small), then bicubic to the screen
	kScaleNeuralM,      //   medium
	kScaleNeuralL,      //   large
	kScaleCount
};

struct Gfx {
	virtual ~Gfx() {}
	// after the GL context is current; shaderDir: where the Anime4K shaders are
	virtual bool init(const char *shaderDir) = 0;
	virtual void fini() = 0;
	// the window's size in pixels (the real ones)
	virtual void resize(int w, int h) = 0;
	// the game's frame (RGBA, w*h*4, top row first)
	virtual void setGame(const uint32_t *pixels, int w, int h) = 0;
	// the blurred picture behind it on wide screens (0: black)
	virtual void setBackdrop(const uint32_t *pixels, int w, int h) = 0;
	// a video's frame, shown instead of the game's until cleared (0)
	virtual void setVideo(const uint32_t *pixels, int w, int h) = 0;
	// what's drawn over everything: RGBA, premultiplied, the window's size (0: nothing)
	virtual void setOverlay(const uint32_t *pixels, int w, int h) = 0;
	// how wide the picture is shown for its height (4/3 for a DOS game's 320x200); 0: as its pixels are
	virtual void setAspect(float aspect) = 0;
	virtual void setMode(int mode) = 0;
	// how many lines FSR and the nets upscale to: the screen's (0), or no more than this (1080: Full HD;
	// the rest of the way to the screen is plain bicubic, cheaper on a 4K screen with weak graphics)
	virtual void setTargetLines(int lines) = 0;
	virtual int mode() const = 0;
	virtual bool modeAvailable(int mode) = 0; // (FSR and the nets: when the card and the shaders allow)
	virtual bool neuralAvailable() const = 0;
	// where the game's picture is on the screen (for the mouse and the texts over it)
	virtual void gameRect(int *x, int *y, int *w, int *h) const = 0;
	// how long the last upscaling took (ms), and to what size the neural net brought the picture
	virtual float lastScaleMs() const = 0;
	virtual void neuralSize(int *w, int *h) const = 0;
	// Frames in between: the game makes 20 pictures a second; with this on, the screen is given a new one
	// every time it refreshes (60 to 165 a second), moved a step further from the game's last picture to
	// its newest along what moved between them. `phase` (draw): 0 the older picture .. 1 the newest.
	virtual void setFrameGen(bool on) = 0;
	virtual bool frameGen() const = 0;
	virtual float lastMotionMs() const = 0;
	virtual void draw(float phase = 1.0f) = 0;
	virtual bool screenshot(const char *path) = 0;
};

Gfx *Gfx_create();
const char *Gfx_modeName(int mode);

#endif
