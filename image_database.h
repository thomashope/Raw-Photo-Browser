#pragma once

#include <string>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <memory>
#include <filesystem>
#include <libraw/libraw.h>
#include <SDL3/SDL.h>
#include "concurrent_queue.h"
#include "texture_types.h"

namespace fs = std::filesystem;

// Forward declaration from main.cpp
CpuTexture loadJpegPreview(LibRaw& rawProcessor);

// Task to load an image
struct LoadTask {
    size_t imageIndex;
    std::string imagePath;
};

// Result from loading (either preview or raw)
enum class ImageType {
    Preview,
    Raw
};

struct LoadResult {
    size_t imageIndex;
    ImageType type;
    CpuTexture cpuTexture;
    libraw_processed_image_t* rawImage;  // Only used for Raw type
    int orientation;

    LoadResult() : imageIndex(0), type(ImageType::Preview), rawImage(nullptr), orientation(0) {}

    ~LoadResult() {
        if (rawImage) {
            LibRaw::dcraw_clear_mem(rawImage);
        }
    }

    // Delete copy
    LoadResult(const LoadResult&) = delete;
    LoadResult& operator=(const LoadResult&) = delete;

    // Allow move
    LoadResult(LoadResult&& other) noexcept
        : imageIndex(other.imageIndex), type(other.type),
          cpuTexture(std::move(other.cpuTexture)), rawImage(other.rawImage),
          orientation(other.orientation) {
        other.rawImage = nullptr;
    }
    LoadResult& operator=(LoadResult&& other) noexcept {
        if (this != &other) {
            if (rawImage) {
                LibRaw::dcraw_clear_mem(rawImage);
            }
            imageIndex = other.imageIndex;
            type = other.type;
            cpuTexture = std::move(other.cpuTexture);
            rawImage = other.rawImage;
            orientation = other.orientation;
            other.rawImage = nullptr;
        }
        return *this;
    }
};

// Entry in the database for a single image
struct ImageEntry {
    GpuTexture preview;
    GpuTexture raw;
    bool previewLoaded = false;
    bool rawLoaded = false;
    bool loadRequested = false;
};

class ImageDatabase {
public:
    ImageDatabase(SDL_Renderer* renderer)
        : renderer_(renderer), running_(false) {}

    ~ImageDatabase() {
        stop();
    }

    // Start the worker thread
    void start() {
        running_ = true;
        workerThread_ = std::thread(&ImageDatabase::workerThreadFunc, this);
    }

    // Stop the worker thread
    void stop() {
        running_ = false;
        if (workerThread_.joinable()) {
            workerThread_.join();
        }
    }

    // Try to get thumbnail for an image
    // Returns nullptr if not loaded yet, and queues a load task
    GpuTexture* tryGetThumbnail(size_t imageIndex, const std::string& imagePath) {
        auto it = entries_.find(imageIndex);
        if (it != entries_.end() && it->second.previewLoaded) {
            return &it->second.preview;
        }

        // Not loaded, queue a task if not already requested
        if (it == entries_.end() || !it->second.loadRequested) {
            if (it == entries_.end()) {
                entries_[imageIndex] = ImageEntry();
            }
            entries_[imageIndex].loadRequested = true;

            LoadTask task;
            task.imageIndex = imageIndex;
            task.imagePath = imagePath;
            taskQueue_.push(std::move(task));
        }

        return nullptr;
    }

    // Try to get raw image
    // Returns nullptr if not loaded yet, and queues a load task if needed
    GpuTexture* tryGetRaw(size_t imageIndex, const std::string& imagePath) {
        auto it = entries_.find(imageIndex);
        if (it != entries_.end() && it->second.rawLoaded) {
            return &it->second.raw;
        }

        // Not loaded, queue a task if not already requested
        if (it == entries_.end() || !it->second.loadRequested) {
            if (it == entries_.end()) {
                entries_[imageIndex] = ImageEntry();
            }
            entries_[imageIndex].loadRequested = true;

            LoadTask task;
            task.imageIndex = imageIndex;
            task.imagePath = imagePath;
            taskQueue_.push(std::move(task));
        }

        return nullptr;
    }

    // Check if both preview and raw are loaded for an image
    bool isFullyLoaded(size_t imageIndex) {
        auto it = entries_.find(imageIndex);
        return it != entries_.end() && it->second.previewLoaded && it->second.rawLoaded;
    }

    // Update - pull results from queue and create GPU textures
    // Call this from the main thread every frame
    void update() {
        LoadResult result;
        while (resultsQueue_.tryPop(result)) {
            auto& entry = entries_[result.imageIndex];

            if (result.type == ImageType::Preview) {
                entry.preview = GpuTexture(renderer_, result.cpuTexture, result.orientation);
                entry.previewLoaded = true;
            } else {  // ImageType::Raw
                entry.raw = GpuTexture(renderer_, result.rawImage, result.orientation);
                entry.rawLoaded = true;
            }
        }
    }

private:
    SDL_Renderer* renderer_;
    std::unordered_map<size_t, ImageEntry> entries_;
    ConcurrentQueue<LoadTask> taskQueue_;
    ConcurrentQueue<LoadResult> resultsQueue_;
    std::thread workerThread_;
    std::atomic<bool> running_;

    // Worker thread function
    void workerThreadFunc() {
        while (running_) {
            LoadTask task;
            if (taskQueue_.tryPop(task)) {
                loadImage(task);
            } else {
                // No tasks, sleep briefly to avoid busy-waiting
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }

    // Load an image and push results
    void loadImage(const LoadTask& task) {
        // Allocate LibRaw on heap to avoid stack overflow
        auto rawProcessor = std::make_unique<LibRaw>();

        // Open and decode the raw file
        int ret = rawProcessor->open_file(task.imagePath.c_str());
        if (ret != LIBRAW_SUCCESS) {
            std::cerr << "Error opening file: " << libraw_strerror(ret) << std::endl;
            return;
        }

        // Unpack the raw data
        ret = rawProcessor->unpack();
        if (ret != LIBRAW_SUCCESS) {
            std::cerr << "Error unpacking raw data: " << libraw_strerror(ret) << std::endl;
            return;
        }

        // Get orientation
        int orientation = rawProcessor->imgdata.sizes.flip;

        // Load and push preview immediately
        {
            LoadResult previewResult;
            previewResult.imageIndex = task.imageIndex;
            previewResult.type = ImageType::Preview;
            previewResult.cpuTexture = loadJpegPreview(*rawProcessor);
            previewResult.orientation = orientation;
            resultsQueue_.push(std::move(previewResult));
        }

        // Configure processing parameters for better color accuracy
        rawProcessor->imgdata.params.use_camera_wb = 1;
        rawProcessor->imgdata.params.output_color = 1;
        rawProcessor->imgdata.params.gamm[0] = 1.0/2.4;
        rawProcessor->imgdata.params.gamm[1] = 12.92;
        rawProcessor->imgdata.params.user_qual = 3;
        rawProcessor->imgdata.params.no_auto_bright = 0;

        // Process the image
        ret = rawProcessor->dcraw_process();
        if (ret != LIBRAW_SUCCESS) {
            std::cerr << "Error processing raw data: " << libraw_strerror(ret) << std::endl;
            return;
        }

        // Get processed image
        libraw_processed_image_t* image = rawProcessor->dcraw_make_mem_image(&ret);
        if (!image) {
            std::cerr << "Error creating memory image: " << libraw_strerror(ret) << std::endl;
            return;
        }

        // Push raw result
        {
            LoadResult rawResult;
            rawResult.imageIndex = task.imageIndex;
            rawResult.type = ImageType::Raw;
            rawResult.rawImage = image;  // Transfer ownership
            // rawResult.orientation = orientation; orientation only applies to the preview image
            resultsQueue_.push(std::move(rawResult));
        }
    }
};
