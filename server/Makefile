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

TARGET   := gameserver

all: $(TARGET)

$(TARGET): $(SRCS) raylib_server_stub.h
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
