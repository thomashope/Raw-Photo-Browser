#include <iostream>
#include <filesystem>
#include <vector>
#include <string>
#include <chrono>
#include <libraw/libraw.h>
#include <SDL3/SDL.h>

#define STB_IMAGE_IMPLEMENTATION
#include "texture_types.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "image_database.h"

namespace fs = std::filesystem;

struct App
{
    std::vector<fs::path> images;
    size_t currentImageIndex = 0;
    size_t requestedImageIndex = 0;
    
    ImageDatabase* database = nullptr;  // Will be initialized after renderer is created
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

    // Initialize database and start worker threads
    app.database = new ImageDatabase(renderer);
    app.database->start();
    
    // Request thumbnails for all images to load in background
    app.database->requestAllThumbnails(app.images);

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

        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN) {
                if (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_Q) {
                    running = false;
                } else if (event.key.key == SDLK_1) {
                    showPreview = true;
                    std::cout << "Switched to JPEG preview" << std::endl;
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
        
        const float thumbnailHeight = 64.0f;  // Fixed thumbnail height
        const float textHeight = ImGui::GetTextLineHeight();
        const float itemHeight = thumbnailHeight + textHeight + 4.0f;  // Thumbnail + text + padding
        
        for (size_t i = 0; i < app.images.size(); i++)
        {
            // Get just the filename from the full path
            std::string filename = app.images[i].filename().string();

            // Try to get thumbnail for this specific item
            GpuTexture* thumbnail = app.database->tryGetThumbnail(i, app.images[i].string());
            float thumbnailWidth = thumbnailHeight;  // Default to square
            
            if (thumbnail && thumbnail->texture) {
                // Calculate width based on aspect ratio
                float aspect = static_cast<float>(thumbnail->getWidth()) / 
                               static_cast<float>(thumbnail->getHeight());
                thumbnailWidth = thumbnailHeight * aspect;
            }
            
            ImGui::PushID(static_cast<int>(i));
            
            // Selectable with thumbnail
            bool is_selected = (i == app.currentImageIndex);
            
            // Create a selectable region
            if (ImGui::Selectable("##select", is_selected, 0, ImVec2(0, itemHeight)))
            {
                if (app.requestedImageIndex != i)
                {
                    app.requestedImageIndex = i;
                }
            }
            
            // Draw thumbnail and text on top of the selectable
            ImVec2 selectableMin = ImGui::GetItemRectMin();
            
            if (thumbnail && thumbnail->texture) {
                ImVec2 thumbnailMin = selectableMin;
                ImVec2 thumbnailMax = ImVec2(selectableMin.x + thumbnailWidth, selectableMin.y + thumbnailHeight);
                
                ImGui::GetWindowDrawList()->AddImage(
                    (ImTextureID)(intptr_t)thumbnail->texture,
                    thumbnailMin,
                    thumbnailMax
                );
                
                // Draw filename below thumbnail
                ImVec2 textPos = ImVec2(selectableMin.x, selectableMin.y + thumbnailHeight + 2.0f);
                ImGui::GetWindowDrawList()->AddText(textPos, ImGui::GetColorU32(ImGuiCol_Text), filename.c_str());
            } else {
                // No thumbnail, just draw text
                ImVec2 textPos = ImVec2(selectableMin.x + 8.0f, selectableMin.y + (itemHeight - textHeight) * 0.5f);
                ImGui::GetWindowDrawList()->AddText(textPos, ImGui::GetColorU32(ImGuiCol_Text), filename.c_str());
            }
            
            ImGui::PopID();
        }
        ImGui::End();

        // Update database - processes completed loads on main thread
        app.database->update();
        
        // Request full raw image for the selected image only (thumbnails are loaded in background)
        app.database->tryGetRaw(app.requestedImageIndex, app.images[app.requestedImageIndex].string());
        
        // Update currentImageIndex only when both preview and raw are loaded
        if (app.database->isFullyLoaded(app.requestedImageIndex)) {
            app.currentImageIndex = app.requestedImageIndex;
        }
        
        // Get current loaded image to display
        GpuTexture* currentPreview = app.database->tryGetThumbnail(app.currentImageIndex, app.images[app.currentImageIndex].string());
        GpuTexture* currentRaw = app.database->tryGetRaw(app.currentImageIndex, app.images[app.currentImageIndex].string());
        
        // Clear and render
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        
        // Render the image if available
        GpuTexture* currentImage = showPreview ? currentPreview : currentRaw;
        if (currentImage && currentImage->texture) {
            // Get display dimensions (accounting for rotation)
            float currentAspect = static_cast<float>(currentImage->getWidth()) /
                                 static_cast<float>(currentImage->getHeight());

            // Get current window size
            int windowWidth, windowHeight;
            SDL_GetWindowSize(window, &windowWidth, &windowHeight);

            // Calculate destination rectangle to maintain aspect ratio
            SDL_FRect destRect = calculateFitRect(windowWidth, windowHeight, currentAspect);

            currentImage->render(renderer, &destRect);
        }

        // Render ImGui
        ImGui::Render();
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);

        SDL_RenderPresent(renderer);
    }

    // Cleanup
    delete app.database;  // Stops worker thread and frees resources
    
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
