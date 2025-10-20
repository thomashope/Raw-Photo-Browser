#include <iostream>
#include <filesystem>
#include <vector>
#include <string>
#include <chrono>
#include <libraw/libraw.h>
#include <SDL3/SDL.h>

#define STBI_NO_FAILURE_STRINGS  // Thread-safe: disables global error string
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

namespace fs = std::filesystem;

struct CpuTexture {
    unsigned char* pixels;
    int width;
    int height;
    int channels;

    CpuTexture() : pixels(nullptr), width(0), height(0), channels(0) {}
    CpuTexture(unsigned char* pix, int w, int h, int ch)
        : pixels(pix), width(w), height(h), channels(ch) {}

    // Delete copy operators
    CpuTexture(const CpuTexture&) = delete;
    CpuTexture& operator=(const CpuTexture&) = delete;

    // Move constructor and assignment
    CpuTexture(CpuTexture&& other) noexcept
        : pixels(other.pixels), width(other.width), height(other.height), channels(other.channels) {
        other.pixels = nullptr;
        other.width = 0;
        other.height = 0;
        other.channels = 0;
    }
    CpuTexture& operator=(CpuTexture&& other) noexcept {
        if (this != &other) {
            if (pixels) {
                stbi_image_free(pixels);
            }
            pixels = other.pixels;
            width = other.width;
            height = other.height;
            channels = other.channels;
            other.pixels = nullptr;
            other.width = 0;
            other.height = 0;
            other.channels = 0;
        }
        return *this;
    }

    ~CpuTexture() {
        if (pixels) {
            stbi_image_free(pixels);
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
        if (cpuTex.pixels && cpuTex.width > 0 && cpuTex.height > 0) {
            // Determine pixel format based on number of channels
            SDL_PixelFormat format;
            int pitch;
            if (cpuTex.channels == 3) {
                format = SDL_PIXELFORMAT_RGB24;
                pitch = cpuTex.width * 3;
            } else if (cpuTex.channels == 4) {
                format = SDL_PIXELFORMAT_RGBA32;
                pitch = cpuTex.width * 4;
            } else {
                std::cerr << "Unsupported channel count: " << cpuTex.channels << std::endl;
                return;
            }

            // Create texture
            texture = SDL_CreateTexture(
                renderer,
                format,
                SDL_TEXTUREACCESS_STATIC,
                cpuTex.width,
                cpuTex.height
            );

            if (texture) {
                // Upload pixel data to GPU
                SDL_UpdateTexture(texture, nullptr, cpuTex.pixels, pitch);
                originalWidth = cpuTex.width;
                originalHeight = cpuTex.height;
            }
        }
    }

    // Constructor from LibRaw processed image
    GpuTexture(SDL_Renderer* renderer, libraw_processed_image_t* image, int orient = 0)
        : texture(nullptr), originalWidth(0), originalHeight(0), orientation(orient) {
        if (image && image->type == LIBRAW_IMAGE_BITMAP) {
            // LibRaw bitmap is RGB24 format
            texture = SDL_CreateTexture(
                renderer,
                SDL_PIXELFORMAT_RGB24,
                SDL_TEXTUREACCESS_STATIC,
                image->width,
                image->height
            );

            if (texture) {
                // Upload pixel data to GPU
                SDL_UpdateTexture(texture, nullptr, image->data, image->width * 3);
                originalWidth = image->width;
                originalHeight = image->height;
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

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;   // Enable Gamepad Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;      // Enable Docking

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();

    // Setup Platform/Renderer backends
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

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
    CpuTexture previewTexture;
    int ret = rawProcessor.unpack_thumb();
    if (ret == LIBRAW_SUCCESS) {
        libraw_processed_image_t* thumb = rawProcessor.dcraw_make_mem_thumb(&ret);
        if (thumb && thumb->type == LIBRAW_IMAGE_JPEG) {
            std::cout << "Found JPEG preview: " << thumb->width << "x" << thumb->height << std::endl;

            // Decode JPEG using stb_image (thread-safe)
            int width, height, channels;
            unsigned char* pixels = stbi_load_from_memory(
                thumb->data,
                thumb->data_size,
                &width,
                &height,
                &channels,
                3  // Force RGB output (3 channels)
            );

            if (pixels) {
                previewTexture = CpuTexture(pixels, width, height, 3);
            } else {
                std::cerr << "Warning: Failed to decode JPEG preview with stb_image" << std::endl;
            }

            LibRaw::dcraw_clear_mem(thumb);
        } else {
            std::cout << "No JPEG preview found in raw file" << std::endl;
        }
    }
    return previewTexture;
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

    // Create GpuTexture for raw and preview images
    outPreview = GpuTexture(renderer, previewSurface, orientation);
    outRaw = GpuTexture(renderer, image);

    // Free LibRaw memory
    LibRaw::dcraw_clear_mem(image);

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
    std::cout << "  Click filename in list to view image" << std::endl;
    std::cout << "  ESC/Q - Quit" << std::endl;

    while (running) {
        // Start the Dear ImGui frame
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        bool reloadImage = false;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
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
                }
            }
        }

        // Show ImGui demo window
        static bool show_demo_window = true;
        if (show_demo_window)
            ImGui::ShowDemoWindow(&show_demo_window);

        // Show file list window
        ImGui::Begin("Image Files");
        for (size_t i = 0; i < app.images.size(); i++)
        {
            // Get just the filename from the full path
            std::string filename = app.images[i].filename().string();

            // Selectable returns true when clicked
            bool is_selected = (i == app.currentImageIndex);
            if (ImGui::Selectable(filename.c_str(), is_selected))
            {
                if (app.currentImageIndex != i)
                {
                    app.currentImageIndex = i;
                    reloadImage = true;
                }
            }
        }
        ImGui::End();

        // Reload image if needed (after processing UI)
        if(reloadImage)
        {
            loadImage(app.images[app.currentImageIndex].string(), app.currentPreviewImage, app.currentRawImage);
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

        // Render ImGui
        ImGui::Render();
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);

        SDL_RenderPresent(renderer);
    }

    // Cleanup
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
