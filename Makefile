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

# macOS deployment target. Empty for dev builds (whatever the SDK defaults to);
# the `app` target sets it to 13.0 so handout builds run on Ventura and later.
# NOTE: this has to be threaded into the IX object rule by hand - that rule
# spells out its own flags and does not use CXXFLAGS. The IX objects are also
# cached in build/ix with no dependency on this flag, so `app` wipes build/ix.
MACOS_MIN ?=
MACOS_MIN_FLAG := $(if $(MACOS_MIN),-mmacos-version-min=$(MACOS_MIN))

# -I/opt/homebrew/include also resolves nlohmann/json.hpp (brew nlohmann-json).
CXXFLAGS := -std=c++17 -O2 -I/opt/homebrew/include -I$(IX_DIR) $(MACOS_MIN_FLAG)
# Extra compile flags for one-off/release builds without editing sources. Mainly
# for baking a server address into a distribution binary (see docs/deploy-vultr.md):
#   make EXTRA_CXXFLAGS='-DPLATFORMZ_DEFAULT_SERVER_HOST=\"203.0.113.10\"'
# DIST_CXXFLAGS is a separate variable (not folded into EXTRA_CXXFLAGS) so the
# `dist` target can pass it as a command-line override without blocking
# secrets.mk's `EXTRA_CXXFLAGS +=` (command-line vars lock out further +=
# assignment to that *same* variable name for the invocation).
CXXFLAGS += $(EXTRA_CXXFLAGS) $(DIST_CXXFLAGS)
# How raylib is linked: the dev default is the Homebrew dylib; dist-pack
# overrides this with the static archive (plus the audio frameworks the dylib
# would otherwise pull in transitively) so handout builds don't need Homebrew.
RAYLIB_LINK ?= -lraylib
LDFLAGS  := -L/opt/homebrew/lib $(RAYLIB_LINK) \
            -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo \
            -framework Security -framework CoreFoundation -lz

TARGET := platformz
SRCS := main.cpp collisions.cpp
HDRS := $(wildcard *.h)

all: $(TARGET)

build/ix:
	mkdir -p build/ix

build/ix/%.o: $(IX_DIR)/ixwebsocket/%.cpp | build/ix
	$(CXX) -std=c++17 -O2 -I$(IX_DIR) $(MACOS_MIN_FLAG) $(IX_DEFS) -c $< -o $@

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

# --- macOS handout: .app bundle, Developer ID signing, notarization -------
# Self-contained handout: static raylib (recipient needs NO Homebrew), assets
# sealed into Contents/Resources, signed with Developer ID + hardened runtime,
# notarized by Apple and stapled - so the recipient just double-clicks and
# plays. No Gatekeeper warning, no xattr incantation, no instructions.
#
# Apple Silicon, macOS 13.0+ only. Server host + join key bake in from
# secrets.mk exactly like a normal build.
#
#   make app            unsigned bundle, no credentials needed (quick test)
#   make pack-unsigned  zip of the above, named so it can't be confused
#                       with the real handout
#   make sign           Developer ID + hardened runtime + secure timestamp
#   make notarize       submit to Apple, wait, staple the ticket
#   make dist-pack      the full handout: build -> sign -> notarize -> zip
#
# One-time setup before `make notarize` ever works (needs a secret typed in,
# so it can't live in this file):
#   xcrun notarytool store-credentials "platformz-notary"
#     Apple ID: mike@michaelmacallister.com   Team ID: 9WM486296X
#     password: an app-specific password from account.apple.com
#
# raylib is built from source rather than taken from Homebrew because the brew
# bottle is compiled with a 15.0 deployment target, which would lock out anyone
# not on Sequoia. See docs/deploy-vultr.md for the one-time build command.
RAYLIB_STATIC  := $(HOME)/raylib-macos13/build-mac/raylib/libraylib.a
APP_NAME       := PLATFORMZ
APP_ID         := space.platformz.game
APP_VERSION    ?= 0.1.0
APP_BUILD      ?= $(shell git rev-list --count HEAD 2>/dev/null || echo 1)
APP_MIN_OS     := 13.0
APP_DIR        := dist/$(APP_NAME).app
APP_ZIP        := dist/$(APP_NAME)-mac-arm64.zip
NOTARY_ZIP     := dist/$(APP_NAME)-notarize.zip
APP_ICON       := packaging/$(APP_NAME).icns
# Not secrets: the cert's private key and the app-specific password both live
# in the login keychain, so only the *names* appear here. Override on the
# command line (or in secrets.mk) if the cert is ever reissued.
CODESIGN_ID    ?= Developer ID Application: Michael MacAllister (9WM486296X)
NOTARY_PROFILE ?= platformz-notary

app:
	rm -f $(TARGET)
	rm -rf build/ix          # cached IX objects have no dep on MACOS_MIN
	$(MAKE) MACOS_MIN=$(APP_MIN_OS) \
	        RAYLIB_LINK='$(RAYLIB_STATIC) -framework CoreAudio -framework AudioToolbox'
	rm -rf $(APP_DIR) $(APP_ZIP) $(NOTARY_ZIP)
	mkdir -p $(APP_DIR)/Contents/MacOS $(APP_DIR)/Contents/Resources
	cp $(TARGET) $(APP_DIR)/Contents/MacOS/$(TARGET)
	cp -R assets $(APP_DIR)/Contents/Resources/assets
	printf 'APPL????' > $(APP_DIR)/Contents/PkgInfo
	sed -e 's/@APP_NAME@/$(APP_NAME)/g'     -e 's/@APP_ID@/$(APP_ID)/g' \
	    -e 's/@APP_EXE@/$(TARGET)/g'        -e 's/@APP_VERSION@/$(APP_VERSION)/g' \
	    -e 's/@APP_BUILD@/$(APP_BUILD)/g'   -e 's/@APP_MIN_OS@/$(APP_MIN_OS)/g' \
	    packaging/Info.plist.in > $(APP_DIR)/Contents/Info.plist
	@if [ -f "$(APP_ICON)" ]; then \
	  cp "$(APP_ICON)" $(APP_DIR)/Contents/Resources/$(APP_NAME).icns; \
	else \
	  echo "note: no $(APP_ICON) - shipping with the generic app icon"; \
	fi
	find $(APP_DIR) -name '.DS_Store' -delete
	xattr -cr $(APP_DIR)
	@echo "==> $(APP_DIR) (UNSIGNED - do not hand out)"

# Deliberately a different filename from $(APP_ZIP) so an unsigned build can
# never be mistaken for the real handout.
pack-unsigned: app
	rm -f dist/$(APP_NAME)-UNSIGNED-mac-arm64.zip
	ditto -c -k --keepParent $(APP_DIR) dist/$(APP_NAME)-UNSIGNED-mac-arm64.zip
	@echo "==> dist/$(APP_NAME)-UNSIGNED-mac-arm64.zip (UNSIGNED - local test only)"

# One codesign call, not --deep: there is no nested code (raylib and
# IXWebSocket are static, otool -L shows only Apple libraries). --deep is a
# *verify* flag; Apple deprecates it for signing.
sign: app
	@security find-identity -v -p codesigning | grep -qF "$(CODESIGN_ID)" || { \
	  echo "ERROR: signing identity not in keychain:"; \
	  echo "       $(CODESIGN_ID)"; \
	  echo "       check with: security find-identity -v -p codesigning"; exit 1; }
	codesign --force --options runtime --timestamp \
	  --sign "$(CODESIGN_ID)" $(APP_DIR)
	codesign --verify --deep --strict --verbose=2 $(APP_DIR)
	@codesign -dv --verbose=4 $(APP_DIR) 2>&1 | grep -q 'flags=.*runtime' || { \
	  echo "ERROR: hardened runtime missing - notarization would reject this"; exit 1; }
	@codesign -dv --verbose=4 $(APP_DIR) 2>&1 | grep -q '^Timestamp=' || { \
	  echo "ERROR: no secure timestamp (saw 'Signed Time'?)."; \
	  echo "       Without one the app stops launching when the cert expires."; \
	  echo "       Check network access to timestamp.apple.com and re-run."; exit 1; }
	@echo "==> signed, hardened runtime + secure timestamp (NOT yet notarized)"

# Hits Apple's servers; usually 1-5 minutes. `notarytool submit --wait` exits
# non-zero on anything but Accepted, so a rejection stops the build here.
notarize: sign
	@xcrun notarytool history --keychain-profile "$(NOTARY_PROFILE)" >/dev/null 2>&1 || { \
	  echo "ERROR: no notarytool keychain profile '$(NOTARY_PROFILE)'."; \
	  echo "       One-time setup (answer the prompts; do NOT pass --password,"; \
	  echo "       it would land in your shell history):"; \
	  echo "         xcrun notarytool store-credentials \"$(NOTARY_PROFILE)\""; \
	  echo "       Apple ID: mike@michaelmacallister.com   Team ID: 9WM486296X"; \
	  echo "       Password: an app-specific password from account.apple.com"; exit 1; }
	rm -f $(NOTARY_ZIP)
	ditto -c -k --keepParent $(APP_DIR) $(NOTARY_ZIP)
	xcrun notarytool submit $(NOTARY_ZIP) --keychain-profile "$(NOTARY_PROFILE)" --wait
	xcrun stapler staple $(APP_DIR)
	xcrun stapler validate $(APP_DIR)
	rm -f $(NOTARY_ZIP)
	@echo "==> notarized + stapled"

# Gatekeeper-asserts BEFORE zipping, so a bundle that would warn on someone
# else's Mac never reaches a zip named like the real handout.
dist-pack: notarize
	@spctl -a -vvv -t exec $(APP_DIR) 2>&1 | tee /dev/stderr | \
	  grep -q 'source=Notarized Developer ID' || { \
	  echo "ERROR: Gatekeeper did not report 'Notarized Developer ID'"; exit 1; }
	rm -f $(APP_ZIP)
	ditto -c -k --keepParent $(APP_DIR) $(APP_ZIP)
	@echo "==> $(APP_ZIP)  (signed, notarized, stapled)"

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

.PHONY: all run clean clean-all dist app pack-unsigned sign notarize dist-pack \
        web webdir clean-web
# The handout chain (dist-pack -> notarize -> sign -> app) is not parallel-safe:
# each step must see the exact bits the previous one produced, and a stale
# binary inheriting a fresh signature is the failure mode most worth avoiding.
.NOTPARALLEL:
