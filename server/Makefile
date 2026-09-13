# server/Makefile
#
# Builds the PLATFORMZ server on Linux (GitHub Actions ubuntu-latest).
# No raylib install needed - raylib_server_stub.h provides the math types.
#
# Include order matters:
#   -I ./    server/ first, so the raylib.h/raymath.h/rlgl.h shims here resolve
#            the game headers' includes (those files don't exist in ../ on Linux)
#   -I ../   game headers (elements.h, gamespace.h, collisions.h, etc.)
#
# BUILD:
#   make       -> gameserver binary
#   make clean -> remove binary

CXX      := g++
SERVERDIR := $(shell pwd)
GAMEDIR   := $(shell pwd)/..

# On macOS, Boost lives under the Homebrew prefix (e.g. /opt/homebrew), not in
# the default system paths. Add it when brew is present. On Linux/CI brew is
# absent so BREW_PREFIX is empty and this is a no-op (Boost is in /usr/include).
BREW_PREFIX := $(shell brew --prefix 2>/dev/null)

# When gamespace.h / collisions.h do #include "raylib.h" (and raymath.h, rlgl.h),
# those directives must physically find a file - a predefined include guard does
# NOT skip the lookup. server/ provides shim headers of those exact names (each
# forwarding to raylib_server_stub.h), and -I $(SERVERDIR) is searched before
# $(GAMEDIR), so the includes resolve to the math-only stub.
#
# -DPLATFORMZ_SERVER guards out shapes.h (and GameSpace::draw) in gamespace.h, so
# the headless server never compiles rendering code that calls real raylib.
# Paths are quoted so a directory containing spaces (e.g. a Dropbox path on a
# local machine) doesn't break the include flags. On CI the path has no spaces.
CXXFLAGS  := -std=c++17 -O2 -Wall \
              -I "$(SERVERDIR)" \
              -I "$(GAMEDIR)" \
              -DPLATFORMZ_SERVER

LDFLAGS   := -lpthread

# Boost.System is header-only (Boost >= 1.69), so we never link libboost_system.
# On macOS, still add Homebrew's include/lib paths so the Boost headers are found;
# on Linux/CI the headers are on the default search path (libboost-dev).
ifneq ($(BREW_PREFIX),)
CXXFLAGS += -I$(BREW_PREFIX)/include
LDFLAGS  += -L$(BREW_PREFIX)/lib
endif

# server_main.cpp + collisions.cpp (game logic, no rendering)
SRCS     := server_main.cpp ../collisions.cpp

# Every header is a dependency, exactly like the root Makefile's `HDRS :=
# $(wildcard *.h)`. Most of this project's logic lives in headers (netbin.h,
# elements.h, gamespace.h, ...), so listing only the .cpp files meant a
# header-only change rebuilt NOTHING here: `git pull` + `make -C server` +
# `systemctl restart` on the box would print "gameserver is up to date" and
# restart the identical old binary. That silently shipped a server whose
# netbin.h constants disagreed with the clients', decoding every position at
# the wrong scale. Deps, not the deploy docs, are the right place to fix that.
#
# Relative (not $(SERVERDIR)/$(GAMEDIR)) on purpose: prerequisites are split on
# whitespace and can't be quoted the way the -I flags above are, so absolute
# paths would break on a paths-with-spaces checkout (e.g. Dropbox on a Mac).
# This also subsumes the old explicit raylib_server_stub.h dep - it's a *.h here.
HDRS     := $(wildcard *.h) $(wildcard ../*.h)

TARGET   := gameserver

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)

# ThreadSanitizer build, for CI (E3) and for reproducing a suspected race by hand.
#
# A separate binary rather than a flag on the normal one: TSan costs ~5-15x in
# speed and ~5x in memory, so it is never what you want to deploy or to measure
# perf with, and having both on disk at once means you can run the probes against
# either without a rebuild.
#
# -O1, not -O2: TSan's reports are only as good as the frames it can name, and
# -O2 inlines away the ones you actually want to read. -g for the same reason.
# -fno-omit-frame-pointer because TSan's own docs ask for it.
TSAN_TARGET := $(TARGET)-tsan

tsan: $(TSAN_TARGET)

$(TSAN_TARGET): $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -fsanitize=thread -O1 -g -fno-omit-frame-pointer \
	    $(SRCS) -o $(TSAN_TARGET) $(LDFLAGS)

# Load generator + WebSocket smoke test (E3). Its own binary and its own single
# translation unit - it is a CLIENT, and linking it against the server's objects
# would be a category error even though it shares every header.
#
# No ../collisions.cpp: it decodes far enough to count packets and read a slot,
# never far enough to need a world.
# No `loadtest: $(LOADTEST)` alias - the binary is already called `loadtest`, so
# that would be a rule depending on itself and make says so.
LOADTEST := loadtest

$(LOADTEST): loadtest.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) loadtest.cpp -o $(LOADTEST) $(LDFLAGS)

clean:
	rm -f $(TARGET) $(TSAN_TARGET) $(LOADTEST)

.PHONY: all clean tsan
