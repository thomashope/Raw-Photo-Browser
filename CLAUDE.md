# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Building and Development Commands

### Dependencies
```bash
brew install libraw sdl3 premake5
```

### Build Commands
```bash
# Debug build (default)
make

# Release build with optimizations
make release

# Clean build artifacts
make clean

# Generate Xcode project for debugging
premake5 xcode4
open ./build/PhotoBrowser.xcworkspace
```

### Running
```bash
# Launch with no arguments (drag-and-drop UI)
./photo-browser

# Launch with a single image
./photo-browser path/to/image.nef

# Launch with a folder of images
./photo-browser path/to/folder
```

## Architecture Overview

This is a C++17 raw photo browser application with multi-threaded image loading. The codebase follows a header-only design for key components.

### Core Components

**main.cpp** - Main application loop and UI
- Handles SDL3 initialization and window management
- ImGui-based UI with file list sidebar and image viewport
- Zoom/pan controls and mouse event handling
- Manages the main render loop and coordinates with ImageDatabase

**image_database.h** - Multi-threaded image loading system
- Manages a pool of worker threads (one per CPU core)
- Uses concurrent task/result queues for thread communication
- Implements smart loading: preview-first, then full raw on demand
- Handles LibRaw initialization and raw file processing
- Main thread calls `update()` each frame to convert CPU textures to GPU textures

**texture_types.h** - Texture wrappers
- `CpuTexture`: CPU-side pixel data (owned by worker threads)
- `GpuTexture`: SDL texture handles (owned by main thread)
- Handles EXIF orientation (0°, 90°, 180°, 270°) for proper image display
- Move-only semantics to prevent accidental copies of large image data

**concurrent_queue.h** - Thread-safe queue
- Simple mutex-based queue for task distribution and result collection
- Used for `LoadTask` (main → workers) and `LoadResult` (workers → main)

### Threading Model

The application uses a producer-consumer pattern:

1. **Main thread** (UI thread):
   - Polls for missing images and queues `LoadTask` items
   - Calls `ImageDatabase::update()` to process `LoadResult` items
   - Creates SDL textures from completed CPU textures
   - Renders UI and images

2. **Worker threads** (N threads, where N = CPU cores):
   - Pull `LoadTask` items from queue
   - Use LibRaw to load preview (JPEG) and/or full raw image
   - Push `LoadResult` items back to main thread
   - Never touch SDL or OpenGL resources

### Image Loading Strategy

The system uses three load types to optimize performance:

- `PreviewOnly`: Load just the embedded JPEG preview (fast, for thumbnails)
- `RawOnly`: Load just the full processed raw image
- `Both`: Load preview first, then raw (used when first viewing an image)

This strategy ensures:
- Thumbnails appear quickly in the sidebar
- The UI remains responsive during large raw file processing
- Duplicate work is avoided (preview is only loaded once)

### Key Design Patterns

**No exceptions**: The codebase avoids exceptions. Functions return error codes or use optional-style patterns.

**Move semantics**: Large objects (textures, raw processors) use move-only semantics to prevent expensive copies.

**Lazy loading**: Images are only loaded when requested by the UI (when scrolled into view or selected).

**RAII**: Resource management uses destructors (SDL textures, LibRaw processors, pixel data).

### External Libraries

- **LibRaw**: Raw image decoding and processing
- **SDL3**: Window management, rendering, and input handling
- **Dear ImGui**: Immediate-mode GUI for the file list and controls
- **stb_image**: JPEG decoding (thread-safe, used for embedded previews)

Third-party code is vendored in `third_party/`:
- `imgui/`: Dear ImGui library
- `stb/`: STB single-header libraries

## Developer Notes from PLAN.md

The author's development approach:
- Break features into sequences of steps where each step leaves working code
- Prompts often contain technical details to guide implementation direction
- Sometimes multiple steps are combined in one prompt
- Dead ends are kept in branches, then main is reset to last good point
- Coding conventions are mostly LLM-decided, but exceptions are avoided
- Watch for duplicate code - LLM may forget existing APIs or not refactor common code unless asked
