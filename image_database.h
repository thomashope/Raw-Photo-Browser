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

// Type of load to perform
enum class LoadType {
    PreviewOnly,  // Only load JPEG preview/thumbnail
    RawOnly,      // Only load full raw image
    Both          // Load both preview and full raw image
};

// Task to load an image
struct LoadTask {
    size_t imageIndex;
    std::string imagePath;
    LoadType loadType;
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
    bool previewRequested = false;  // Preview-only load requested
    bool rawRequested = false;       // Raw load requested
};

class ImageDatabase {
public:
    ImageDatabase(SDL_Renderer* renderer)
        : renderer_(renderer), running_(false) {}

    ~ImageDatabase() {
        stop();
    }

    // Start worker threads (one per CPU core)
    void start() {
        running_ = true;
        unsigned int numThreads = std::thread::hardware_concurrency();
        if (numThreads == 0) {
            numThreads = 1;  // Fallback if hardware_concurrency fails
        }
        
        workerThreads_.reserve(numThreads);
        for (unsigned int i = 0; i < numThreads; ++i) {
            workerThreads_.emplace_back(&ImageDatabase::workerThreadFunc, this);
        }
        
        std::cout << "Started " << numThreads << " worker threads for image loading" << std::endl;
    }

    // Stop all worker threads
    void stop() {
        running_ = false;
        for (auto& thread : workerThreads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        workerThreads_.clear();
    }

    // Try to get thumbnail for an image
    // Returns nullptr if not loaded yet, and queues a preview-only load task
    GpuTexture* tryGetThumbnail(size_t imageIndex, const std::string& imagePath) {
        auto it = entries_.find(imageIndex);
        if (it != entries_.end() && it->second.previewLoaded) {
            return &it->second.preview;
        }

        // Not loaded, queue a preview-only task if not already requested
        if (it == entries_.end() || !it->second.previewRequested) {
            if (it == entries_.end()) {
                entries_[imageIndex] = ImageEntry();
            }
            entries_[imageIndex].previewRequested = true;

            LoadTask task;
            task.imageIndex = imageIndex;
            task.imagePath = imagePath;
            task.loadType = LoadType::PreviewOnly;
            taskQueue_.push(std::move(task));
        }

        return nullptr;
    }

    // Try to get raw image
    // Returns nullptr if not loaded yet, and queues a raw load task if needed
    GpuTexture* tryGetRaw(size_t imageIndex, const std::string& imagePath) {
        auto it = entries_.find(imageIndex);
        if (it != entries_.end() && it->second.rawLoaded) {
            return &it->second.raw;
        }

        // Not loaded, queue a load task if not already requested
        if (it == entries_.end() || !it->second.rawRequested) {
            if (it == entries_.end()) {
                entries_[imageIndex] = ImageEntry();
                it = entries_.find(imageIndex);
            }
            
            ImageEntry& entry = it->second;
            entry.rawRequested = true;

            // Determine load type based on whether preview is already loaded/requested
            LoadType loadType;
            if (entry.previewLoaded || entry.previewRequested) {
                loadType = LoadType::RawOnly;
            } else {
                loadType = LoadType::Both;
                entry.previewRequested = true;  // Mark preview as requested too
            }

            LoadTask task;
            task.imageIndex = imageIndex;
            task.imagePath = imagePath;
            task.loadType = loadType;
            taskQueue_.push(std::move(task));
        }

        return nullptr;
    }

    // Check if both preview and raw are loaded for an image
    bool isFullyLoaded(size_t imageIndex) {
        auto it = entries_.find(imageIndex);
        return it != entries_.end() && it->second.previewLoaded && it->second.rawLoaded;
    }

    // Request thumbnails for all images in the collection
    // This queues preview-only loads for all images
    void requestAllThumbnails(const std::vector<fs::path>& images) {
        for (size_t i = 0; i < images.size(); ++i) {
            // Check if preview already loaded or requested
            auto it = entries_.find(i);
            if (it != entries_.end() && (it->second.previewLoaded || it->second.previewRequested)) {
                continue;  // Already loaded or queued
            }

            // Create entry if needed
            if (it == entries_.end()) {
                entries_[i] = ImageEntry();
            }
            entries_[i].previewRequested = true;

            // Queue preview-only task
            LoadTask task;
            task.imageIndex = i;
            task.imagePath = images[i].string();
            task.loadType = LoadType::PreviewOnly;
            taskQueue_.push(std::move(task));
        }
        
        std::cout << "Queued thumbnail loads for " << images.size() << " images" << std::endl;
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
    std::vector<std::thread> workerThreads_;
    std::atomic<bool> running_;

    // Worker thread function
    void workerThreadFunc() {
        while (running_) {
            LoadTask task;
            if (taskQueue_.tryPop(task)) {
                // Initialize and open the raw file
                auto rawProcessor = initializeRawProcessor(task.imagePath);
                if (!rawProcessor) {
                    continue;  // Failed to initialize, skip this task
                }
                
                if (task.loadType == LoadType::PreviewOnly) {
                    loadPreview(task, *rawProcessor);
                } else if (task.loadType == LoadType::RawOnly) {
                    loadRaw(task, *rawProcessor);
                } else {  // LoadType::Both
                    loadPreview(task, *rawProcessor);
                    loadRaw(task, *rawProcessor);
                }
            } else {
                // No tasks, sleep briefly to avoid busy-waiting
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }

    // Initialize and open a raw file with LibRaw
    // Returns nullptr on failure
    std::unique_ptr<LibRaw> initializeRawProcessor(const std::string& imagePath) {
        auto startTime = std::chrono::high_resolution_clock::now();
        
        // Allocate LibRaw on heap to avoid stack overflow
        auto rawProcessor = std::make_unique<LibRaw>();

        // Open and decode the raw file
        int ret = rawProcessor->open_file(imagePath.c_str());
        if (ret != LIBRAW_SUCCESS) {
            std::cerr << "Error opening file: " << libraw_strerror(ret) << std::endl;
            return nullptr;
        }

        // Unpack the raw data
        ret = rawProcessor->unpack();
        if (ret != LIBRAW_SUCCESS) {
            std::cerr << "Error unpacking raw data: " << libraw_strerror(ret) << std::endl;
            return nullptr;
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        
        // Extract just the filename
        fs::path path(imagePath);
        std::cout << "Opened raw file: " << path.filename().string() << " in " << duration.count() << " ms" << std::endl;

        return rawProcessor;
    }

    // Load the preview/thumbnail for an image
    void loadPreview(const LoadTask& task, LibRaw& rawProcessor) {
        auto startTime = std::chrono::high_resolution_clock::now();
        
        // Get orientation
        int orientation = rawProcessor.imgdata.sizes.flip;

        // Load and push preview
        LoadResult previewResult;
        previewResult.imageIndex = task.imageIndex;
        previewResult.type = ImageType::Preview;
        previewResult.cpuTexture = loadJpegPreview(rawProcessor);
        previewResult.orientation = orientation;
        resultsQueue_.push(std::move(previewResult));
        
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        
        // Extract just the filename
        fs::path path(task.imagePath);
        std::cout << "Loaded preview: " << path.filename().string() << " in " << duration.count() << " ms" << std::endl;
    }

    // Load the full raw image
    void loadRaw(const LoadTask& task, LibRaw& rawProcessor) {
        auto startTime = std::chrono::high_resolution_clock::now();
        
        // Configure processing parameters for better color accuracy
        rawProcessor.imgdata.params.use_camera_wb = 1;
        rawProcessor.imgdata.params.output_color = 1;
        rawProcessor.imgdata.params.gamm[0] = 1.0/2.4;
        rawProcessor.imgdata.params.gamm[1] = 12.92;
        rawProcessor.imgdata.params.user_qual = 3;
        rawProcessor.imgdata.params.no_auto_bright = 0;

        // Process the image
        int ret = rawProcessor.dcraw_process();
        if (ret != LIBRAW_SUCCESS) {
            std::cerr << "Error processing raw data: " << libraw_strerror(ret) << std::endl;
            return;
        }

        // Get processed image
        libraw_processed_image_t* image = rawProcessor.dcraw_make_mem_image(&ret);
        if (!image) {
            std::cerr << "Error creating memory image: " << libraw_strerror(ret) << std::endl;
            return;
        }

        // Push raw result
        LoadResult rawResult;
        rawResult.imageIndex = task.imageIndex;
        rawResult.type = ImageType::Raw;
        rawResult.rawImage = image;  // Transfer ownership
        rawResult.orientation = 0;  // Orientation is not set for raw images
        resultsQueue_.push(std::move(rawResult));
        
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        
        // Extract just the filename
        fs::path path(task.imagePath);
        std::cout << "Loaded raw: " << path.filename().string() << " in " << duration.count() << " ms" << std::endl;
    }
};
