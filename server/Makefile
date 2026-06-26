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
CXXFLAGS := -std=c++17 -O2 -Wall \
             -I ./ \
             -I ../
LDFLAGS  := -lboost_system -lpthread

# server_main.cpp + collisions.cpp (game logic, no rendering)
SRCS     := server_main.cpp ../collisions.cpp

TARGET   := gameserver

all: $(TARGET)

$(TARGET): $(SRCS) raylib_server_stub.h
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
