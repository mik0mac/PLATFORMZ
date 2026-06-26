# server/Makefile
#
# Builds the PLATFORMZ server on Linux (GitHub Actions ubuntu-latest).
# No raylib install needed - raylib_server_stub.h provides the math types.
#
# Include order matters:
#   -I ./    server/ first, so raylib_server_stub.h shadows raylib.h
#   -I ../   game headers (elements.h, gamespace.h, collisions.h, etc.)
#
# BUILD:
#   make       -> gameserver binary
#   make clean -> remove binary

CXX      := g++
SERVERDIR := $(shell pwd)
GAMEDIR   := $(shell pwd)/..

# -include injects the stub as the very first line of every compiled file,
# before any #include in the source. This means when gamespace.h or
# collisions.h do #include "raylib.h", the RAYLIB_H guard from the stub
# is already defined and the include is skipped - even though those headers
# live in ../ and would otherwise resolve "raylib.h" relative to themselves.
CXXFLAGS  := -std=c++17 -O2 -Wall \
              -I $(SERVERDIR) \
              -I $(GAMEDIR) \
              -include $(SERVERDIR)/raylib_server_stub.h

LDFLAGS   := -lboost_system -lpthread

# server_main.cpp + collisions.cpp (game logic, no rendering)
SRCS     := server_main.cpp ../collisions.cpp

TARGET   := gameserver

all: $(TARGET)

$(TARGET): $(SRCS) raylib_server_stub.h
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
