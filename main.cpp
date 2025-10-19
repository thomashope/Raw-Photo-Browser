#include <iostream>
#include <filesystem>
#include <vector>
#include <string>
#include <chrono>
#include <libraw/libraw.h>
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

namespace fs = std::filesystem;

struct CpuTexture {
    SDL_Surface* surface;

    CpuTexture() : surface(nullptr) {}
    explicit CpuTexture(SDL_Surface* surf) : surface(surf) {}

    // Delete copy and move operators
    CpuTexture(const CpuTexture&) = delete;
    CpuTexture& operator=(const CpuTexture&) = delete;
    CpuTexture(CpuTexture&&) = delete;
    CpuTexture& operator=(CpuTexture&&) = delete;

    ~CpuTexture() {
        if (surface) {
            SDL_DestroySurface(surface);
        }
    }
};

struct GpuTexture {
    SDL_Texture* texture;
    int originalWidth;
    int originalHeight;
    int orientation; // LibRaw flip value: 0, 3, 5, 6

    GpuTexture() : texture(nullptr), originalWidth(0), originalHeight(0), orientation(0) {}
    GpuTexture(SDL_Texture* tex, int width, int height, int orientation = 0) : texture(tex), originalWidth(width), originalHeight(height), orientation(orientation) {}
    
    // Constructor from CpuTexture
    GpuTexture(SDL_Renderer* renderer, const CpuTexture& cpuTex, int orient = 0) 
        : texture(nullptr), originalWidth(0), originalHeight(0), orientation(orient) {
        if (cpuTex.surface) {
            texture = SDL_CreateTextureFromSurface(renderer, cpuTex.surface);
            if (texture) {
                originalWidth = cpuTex.surface->w;
                originalHeight = cpuTex.surface->h;
            }
        }
    }

    // Delete copy and move operators
    GpuTexture(const GpuTexture&) = delete;
    void operator = (const GpuTexture&) = delete;
    GpuTexture(GpuTexture&&) = delete;
    void operator = (GpuTexture&&) = delete;

    ~GpuTexture()
    {
        if(texture)
            SDL_DestroyTexture(texture);
    }

    // Get the display dimensions (accounting for 90° rotations)
    int getWidth() const {
        return (orientation == 5 || orientation == 6) ? originalHeight : originalWidth;
    }

    int getHeight() const {
        return (orientation == 5 || orientation == 6) ? originalWidth : originalHeight;
    }

    // Get rotation angle in degrees for SDL_RenderTextureRotated
    double getRotationDegrees() const {
        switch (orientation) {
            case 3: return 180.0;
            case 5: return 270.0; // 90° CCW = 270° CW
            case 6: return 90.0;
            default: return 0.0;
        }
    }

    // Render the texture with proper orientation
    void render(SDL_Renderer* renderer, const SDL_FRect* destRect) const {
        if (!texture) return;

        if (orientation == 0) {
            // No rotation needed
            SDL_RenderTexture(renderer, texture, nullptr, destRect);
        } else if (orientation == 3) {
            // 180° rotation - dimensions stay the same
            SDL_RenderTextureRotated(renderer, texture, nullptr, destRect, 180.0, nullptr, SDL_FLIP_NONE);
        } else {
            // 90° or 270° rotation - need to swap width/height for the render call
            // The destRect is sized for the rotated output, but SDL expects pre-rotation dimensions
            SDL_FRect adjustedRect;
            adjustedRect.w = destRect->h;  // Swap dimensions
            adjustedRect.h = destRect->w;
            adjustedRect.x = destRect->x + (destRect->w - destRect->h) / 2.0f;
            adjustedRect.y = destRect->y + (destRect->h - destRect->w) / 2.0f;

            SDL_RenderTextureRotated(renderer, texture, nullptr, &adjustedRect, getRotationDegrees(), nullptr, SDL_FLIP_NONE);
        }
    }
};

// Function to display a single raw image
int displayImage(const std::string& imagePath) {
    std::error_code ec;

    // Initialize LibRaw
    LibRaw rawProcessor;

    std::cout << "Loading raw image: " << imagePath << std::endl;

    // Open and decode the raw file
    int ret = rawProcessor.open_file(imagePath.c_str());
    if (ret != LIBRAW_SUCCESS) {
        std::cerr << "Error opening file: " << libraw_strerror(ret) << std::endl;
        return 1;
    }

    // Unpack the raw data
    ret = rawProcessor.unpack();
    if (ret != LIBRAW_SUCCESS) {
        std::cerr << "Error unpacking raw data: " << libraw_strerror(ret) << std::endl;
        return 1;
    }

    // LibRaw's flip value corresponds to EXIF orientation
    // 0 = no rotation, 3 = 180°, 5 = 90° CCW + horizontal flip, 6 = 90° CW
    int orientation = rawProcessor.imgdata.sizes.flip;
    std::cout << "EXIF orientation (flip value): " << orientation << std::endl;

    // Try to extract embedded JPEG preview
    CpuTexture previewSurface;
    ret = rawProcessor.unpack_thumb();
    if (ret == LIBRAW_SUCCESS) {
        libraw_processed_image_t* thumb = rawProcessor.dcraw_make_mem_thumb(&ret);
        if (thumb && thumb->type == LIBRAW_IMAGE_JPEG) {
            std::cout << "Found JPEG preview: " << thumb->width << "x" << thumb->height << std::endl;

            // Decode JPEG using SDL_image
            SDL_IOStream* rw = SDL_IOFromConstMem(thumb->data, thumb->data_size);
            if (rw) {
                previewSurface.surface = IMG_Load_IO(rw, true);
                if (!previewSurface.surface) {
                    std::cerr << "Warning: Failed to decode JPEG preview: " << SDL_GetError() << std::endl;
                }
            }
            LibRaw::dcraw_clear_mem(thumb);
        } else {
            std::cout << "No JPEG preview found in raw file" << std::endl;
        }
    }

    // Configure processing parameters for better color accuracy
    rawProcessor.imgdata.params.use_camera_wb = 1;      // Use camera white balance
    rawProcessor.imgdata.params.output_color = 1;       // sRGB color space
    rawProcessor.imgdata.params.gamm[0] = 1.0/2.4;      // sRGB gamma curve
    rawProcessor.imgdata.params.gamm[1] = 12.92;        // sRGB gamma slope
    rawProcessor.imgdata.params.user_qual = 3;          // AHD demosaicing (high quality)
    rawProcessor.imgdata.params.no_auto_bright = 0;     // Enable auto brightness

    // Process the image (demosaic, white balance, etc.)
    ret = rawProcessor.dcraw_process();
    if (ret != LIBRAW_SUCCESS) {
        std::cerr << "Error processing raw data: " << libraw_strerror(ret) << std::endl;
        return 1;
    }

    // Get processed image
    libraw_processed_image_t* image = rawProcessor.dcraw_make_mem_image(&ret);
    if (!image) {
        std::cerr << "Error creating memory image: " << libraw_strerror(ret) << std::endl;
        return 1;
    }

    std::cout << "Raw image decoded: " << image->width << "x" << image->height << " (" << image->colors << " colors, " << image->bits << " bits)" << std::endl;

    // Initialize SDL
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << std::endl;
        LibRaw::dcraw_clear_mem(image);
        return 1;
    }

    // Create resizable window with initial fixed size
    const int initialWidth = 1280;
    const int initialHeight = 800;

    SDL_Window* window = SDL_CreateWindow(
        imagePath.c_str(),
        initialWidth,
        initialHeight,
        SDL_WINDOW_RESIZABLE
    );

    if (!window) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << std::endl;
        SDL_Quit();
        LibRaw::dcraw_clear_mem(image);
        return 1;
    }

    // Create renderer
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        std::cerr << "SDL_CreateRenderer failed: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        LibRaw::dcraw_clear_mem(image);
        return 1;
    }

    // Create GpuTexture for raw image data
    GpuTexture rawImage;
    rawImage.texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_RGB24,
        SDL_TEXTUREACCESS_STATIC,
        image->width,
        image->height
    );

    if (!rawImage.texture) {
        std::cerr << "SDL_CreateTexture failed: " << SDL_GetError() << std::endl;
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        LibRaw::dcraw_clear_mem(image);
        return 1;
    }

    // Upload raw image data to texture
    SDL_UpdateTexture(rawImage.texture, nullptr, image->data, image->width * 3);

    // Store raw image info
    rawImage.originalWidth = image->width;
    rawImage.originalHeight = image->height;

    // Free LibRaw memory
    LibRaw::dcraw_clear_mem(image);

    // Create GpuTexture from JPEG preview if available
    GpuTexture previewImage(renderer, previewSurface, orientation);

    // Main event loop
    bool running = true;
    bool showPreview = false;  // Start with raw image (press 1 for preview, 2 for raw)
    SDL_Event event;

    std::cout << "\nControls:" << std::endl;
    std::cout << "  1 - Show JPEG preview" << std::endl;
    std::cout << "  2 - Show raw image" << std::endl;
    std::cout << "  ESC/Q - Quit" << std::endl;

    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN) {
                if (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_Q) {
                    running = false;
                } else if (event.key.key == SDLK_1) {
                    if (previewImage.texture) {
                        showPreview = true;
                        std::cout << "Switched to JPEG preview" << std::endl;
                    } else {
                        std::cout << "No JPEG preview available" << std::endl;
                    }
                } else if (event.key.key == SDLK_2) {
                    showPreview = false;
                    std::cout << "Switched to raw image" << std::endl;
                }
            }
        }

        // Select current image
        const GpuTexture& currentImage = showPreview ? previewImage : rawImage;

        // Get display dimensions (accounting for rotation)
        float currentAspect = static_cast<float>(currentImage.getWidth()) /
                             static_cast<float>(currentImage.getHeight());

        // Get current window size
        int windowWidth, windowHeight;
        SDL_GetWindowSize(window, &windowWidth, &windowHeight);

        // Calculate destination rectangle to maintain aspect ratio
        float windowAspect = static_cast<float>(windowWidth) / static_cast<float>(windowHeight);
        SDL_FRect destRect;

        if (windowAspect > currentAspect) {
            // Window is wider than image - fit to height
            destRect.h = static_cast<float>(windowHeight);
            destRect.w = destRect.h * currentAspect;
            destRect.x = (windowWidth - destRect.w) / 2.0f;
            destRect.y = 0.0f;
        } else {
            // Window is taller than image - fit to width
            destRect.w = static_cast<float>(windowWidth);
            destRect.h = destRect.w / currentAspect;
            destRect.x = 0.0f;
            destRect.y = (windowHeight - destRect.h) / 2.0f;
        }

        // Clear and render
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        currentImage.render(renderer, &destRect);
        SDL_RenderPresent(renderer);
    }

    // Cleanup
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}

// Function to iterate directory and build file list
int browseDirectory(const std::string& folderPath) {
    std::error_code ec;

    // Build list of all children recursively
    std::vector<fs::path> children;

    // Start timer
    auto start = std::chrono::high_resolution_clock::now();

    for (const auto& entry : fs::recursive_directory_iterator(folderPath, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) {
            std::cerr << "Warning: Error accessing some entries: " << ec.message() << std::endl;
            ec.clear();
            continue;
        }
        children.push_back(entry.path());
    }

    // Stop timer
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    if (ec) {
        std::cerr << "Error during directory iteration: " << ec.message() << std::endl;
        return 1;
    }

    // Display the results
    std::cout << "Contents of: " << folderPath << std::endl;
    std::cout << "Found " << children.size() << " item(s) recursively in " << duration.count() << " ms" << std::endl;
    std::cout << std::string(80, '-') << std::endl;

#if 0
    for (const auto& child : children) {
        std::string type = fs::is_directory(child, ec) ? "[DIR] " : "[FILE]";
        if (ec) {
            type = "[???] ";
            ec.clear();
        }
        std::cout << type << " " << child.string() << std::endl;
    }
#endif

    return 0;
}

int main(int argc, char* argv[]) {
    // Check if path argument is provided
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <path>" << std::endl;
        std::cerr << "  <path> can be a folder (to browse) or a file (to display)" << std::endl;
        return 1;
    }

    std::string path = argv[1];

    // Check if the path exists
    std::error_code ec;

    if (!fs::exists(path, ec)) {
        std::cerr << "Error: Path does not exist: " << path << std::endl;
        if (ec) {
            std::cerr << "Error code: " << ec.message() << std::endl;
        }
        return 1;
    }

    // Determine if path is a file or directory
    if (fs::is_directory(path, ec)) {
        // Browse directory
        return browseDirectory(path);
    } else if (fs::is_regular_file(path, ec)) {
        // Display single image
        return displayImage(path);
    } else {
        std::cerr << "Error: Path is neither a file nor a directory: " << path << std::endl;
        return 1;
    }
}
