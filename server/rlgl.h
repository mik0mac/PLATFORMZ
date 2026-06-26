// server/rlgl.h
//
// Server-build shim for #include "rlgl.h". The headless server compiles no
// rendering code (shapes.h is guarded out by PLATFORMZ_SERVER), so this only
// needs to satisfy the include directive. Forwards to the stub. See raylib.h.
#pragma once
#include "raylib_server_stub.h"
