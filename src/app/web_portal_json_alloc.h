#pragma once

#include <stddef.h>

#include <esp_heap_caps.h>
#include <soc/soc_caps.h>

struct MacrosJsonAllocator {
    void* allocate(size_t size) {
#if SOC_SPIRAM_SUPPORTED
        if (psramFound()) {
            void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (p) return p;
        }
#endif
        return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    void deallocate(void* ptr) {
        heap_caps_free(ptr);
    }

    void* reallocate(void* ptr, size_t new_size) {
#if SOC_SPIRAM_SUPPORTED
        if (psramFound()) {
            void* p = heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (p) return p;
        }
#endif
        return heap_caps_realloc(ptr, new_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
};
