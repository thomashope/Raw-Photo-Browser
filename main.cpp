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

    // Delete copy operators
    CpuTexture(const CpuTexture&) = delete;
    CpuTexture& operator=(const CpuTexture&) = delete;

    // Move constructor and assignment
    CpuTexture(CpuTexture&& other) noexcept : surface(other.surface) {
        other.surface = nullptr;
    }
    CpuTexture& operator=(CpuTexture&& other) noexcept {
        if (this != &other) {
            if (surface) {
                SDL_DestroySurface(surface);
            }
            surface = other.surface;
            other.surface = nullptr;
        }
        return *this;
    }

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

    // Delete copy operators
    GpuTexture(const GpuTexture&) = delete;
    void operator = (const GpuTexture&) = delete;

    GpuTexture(GpuTexture&& other) noexcept : texture(other.texture), originalWidth(other.originalWidth), originalHeight(other.originalHeight), orientation(other.orientation) {
        other.texture = nullptr;
    };
    GpuTexture& operator = (GpuTexture&& other) noexcept {
        if(this != &other) {
            if(texture)
            {
                SDL_DestroyTexture(texture);
            }
            texture = other.texture;
            originalWidth = other.originalWidth;
            originalHeight = other.originalHeight;
            orientation = other.orientation;
            other.texture = nullptr;
        }
        return *this;
    };

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

struct App
{
    std::vector<fs::path> images;
    size_t currentImageIndex = 0;

    GpuTexture currentRawImage;
    GpuTexture currentPreviewImage;
};

namespace
{
    App app;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
}

bool initializeSDL(int width, int height) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << std::endl;
        return false;
    }

    window = SDL_CreateWindow("Photo Browser", width, height, SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return false;
    }

    renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        std::cerr << "SDL_CreateRenderer failed: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return false;
    }
    return true;
}

// Calculate destination rectangle thats fits within window while maintaining aspect ratio
SDL_FRect calculateFitRect(int windowWidth, int windowHeight, float imageAspect) {
    float windowAspect = static_cast<float>(windowWidth) / static_cast<float>(windowHeight);
    SDL_FRect destRect;

    if (windowAspect > imageAspect) {
        // Window is wider than image - fit to height
        destRect.h = static_cast<float>(windowHeight);
        destRect.w = destRect.h * imageAspect;
        destRect.x = (windowWidth - destRect.w) / 2.0f;
        destRect.y = 0.0f;
    } else {
        // Window is taller than image - fit to width
        destRect.w = static_cast<float>(windowWidth);
        destRect.h = destRect.w / imageAspect;
        destRect.x = 0.0f;
        destRect.y = (windowHeight - destRect.h) / 2.0f;
    }
    return destRect;
}

// Extract JPEG preview from raw file
CpuTexture loadJpegPreview(LibRaw& rawProcessor) {
    CpuTexture previewSurface;
    int ret = rawProcessor.unpack_thumb();
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
    return previewSurface;
}

// Function to display a single raw image
int loadImage(const std::string& imagePath, GpuTexture& outPreview, GpuTexture& outRaw) {
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
    CpuTexture previewSurface = loadJpegPreview(rawProcessor);

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
        LibRaw::dcraw_clear_mem(image);
        return 1;
    }

    // Upload raw image data to texture
    SDL_UpdateTexture(rawImage.texture, nullptr, image->data, image->width * 3);

    // Store raw image info
    rawImage.originalWidth = image->width;
    rawImage.originalHeight = image->height;
    // rawImage.orientation = orientation;

    // Free LibRaw memory
    LibRaw::dcraw_clear_mem(image);

    // Move to output parameters
    outPreview = GpuTexture(renderer, previewSurface, orientation);
    outRaw = std::move(rawImage);
    return 0;
}

// Function to check if a file has a raw image extension
bool isRawFileExtension(const fs::path& filePath) {
    if (!fs::is_regular_file(filePath)) {
        return false;
    }

    std::string ext = filePath.extension().string();

    // Convert to lowercase for case-insensitive comparison
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c){ return std::tolower(c); });

    // Common raw file extensions
    static const std::vector<std::string> rawExtensions = {
        ".nef",  // Nikon
        ".cr2", ".cr3",  // Canon
        ".arw", ".srf", ".sr2",  // Sony
        ".orf",  // Olympus
        ".rw2",  // Panasonic
        ".dng",  // Adobe (universal raw)
        ".raf",  // Fujifilm
        ".pef",  // Pentax
        ".3fr",  // Hasselblad
        ".dcr", ".k25", ".kdc",  // Kodak
        ".mrw",  // Minolta
        ".nrw",  // Nikon (newer)
        ".raw",  // Generic
        ".rwl",  // Leica
        ".srw",  // Samsung
        ".x3f",  // Sigma
        ".iiq",  // Phase One
        ".erf",  // Epson
        ".mef",  // Mamiya
        ".mos",  // Leaf
        ".r3d",  // RED
    };

    return std::find(rawExtensions.begin(), rawExtensions.end(), ext) != rawExtensions.end();
}

int addImagesInDirectory(const std::string& folderPath) {
    std::error_code ec;

    // Start timer
    auto start = std::chrono::high_resolution_clock::now();

    for (const auto& entry : fs::recursive_directory_iterator(folderPath, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) {
            std::cerr << "Warning: Error accessing some entries: " << ec.message() << std::endl;
            ec.clear();
            continue;
        }

        if(isRawFileExtension(entry.path()))
            app.images.push_back(entry.path());
    }

    // Stop timer
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    if (ec) {
        std::cerr << "Error during directory iteration: " << ec.message() << std::endl;
        return 1;
    }

    std::cout << "Contents of: " << folderPath << std::endl;
    std::cout << "Found " << app.images.size() << " item(s) recursively in " << duration.count() << " ms" << std::endl;

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
        addImagesInDirectory(path);
    } else if (fs::is_regular_file(path, ec)) {
        if (isRawFileExtension(path))
            app.images.push_back(path);
    } else {
        std::cerr << "Error: Path is neither a file nor a directory: " << path << std::endl;
        return 1;
    }

    if (app.images.empty())
        return 0;

    const int initialWidth = 1280;
    const int initialHeight = 800;
    if (!initializeSDL(initialWidth, initialHeight)) {
        return 1;
    }

    // Load the first image (now that SDL and renderer are initialized)
    loadImage(app.images[0].string(), app.currentPreviewImage, app.currentRawImage);

    // Main event loop
    bool running = true;
    bool showPreview = false;  // Start with raw image (press 1 for preview, 2 for raw)
    SDL_Event event;

    std::cout << "\nControls:" << std::endl;
    std::cout << "  1 - Show JPEG preview" << std::endl;
    std::cout << "  2 - Show raw image" << std::endl;
    std::cout << "  ESC/Q - Quit" << std::endl;

    while (running) {
        bool reloadImage = false;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN) {
                if (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_Q) {
                    running = false;
                } else if (event.key.key == SDLK_1) {
                    if (app.currentPreviewImage.texture) {
                        showPreview = true;
                        std::cout << "Switched to JPEG preview" << std::endl;
                    } else {
                        std::cout << "No JPEG preview available" << std::endl;
                    }
                } else if (event.key.key == SDLK_2) {
                    showPreview = false;
                    std::cout << "Switched to raw image" << std::endl;
                } else if (event.key.key == SDLK_LEFT) {
                    if(app.currentImageIndex > 0)
                    {
                        app.currentImageIndex--;
                        reloadImage = true;
                    }
                } else if (event.key.key == SDLK_RIGHT) {
                    if(app.currentImageIndex + 1 < app.images.size())
                    {
                        app.currentImageIndex++;
                        reloadImage = true;
                    }
                }
            }
        }

        if(reloadImage)
        {
            loadImage(app.images[app.currentImageIndex], app.currentPreviewImage, app.currentRawImage);
        }

        // Select current image
        const GpuTexture& currentImage = showPreview ? app.currentPreviewImage : app.currentRawImage;

        // Get display dimensions (accounting for rotation)
        float currentAspect = static_cast<float>(currentImage.getWidth()) /
                             static_cast<float>(currentImage.getHeight());

        // Get current window size
        int windowWidth, windowHeight;
        SDL_GetWindowSize(window, &windowWidth, &windowHeight);

        // Calculate destination rectangle to maintain aspect ratio
        SDL_FRect destRect = calculateFitRect(windowWidth, windowHeight, currentAspect);

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
