/*
 * ThorVG Memory Allocation Patch
 * Redirect ThorVG's memory allocation to use PSRAM
 */

#ifndef THORVG_MEMORY_PATCH_H
#define THORVG_MEMORY_PATCH_H

#include "esp_heap_caps.h"
#include <cstdlib>

// Override ThorVG's memory allocation functions
#define calloc(n, size) thorvg_calloc(n, size)
#define malloc(size) thorvg_malloc(size)
#define free(ptr) thorvg_free(ptr)
#define realloc(ptr, size) thorvg_realloc(ptr, size)

// ThorVG memory allocation functions that use PSRAM
inline void* thorvg_calloc(size_t n, size_t size) {
    size_t total_size = n * size;
    void* ptr = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM);
    if (ptr == NULL) {
        // Fallback to internal RAM if PSRAM is full
        ptr = std::calloc(n, size);
    }
    return ptr;
}

inline void* thorvg_malloc(size_t size) {
    void* ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (ptr == NULL) {
        // Fallback to internal RAM if PSRAM is full
        ptr = std::malloc(size);
    }
    return ptr;
}

inline void thorvg_free(void* ptr) {
    if (ptr != NULL) {
        heap_caps_free(ptr);
    }
}

inline void* thorvg_realloc(void* ptr, size_t size) {
    if (ptr == NULL) {
        return thorvg_malloc(size);
    }
    if (size == 0) {
        thorvg_free(ptr);
        return NULL;
    }
    
    void* new_ptr = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM);
    if (new_ptr == NULL) {
        // Fallback to internal RAM if PSRAM is full
        new_ptr = std::realloc(ptr, size);
    }
    return new_ptr;
}

#endif // THORVG_MEMORY_PATCH_H
