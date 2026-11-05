#include "psram_heap.h"
#include <stdint.h>
#include <string.h>

#ifdef ARDUINO
void *extmem_malloc(size_t size);
#else
#include <stdlib.h>
#define extmem_malloc malloc
#endif

#define SMALL_MAX   2048u
#define N_CLASSES   (SMALL_MAX / 8u)
#define MIN_SPLIT   32u                  // leftover payload worth keeping when a large block is split

typedef struct {
    uint32_t size;      // payload capacity, multiple of 8
    uint32_t tag;       // TAG_USED or TAG_FREE
} hdr_t;
#define TAG_USED 0x55534544u
#define TAG_FREE 0x46524545u

typedef struct node { struct node *next; } node_t;

static uint8_t *s_bump, *s_end;
static size_t s_cap;
static node_t *s_small[N_CLASSES];
static node_t *s_large;

static inline hdr_t *hdr_of(const void *p) { return (hdr_t *)((uint8_t *)p - sizeof(hdr_t)); }
static inline size_t round8(size_t n) { return (n + 7u) & ~(size_t)7u; }

bool ph_init(size_t bytes) {
    uint8_t *base = (uint8_t *)extmem_malloc(bytes);
    if (!base) return false;
    uintptr_t a = ((uintptr_t)base + 7u) & ~(uintptr_t)7u;
    s_bump = (uint8_t *)a;
    s_end = base + bytes;
    s_cap = (size_t)(s_end - s_bump);
    return true;
}

static void *from_bump(size_t n) {
    if ((size_t)(s_end - s_bump) < n + sizeof(hdr_t)) return NULL;
    hdr_t *h = (hdr_t *)s_bump;
    s_bump += n + sizeof(hdr_t);
    h->size = (uint32_t)n;
    h->tag = TAG_USED;
    return h + 1;
}

static void push_free(hdr_t *h) {
    h->tag = TAG_FREE;
    node_t *nd = (node_t *)(h + 1);
    if (h->size <= SMALL_MAX) { unsigned c = h->size / 8u - 1u; nd->next = s_small[c]; s_small[c] = nd; }
    else { nd->next = s_large; s_large = nd; }
}

// First fit among the large free blocks; splits off what is left over.
static void *from_large(size_t n) {
    node_t **pp = &s_large;
    for (node_t *nd = s_large; nd; pp = &nd->next, nd = nd->next) {
        hdr_t *h = hdr_of(nd);
        if (h->size < n) continue;
        *pp = nd->next;
        size_t left = h->size - n;
        if (left >= MIN_SPLIT + sizeof(hdr_t)) {
            hdr_t *rest = (hdr_t *)((uint8_t *)(h + 1) + n);
            rest->size = (uint32_t)(left - sizeof(hdr_t));
            push_free(rest);
            h->size = (uint32_t)n;
        }
        h->tag = TAG_USED;
        return h + 1;
    }
    return NULL;
}

void *ph_malloc(size_t n) {
    if (!s_bump) return NULL;
    n = round8(n ? n : 8);
    void *p = NULL;
    if (n <= SMALL_MAX) {
        unsigned c = (unsigned)(n / 8u - 1u);
        node_t *nd = s_small[c];
        if (nd) { s_small[c] = nd->next; hdr_of(nd)->tag = TAG_USED; return nd; }
        p = from_bump(n);
        if (!p) p = from_large(n);
        if (!p) {                         // out of fresh memory: take any bigger free small block whole
            for (unsigned k = c + 1; k < N_CLASSES && !p; k++)
                if (s_small[k]) { nd = s_small[k]; s_small[k] = nd->next; hdr_of(nd)->tag = TAG_USED; p = nd; }
        }
        return p;
    }
    p = from_large(n);
    if (!p) p = from_bump(n);
    return p;
}

void *ph_calloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) return NULL;
    size_t n = count * size;
    void *p = ph_malloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void ph_free(void *p) {
    if (!p) return;
    hdr_t *h = hdr_of(p);
    if (h->tag != TAG_USED) return;       // double free or not ours: ignore rather than corrupt the lists
    push_free(h);
}

void *ph_realloc(void *p, size_t n) {
    if (!p) return ph_malloc(n);
    if (!n) { ph_free(p); return NULL; }
    hdr_t *h = hdr_of(p);
    if (round8(n) <= h->size) return p;
    void *q = ph_malloc(n);
    if (!q) return NULL;
    memcpy(q, p, h->size);
    ph_free(p);
    return q;
}

size_t ph_usable(const void *p) { return p ? hdr_of(p)->size : 0; }
size_t ph_capacity(void) { return s_cap; }
size_t ph_untouched(void) { return s_bump ? (size_t)(s_end - s_bump) : 0; }
