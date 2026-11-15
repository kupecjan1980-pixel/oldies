# Oldies: the shell around the emulator cores.
#   make            the program (needs g++, SDL2's development files, OpenGL ES 3 headers)
#   make test-core  cores/test_core_libretro.so, a stand-in core for testing the shell
#   make dist       the folder and .tar.gz to hand out (no games, no cores: install.sh fetches the core)
CXX ?= g++
CXXFLAGS = -g -O2 -std=c++11 -Wall -Wno-format-truncation -include glibc_compat.h `sdl2-config --cflags`
LIBS = `sdl2-config --libs` -lGLESv2 -ldl
OBJS = obj/shell.o obj/gfx.o obj/ui.o
VERSION = $(shell sed -n 's/^\#define OLDIES_VERSION "\(.*\)"/\1/p' shell.cpp)

all: obj oldies oldies-view

# the viewer for a game in an X server of its own (Ace Ventura under Wine): needs libX11, libXext, libXtst, libXfixes
oldies-view: obj/winview.o obj/gfx.o
	$(CXX) -static-libstdc++ -static-libgcc -o $@ $^ $(LIBS) -lX11 -lXext -lXtst -lXfixes

oldies: $(OBJS)
	$(CXX) -static-libstdc++ -static-libgcc -o $@ $^ $(LIBS)

obj:
	mkdir -p obj

obj/%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -c $< -o $@

test-core:
	mkdir -p cores
	$(CXX) -O2 -shared -fPIC -o cores/test_core_libretro.so test_core.cpp

dist: all
	rm -rf dist && mkdir -p dist/oldies/assets dist/oldies/shaders dist/oldies/cores
	cp oldies README.md CHANGELOG.md install.sh dist/oldies/
	cp assets/* dist/oldies/assets/
	cp shaders/* dist/oldies/shaders/
	strip dist/oldies/oldies
	cd dist && tar -czf oldies-linux-$(VERSION).tar.gz oldies

clean:
	rm -rf obj oldies oldies-view

-include obj/*.d
