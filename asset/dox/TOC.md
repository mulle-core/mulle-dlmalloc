# mulle-dlmalloc Library Documentation for AI
<!-- Keywords: memory, allocator, dlmalloc, mspace, shared-memory -->

## 1. Introduction & Purpose

- Single-file implementation of Doug Lea's dlmalloc adapted for shared-memory and mspace usage.
- Solves general-purpose dynamic allocation with options for multiple independent allocation spaces (mspaces), mmap support, and tunable behavior via compile-time flags and mallopt.
- Key features: standard malloc/free/realloc/calloc, memalign/posix_memalign, pvalloc/valloc, mallinfo/mallopt, malloc_trim and a full mspace API (create_mspace, mspace_malloc, mspace_free, etc.).
- Relationship: component intended to be compiled into projects (no public headers shipped here); used by mulle-mmapallocator for shared-memory arenas.

## 2. Key Concepts & Design Philosophy

- Monolithic, macro-heavy C implementation optimized for speed and space trade-offs.
- MSPACES: independent allocator instances to isolate allocations (useful for shared arenas or per-thread allocators).
- Uses MORECORE (sbrk) and/or MMAP for system memory management; behavior tunable via compile-time defines (ONLY_MSPACES, MSPACES, USE_LOCKS, FOOTERS, etc.).
- Designed for robustness: checks detect many misuse cases; optional FOOTERS and PROCEED_ON_ERROR change safety/performance trade-offs.

## 3. Core API & Data Structures

This project exposes its API from src/dlmalloc.c. There are no packaged public header files in this repo; for explicit prototypes see the historic malloc-2.8.6.h or extract the prototypes from dlmalloc.c.

### 3.1. Global allocator (drop-in names / dl-prefixed aliases)
- malloc(size_t) / dlmalloc: allocate memory
- free(void*) / dlfree: free memory
- realloc(void*, size_t) / dlrealloc: resize allocation
- calloc(size_t, size_t) / dlcalloc: allocate and zero
- memalign(size_t, size_t) / dlmemalign: aligned allocation
- posix_memalign(void**, size_t, size_t) / dlposix_memalign: POSIX aligned allocation
- valloc(size_t) / dlvalloc; pvalloc(size_t) / dlpvalloc: page-aligned helpers
- malloc_trim(size_t) / dlmalloc_trim: return free top space to OS
- malloc_usable_size(void*) / dlmalloc_usable_size: usable bytes of allocation
- malloc_stats() / dlmalloc_stats and mallinfo() / dlmallinfo: allocator stats
- mallopt(int,int) / dlmallopt: tune runtime parameters
- malloc_set_footprint_limit(size_t) / dlmalloc_set_footprint_limit: cap footprint

### 3.2. Mspace API (per-arena)
- create_mspace(size_t capacity, int mode): create an mspace
- create_mspace_with_base(void* base, size_t capacity, int mode): use provided base memory
- mspace_malloc(mspace, size_t), mspace_free(mspace, void*), mspace_realloc(mspace, void*, size_t), mspace_calloc(mspace, n, size)
- mspace_memalign(mspace, alignment, bytes), mspace_independent_calloc/comalloc, mspace_bulk_free
- mspace_trim(mspace, size_t pad), mspace_malloc_stats(mspace), mspace_mallinfo(mspace)
- mspace_footprint / mspace_max_footprint / mspace_set_footprint_limit

## 4. Performance Characteristics

- Complexity: allocation/free are constant-time bounded by a factor of size_t bits (practical O(1)); mallinfo/malloc_stats may traverse heaps and cost O(n).
- Small-object fast path via bins (low fragmentation); large requests use mmap (threshold tunable).
- Memory overhead: per-chunk header (4/8 bytes plus alignment), per-mspace metadata; FOOTERS/DEBUG add overhead.
- Threading: Not thread-safe by default. Enable USE_LOCKS or use separate mspaces per thread for concurrency.

## 5. AI Usage Recommendations & Patterns

Best practices:
- Use mspaces for isolated or shared-memory arenas (create_mspace + mspace_malloc/mspace_free).
- Compile with ONLY_MSPACES if you only want explicit mspace API (no global malloc overrides).
- Tune large-object behavior with mallopt(M_MMAP_THRESHOLD) and trimming with M_TRIM_THRESHOLD.
- Always pair frees with the same mspace used for allocation; do not mix mspaces unless FOOTERS is enabled and understood.

Common pitfalls:
- Relying on global dlmalloc in threaded apps without USE_LOCKS causes races.
- Passing non-power-of-two alignments to memalign/posix_memalign.

Idiomatic pattern: thread-local mspace (static __thread mspace ms = 0; if (!ms) ms = create_mspace(0,0); use mspace_malloc/mspace_free).

## 6. Integration Examples

### Example 1: Creating and populating an mspace

```c
#include <stdio.h>

int main()
{
   mspace ms;
   void* p;

   ms = create_mspace(0, 0);
   p  = mspace_malloc(ms, 256);
   if (p == NULL) return(1);
   /* use p */
   mspace_free(ms, p);
   return(0);
}
```

### Example 2: Using a custom allocator alias

```c
/* compile-time: -DONLY_MSPACES */
static mspace mymspace = NULL;

void* mymalloc(size_t bytes)
{
   if (mymspace == NULL) mymspace = create_mspace(0,0);
   return mspace_malloc(mymspace, bytes);
}

void myfree(void* p)
{
   mspace_free(mymspace, p);
}
```

## 7. Dependencies

- No direct mulle-sde library dependencies (clib.json lists only src/dlmalloc.c).
- Commonly used by: mulle-mmapallocator for shared-memory arena management.

## 8. Shortcut / Notes for AI

- Primary authoritative source: src/dlmalloc.c (exported symbols listed above). There are dl-prefixed aliases for many functions (dlmalloc/dlfree/etc.).
- Use mspace API for explicit control; tune behavior with mallopt and compile-time flags (ONLY_MSPACES, MSPACES, USE_LOCKS, FOOTERS).
- When generating code, prefer mspace-based allocation for shared or isolated heaps; avoid global dlmalloc in concurrent contexts unless locking is enabled.

-- End of TOC.md
