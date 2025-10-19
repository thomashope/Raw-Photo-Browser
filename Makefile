CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra
CXXFLAGS_DEBUG = -g
CXXFLAGS_RELEASE = -O3 -DNDEBUG
TARGET = photo-browser
SRC = main.cpp

# Library flags for homebrew installations
LDFLAGS = -L/opt/homebrew/lib
INCLUDES = -I/opt/homebrew/include
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
