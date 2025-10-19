CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra
CXXFLAGS_DEBUG = -g
CXXFLAGS_RELEASE = -O3 -DNDEBUG
TARGET = photo-browser

# Source files
SRC = main.cpp \
	third_party/imgui/imgui.cpp \
	third_party/imgui/imgui_demo.cpp \
	third_party/imgui/imgui_draw.cpp \
	third_party/imgui/imgui_tables.cpp \
	third_party/imgui/imgui_widgets.cpp \
	third_party/imgui/backends/imgui_impl_sdl3.cpp \
	third_party/imgui/backends/imgui_impl_sdlrenderer3.cpp

# Library flags for homebrew installations
LDFLAGS = -L/opt/homebrew/lib
INCLUDES = -I/opt/homebrew/include -Ithird_party/imgui -Ithird_party/imgui/backends
LIBS = -lraw -lSDL3 -lSDL3_image

# Default target (debug build)
$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(CXXFLAGS_DEBUG) $(INCLUDES) $(SRC) $(LDFLAGS) $(LIBS) -o $(TARGET)

# Release build with optimizations
release: $(SRC)
	$(CXX) $(CXXFLAGS) $(CXXFLAGS_RELEASE) $(INCLUDES) $(SRC) $(LDFLAGS) $(LIBS) -o $(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: clean release
