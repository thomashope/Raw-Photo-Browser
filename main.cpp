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

    // Main event loop
    bool running = true;
    SDL_Event event;

    std::cout << "\nControls:" << std::endl;
    std::cout << "  Click filename in list to view image" << std::endl;
    std::cout << "  ESC/Q - Quit" << std::endl;

    while (running) {
        // Start the Dear ImGui frame
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        static bool showImGuiDemoWindow = false;

        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN) {
                if (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_Q) {
                    running = false;
                } else if (event.key.key == SDLK_F12) {
                    showImGuiDemoWindow = !showImGuiDemoWindow;
                }
            }
        }

        if (showImGuiDemoWindow)
            ImGui::ShowDemoWindow(&showImGuiDemoWindow);

        // Get window size for sidebar layout
        int windowWidth, windowHeight;
        SDL_GetWindowSize(window, &windowWidth, &windowHeight);

        // Setup sidebar window as left panel
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(std::min(windowWidth * 0.2f, 250.0f), 0.0f), ImGuiCond_Once);
        ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, windowHeight), ImVec2(windowWidth * 0.9f, windowHeight));
        ImGui::Begin("##Sidebar", nullptr,
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse);

        float sidebarWidth = ImGui::GetWindowWidth();

        const float thumbnailHeight = 64.0f;  // Fixed thumbnail height
        const float textHeight = ImGui::GetTextLineHeight();
        const float itemHeight = thumbnailHeight + textHeight + 4.0f;  // Thumbnail + text + padding

        for (size_t i = 0; i < app.images.size(); i++)
        {
            // Get just the filename from the full path
            std::string filename = app.images[i].filename().string();

            ImGui::PushID(static_cast<int>(i));

            // Selectable with thumbnail
            bool is_selected = (i == app.currentImageIndex);

            // Create a selectable region
            if (ImGui::Selectable("##select", is_selected, 0, ImVec2(0, itemHeight)))
            {
                app.currentImageIndex = i;
            }
            
            // Check if this item is visible in the scroll region
            bool isVisible = ImGui::IsItemVisible();
            
            // Only request thumbnail if the item is visible
            GpuTexture* thumbnail = nullptr;
            if (isVisible) {
                thumbnail = app.database->tryGetThumbnail(i, app.images[i].string());
            }
            
            float thumbnailWidth = thumbnailHeight;  // Default to square

            if (thumbnail && thumbnail->texture) {
                // Calculate width based on aspect ratio
                float aspect = static_cast<float>(thumbnail->getWidth()) /
                               static_cast<float>(thumbnail->getHeight());
                thumbnailWidth = thumbnailHeight * aspect;
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

        // Request images for the selected image
        GpuTexture* currentPreview = app.database->tryGetThumbnail(app.currentImageIndex, app.images[app.currentImageIndex].string());
        GpuTexture* currentRaw = app.database->tryGetRaw(app.currentImageIndex, app.images[app.currentImageIndex].string());

        // Clear and render
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        // Determine what to display based on loading state
        GpuTexture* imageToDisplay = nullptr;
        const char* loadingText = nullptr;
        
        if (currentRaw && currentRaw->texture) {
            // Raw image is ready, show it
            imageToDisplay = currentRaw;
        } else if (currentPreview && currentPreview->texture) {
            // Preview is ready but not raw, show preview with loading text
            imageToDisplay = currentPreview;
            loadingText = "Loading full image...";
        } else {
            // Nothing ready yet
            loadingText = "Loading preview...";
        }

        // Render the image if available
        if (imageToDisplay) {
            // Get display dimensions (accounting for rotation)
            float currentAspect = static_cast<float>(imageToDisplay->getWidth()) /
                                 static_cast<float>(imageToDisplay->getHeight());

            // Calculate available space for image (excluding sidebar)
            int availableWidth = windowWidth - static_cast<int>(sidebarWidth);
            int availableHeight = windowHeight;

            // Calculate destination rectangle to maintain aspect ratio in available space
            SDL_FRect destRect = calculateFitRect(availableWidth, availableHeight, currentAspect);

            // Offset by sidebar width
            destRect.x += sidebarWidth;

            imageToDisplay->render(renderer, &destRect);
        }
        
        // Display loading text if needed
        if (loadingText) {
            ImGui::SetNextWindowPos(ImVec2(sidebarWidth + 10, 10));
            ImGui::Begin("##LoadingStatus", nullptr, 
                ImGuiWindowFlags_NoTitleBar | 
                ImGuiWindowFlags_NoResize | 
                ImGuiWindowFlags_NoMove | 
                ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoBackground);
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%s", loadingText);
            ImGui::End();
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
