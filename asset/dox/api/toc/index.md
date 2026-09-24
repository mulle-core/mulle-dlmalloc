# mulle-dlmalloc Library Documentation for AI
<!-- Keywords: memory, allocator, dlmalloc, shared-memory, relocatable, offset -->

## 1. Introduction & Purpose

- Single-file allocator for one fixed-capacity, contiguous shared-memory arena.
- All allocator metadata is stored as arena-relative offsets, never as
  process-local virtual addresses, so the same arena can be mapped at a
  different address in every attaching process.
- Relationship: component intended to be compiled into projects (no public
  headers shipped here); used by mulle-mmapallocator for shared-memory arenas.
- The implementation is self-contained in `src/dlmalloc.c` and requires no
  compile-time flags. The historic Doug Lea `dlmalloc`/`mspace` allocator is
  gone.

## 2. Key Concepts & Design Philosophy

- **Offset identity.** A persistent reference is a `dlmalloc_offset_t`, the
  arena-relative byte offset of user payload. Offset 0 means null/failure.
- **Process-local view.** `dlmalloc_offset_view` holds this process's base
  pointer and mapped length. It is never shared or persisted.
- **No local addresses, at all.** No code path writes a process address into the
  arena, so no translation pass exists. `dlmalloc_offset_resolve` turns an offset
  into a local pointer that is valid only in the calling process while the object
  is live.
- **One fixed arena.** Caller supplies and maps the region. There is no growth,
  no trimming to a system allocator, no multiple segments, and no allocation
  outside the arena.
- **Synchronization.** One process-shared spin lock in the arena header
  serializes all operations. It has no owner tracking: a process that dies while
  holding it wedges the arena, and an interrupted operation can leave metadata
  inconsistent. There is no crash recovery; discard or reinitialize instead.
- **Application data is not automatically relocatable.** The allocator cannot
  make arbitrary pointer-bearing payloads portable. Shared structures must store
  offsets (for example a `next` field) and resolve them locally on each access.

## 3. Core API & Data Structures

This project exposes its API from `src/dlmalloc.c`. There are no packaged public
header files in this repo; consumers include the source (it is an amalgamation).

### 3.1. Types

- `typedef uint64_t dlmalloc_offset_t`: a persistent reference. Arena-relative
  byte offset to user payload; 0 means null/failure.
- `typedef struct dlmalloc_offset_view { void *base; size_t capacity; }`:
  process-local attachment context holding this process's mapping and its length.
  Never share this struct or its contents.

### 3.2. Arena lifecycle

- `int dlmalloc_offset_arena_init(void *base, size_t capacity)`: initialize the
  arena once, in the creating process. `base` must be aligned to
  `DLMALLOC_OFFSET_ALIGN` (16). Clears and rebuilds the region, so it may be
  called again as a reset when the arena is quiescent and exclusively held.
- `int dlmalloc_offset_arena_attach(dlmalloc_offset_view *view, void *base, size_t capacity)`:
  attach any process at any address. Checks the versioned header and the
  configuration word, then walks the arena once to confirm blocks tile the
  region, every back offset matches the block before it, and the free lists
  describe exactly the free blocks, each once.
- `void dlmalloc_offset_arena_detach(dlmalloc_offset_view *view)`: discard
  process-local state only. Destroys no shared metadata and no lock, so it is
  safe while other processes remain attached.

### 3.3. Allocation (offsets in, offsets out)

- `dlmalloc_offset_t dlmalloc_offset_malloc(view, bytes)`
- `dlmalloc_offset_t dlmalloc_offset_memalign(view, alignment, bytes)`
  (power-of-two alignment up to 1 MiB; alignments above the natural one carve
  the aligned payload out of a larger block and return the front part to the
  free lists)
- `dlmalloc_offset_t dlmalloc_offset_calloc(view, count, bytes)`
- `dlmalloc_offset_t dlmalloc_offset_realloc(view, offset, bytes)`: offset 0
  allocates; a 0 return means the request failed and the original offset is
  still valid. If the payload had to move, the returned offset differs and the
  caller must publish the new one.
- `int dlmalloc_offset_free(view, offset)`

### 3.4. Inspection

- `void *dlmalloc_offset_resolve(view, offset)`: translate an offset to a
  process-local pointer, or NULL when the offset is not a live allocation.
- `size_t dlmalloc_offset_usable_size(view, offset)`: usable bytes of a live
  allocation.

Allocation identity is the offset, not the pointer. A `void *` returned by
`dlmalloc_offset_resolve` is valid only in the calling process and only while the
object is live; it must never be written into shared memory or sent to another
process.

## 4. Performance Characteristics

- Free lists are segregated into 32 power-of-two size classes with offsets as
  links and a bitmap of non-empty classes. A request scans its own class for a
  first fit; any block in a higher class is large enough by construction, so
  allocation costs at most one list walk inside a single class.
- `attach` costs one full pass over the arena (integrity walk); ordinary
  allocation, free, realloc and resolve take the lock and are bounded by the
  size-class structure.
- Per-block overhead is a 16-byte header. Blocks tile the region exactly, so
  coalescing needs only each block's `size` and `prev_size` (no footer, and an
  in-use block is never written past the end of its payload).

## 5. AI Usage Recommendations & Patterns

Best practices:

- Map the shared object, then `dlmalloc_offset_arena_init` once and
  `dlmalloc_offset_arena_attach` per process.
- Store `dlmalloc_offset_t` values in shared data; resolve locally and only for
  the duration of an access.
- Free by the same view and offset; do not pass a resolved pointer to another
  process.
- Treat the arena as writable by any attaching process: only share it between
  processes that trust each other. Caller-supplied offsets are checked for
  alignment, range and a live in-use block, but not proven to be real block
  boundaries.

Common pitfalls:

- Persisting or exchanging resolved `void *` values (they are process-local).
- Assuming growth or `realloc` in place: a move changes the offset.
- Assuming crash recovery: a dead lock owner wedges the arena.

## 6. Integration Example

```c
#include <sys/mman.h>
#include <stdint.h>
#include <string.h>

#include "mulle-dlmalloc/src/dlmalloc.c"

int main()
{
   dlmalloc_offset_view view = { 0, 0 };
   dlmalloc_offset_t    root;
   char                *text;

   base = mmap(NULL, capacity, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   if(base == MAP_FAILED)
      return(1);

   /* once, in the creating process */
   if(! dlmalloc_offset_arena_init(base, capacity))
      return(1);

   /* in every process, at any address */
   if(! dlmalloc_offset_arena_attach(&view, base, capacity))
      return(1);

   root = dlmalloc_offset_malloc(&view, 64);
   text = dlmalloc_offset_resolve(&view, root);
   strcpy(text, "alpha");

   /* store `root` (the offset), never `text` (the pointer), in shared data */
   return(0);
}
```

## 7. Dependencies

- No direct mulle-sde library dependencies (clib.json lists src/dlmalloc.c).
- Commonly used by: mulle-mmapallocator for shared-memory arena management.

## 8. Shortcut / Notes for AI

- Primary authoritative source: `src/dlmalloc.c` (all symbols are the
  `dlmalloc_offset_*` family).
- Use the offset API, not `void *` values, for anything that outlives the
  current call or crosses a process boundary.
- `DLMALLOC_OFFSET_ALIGN` is 16; the arena header starts at offset 0 and the
  first block starts at an aligned offset after the header and bucket table.
- `dlmalloc_offset_arena_init` doubles as a reset; it requires exclusive access
  and discards all previous allocations.

-- End of TOC.md