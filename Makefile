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

.PHONY: all run clean clean-all
