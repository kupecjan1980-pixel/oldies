/*
 * Oldies: a stand-in emulator core for testing the shell without a real one. A 320x200 picture at 70 a second
 * that changes 35 times a second (a box moving, as a DOS game's would), a tone, the keys' state, a state to
 * save. Built as cores/test_core.so by `make test-core`; never part of what's handed out.
 */

#include <math.h>
#include <string.h>
#include "third_party/libretro.h"

static retro_video_refresh_t g_video;
static retro_audio_sample_batch_t g_audio;
static retro_environment_t g_env;
static retro_input_poll_t g_poll;
static retro_input_state_t g_input;
static uint32_t g_frame[320 * 200];
static int g_tick, g_x = 20, g_y = 60, g_dx = 4, g_dy = 2;
static bool g_keys[RETROK_LAST];
static double g_phase;

static void RETRO_CALLCONV keyboard(bool down, unsigned keycode, uint32_t, uint16_t) {
	if (keycode < RETROK_LAST) {
		g_keys[keycode] = down;
	}
}

extern "C" {

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }
RETRO_API void retro_set_environment(retro_environment_t cb) {
	g_env = cb;
	bool yes = true;
	cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &yes);
}
RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { g_video = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t) {}
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { g_audio = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { g_poll = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { g_input = cb; }
RETRO_API void retro_init(void) {}
RETRO_API void retro_deinit(void) {}
RETRO_API void retro_get_system_info(struct retro_system_info *info) {
	memset(info, 0, sizeof(*info));
	info->library_name = "Oldies test core";
	info->library_version = "1";
	info->valid_extensions = "exe|bat|com";
	info->need_fullpath = true;
}
RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info) {
	info->geometry.base_width = info->geometry.max_width = 320;
	info->geometry.base_height = info->geometry.max_height = 200;
	info->geometry.aspect_ratio = 4.0f / 3.0f;
	info->timing.fps = 70.0;
	info->timing.sample_rate = 44100.0;
}
RETRO_API void retro_set_controller_port_device(unsigned, unsigned) {}
RETRO_API void retro_reset(void) { g_x = 20; g_y = 60; }
RETRO_API bool retro_load_game(const struct retro_game_info *) {
	enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
	g_env(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
	struct retro_keyboard_callback kb = { keyboard };
	g_env(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &kb);
	return true;
}
RETRO_API bool retro_load_game_special(unsigned, const struct retro_game_info *, size_t) { return false; }
RETRO_API void retro_unload_game(void) {}
RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
RETRO_API void *retro_get_memory_data(unsigned) { return 0; }
RETRO_API size_t retro_get_memory_size(unsigned) { return 0; }
RETRO_API size_t retro_serialize_size(void) { return 4 * sizeof(int); }
RETRO_API bool retro_serialize(void *data, size_t) {
	int v[4] = { g_x, g_y, g_dx, g_dy };
	memcpy(data, v, sizeof(v));
	return true;
}
RETRO_API bool retro_unserialize(const void *data, size_t) {
	int v[4];
	memcpy(v, data, sizeof(v));
	g_x = v[0]; g_y = v[1]; g_dx = v[2]; g_dy = v[3];
	return true;
}
RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned, bool, const char *) {}

RETRO_API void retro_run(void) {
	g_poll();
	++g_tick;
	if (g_tick & 1) { // (a new picture every other frame; the same one again in between)
		if (g_keys[RETROK_LEFT]) g_dx = -4;
		if (g_keys[RETROK_RIGHT]) g_dx = 4;
		if (g_keys[RETROK_UP]) g_dy = -2;
		if (g_keys[RETROK_DOWN]) g_dy = 2;
		g_x += g_dx;
		g_y += g_dy;
		if (g_x < 0 || g_x > 320 - 40) { g_dx = -g_dx; g_x += g_dx; }
		if (g_y < 0 || g_y > 200 - 40) { g_dy = -g_dy; g_y += g_dy; }
		for (int y = 0; y < 200; ++y) {
			for (int x = 0; x < 320; ++x) {
				const bool check = ((x / 16) + (y / 16)) & 1;
				g_frame[y * 320 + x] = check ? 0x203040 : 0x304860;
			}
		}
		const uint32_t box = g_keys[RETROK_SPACE] ? 0xFF4040 : g_keys[RETROK_LCTRL] ? 0x40FF40 : 0xFFD27A;
		for (int y = 0; y < 40; ++y) {
			for (int x = 0; x < 40; ++x) {
				const bool edge = x < 3 || y < 3 || x > 36 || y > 36 || x == y;
				g_frame[(g_y + y) * 320 + g_x + x] = edge ? 0x101010 : box;
			}
		}
	}
	g_video(g_frame, 320, 200, 320 * sizeof(uint32_t));
	int16_t samples[630 * 2];
	for (int i = 0; i < 630; ++i) {
		const int16_t v = (int16_t)(sin(g_phase) * 3000);
		g_phase += 2 * 3.14159265 * 330 / 44100;
		samples[i * 2] = samples[i * 2 + 1] = v;
	}
	g_audio(samples, 630);
}

}
