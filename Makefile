CXX := g++
CXXFLAGS := -std=c++17 -I/opt/homebrew/include
LDFLAGS := -L/opt/homebrew/lib -lraylib -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo

TARGET := platformz
SRCS := main.cpp collisions.cpp
HDRS := $(wildcard *.h)

all: $(TARGET)

# Depend on the headers too, so editing any .h triggers a rebuild.
$(TARGET): $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all run clean
