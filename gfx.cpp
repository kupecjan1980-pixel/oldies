/*
 * Bermuda Syndrome Linux port: the picture on the screen (OpenGL ES 3).
 *
 * The neural upscaling runs Anime4K's "Upscale CNN x2" shaders (MIT, bloc97; in shaders/). They're written for
 * mpv: several passes, each a block starting with //!HOOK lines saying which pictures it reads (//!BIND), what
 * it writes (//!SAVE, or back into MAIN) and how big (//!WIDTH, //!HEIGHT, in reverse Polish). ShaderChain
 * below reads that format and runs it, with the game's frame as MAIN. The net is run only when the game's
 * frame changes (20 times a second at most), not for every frame the screen shows.
 */

#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <time.h>
#include "gfx.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "third_party/stb_image_write.h"

static const char *kVertex =
	"#version 300 es\n"
	"layout(location=0) in vec2 a_pos;\n"
	"uniform vec4 u_rect;\n" // x0, y0, x1, y1 in clip space
	"uniform float u_flip;\n"
	"out vec2 v_uv;\n"
	"void main() {\n"
	"  v_uv = vec2(a_pos.x, mix(a_pos.y, 1.0 - a_pos.y, u_flip));\n"
	"  gl_Position = vec4(mix(u_rect.xy, u_rect.zw, a_pos), 0.0, 1.0);\n"
	"}\n";

static const char *kFragPlain =
	"#version 300 es\nprecision highp float;\n"
	"in vec2 v_uv; out vec4 o; uniform sampler2D u_tex;\n"
	"void main() { o = texture(u_tex, v_uv); }\n";

// Catmull-Rom from nine bilinear taps
static const char *kFragBicubic =
	"#version 300 es\nprecision highp float;\n"
	"in vec2 v_uv; out vec4 o; uniform sampler2D u_tex; uniform vec2 u_size;\n"
	"void main() {\n"
	"  vec2 p = v_uv * u_size; vec2 c = floor(p - 0.5) + 0.5; vec2 f = p - c;\n"
	"  vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));\n"
	"  vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
	"  vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));\n"
	"  vec2 w3 = f * f * (-0.5 + 0.5 * f);\n"
	"  vec2 w12 = w1 + w2; vec2 o12 = w2 / w12;\n"
	"  vec2 t0 = (c - 1.0) / u_size, t3 = (c + 2.0) / u_size, t12 = (c + o12) / u_size;\n"
	"  vec4 r = texture(u_tex, vec2(t0.x, t0.y)) * w0.x * w0.y + texture(u_tex, vec2(t12.x, t0.y)) * w12.x * w0.y\n"
	"    + texture(u_tex, vec2(t3.x, t0.y)) * w3.x * w0.y + texture(u_tex, vec2(t0.x, t12.y)) * w0.x * w12.y\n"
	"    + texture(u_tex, vec2(t12.x, t12.y)) * w12.x * w12.y + texture(u_tex, vec2(t3.x, t12.y)) * w3.x * w12.y\n"
	"    + texture(u_tex, vec2(t0.x, t3.y)) * w0.x * w3.y + texture(u_tex, vec2(t12.x, t3.y)) * w12.x * w3.y\n"
	"    + texture(u_tex, vec2(t3.x, t3.y)) * w3.x * w3.y;\n"
	"  o = vec4(clamp(r.rgb, 0.0, 1.0), 1.0);\n"
	"}\n";

static double nowMs() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static GLuint compile(GLenum type, const char *src, const char *what) {
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, 0);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[2048];
		glGetShaderInfoLog(s, sizeof(log), 0, log);
		fprintf(stderr, "shader (%s) did not compile: %s\n", what, log);
		glDeleteShader(s);
		return 0;
	}
	return s;
}

static GLuint program(const char *frag, const char *what) {
	GLuint vs = compile(GL_VERTEX_SHADER, kVertex, "vertex"), fs = compile(GL_FRAGMENT_SHADER, frag, what);
	if (!vs || !fs) {
		return 0;
	}
	GLuint p = glCreateProgram();
	glAttachShader(p, vs);
	glAttachShader(p, fs);
	glLinkProgram(p);
	glDeleteShader(vs);
	glDeleteShader(fs);
	GLint ok = 0;
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		fprintf(stderr, "shader (%s) did not link\n", what);
		glDeleteProgram(p);
		return 0;
	}
	return p;
}

struct Tex {
	GLuint id;
	int w, h;
	Tex() : id(0), w(0), h(0) {}
};

static void texAlloc(Tex &t, int w, int h, GLint internal, GLenum format, GLenum type, const void *data, GLint filter) {
	if (!t.id) {
		glGenTextures(1, &t.id);
	}
	glBindTexture(GL_TEXTURE_2D, t.id);
	if (t.w != w || t.h != h || !data) {
		glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, data);
		t.w = w;
		t.h = h;
	} else {
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, format, type, data);
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

// --- the mpv-style shader file, run ------------------------------------------------------------------------

struct Pass {
	std::vector<std::string> binds;
	std::string save, widthExpr, heightExpr, body, desc;
	GLuint prog;
	Pass() : prog(0) {}
};

struct ShaderChain {
	std::vector<Pass> passes;
	std::map<std::string, Tex> saved;
	GLuint fbo;
	bool ok;

	ShaderChain() : fbo(0), ok(false) {}

	bool load(const char *path) {
		FILE *f = fopen(path, "rb");
		if (!f) {
			return false;
		}
		std::string text;
		char buf[65536];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
			text.append(buf, n);
		}
		fclose(f);
		Pass cur;
		bool in = false;
		size_t pos = 0;
		while (pos < text.size()) {
			size_t e = text.find('\n', pos);
			if (e == std::string::npos) {
				e = text.size();
			}
			std::string line = text.substr(pos, e - pos);
			pos = e + 1;
			if (line.compare(0, 3, "//!") == 0) {
				size_t sp = line.find(' ');
				std::string key = line.substr(3, sp == std::string::npos ? std::string::npos : sp - 3);
				std::string val = sp == std::string::npos ? "" : line.substr(sp + 1);
				while (!val.empty() && (val[val.size() - 1] == '\r' || val[val.size() - 1] == ' ')) {
					val.erase(val.size() - 1);
				}
				if (in && !cur.body.empty()) { // (a pass's lines begin after the one before's code, in any order)
					passes.push_back(cur);
					cur = Pass();
				}
				in = true;
				if (key == "DESC") cur.desc = val;
				else if (key == "BIND") cur.binds.push_back((val == "HOOKED" || val == "LUMA") ? "MAIN" : val);
				else if (key == "SAVE") cur.save = val;
				else if (key == "WIDTH") cur.widthExpr = val;
				else if (key == "HEIGHT") cur.heightExpr = val;
			} else if (in) {
				cur.body += line + "\n";
			}
		}
		if (in && !cur.body.empty()) {
			passes.push_back(cur);
		}
		for (size_t i = 0; i < passes.size(); ++i) {
			Pass &p = passes[i];
			bool hasMain = false; // (the hooked picture is there for every pass, bound or not, as in mpv)
			for (size_t b = 0; b < p.binds.size(); ++b) {
				hasMain = hasMain || p.binds[b] == "MAIN";
			}
			if (!hasMain) {
				p.binds.push_back("MAIN");
			}
			std::string src = "#version 300 es\nprecision highp float;\nprecision highp sampler2D;\nin vec2 v_uv;\nout vec4 fvp_out;\n";
			for (size_t b = 0; b < p.binds.size(); ++b) {
				const std::string &n = p.binds[b];
				src += "uniform sampler2D T_" + n + ";\nuniform vec2 S_" + n + ";\n";
				src += "#define " + n + "_pos v_uv\n#define " + n + "_size S_" + n + "\n#define " + n + "_pt (1.0 / S_" + n + ")\n";
				src += "#define " + n + "_tex(p) texture(T_" + n + ", p)\n";
				src += "#define " + n + "_texOff(o) texture(T_" + n + ", v_uv + (o) / S_" + n + ")\n";
			}
			src += "#define HOOKED_pos MAIN_pos\n#define HOOKED_size MAIN_size\n#define HOOKED_pt MAIN_pt\n"
				"#define HOOKED_tex(p) MAIN_tex(p)\n#define HOOKED_texOff(o) MAIN_texOff(o)\n"
				"#define LUMA_pos MAIN_pos\n#define LUMA_size MAIN_size\n#define LUMA_pt MAIN_pt\n"
				"#define LUMA_tex(p) MAIN_tex(p)\n#define LUMA_texOff(o) MAIN_texOff(o)\n";
			src += p.body + "\nvoid main() { fvp_out = hook(); }\n";
			p.prog = program(src.c_str(), p.desc.c_str());
			if (!p.prog) {
				return false;
			}
		}
		glGenFramebuffers(1, &fbo);
		ok = !passes.empty();
		return ok;
	}

	static float rpn(const std::string &expr, std::map<std::string, Tex> &tex, float fallback) {
		if (expr.empty()) {
			return fallback;
		}
		std::vector<float> st;
		size_t pos = 0;
		while (pos < expr.size()) {
			size_t e = expr.find(' ', pos);
			if (e == std::string::npos) {
				e = expr.size();
			}
			std::string t = expr.substr(pos, e - pos);
			pos = e + 1;
			if (t.empty()) {
				continue;
			}
			if (t == "*" || t == "/" || t == "+" || t == "-" || t == "<" || t == ">" || t == "=") {
				if (st.size() < 2) {
					return fallback;
				}
				float b = st.back();
				st.pop_back();
				float a = st.back();
				st.pop_back();
				st.push_back(t == "*" ? a * b : t == "/" ? a / b : t == "+" ? a + b : t == "-" ? a - b
					: t == "<" ? (a < b ? 1.f : 0.f) : t == ">" ? (a > b ? 1.f : 0.f) : (a == b ? 1.f : 0.f));
			} else if (t.size() > 2 && t[t.size() - 2] == '.') {
				std::string n = t.substr(0, t.size() - 2);
				if (n == "HOOKED" || n == "LUMA") n = "MAIN";
				Tex &x = tex[n];
				st.push_back(t[t.size() - 1] == 'w' ? x.w : x.h);
			} else {
				st.push_back((float)atof(t.c_str()));
			}
		}
		return st.empty() ? fallback : st.back();
	}

	// MAIN in (w x h), the result out (the nets: twice as big each way; FSR: up to twice, no bigger than
	// outW x outH, the size wanted in the end, which the shaders know as OUTPUT)
	Tex run(const Tex &main, GLuint vao, int outW = 0, int outH = 0) {
		saved["MAIN"] = main;
		Tex output;
		output.w = outW ? outW : main.w * 2;
		output.h = outH ? outH : main.h * 2;
		saved["OUTPUT"] = output;
		Tex result = main;
		glBindVertexArray(vao);
		glDisable(GL_BLEND);
		for (size_t i = 0; i < passes.size(); ++i) {
			Pass &p = passes[i];
			int w = (int)(rpn(p.widthExpr, saved, saved["MAIN"].w) + 0.5f);
			int h = (int)(rpn(p.heightExpr, saved, saved["MAIN"].h) + 0.5f);
			const std::string name = p.save.empty() ? "__out" : p.save;
			Tex &dst = own[name];
			if (dst.w != w || dst.h != h) {
				texAlloc(dst, w, h, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 0, GL_LINEAR);
			}
			glBindFramebuffer(GL_FRAMEBUFFER, fbo);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst.id, 0);
			glViewport(0, 0, w, h);
			glUseProgram(p.prog);
			glUniform4f(glGetUniformLocation(p.prog, "u_rect"), -1, -1, 1, 1);
			glUniform1f(glGetUniformLocation(p.prog, "u_flip"), 0);
			for (size_t b = 0; b < p.binds.size(); ++b) {
				const std::string &n = p.binds[b];
				Tex &src = saved[n];
				glActiveTexture(GL_TEXTURE0 + b);
				glBindTexture(GL_TEXTURE_2D, src.id);
				glUniform1i(glGetUniformLocation(p.prog, ("T_" + n).c_str()), (int)b);
				glUniform2f(glGetUniformLocation(p.prog, ("S_" + n).c_str()), (float)src.w, (float)src.h);
			}
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			if (p.save.empty() || p.save == "MAIN") {
				result = dst;
				saved["MAIN"] = dst;
			} else {
				saved[name] = dst;
			}
		}
		glActiveTexture(GL_TEXTURE0);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		return result;
	}

	std::map<std::string, Tex> own;
};

// --- the presenter -----------------------------------------------------------------------------------------

// the picture's light part alone (FSR works on it), in the red channel
static const char *kFragLuma =
	"#version 300 es\nprecision highp float;\n"
	"in vec2 v_uv; out vec4 o; uniform sampler2D u_tex;\n"
	"void main() { vec3 c = texture(u_tex, v_uv).rgb; o = vec4(dot(c, vec3(0.299, 0.587, 0.114)), 0.0, 0.0, 1.0); }\n";

// the upscaled light part put back together with the colour of the picture it came from
static const char *kFragMerge =
	"#version 300 es\nprecision highp float;\n"
	"in vec2 v_uv; out vec4 o; uniform sampler2D u_tex; uniform sampler2D u_rgb;\n"
	"void main() {\n"
	"  float y = texture(u_tex, v_uv).r; vec3 c = texture(u_rgb, v_uv).rgb;\n"
	"  float y0 = dot(c, vec3(0.299, 0.587, 0.114));\n"
	"  o = vec4(clamp(c + (y - y0), 0.0, 1.0), 1.0);\n"
	"}\n";

// What moved between two of the game's pictures: for each 8x8 block of the newer one, where it was in the
// older one (the shift with the smallest difference, looked for up to 12 pixels each way, the nearer shift
// taken when two are as good). Out: the shift in pixels (x, y), how bad the best match still is.
static const char *kFragMotion =
	"#version 300 es\nprecision highp float;\n"
	"in vec2 v_uv; out vec4 o; uniform sampler2D u_prev; uniform sampler2D u_cur; uniform vec2 u_size;\n"
	"float lum(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
	"void main() {\n"
	"  vec2 base = floor(v_uv * u_size / 8.0) * 8.0;\n"
	"  float cur[9];\n"
	"  for (int j = 0; j < 3; ++j) for (int i = 0; i < 3; ++i)\n"
	"    cur[j * 3 + i] = lum(texture(u_cur, (base + vec2(float(i) * 3.0 + 1.0, float(j) * 3.0 + 1.0)) / u_size).rgb);\n"
	"  float best = 1e9; vec2 mv = vec2(0.0); float zero = 0.0;\n"
	"  for (int dy = -6; dy <= 6; ++dy) for (int dx = -6; dx <= 6; ++dx) {\n"
	"    vec2 d = vec2(float(dx), float(dy)) * 2.0; float sad = 0.0;\n"
	"    for (int j = 0; j < 3; ++j) for (int i = 0; i < 3; ++i)\n"
	"      sad += abs(cur[j * 3 + i] - lum(texture(u_prev, (base + vec2(float(i) * 3.0 + 1.0, float(j) * 3.0 + 1.0) - d) / u_size).rgb));\n"
	"    sad *= 1.78;\n" // (as if 16 points were compared: the limits below are for that many)
	"    if (dx == 0 && dy == 0) zero = sad;\n"
	"    sad += length(d) * 0.012;\n"
	"    if (sad < best) { best = sad; mv = d; }\n"
	"  }\n"
	"  if (zero < 0.08) { mv = vec2(0.0); best = zero; }\n" // (nothing changed here: nothing moves)
	"  o = vec4(mv, best, 1.0);\n"
	"}\n";

// A picture between the older and the newer one: what moved is put a part of its way (from the older
// picture forwards, from the newer one backwards, the two mixed); where no good match was found, the
// older picture is shown for the first half of the time and the newer one for the second (no ghosts).
static const char *kFragBetween =
	"#version 300 es\nprecision highp float;\n"
	"in vec2 v_uv; out vec4 o; uniform sampler2D u_prev; uniform sampler2D u_cur; uniform sampler2D u_motion;\n"
	"uniform vec2 u_size; uniform float u_t;\n"
	"void main() {\n"
	"  vec3 m = texture(u_motion, v_uv).rgb; vec2 mv = m.xy / u_size;\n"
	"  vec4 a = texture(u_prev, v_uv - mv * u_t); vec4 b = texture(u_cur, v_uv + mv * (1.0 - u_t));\n"
	"  float moving = step(0.5, length(m.xy));\n"
	"  float good = 1.0 - smoothstep(1.2, 2.4, m.z);\n"
	"  vec4 cut = u_t < 0.5 ? texture(u_prev, v_uv) : texture(u_cur, v_uv);\n"
	"  vec4 still = mix(texture(u_prev, v_uv), texture(u_cur, v_uv), m.z < 0.08 ? u_t : step(0.5, u_t));\n"
	"  o = vec4(mix(still, mix(cut, mix(a, b, u_t), good), moving).rgb, 1.0);\n"
	"}\n";

struct GfxGL : Gfx {
	int _w, _h, _mode;
	Tex _game[2], _final[2], _luma, _motion, _between, _backdrop, _video, _overlay;
	int _cur; // which of the two is the newest
	int _frames; // how many of the game's pictures have come (two are needed for a picture in between)
	bool _hasBackdrop, _hasVideo, _hasOverlay, _gameDirty, _floatOk, _frameGen;
	GLuint _vao, _vbo, _fbo, _plain, _bicubic, _lumaProg, _merge, _motionProg, _betweenProg;
	ShaderChain _chain[3], _chain2[3], _chain3[3], _fsr, _fsr2, _fsr3; // (copies for the second and third doubling)
	bool _chainTried[3], _fsrTried;
	std::string _shaderDir;
	float _scaleMs, _motionMs;
	int _nw, _nh, _targetLines;
	float _aspect;

	GfxGL() : _w(640), _h(480), _mode(kScaleSharp), _cur(0), _frames(0), _hasBackdrop(false), _hasVideo(false),
		_hasOverlay(false), _gameDirty(true), _floatOk(false), _frameGen(false), _vao(0), _vbo(0), _fbo(0), _plain(0),
		_bicubic(0), _lumaProg(0), _merge(0), _motionProg(0), _betweenProg(0), _fsrTried(false), _scaleMs(0), _motionMs(0),
		_nw(0), _nh(0), _targetLines(0), _aspect(0) {
		_chainTried[0] = _chainTried[1] = _chainTried[2] = false;
	}

	virtual bool init(const char *shaderDir) {
		_shaderDir = shaderDir ? shaderDir : "shaders";
		_plain = program(kFragPlain, "plain");
		_bicubic = program(kFragBicubic, "bicubic");
		_lumaProg = program(kFragLuma, "luma");
		_merge = program(kFragMerge, "merge");
		_motionProg = program(kFragMotion, "motion");
		_betweenProg = program(kFragBetween, "between");
		if (!_plain || !_bicubic) {
			return false;
		}
		static const float quad[] = { 0, 0, 1, 0, 0, 1, 1, 1 };
		glGenVertexArrays(1, &_vao);
		glBindVertexArray(_vao);
		glGenBuffers(1, &_vbo);
		glBindBuffer(GL_ARRAY_BUFFER, _vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
		glGenFramebuffers(1, &_fbo);
		// the nets' layers and the shifts hold signed numbers: pictures of half floats to draw into are needed
		const char *ext = (const char *)glGetString(GL_EXTENSIONS);
		const char *ver = (const char *)glGetString(GL_VERSION);
		_floatOk = (ext && (strstr(ext, "EXT_color_buffer_half_float") || strstr(ext, "EXT_color_buffer_float")))
			|| (ver && strstr(ver, "ES 3.2"));
		return true;
	}

	virtual void fini() {
	}

	virtual void resize(int w, int h) {
		if (w > 0 && h > 0 && (w != _w || h != _h)) {
			_w = w;
			_h = h;
			_gameDirty = true;
		}
	}

	virtual void setGame(const uint32_t *pixels, int w, int h) {
		_cur ^= 1; // (the one before is kept: the pictures in between are made from the two)
		texAlloc(_game[_cur], w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, pixels, GL_LINEAR);
		_gameDirty = true;
		++_frames;
	}

	virtual void setBackdrop(const uint32_t *pixels, int w, int h) {
		_hasBackdrop = pixels != 0;
		if (pixels) {
			texAlloc(_backdrop, w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, pixels, GL_LINEAR);
		}
	}

	virtual void setVideo(const uint32_t *pixels, int w, int h) {
		_hasVideo = pixels != 0;
		if (pixels) {
			texAlloc(_video, w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, pixels, GL_LINEAR);
		} else {
			_frames = 0;
		}
	}

	virtual void setOverlay(const uint32_t *pixels, int w, int h) {
		_hasOverlay = pixels != 0;
		if (pixels) {
			texAlloc(_overlay, w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, pixels, GL_LINEAR);
		}
	}

	virtual void setMode(int mode) {
		if (mode < 0 || mode >= kScaleCount) {
			mode = 0;
		}
		if (mode >= kScaleNeuralS && !chain(mode - kScaleNeuralS)) {
			mode = kScaleSharp; // (the card can't, or the shaders aren't there)
		}
		if (mode == kScaleFsr && !fsr()) {
			mode = kScaleSharp;
		}
		_mode = mode;
		_gameDirty = true;
		_frames = _frames > 1 ? 1 : _frames; // (the older picture was made the other way: not mixed with)
	}

	virtual void setAspect(float aspect) {
		_aspect = aspect;
	}

	virtual void setTargetLines(int lines) {
		if (lines != _targetLines) {
			_targetLines = lines;
			_gameDirty = true;
			_frames = _frames > 1 ? 1 : _frames;
		}
	}

	virtual int mode() const { return _mode; }
	virtual bool modeAvailable(int mode) {
		return mode < kScaleFsr || (mode == kScaleFsr ? fsr() : chain(mode - kScaleNeuralS));
	}
	virtual bool neuralAvailable() const { return _floatOk; }
	virtual float lastScaleMs() const { return _scaleMs; }
	virtual float lastMotionMs() const { return _motionMs; }
	virtual void neuralSize(int *w, int *h) const { *w = _nw; *h = _nh; }
	virtual void setFrameGen(bool on) {
		_frameGen = on && _floatOk && _motionProg && _betweenProg;
		_frames = _frames > 1 ? 1 : _frames;
	}
	virtual bool frameGen() const { return _frameGen; }

	bool chain(int n) {
		if (!_floatOk) {
			return false;
		}
		if (!_chainTried[n]) {
			_chainTried[n] = true;
			static const char *names[] = { "S", "M", "L" };
			std::string path = _shaderDir + "/Anime4K_Upscale_CNN_x2_" + names[n] + ".glsl";
			if (_chain[n].load(path.c_str())) {
				_chain2[n].load(path.c_str());
				_chain3[n].load(path.c_str());
			} else {
				fprintf(stderr, "neural upscaling: '%s' could not be used\n", path.c_str());
			}
		}
		return _chain[n].ok;
	}

	bool fsr() {
		if (!_floatOk || !_lumaProg || !_merge) {
			return false;
		}
		if (!_fsrTried) {
			_fsrTried = true;
			const std::string path = _shaderDir + "/FSR.glsl";
			if (_fsr.load(path.c_str())) {
				_fsr2.load(path.c_str());
				_fsr3.load(path.c_str());
			} else {
				fprintf(stderr, "FSR: '%s' could not be used\n", path.c_str());
			}
		}
		return _fsr.ok;
	}

	virtual void gameRect(int *x, int *y, int *w, int *h) const {
		const Tex &g = _game[_cur];
		const int gw = _hasVideo ? _video.w : (g.w ? g.w : 640), gh = _hasVideo ? _video.h : (g.h ? g.h : 480);
		int rw, rh;
		const float ratio = _hasVideo ? 4.f / 3 : (_aspect > 0 ? _aspect : (float)gw / gh);
		if (_mode == kScaleOriginal && !_hasVideo) {
			rw = gw;
			rh = gh;
		} else if (_mode == kScalePixels && !_hasVideo) {
			int k = _h / gh; // whole multiples of its lines (its width as the picture's shape asks)
			while (k > 1 && (int)(gh * k * ratio) > _w) --k;
			if (k < 1) k = 1;
			rh = gh * k;
			rw = (int)(rh * ratio + 0.5f);
		} else {
			// to the screen's height
			rh = _h;
			rw = (int)(_h * ratio + 0.5f);
			if (rw > _w) {
				rw = _w;
				rh = (int)(_w / ratio + 0.5f);
			}
		}
		*x = (_w - rw) / 2;
		*y = (_h - rh) / 2;
		*w = rw;
		*h = rh;
	}

	void use(GLuint prog, const Tex &t, int unit, const char *name, bool nearest = false) {
		glActiveTexture(GL_TEXTURE0 + unit);
		glBindTexture(GL_TEXTURE_2D, t.id);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
		glUniform1i(glGetUniformLocation(prog, name), unit);
	}

	// draws into a picture (the whole of it)
	void into(Tex &dst, int w, int h, GLint internal, GLenum type, GLuint prog) {
		if (dst.w != w || dst.h != h || !dst.id) {
			texAlloc(dst, w, h, internal, GL_RGBA, type, 0, GL_LINEAR);
		}
		glBindFramebuffer(GL_FRAMEBUFFER, _fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst.id, 0);
		glViewport(0, 0, w, h);
		glBindVertexArray(_vao);
		glDisable(GL_BLEND);
		glUseProgram(prog);
		glUniform4f(glGetUniformLocation(prog, "u_rect"), -1, -1, 1, 1);
		glUniform1f(glGetUniformLocation(prog, "u_flip"), 0);
	}

	void quad(GLuint prog, const Tex &t, int x, int y, int w, int h, bool nearest) {
		glUseProgram(prog);
		const float x0 = 2.f * x / _w - 1, x1 = 2.f * (x + w) / _w - 1;
		const float y1 = 1 - 2.f * y / _h, y0 = 1 - 2.f * (y + h) / _h;
		glUniform4f(glGetUniformLocation(prog, "u_rect"), x0, y0, x1, y1);
		glUniform1f(glGetUniformLocation(prog, "u_flip"), 1); // (a picture's first row is its top)
		const GLint size = glGetUniformLocation(prog, "u_size");
		if (size >= 0) {
			glUniform2f(size, (float)t.w, (float)t.h);
		}
		use(prog, t, 0, "u_tex", nearest);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}

	// the game's newest picture made ready for the screen (_final[_cur]): upscaled by the net or by FSR,
	// or as it is; and, for the pictures in between, what moved since the one before
	void prepare(int outH) {
		const Tex &g = _game[_cur];
		const double t0 = nowMs();
		Tex result = g;
		if (_mode >= kScaleNeuralS) {
			const int n = _mode - kScaleNeuralS;
			result = _chain[n].run(g, _vao);
			// doubled again while the screen has half as many lines again as the picture (a 320x200 picture
			// on a 4K screen: three times, to 2560x1600)
			if (outH * 2 >= result.h * 3 && _chain2[n].ok) {
				result = _chain2[n].run(result, _vao);
				if (outH * 2 >= result.h * 3 && _chain3[n].ok) {
					result = _chain3[n].run(result, _vao);
				}
			}
		} else if (_mode == kScaleFsr) {
			const int outW = (int)(outH * (_aspect > 0 ? _aspect : (float)g.w / g.h) + 0.5f);
			into(_luma, g.w, g.h, GL_RGBA16F, GL_HALF_FLOAT, _lumaProg);
			use(_lumaProg, g, 0, "u_tex");
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			Tex y = _fsr.run(_luma, _vao, outW, outH);
			if (y.h < outH && _fsr2.ok) { // (FSR at most doubles: again for the rest of the way)
				y = _fsr2.run(y, _vao, outW, outH);
				if (y.h < outH && _fsr3.ok) {
					y = _fsr3.run(y, _vao, outW, outH);
				}
			}
			into(_final[_cur], y.w, y.h, GL_RGBA8, GL_UNSIGNED_BYTE, _merge);
			use(_merge, y, 0, "u_tex");
			use(_merge, g, 1, "u_rgb");
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			glActiveTexture(GL_TEXTURE0);
			result = _final[_cur];
		}
		if (_mode != kScaleFsr) { // (kept as a picture of its own: the one before stays as it was)
			into(_final[_cur], result.w, result.h, GL_RGBA8, GL_UNSIGNED_BYTE, _plain);
			use(_plain, result, 0, "u_tex", true);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		}
		if (_mode >= kScaleFsr) {
			glFinish();
			_scaleMs = (float)(nowMs() - t0);
			_nw = _final[_cur].w;
			_nh = _final[_cur].h;
		} else {
			_scaleMs = 0;
			_nw = _nh = 0;
		}
		if (_frameGen && _frames >= 2 && _game[_cur ^ 1].w == g.w) {
			const double t1 = nowMs();
			into(_motion, g.w / 8, g.h / 8, GL_RGBA16F, GL_HALF_FLOAT, _motionProg);
			use(_motionProg, _game[_cur ^ 1], 0, "u_prev");
			use(_motionProg, g, 1, "u_cur");
			glUniform2f(glGetUniformLocation(_motionProg, "u_size"), (float)g.w, (float)g.h);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			glActiveTexture(GL_TEXTURE0);
			glFinish();
			_motionMs = (float)(nowMs() - t1);
		}
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}

	virtual void draw(float phase) {
		int x, y, w, h;
		gameRect(&x, &y, &w, &h);
		if (_gameDirty && _game[_cur].id && !_hasVideo) {
			prepare((_targetLines && _targetLines < h) ? _targetLines : h);
		}
		_gameDirty = false;
		// a picture between the last two, when there are two alike and the newest isn't due yet
		const Tex *show = &_final[_cur];
		const bool between = _frameGen && !_hasVideo && _frames >= 2 && phase < 0.999f && _motion.id
			&& _final[_cur ^ 1].id && _final[_cur ^ 1].w == _final[_cur].w && _final[_cur ^ 1].h == _final[_cur].h;
		if (between) {
			into(_between, _final[_cur].w, _final[_cur].h, GL_RGBA8, GL_UNSIGNED_BYTE, _betweenProg);
			use(_betweenProg, _final[_cur ^ 1], 0, "u_prev");
			use(_betweenProg, _final[_cur], 1, "u_cur");
			use(_betweenProg, _motion, 2, "u_motion");
			glUniform2f(glGetUniformLocation(_betweenProg, "u_size"), (float)_game[_cur].w, (float)_game[_cur].h);
			glUniform1f(glGetUniformLocation(_betweenProg, "u_t"), phase < 0 ? 0 : phase);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			glActiveTexture(GL_TEXTURE0);
			show = &_between;
		}
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glViewport(0, 0, _w, _h);
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glBindVertexArray(_vao);
		glDisable(GL_BLEND);
		if (_hasVideo) {
			quad(_bicubic, _video, x, y, w, h, false);
		} else if (show->id) {
			if (_hasBackdrop && (x > 0 || y > 0)) {
				quad(_plain, _backdrop, 0, 0, _w, _h, false);
			}
			switch (_mode) {
			case kScaleOriginal:
			case kScalePixels:
				quad(_plain, *show, x, y, w, h, true);
				break;
			case kScaleSmooth:
				quad(_plain, *show, x, y, w, h, false);
				break;
			default:
				quad(_bicubic, *show, x, y, w, h, false);
				break;
			}
		}
		if (_hasOverlay) {
			glEnable(GL_BLEND);
			glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
			quad(_plain, _overlay, 0, 0, _w, _h, false);
			glDisable(GL_BLEND);
		}
	}

	static void writeFn(void *ctx, void *data, int size) {
		fwrite(data, 1, size, (FILE *)ctx);
	}

	virtual bool screenshot(const char *path) {
		std::vector<uint8_t> px(_w * _h * 4), row(_w * 4);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glReadPixels(0, 0, _w, _h, GL_RGBA, GL_UNSIGNED_BYTE, &px[0]);
		for (int y = 0; y < _h / 2; ++y) {
			memcpy(&row[0], &px[y * _w * 4], _w * 4);
			memcpy(&px[y * _w * 4], &px[(_h - 1 - y) * _w * 4], _w * 4);
			memcpy(&px[(_h - 1 - y) * _w * 4], &row[0], _w * 4);
		}
		for (int i = 0; i < _w * _h; ++i) {
			px[i * 4 + 3] = 255;
		}
		FILE *f = fopen(path, "wb");
		if (!f) {
			return false;
		}
		const int ok = stbi_write_png_to_func(writeFn, f, _w, _h, 4, &px[0], _w * 4);
		fclose(f);
		return ok != 0;
	}
};

Gfx *Gfx_create() {
	return new GfxGL();
}

const char *Gfx_modeName(int mode) {
	static const char *names[] = { "original", "pixels", "smooth", "sharp", "fsr", "neural-s", "neural-m", "neural-l" };
	return (mode >= 0 && mode < kScaleCount) ? names[mode] : "?";
}
