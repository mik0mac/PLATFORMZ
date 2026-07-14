CXX := g++

# Local secrets (optional, gitignored). Create secrets.mk to bake private
# values into handout builds without them ever touching the repo, e.g.:
#   EXTRA_CXXFLAGS += -DPLATFORMZ_DEFAULT_SERVER_KEY='"the-join-key"'
#   EXTRA_CXXFLAGS += -DPLATFORMZ_DEFAULT_SERVER_HOST='"yourdomain.com"'
# The web build ignores EXTRA_CXXFLAGS on purpose - the wasm bundle is served
# to anyone who visits the page, so a key baked there would be public. Browser
# players get the key from their invite link (?key=...) instead.
-include secrets.mk

# --- IXWebSocket (vendored git submodule) ---------------------------------
# Compiled once into a local static lib so editing game code doesn't recompile
# it. TLS on macOS uses Secure Transport (-framework Security); no OpenSSL needed.
IX_DIR  := third_party/IXWebSocket
IX_SRCS := $(wildcard $(IX_DIR)/ixwebsocket/*.cpp)
IX_OBJS := $(patsubst $(IX_DIR)/ixwebsocket/%.cpp,build/ix/%.o,$(IX_SRCS))
IX_LIB  := build/ix/libixwebsocket_local.a
IX_DEFS := -DIXWEBSOCKET_USE_TLS -DIXWEBSOCKET_USE_SECURE_TRANSPORT

# -I/opt/homebrew/include also resolves nlohmann/json.hpp (brew nlohmann-json).
CXXFLAGS := -std=c++17 -O2 -I/opt/homebrew/include -I$(IX_DIR)
# Extra compile flags for one-off/release builds without editing sources. Mainly
# for baking a server address into a distribution binary (see docs/deploy-vultr.md):
#   make EXTRA_CXXFLAGS='-DPLATFORMZ_DEFAULT_SERVER_HOST=\"203.0.113.10\"'
# DIST_CXXFLAGS is a separate variable (not folded into EXTRA_CXXFLAGS) so the
# `dist` target can pass it as a command-line override without blocking
# secrets.mk's `EXTRA_CXXFLAGS +=` (command-line vars lock out further +=
# assignment to that *same* variable name for the invocation).
CXXFLAGS += $(EXTRA_CXXFLAGS) $(DIST_CXXFLAGS)
LDFLAGS  := -L/opt/homebrew/lib -lraylib \
            -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo \
            -framework Security -framework CoreFoundation -lz

TARGET := platformz
SRCS := main.cpp collisions.cpp
HDRS := $(wildcard *.h)

all: $(TARGET)

build/ix:
	mkdir -p build/ix

build/ix/%.o: $(IX_DIR)/ixwebsocket/%.cpp | build/ix
	$(CXX) -std=c++17 -O2 -I$(IX_DIR) $(IX_DEFS) -c $< -o $@

$(IX_LIB): $(IX_OBJS)
	ar rcs $@ $(IX_OBJS)

# Game links the prebuilt IX lib. Editing a header rebuilds the game TUs (main +
# collisions) but not IX.
$(TARGET): $(SRCS) $(HDRS) $(IX_LIB)
	$(CXX) $(CXXFLAGS) $(SRCS) $(IX_LIB) -o $(TARGET) $(LDFLAGS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

# Also drops the cached IXWebSocket objects (slow to rebuild).
clean-all: clean
	rm -rf build

# Distribution build: bakes a server address into the binary so recipients can
# just run ./platformz with no URL (see docs/deploy-vultr.md). -B forces a
# rebuild since the baked define isn't tracked as a make dependency, so a
# stale dev build wouldn't otherwise be replaced. Composes automatically with a
# secrets.mk join key (see the note at the top of this file) - HOST/PORT go
# through DIST_CXXFLAGS, a separate variable from secrets.mk's EXTRA_CXXFLAGS,
# so passing one on the command line doesn't block the other.
# HOST is optional when secrets.mk already defines PLATFORMZ_DEFAULT_SERVER_HOST
# (then plain `make dist` uses it); HOST= on the command line overrides it.
#   make dist [HOST=203.0.113.10] [PORT=9000]
SECRETS_HOST := $(findstring PLATFORMZ_DEFAULT_SERVER_HOST,$(EXTRA_CXXFLAGS))
dist:
	@if [ -z "$(HOST)" ] && [ -z "$(SECRETS_HOST)" ]; then \
		echo "Usage: make dist HOST=<server-ip-or-domain> [PORT=<port>]"; \
		echo "       (or define PLATFORMZ_DEFAULT_SERVER_HOST in secrets.mk)"; \
		exit 1; \
	fi
	$(MAKE) -B DIST_CXXFLAGS='$(if $(HOST),-DPLATFORMZ_DEFAULT_SERVER_HOST=\"$(HOST)\")$(if $(PORT), -DPLATFORMZ_DEFAULT_SERVER_PORT=\"$(PORT)\")'

# --- Web (Emscripten / WASM) build ----------------------------------------
# Builds the browser client. Requires the emsdk toolchain (emcc on PATH) and a
# web build of raylib (compiled with emcc for PLATFORM_WEB). Point RAYLIB_WEB_DIR
# at that raylib checkout (it must contain src/libraylib.a + the raylib headers):
#   make web RAYLIB_WEB_DIR=/path/to/raylib
#
# No IXWebSocket / Apple frameworks here: the browser backend in net_client.h
# uses the JS WebSocket API (-lwebsocket.js), and emcc auto-defines __EMSCRIPTEN__
# which selects that backend + the query-string server URL in main.cpp.
# nlohmann/json is header-only, so the same brew include path works under emcc.
EMCC           := emcc
RAYLIB_WEB_DIR ?= $(HOME)/raylib
RAYLIB_WEB_INC ?= $(RAYLIB_WEB_DIR)/src
RAYLIB_WEB_LIB ?= $(RAYLIB_WEB_DIR)/src/libraylib.a
WEB_OUT        := web/platformz.html

WEB_CXXFLAGS := -std=c++17 -O2 -I/opt/homebrew/include -I$(RAYLIB_WEB_INC)
# -sASYNCIFY lets the existing blocking while(!WindowShouldClose()) loop yield to
# the browser. It can be dropped once the loop uses emscripten_set_main_loop().
# -sEXPORTED_RUNTIME_METHODS=HEAPF32: raylib 5.5's bundled miniaudio reads
# `Module.HEAPF32.buffer` in its Web Audio callback, but emscripten 6.x no longer
# attaches HEAPF32 to Module by default - without this export it's undefined and
# the audio callback throws every frame (silent game). See miniaudio.h ScriptNode.
# --preload-file assets: assets/ ships WHOLESALE into platformz.data, so keep
# only runtime files in it - audio masters/WIP/retired live in audio-src/, which
# is never built or deployed. The one exclusion is .DS_Store (macOS recreates it).
WEB_LDFLAGS  := -sUSE_GLFW=3 -sALLOW_MEMORY_GROWTH=1 -sASYNCIFY \
                -sEXPORTED_RUNTIME_METHODS=HEAPF32 \
                -lwebsocket.js \
                --preload-file assets \
                --exclude-file "*.DS_Store" \
                --shell-file shell.html

# Download-size budget for the preloaded assets. assets/ ships wholesale (see
# WEB_LDFLAGS above), so a misplaced audio master would silently bloat every
# player's platformz.data - fail the build instead of shipping it.
WEB_ASSET_BUDGET_MB := 20

web: $(SRCS) $(HDRS) shell.html | webdir
	@size=$$(du -sm assets | cut -f1); \
	if [ $$size -gt $(WEB_ASSET_BUDGET_MB) ]; then \
	  echo "ERROR: assets/ is $${size}MB, over the $(WEB_ASSET_BUDGET_MB)MB web-download budget."; \
	  echo "assets/ ships wholesale into platformz.data. Largest files:"; \
	  find assets -type f -size +1M -exec du -h {} + | sort -rh | head; \
	  echo "Move non-runtime audio (masters/WIP/retired) to audio-src/, or raise WEB_ASSET_BUDGET_MB."; \
	  exit 1; \
	fi; \
	echo "assets/ -> platformz.data: $${size}MB (budget $(WEB_ASSET_BUDGET_MB)MB)"
	$(EMCC) $(WEB_CXXFLAGS) $(SRCS) $(RAYLIB_WEB_LIB) -o $(WEB_OUT) $(WEB_LDFLAGS)

webdir:
	mkdir -p web

clean-web:
	rm -f web/platformz.html web/platformz.js web/platformz.wasm web/platformz.data

.PHONY: all run clean clean-all dist web webdir clean-web
