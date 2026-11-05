#pragma once
/*
 * Heap for the OpenMouse engine in the Teensy's PSRAM.
 *
 * QuickJS makes tens of thousands of small allocations while it loads the OpenMouse bundle; the Teensy core's
 * extmem_malloc (smalloc) searches its pool linearly, which turns that into seconds of work. This takes one
 * block from extmem_malloc at boot and serves it with exact size classes (8-byte steps up to 2 KB, O(1)) and a
 * first-fit list for larger blocks, which split. No coalescing: the engine reuses the same sizes over and over.
 */
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool   ph_init(size_t bytes);
void  *ph_malloc(size_t n);
void  *ph_calloc(size_t count, size_t size);
void   ph_free(void *p);
void  *ph_realloc(void *p, size_t n);
size_t ph_usable(const void *p);
size_t ph_capacity(void);       // bytes managed
size_t ph_untouched(void);      // bytes never handed out yet

#ifdef __cplusplus
}
#endif
