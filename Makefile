CXX := g++

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
WEB_LDFLAGS  := -sUSE_GLFW=3 -sALLOW_MEMORY_GROWTH=1 -sASYNCIFY \
                -sEXPORTED_RUNTIME_METHODS=HEAPF32 \
                -lwebsocket.js \
                --preload-file assets \
                --exclude-file "*audio_WIP*" \
                --shell-file shell.html

web: $(SRCS) $(HDRS) shell.html | webdir
	$(EMCC) $(WEB_CXXFLAGS) $(SRCS) $(RAYLIB_WEB_LIB) -o $(WEB_OUT) $(WEB_LDFLAGS)

webdir:
	mkdir -p web

clean-web:
	rm -f web/platformz.html web/platformz.js web/platformz.wasm web/platformz.data

.PHONY: all run clean clean-all web webdir clean-web
