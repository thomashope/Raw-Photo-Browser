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

    // Zoom and pan state
    float zoom = 1.0f;
    float panX = 0.0f;
    float panY = 0.0f;
    bool isPanning = false;
    float lastMouseX = 0.0f;
    float lastMouseY = 0.0f;
    bool showPreview = false;
    float sidebarWidth = 250.0f;  // Current sidebar width
    float currentImageAspect = 1.0f;  // Aspect ratio of current image
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

// Convert pixel coordinates to UV coordinates (0-1 range in the image)
// Takes into account viewport size, zoom, and pan
struct Vec2 { float x, y; };
Vec2 pixelToUv(float pixelX, float pixelY, float viewportWidth, float viewportHeight,
               float imageAspect, float zoom, float panX, float panY) {
    // Calculate the base fit rect (at zoom 1.0, pan 0)
    SDL_FRect fitRect = calculateFitRect(viewportWidth, viewportHeight, imageAspect);

    // Apply zoom to get actual image dimensions
    float imageWidth = fitRect.w * zoom;
    float imageHeight = fitRect.h * zoom;

    // Calculate image top-left position with pan
    float imageX = (viewportWidth - imageWidth) / 2.0f + panX;
    float imageY = (viewportHeight - imageHeight) / 2.0f + panY;

    // Convert pixel to UV (0-1 range)
    Vec2 uv;
    uv.x = (pixelX - imageX) / imageWidth;
    uv.y = (pixelY - imageY) / imageHeight;
    return uv;
}

// Convert UV coordinates (0-1 range in the image) to pixel coordinates
// Takes into account viewport size, zoom, and pan
Vec2 uvToPixel(float uvX, float uvY, float viewportWidth, float viewportHeight,
               float imageAspect, float zoom, float panX, float panY) {
    // Calculate the base fit rect (at zoom 1.0, pan 0)
    SDL_FRect fitRect = calculateFitRect(viewportWidth, viewportHeight, imageAspect);

    // Apply zoom to get actual image dimensions
    float imageWidth = fitRect.w * zoom;
    float imageHeight = fitRect.h * zoom;

    // Calculate image top-left position with pan
    float imageX = (viewportWidth - imageWidth) / 2.0f + panX;
    float imageY = (viewportHeight - imageHeight) / 2.0f + panY;

    // Convert UV to pixel
    Vec2 pixel;
    pixel.x = imageX + uvX * imageWidth;
    pixel.y = imageY + uvY * imageHeight;
    return pixel;
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
        // Get window size early for event processing
        int windowWidth, windowHeight;
        SDL_GetWindowSize(window, &windowWidth, &windowHeight);
        float viewportWidth = static_cast<float>(windowWidth) - app.sidebarWidth;
        float viewportHeight = static_cast<float>(windowHeight) - 40.0f; // Reserve for controls

        // Start the Dear ImGui frame
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        static bool showImGuiDemoWindow = false;

        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);

            // Only process these events if ImGui didn't capture them
            bool imgui_wants_mouse = ImGui::GetIO().WantCaptureMouse;

            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN) {
                if (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_Q) {
                    running = false;
                } else if (event.key.key == SDLK_F12) {
                    showImGuiDemoWindow = !showImGuiDemoWindow;
                }
            } else if (!imgui_wants_mouse && event.type == SDL_EVENT_MOUSE_WHEEL) {
                // Zoom with scroll wheel, centered on mouse position
                float mouseX, mouseY;
                SDL_GetMouseState(&mouseX, &mouseY);

                // Convert mouse to viewport coordinates (relative to image area)
                float mouseViewportX = mouseX - app.sidebarWidth;
                float mouseViewportY = mouseY;

                // Get UV coordinates at mouse position with old zoom
                Vec2 uv = pixelToUv(mouseViewportX, mouseViewportY, viewportWidth, viewportHeight,
                                   app.currentImageAspect, app.zoom, app.panX, app.panY);

                // Apply zoom
                float oldZoom = app.zoom;
                float zoomFactor = 1.1f;

                if (event.wheel.y > 0) {
                    app.zoom *= zoomFactor;
                } else if (event.wheel.y < 0) {
                    app.zoom /= zoomFactor;
                }

                // Clamp zoom
                app.zoom = std::max(0.1f, std::min(app.zoom, 10.0f));

                if (app.zoom != oldZoom) {
                    // Get pixel position where that UV coordinate would be with new zoom
                    Vec2 newPixel = uvToPixel(uv.x, uv.y, viewportWidth, viewportHeight,
                                             app.currentImageAspect, app.zoom, app.panX, app.panY);

                    // Adjust pan by the difference to keep mouse over same UV point
                    app.panX += mouseViewportX - newPixel.x;
                    app.panY += mouseViewportY - newPixel.y;
                }
            } else if (!imgui_wants_mouse && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (event.button.button == SDL_BUTTON_LEFT) {
                    app.isPanning = true;
                    app.lastMouseX = event.button.x;
                    app.lastMouseY = event.button.y;
                }
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                if (event.button.button == SDL_BUTTON_LEFT) {
                    app.isPanning = false;
                }
            } else if (!imgui_wants_mouse && event.type == SDL_EVENT_MOUSE_MOTION) {
                if (app.isPanning) {
                    app.panX += event.motion.x - app.lastMouseX;
                    app.panY += event.motion.y - app.lastMouseY;
                    app.lastMouseX = event.motion.x;
                    app.lastMouseY = event.motion.y;
                }
            }
        }

        if (showImGuiDemoWindow)
            ImGui::ShowDemoWindow(&showImGuiDemoWindow);

        // Setup sidebar window as left panel
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(std::min(windowWidth * 0.2f, 250.0f), 0.0f), ImGuiCond_Once);
        ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, windowHeight), ImVec2(windowWidth * 0.9f, windowHeight));
        ImGui::Begin("##Sidebar", nullptr,
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse);

        app.sidebarWidth = ImGui::GetWindowWidth();

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
                // Reset zoom and pan when changing images
                app.zoom = 1.0f;
                app.panX = 0.0f;
                app.panY = 0.0f;
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

        // Determine what to display based on loading state and showPreview checkbox
        GpuTexture* imageToDisplay = nullptr;
        const char* loadingText = nullptr;

        if (!app.showPreview && currentRaw && currentRaw->texture) {
            imageToDisplay = currentRaw;
        } else if (currentPreview && currentPreview->texture) {
            // Preview is ready but not raw, show preview with loading text
            imageToDisplay = currentPreview;
            if(!app.showPreview)
            {
                loadingText = "Loading full image...";
            }
        } else {
            // Nothing ready yet
            loadingText = "Loading preview...";
        }

        // Render the image if available
        if (imageToDisplay) {
            // Get display dimensions (accounting for rotation)
            app.currentImageAspect = static_cast<float>(imageToDisplay->getWidth()) /
                                     static_cast<float>(imageToDisplay->getHeight());

            // Calculate available space for image (excluding sidebar and controls bar)
            int availableWidth = windowWidth - static_cast<int>(app.sidebarWidth);
            int availableHeight = windowHeight - 40;  // Reserve 40px for controls

            // Calculate destination rectangle to maintain aspect ratio in available space
            SDL_FRect destRect = calculateFitRect(availableWidth, availableHeight, app.currentImageAspect);

            // Apply zoom
            float zoomedWidth = destRect.w * app.zoom;
            float zoomedHeight = destRect.h * app.zoom;

            // Center the zoomed image and apply pan
            destRect.x = app.sidebarWidth + (availableWidth - zoomedWidth) / 2.0f + app.panX;
            destRect.y = (availableHeight - zoomedHeight) / 2.0f + app.panY;
            destRect.w = zoomedWidth;
            destRect.h = zoomedHeight;

            imageToDisplay->render(renderer, &destRect);
        }

        // Display loading text if needed
        if (loadingText) {
            ImGui::SetNextWindowPos(ImVec2(app.sidebarWidth + 10, 10));
            ImGui::Begin("##LoadingStatus", nullptr,
                ImGuiWindowFlags_NoTitleBar |
                ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoBackground);
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%s", loadingText);
            ImGui::End();
        }

        // Controls window at bottom
        ImGui::SetNextWindowPos(ImVec2(app.sidebarWidth, static_cast<float>(windowHeight) - 40));
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(windowWidth) - app.sidebarWidth, 40));
        ImGui::Begin("##Controls", nullptr,
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse);

        ImGui::Checkbox("Show Preview", &app.showPreview);
        ImGui::SameLine();
        if (ImGui::Button("Reset Zoom")) {
            app.zoom = 1.0f;
            app.panX = 0.0f;
            app.panY = 0.0f;
        }
        ImGui::SameLine();
        ImGui::Text("Zoom: %.1fx", app.zoom);

        ImGui::End();

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
