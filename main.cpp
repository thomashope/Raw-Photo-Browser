#include <iostream>
#include <filesystem>
#include <vector>
#include <string>
#include <chrono>
#include <libraw/libraw.h>
#include <SDL3/SDL.h>

namespace fs = std::filesystem;

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

    std::cout << "Image decoded: " << image->width << "x" << image->height
              << " (" << image->colors << " colors, " << image->bits << " bits)" << std::endl;

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

    // Create texture from image data
    SDL_Texture* texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_RGB24,
        SDL_TEXTUREACCESS_STATIC,
        image->width,
        image->height
    );

    if (!texture) {
        std::cerr << "SDL_CreateTexture failed: " << SDL_GetError() << std::endl;
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        LibRaw::dcraw_clear_mem(image);
        return 1;
    }

    // Upload image data to texture
    SDL_UpdateTexture(texture, nullptr, image->data, image->width * 3);

    // Store image dimensions for aspect ratio calculation
    const float imageWidth = static_cast<float>(image->width);
    const float imageHeight = static_cast<float>(image->height);
    const float imageAspect = imageWidth / imageHeight;

    // Free LibRaw memory
    LibRaw::dcraw_clear_mem(image);

    // Main event loop
    bool running = true;
    SDL_Event event;

    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN) {
                if (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_Q) {
                    running = false;
                }
            }
        }

        // Get current window size
        int windowWidth, windowHeight;
        SDL_GetWindowSize(window, &windowWidth, &windowHeight);

        // Calculate destination rectangle to maintain aspect ratio
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

        // Clear and render
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_RenderTexture(renderer, texture, nullptr, &destRect);
        SDL_RenderPresent(renderer);
    }

    // Cleanup
    SDL_DestroyTexture(texture);
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
