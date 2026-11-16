// Which part of the game's data files is being read: for subtitles (the viewer learns which sound clip starts).
// Preloaded (LD_PRELOAD) into Wine: reads of files below $TGSPY_DIR are told to the datagram socket $TGSPY_SOCK,
// or appended to the file $TGSPY_LOG, as "R name offset size\n". The sound the game plays (what Wine hands to
// PulseAudio) goes there too, as "A rate channels format bytes\n" and the samples: by it the viewer hears
// which clip plays when. (Format: PulseAudio's number, 3 = 16-bit, 5 = float.)
//   cc -O2 -shared -fPIC -o tgspy.so tgspy.c -ldl
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

enum { kMaxFd = 4096 };
static char g_state[kMaxFd];          // 0 not looked at, 1 not ours, 2 a game file
static char g_name[kMaxFd][24];
static const char *g_dir;
static size_t g_dirLen;
static int g_out = -2;
static struct sockaddr_un g_addr;
static int g_dgram;

static ssize_t (*real_read)(int, void *, size_t);
static ssize_t (*real_pread)(int, void *, size_t, off_t);
static int (*real_close)(int);

static void init(void) {
	real_read = dlsym(RTLD_NEXT, "read");
	real_pread = dlsym(RTLD_NEXT, "pread64");
	real_close = dlsym(RTLD_NEXT, "close");
	g_dir = getenv("TGSPY_DIR");
	g_dirLen = g_dir ? strlen(g_dir) : 0;
	const char *sock = getenv("TGSPY_SOCK"), *log = getenv("TGSPY_LOG");
	g_out = -1;
	if (!g_dirLen) return;
	if (sock && strlen(sock) < sizeof(g_addr.sun_path)) {
		g_out = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
		g_addr.sun_family = AF_UNIX;
		strcpy(g_addr.sun_path, sock);
		g_dgram = 1;
	} else if (log) {
		g_out = open(log, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
	}
}

static int ours(int fd) {
	if (g_out == -2) init();
	if (g_out < 0 || fd < 0 || fd >= kMaxFd) return 0;
	if (!g_state[fd]) {
		char link[40], path[1024];
		snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
		ssize_t n = readlink(link, path, sizeof(path) - 1);
		g_state[fd] = 1;
		if (n > (ssize_t)g_dirLen && !memcmp(path, g_dir, g_dirLen) && path[g_dirLen] == '/') {
			path[n] = 0;
			const char *base = strrchr(path, '/') + 1;
			if (strlen(base) < sizeof(g_name[0])) {
				strcpy(g_name[fd], base);
				g_state[fd] = 2;
			}
		}
	}
	return g_state[fd] == 2;
}

static void tell(int fd, long long off, size_t size) {
	char line[80];
	int n = snprintf(line, sizeof(line), "R %s %lld %zu\n", g_name[fd], off, size);
	int e = errno;
	if (g_dgram) sendto(g_out, line, n, 0, (struct sockaddr *)&g_addr, sizeof(g_addr));
	else if (write(g_out, line, n) < 0) { }
	errno = e;
}

ssize_t read(int fd, void *buf, size_t size) {
	if (!real_read) init();
	if (ours(fd)) tell(fd, (long long)lseek(fd, 0, SEEK_CUR), size);
	return real_read(fd, buf, size);
}

ssize_t pread64(int fd, void *buf, size_t size, off_t off) {
	if (!real_pread) init();
	if (ours(fd)) tell(fd, (long long)off, size);
	return real_pread(fd, buf, size, off);
}

ssize_t pread(int fd, void *buf, size_t size, off_t off) {
	return pread64(fd, buf, size, off);
}

// (libpulse's types, without its headers)
struct SampleSpec { int format; unsigned rate; unsigned char channels; };

int pa_stream_write(void *stream, const void *data, size_t bytes, void (*freeCb)(void *), long long offset, int seek) {
	static int (*real)(void *, const void *, size_t, void (*)(void *), long long, int);
	static const struct SampleSpec *(*spec)(void *);
	if (!real) {
		// (Wine loads its sound driver, and libpulse with it, out of the global scope: asked for by name)
		void *lib = dlopen("libpulse.so.0", RTLD_NOW | RTLD_NOLOAD);
		real = dlsym(lib ? lib : RTLD_NEXT, "pa_stream_write");
		spec = dlsym(lib ? lib : RTLD_NEXT, "pa_stream_get_sample_spec");
		if (!real) return -1;
	}
	if (g_out == -2) init();
	if (g_out >= 0 && spec) {
		const struct SampleSpec *ss = spec(stream);
		const char *p = data;
		size_t left = bytes;
		int e = errno;
		while (ss && left) {
			size_t n = left < 16384 ? left : 16384;
			char msg[64 + 16384];
			int h = snprintf(msg, 64, "A %u %u %d %zu\n", ss->rate, ss->channels, ss->format, n);
			memcpy(msg + h, p, n);
			if (g_dgram) sendto(g_out, msg, h + n, 0, (struct sockaddr *)&g_addr, sizeof(g_addr));
			else if (write(g_out, msg, h + n) < 0) { }
			p += n;
			left -= n;
		}
		errno = e;
	}
	return real(stream, data, bytes, freeCb, offset, seek);
}

int close(int fd) {
	if (!real_close) init();
	if (fd >= 0 && fd < kMaxFd) g_state[fd] = 0;
	return real_close(fd);
}
