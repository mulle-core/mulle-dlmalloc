# mulle-dlmalloc Library Documentation for AI

## 1. Introduction & Purpose

mulle-dlmalloc is an adaptation of Doug Lea's malloc.c specifically configured with `-DMSPACE_ONLY` for creating independent memory allocation spaces (mspace). It serves as the underlying allocator for mulle-mmapallocator, enabling efficient sub-heap allocation within fixed memory regions, shared memory contexts, and isolated allocation arenas. This version removes global heap management in favor of mspace-based allocation.

## 2. Key Concepts & Design Philosophy

- **mspace API**: Provides multi-space malloc interface instead of global heap
- **Independent Arenas**: Create isolated allocation spaces with separate metadata
- **Shared Memory Compatible**: Designed to work within pre-allocated regions (e.g., mmap'd files)
- **Doug Lea's Algorithm**: Battle-tested malloc implementation with good fragmentation characteristics
- **No Global State**: DMSPACE_ONLY disables global heap; all operations on explicit mspace
- **Configurable Sizes**: Can tune chunk sizes and behavior for specific use cases

## 3. Core API & Data Structures

### mspace Functions (Primary API)

#### Mspace Creation & Destruction

- `mspace_create(capacity)` → `mspace`: Creates new allocation space with given capacity
- `mspace_destroy(mspace)` → `int`: Destroys mspace; returns 0 on success
- `mspace_footprint(mspace)` → `size_t`: Returns current size allocated to mspace
- `mspace_max_footprint(mspace)` → `size_t`: Returns maximum size ever allocated

#### Memory Allocation from Mspace

- `mspace_malloc(mspace, size)` → `void *`: Allocates memory from specific mspace
- `mspace_free(mspace, ptr)` → `void`: Frees memory to specific mspace
- `mspace_realloc(mspace, ptr, size)` → `void *`: Resizes allocation within same mspace
- `mspace_calloc(mspace, count, size)` → `void *`: Allocates and zeroes memory

#### Memory Inspection

- `mspace_usable_size(ptr)` → `size_t`: Returns usable size of allocated block
- `mspace_stats_t` struct: Contains allocation statistics

#### Advanced Operations

- `mspace_trim(mspace, pad)` → `int`: Attempts to shrink mspace; returns 0 if trimmed
- `mspace_malloc_stats(mspace)` → `void`: Prints allocation statistics
- `mspace_is_heap_object(mspace, ptr)` → `int`: Checks if pointer belongs to mspace

## 4. Performance Characteristics

- **Allocation**: O(1) amortized; O(log n) in pathological cases
- **Fragmentation**: Good fragmentation characteristics; typically 5-10% overhead
- **Memory Overhead**: Small per-mspace metadata; per-allocation overhead ~16-32 bytes
- **Deallocation**: O(1) amortized; may coalesce adjacent free blocks
- **Thread-Safety**: NOT thread-safe; requires external synchronization if used from multiple threads
- **Scale**: Designed for allocations from tens of bytes to gigabytes

## 5. AI Usage Recommendations & Patterns

### Best Practices

- **Create Once**: Create mspace once during initialization; don't recreate frequently
- **Fixed Capacity**: Capacity fixed at creation time; plan size requirements in advance
- **Check Return Values**: mspace_malloc returns NULL on allocation failure (unlike global malloc)
- **Paired Free**: Always use mspace_free with same mspace as allocated from
- **No Globals**: Different mspaces are completely independent; don't mix pointers
- **Inspect Stats**: Use mspace_malloc_stats() during debugging to understand allocation patterns

### Common Pitfalls

- **Capacity Exhaustion**: Will return NULL when mspace full; no automatic growth
- **Cross-mspace Free**: Freeing ptr from wrong mspace causes corruption; track allocator source
- **Thread Safety**: Not thread-safe; use locks or thread-local mspaces if concurrent access needed
- **Memory Leaks**: Unreleased mspace persists; manually track and free or use cleanup functions
- **Pointer Validity**: Pointers only valid within allocated block range; extent checking disabled

### Idiomatic Usage

```c
// Pattern 1: Simple mspace usage
mspace ms = mspace_create(10*1024*1024);
void *p = mspace_malloc(ms, 1000);
mspace_free(ms, p);
mspace_destroy(ms);

// Pattern 2: Arena for temporary allocations
mspace temp_arena = mspace_create(1024*1024);
// ... allocate and use
mspace_destroy(temp_arena);  // Free everything at once

// Pattern 3: Separate mspace per subsystem
mspace graphics_heap = mspace_create(50*1024*1024);
mspace network_heap = mspace_create(10*1024*1024);
```

## 6. Integration Examples

### Example 1: Basic Mspace Usage

```c
#include <mulle-dlmalloc/mulle-dlmalloc.h>
#include <stdio.h>
#include <string.h>

int main() {
    mspace ms = mspace_create(1024*1024);
    if (!ms) {
        fprintf(stderr, "Failed to create mspace\n");
        return 1;
    }
    
    char *buf = (char *)mspace_malloc(ms, 256);
    strcpy(buf, "Hello from mspace");
    printf("%s\n", buf);
    
    mspace_free(ms, buf);
    mspace_destroy(ms);
    return 0;
}
```

### Example 2: Multiple Mspaces

```c
#include <mulle-dlmalloc/mulle-dlmalloc.h>

int main() {
    mspace alloc1 = mspace_create(1024*1024);
    mspace alloc2 = mspace_create(2*1024*1024);
    
    int *p1 = (int *)mspace_malloc(alloc1, sizeof(int) * 100);
    double *p2 = (double *)mspace_malloc(alloc2, sizeof(double) * 50);
    
    p1[0] = 42;
    p2[0] = 3.14159;
    
    mspace_free(alloc1, p1);
    mspace_free(alloc2, p2);
    
    mspace_destroy(alloc1);
    mspace_destroy(alloc2);
    return 0;
}
```

### Example 3: Checking Allocation Size

```c
#include <mulle-dlmalloc/mulle-dlmalloc.h>
#include <stdio.h>

int main() {
    mspace ms = mspace_create(512*1024);
    
    void *p = mspace_malloc(ms, 100);
    size_t usable = mspace_usable_size(p);
    
    printf("Requested: 100, Usable: %zu\n", usable);
    
    mspace_free(ms, p);
    mspace_destroy(ms);
    return 0;
}
```

### Example 4: Realloc in Mspace

```c
#include <mulle-dlmalloc/mulle-dlmalloc.h>
#include <string.h>

int main() {
    mspace ms = mspace_create(1024*1024);
    
    int *arr = (int *)mspace_malloc(ms, 10 * sizeof(int));
    for (int i = 0; i < 10; i++)
        arr[i] = i;
    
    // Resize to 20 elements
    arr = (int *)mspace_realloc(ms, arr, 20 * sizeof(int));
    for (int i = 10; i < 20; i++)
        arr[i] = i;
    
    mspace_free(ms, arr);
    mspace_destroy(ms);
    return 0;
}
```

### Example 5: Calloc (Zeroed Allocation)

```c
#include <mulle-dlmalloc/mulle-dlmalloc.h>
#include <stdio.h>

int main() {
    mspace ms = mspace_create(1024*1024);
    
    // Allocate and zero 100 integers
    int *arr = (int *)mspace_calloc(ms, 100, sizeof(int));
    
    // All elements are 0
    for (int i = 0; i < 100; i++) {
        if (arr[i] != 0) {
            printf("ERROR: Element %d not zeroed\n", i);
            break;
        }
    }
    
    mspace_free(ms, arr);
    mspace_destroy(ms);
    return 0;
}
```

### Example 6: Trim Unused Memory

```c
#include <mulle-dlmalloc/mulle-dlmalloc.h>

int main() {
    mspace ms = mspace_create(10*1024*1024);
    
    void *p = mspace_malloc(ms, 1024*1024);
    mspace_free(ms, p);
    
    // Mspace still has capacity reserved
    size_t before = mspace_footprint(ms);
    
    // Try to trim unused pages
    if (mspace_trim(ms, 0) > 0) {
        size_t after = mspace_footprint(ms);
        printf("Trimmed from %zu to %zu\n", before, after);
    }
    
    mspace_destroy(ms);
    return 0;
}
```

## 7. Dependencies

- libc (standard C library)
- (mulle-dlmalloc is typically used by mulle-mmapallocator, not directly)
