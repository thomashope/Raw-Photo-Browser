#pragma once

#include <iostream>
#include <libraw/libraw.h>
#include <SDL3/SDL.h>

#define STBI_NO_FAILURE_STRINGS  // Thread-safe: disables global error string
#include "stb_image.h"

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
