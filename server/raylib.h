// server/raylib.h
//
// Server-build shim. On the Linux runner there is no real raylib, so when the
// game headers do #include "raylib.h" this resolves here (server/ is ahead of
// ../ on the include path) and forwards to the math-only stub.
#pragma once
#include "raylib_server_stub.h"
