# CMakeLists.txt - the cross-platform build (F1, #92).
#
# Three targets from one tree:
#   platformz    the native client   - macOS, Windows
#   gameserver   the headless server - Linux (the box), macOS (dev)
#   loadtest     the server's load generator / WS smoke test (a client, see server/Makefile)
#
# Quick start (CMakePresets.json names the build dirs):
#   cmake --preset dev        && cmake --build --preset dev         # client + server
#   cmake --preset server     && cmake --build --preset server      # server only, no raylib needed
#
# What this does NOT replace - the Makefiles stay, and stay authoritative for:
#   make app / sign / notarize / dist-pack   the macOS handout chain. It works,
#       it asserts Gatekeeper before zipping, and it links a raylib built for
#       macOS 13 that nothing here knows about. Rebuilding it on CMake would be
#       tidier and would buy nothing.
#   make web     the browser client. This file REFUSES Emscripten, see below.
#   make -C server   what docs/redeploy.md runs on the box, and what the test
#       scripts build before they test.
# Two builds of the same sources have to agree on the source list: when a .cpp
# is added, it goes in SRCS in the Makefile AND in PLATFORMZ_CLIENT_SOURCES here.

cmake_minimum_required(VERSION 3.21)
# The version players see (Windows exe properties, the zip name). Keep in step
# with APP_VERSION in the Makefile, which stamps the macOS Info.plist.
project(PLATFORMZ VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# compile_commands.json for clangd, which is otherwise run without the Homebrew
# include path (see "IDE diagnostics are false positives" in CLAUDE.md).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)   # the Makefiles build -O2; so does Release, give or take
endif()
# Windows: the C++ runtime linked INTO the exe (/MT), for every target including
# the fetched raylib and mbedTLS - which is why it is set here, before any of them
# is declared. The default (/MD) needs vcruntime140.dll from Microsoft's VC++
# Redistributable, which a player may simply not have: the game would refuse to
# start with a missing-DLL error. Same idea as the Mac handout linking raylib
# statically so the player needs no Homebrew. packaging/windows/pack.ps1 checks it.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The web build is `make web` and only `make web`. The Makefile deliberately
# keeps EXTRA_CXXFLAGS (secrets.mk's join key) out of the wasm bundle, because
# that bundle is served to anyone who opens the page. Everything below bakes
# secrets.mk in, so an emcmake configure would put the key on the public web.
if(EMSCRIPTEN)
  message(FATAL_ERROR
    "The web client is built with `make web`, not CMake: this build bakes "
    "secrets.mk into the binary, and the wasm bundle is public.")
endif()

option(PLATFORMZ_BUILD_CLIENT "Build the native client (needs raylib)" ON)
option(PLATFORMZ_BUILD_SERVER "Build the headless gameserver (needs Boost headers)" ON)
option(PLATFORMZ_BUILD_LOADTEST "Build server/loadtest (needs nlohmann/json)" OFF)
# The Steam build (F4, #95). OFF is the website/handout build, which contains no
# Steam code and ships no Valve library - two builds from one tree, see platform.h.
option(PLATFORMZ_STEAM "Build the Steam edition (needs the Steamworks SDK)" OFF)
# Default: a steamworks_sdk folder BESIDE the checkout - outside the repo, where
# it has to be. Either the sdk/ folder itself or the zip's top folder above it.
set(STEAMWORKS_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../steamworks_sdk" CACHE PATH
    "The unzipped Steamworks SDK (the folder holding public/ and redistributable_bin/, or the one above it)")
# 480 is Valve's public test app, Spacewar: lets the Steam features run before
# PLATFORMZ has an App ID of its own. Replace it once the store entry exists.
set(PLATFORMZ_STEAM_APPID "480" CACHE STRING "Steam App ID for development runs (steam_appid.txt)")

include(FetchContent)

#MARK: Baked-in server address and join key
# Parity with `make`: secrets.mk's `EXTRA_CXXFLAGS += ...` lines are read and
# applied to the CLIENT, so a CMake dev build connects where a make dev build
# does. (Which is the LIVE server - see docs/multiplayer-testing.md.) Only lines of
# that one shape are understood; secrets.mk is a Makefile and nothing else in it
# is interpreted. It is re-read whenever it changes. Client only: the server
# takes its key from PLATFORMZ_KEY at run time.
set(PLATFORMZ_EXTRA_CXXFLAGS "")
set(_secrets "${CMAKE_CURRENT_SOURCE_DIR}/secrets.mk")
if(PLATFORMZ_BUILD_CLIENT AND EXISTS "${_secrets}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_secrets}")
  file(STRINGS "${_secrets}" _lines REGEX "^[ \t]*EXTRA_CXXFLAGS[ \t]*\\+?=")
  foreach(_line IN LISTS _lines)
    string(REGEX REPLACE "^[ \t]*EXTRA_CXXFLAGS[ \t]*\\+?=[ \t]*" "" _value "${_line}")
    # The values are shell-quoted for make's recipe line; undo that the same way.
    separate_arguments(_args UNIX_COMMAND "${_value}")
    list(APPEND PLATFORMZ_EXTRA_CXXFLAGS ${_args})
  endforeach()
  list(LENGTH PLATFORMZ_EXTRA_CXXFLAGS _n)
  message(STATUS "secrets.mk: ${_n} client flag(s) baked in (values not printed)")
endif()
# `make dist HOST=... PORT=...`, as cache variables. Empty = not set.
set(PLATFORMZ_DEFAULT_SERVER_HOST "" CACHE STRING "Server address baked into the client (make dist HOST=)")
set(PLATFORMZ_DEFAULT_SERVER_PORT "" CACHE STRING "Server port baked into the client (make dist PORT=)")

#MARK: Client
if(PLATFORMZ_BUILD_CLIENT)
  # raylib 5.5. Homebrew's on a Mac; anywhere it is not installed (Windows, a
  # clean Linux box) it is fetched and built from the same tag. Homebrew's
  # config names the target `raylib`, and so does raylib's own CMake build.
  find_package(raylib 5.5 QUIET)
  if(NOT raylib_FOUND)
    message(STATUS "raylib: not installed, fetching 5.5")
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES    OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(raylib
      URL https://github.com/raysan5/raylib/archive/refs/tags/5.5.tar.gz
      URL_HASH SHA256=aea98ecf5bc5c5e0b789a76de0083a21a70457050ea4cc2aec7566935f5e258e
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(raylib)
  endif()

  # nlohmann/json (wire.h). Header-only.
  find_package(nlohmann_json 3 QUIET)
  if(NOT nlohmann_json_FOUND)
    message(STATUS "nlohmann_json: not installed, fetching 3.11.3")
    FetchContent_Declare(nlohmann_json
      URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
      URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(nlohmann_json)
  endif()

  # IXWebSocket, the vendored submodule, through its own CMakeLists - with the
  # same choices the Makefile makes by hand: TLS on, zlib off (nothing here
  # compresses, and the Makefile never defines IXWEBSOCKET_USE_ZLIB). The TLS
  # backend is IX's default per platform: Secure Transport on macOS (no OpenSSL,
  # as with make), mbedTLS on Windows, OpenSSL elsewhere.
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/IXWebSocket/CMakeLists.txt")
    message(FATAL_ERROR "third_party/IXWebSocket is empty: git submodule update --init")
  endif()
  set(USE_TLS  ON  CACHE BOOL "" FORCE)
  set(USE_ZLIB OFF CACHE BOOL "" FORCE)
  set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
  if(WIN32)
    # IX finds mbedTLS with its own find module, which a stock Windows box will
    # not satisfy. Fetch it instead and hand IX the answer it would have found;
    # IX skips its search when MBEDTLS_FOUND is already set.
    find_package(MbedTLS QUIET)
    if(NOT MbedTLS_FOUND AND NOT MBEDTLS_FOUND)
      message(STATUS "mbedTLS: not installed, fetching 3.6.2")
      set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
      set(ENABLE_TESTING  OFF CACHE BOOL "" FORCE)
      FetchContent_Declare(mbedtls
        URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2
        URL_HASH SHA256=8b54fb9bcf4d5a7078028e0520acddefb7900b3e66fec7f7175bb5b7d85ccdca
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
      FetchContent_MakeAvailable(mbedtls)
      set(MBEDTLS_FOUND TRUE)
      set(MBEDTLS_INCLUDE_DIRS "${mbedtls_SOURCE_DIR}/include")
      set(MBEDTLS_LIBRARIES mbedtls mbedx509 mbedcrypto)
      set(_ix_mbedtls3 TRUE)   # IX's find module would have said so
    endif()
  endif()
  add_subdirectory(third_party/IXWebSocket EXCLUDE_FROM_ALL)
  # IX turns on -Wall -Wextra -pedantic for itself; the Makefile builds it with
  # no warning flags at all. Vendored code we do not edit - keep it quiet.
  if(MSVC)
    target_compile_options(ixwebsocket PRIVATE /W0)
  else()
    target_compile_options(ixwebsocket PRIVATE -w)
  endif()
  if(_ix_mbedtls3)
    target_compile_definitions(ixwebsocket PRIVATE IXWEBSOCKET_USE_MBED_TLS_MIN_VERSION_3)
  endif()

  # Keep in step with SRCS in the Makefile.
  set(PLATFORMZ_CLIENT_SOURCES main.cpp collisions.cpp net_native.cpp)
  add_executable(platformz ${PLATFORMZ_CLIENT_SOURCES})
  if(WIN32)
    # Version resource: what Explorer shows under Properties > Details, and
    # the product name SmartScreen shows alongside the (for now absent)
    # publisher. Stamped from project(VERSION) above.
    configure_file(packaging/windows/platformz.rc.in
                   "${CMAKE_CURRENT_BINARY_DIR}/platformz.rc" @ONLY)
    target_sources(platformz PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/platformz.rc")
  endif()
  target_link_libraries(platformz PRIVATE raylib nlohmann_json::nlohmann_json ixwebsocket)
  target_compile_options(platformz PRIVATE ${PLATFORMZ_EXTRA_CXXFLAGS})
  if(PLATFORMZ_DEFAULT_SERVER_HOST)
    target_compile_definitions(platformz PRIVATE
      PLATFORMZ_DEFAULT_SERVER_HOST="${PLATFORMZ_DEFAULT_SERVER_HOST}")
  endif()
  if(PLATFORMZ_DEFAULT_SERVER_PORT)
    target_compile_definitions(platformz PRIVATE
      PLATFORMZ_DEFAULT_SERVER_PORT="${PLATFORMZ_DEFAULT_SERVER_PORT}")
  endif()
  if(WIN32)
    # A game, not a console tool: without this Windows opens a black console
    # window behind the game for the whole session. Debug keeps the console,
    # because that is where raylib's log and the PERF lines go. main() stays the
    # entry point (no WinMain) under both.
    if(MSVC)
      target_link_options(platformz PRIVATE
        $<$<NOT:$<CONFIG:Debug>>:/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup>)
    else()
      target_link_options(platformz PRIVATE $<$<NOT:$<CONFIG:Debug>>:-mwindows>)
    endif()
  endif()
  if(APPLE)
    # What the Makefile's LDFLAGS name. Homebrew's raylib is a dylib that brings
    # its own; a static or fetched one does not, and these are harmless twice.
    target_link_libraries(platformz PRIVATE
      "-framework OpenGL" "-framework Cocoa" "-framework IOKit" "-framework CoreVideo"
      "-framework CoreAudio" "-framework AudioToolbox")
  endif()

  #MARK: Steam edition
  # The Steamworks SDK is NOT in this repo and must never be: the repo is public
  # and Valve's SDK terms do not allow republishing it. It is downloaded from
  # partner.steamgames.com and unzipped outside the tree; this points at it. CI
  # never turns this on, so CI builds only the edition with no Steam in it.
  if(PLATFORMZ_STEAM)
    if(NOT EXISTS "${STEAMWORKS_SDK_DIR}/public/steam/steam_api.h"
       AND EXISTS "${STEAMWORKS_SDK_DIR}/sdk/public/steam/steam_api.h")
      set(STEAMWORKS_SDK_DIR "${STEAMWORKS_SDK_DIR}/sdk")   # pointed at the zip's top folder
    endif()
    if(NOT EXISTS "${STEAMWORKS_SDK_DIR}/public/steam/steam_api.h")
      message(FATAL_ERROR
        "PLATFORMZ_STEAM is ON but no Steamworks SDK at STEAMWORKS_SDK_DIR=${STEAMWORKS_SDK_DIR}. "
        "Download it from partner.steamgames.com, unzip it beside this checkout as "
        "steamworks_sdk/, or pass -DSTEAMWORKS_SDK_DIR=<path>")
    endif()
    message(STATUS "Steam edition: SDK at ${STEAMWORKS_SDK_DIR}, dev App ID ${PLATFORMZ_STEAM_APPID}")
    set(_sdk_bin "${STEAMWORKS_SDK_DIR}/redistributable_bin")
    if(WIN32)
      set(_steam_lib "${_sdk_bin}/win64/steam_api64.lib")
      set(_steam_rt  "${_sdk_bin}/win64/steam_api64.dll")
    elseif(APPLE)
      set(_steam_lib "${_sdk_bin}/osx/libsteam_api.dylib")
      set(_steam_rt  "${_steam_lib}")
    else()
      set(_steam_lib "${_sdk_bin}/linux64/libsteam_api.so")
      set(_steam_rt  "${_steam_lib}")
    endif()
    target_sources(platformz PRIVATE platform_steam.cpp)
    target_compile_definitions(platformz PRIVATE PLATFORMZ_STEAM=1)
    target_include_directories(platformz PRIVATE "${STEAMWORKS_SDK_DIR}/public")
    target_link_libraries(platformz PRIVATE "${_steam_lib}")
    # The library ships BESIDE the binary, and the binary looks for it there.
    if(APPLE)
      set_target_properties(platformz PROPERTIES BUILD_RPATH "@executable_path")
    elseif(UNIX)
      set_target_properties(platformz PROPERTIES BUILD_RPATH "$ORIGIN")
    endif()
    # steam_appid.txt is development only: it tells the Steam library which game
    # this is when it is NOT launched by Steam. A build Steam installs never has
    # it - Steam supplies the App ID itself - so it must not go into a shipped
    # depot. Written here and copied, because a build step cannot redirect output
    # portably.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/steam_appid.txt" "${PLATFORMZ_STEAM_APPID}\n")
    add_custom_command(TARGET platformz POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_steam_rt}" "$<TARGET_FILE_DIR:platformz>"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${CMAKE_CURRENT_BINARY_DIR}/steam_appid.txt" "$<TARGET_FILE_DIR:platformz>"
      VERBATIM)
  endif()

  # Assets load by RELATIVE path from the binary's own directory (main.cpp
  # anchors the cwd there), so a binary in the build dir needs assets/ beside
  # it. A link where the platform has cheap ones, so an edited sound is picked
  # up without a rebuild; a copy on Windows, where links need admin rights.
  if(WIN32)
    add_custom_command(TARGET platformz POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${CMAKE_CURRENT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:platformz>/assets"
      VERBATIM)
  else()
    add_custom_command(TARGET platformz POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E create_symlink
              "${CMAKE_CURRENT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:platformz>/assets"
      VERBATIM)   # the checkout path has spaces and parentheses in it
  endif()
endif()

#MARK: Server
if(PLATFORMZ_BUILD_SERVER OR PLATFORMZ_BUILD_LOADTEST)
  # Header-only Boost (Asio + Beast); Boost.System has been header-only since
  # 1.69, so nothing is linked. Boost's own config first, CMake's module second.
  find_package(Boost 1.69 CONFIG QUIET)
  if(NOT Boost_FOUND)
    find_package(Boost 1.69 REQUIRED)
  endif()
  find_package(Threads REQUIRED)

  # The server compiles the GAME's headers against a math-only raylib: the
  # server/ directory holds raylib.h/raymath.h/rlgl.h shims that forward to
  # raylib_server_stub.h, and it has to be searched BEFORE the repo root so those
  # names resolve there. PLATFORMZ_SERVER guards out every piece of rendering.
  # Deliberately an INTERFACE target of its own and never linked beside raylib:
  # the two raylib.h's must never meet in one include path.
  add_library(platformz_server_env INTERFACE)
  target_include_directories(platformz_server_env INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/server"
    "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_definitions(platformz_server_env INTERFACE PLATFORMZ_SERVER)
  target_link_libraries(platformz_server_env INTERFACE Boost::headers Threads::Threads)
  if(NOT MSVC)
    target_compile_options(platformz_server_env INTERFACE -Wall)
  endif()
endif()

if(PLATFORMZ_BUILD_SERVER)
  # Keep in step with SRCS in server/Makefile. No secrets.mk here: the server
  # takes its key from PLATFORMZ_KEY at run time, never at compile time.
  add_executable(gameserver server/server_main.cpp collisions.cpp)
  target_link_libraries(gameserver PRIVATE platformz_server_env)
endif()

if(PLATFORMZ_BUILD_LOADTEST)
  # A client of the server, built from one TU with no world (see server/Makefile).
  if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json 3 REQUIRED)
  endif()
  add_executable(loadtest server/loadtest.cpp)
  target_link_libraries(loadtest PRIVATE platformz_server_env nlohmann_json::nlohmann_json)
endif()

#MARK: Tests
# The existing suites, unchanged: each script builds its own test binaries.
# ctest just runs them. Bash, so not on Windows.
if(UNIX)
  enable_testing()
  add_test(NAME client_tests COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/test/run.sh")
  add_test(NAME server_unit_tests COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/server/test/run_all.sh")
  set_tests_properties(client_tests server_unit_tests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
