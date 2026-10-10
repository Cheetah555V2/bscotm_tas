# Needs a 32-bit MinGW-w64 toolchain (the game is 32-bit):
#   pacman -S mingw-w64-i686-gcc mingw-w64-i686-make
# Run `mingw32-make` from the MSYS2 MINGW32 shell or via build.bat.
CXX      ?= g++
WINDRES  ?= windres
B        := build
CXXFLAGS := -std=c++17 -Os -Wall -Wno-missing-field-initializers -Wno-cast-function-type -ffunction-sections -fdata-sections -DUNICODE -D_UNICODE
LDFLAGS  := -static -s -Wl,--gc-sections

ifeq (,$(findstring i686,$(shell $(CXX) -dumpmachine)))
$(error $(CXX) is not a 32-bit (i686) compiler; see the note at the top of this Makefile)
endif

all: $B/bscotm_tas.exe $B/bscotm_hook.dll

$B:
	-mkdir $B

$B/app.o: src/app.rc src/app.manifest | $B
	$(WINDRES) -O coff -i $< -o $@

$B/bscotm_tas.exe: src/main.cpp src/engine.cpp src/movie.cpp src/common.h src/engine.h src/movie.h $B/app.o
	$(CXX) $(CXXFLAGS) -mwindows -municode src/main.cpp src/engine.cpp src/movie.cpp $B/app.o -o $@ $(LDFLAGS) -lcomctl32 -lcomdlg32

$B/bscotm_hook.dll: src/hook.cpp src/common.h | $B
	$(CXX) $(CXXFLAGS) -shared src/hook.cpp -o $@ $(LDFLAGS)

# Test tools (not in the release zip): `build.bat tools`; tools\verify.ps1 builds and runs them.
tools: $B/sstest.exe $B/dumpdiff.exe

$B/sstest.exe: tools/sstest.cpp src/engine.cpp src/movie.cpp src/common.h src/engine.h src/movie.h | $B
	$(CXX) $(CXXFLAGS) -municode tools/sstest.cpp src/engine.cpp src/movie.cpp -o $@ $(LDFLAGS) -lpsapi

$B/dumpdiff.exe: tools/dumpdiff.cpp | $B
	$(CXX) $(CXXFLAGS) tools/dumpdiff.cpp -o $@ $(LDFLAGS)

clean:
	rm -rf $B

.PHONY: all clean tools
