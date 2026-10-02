# mulle-dlmalloc Library Documentation for AI
<!-- Keywords: memory, allocator, dlmalloc, shared-memory, relocatable, offset -->

## 1. Introduction & Purpose

- Single-file relocatable allocator for a shared-memory arena, growable in
  segments when the embedder supplies mapping callbacks.
- Relationship: component intended to be compiled into projects (no public
  headers shipped here); used by mulle-mmapallocator for shared-memory arenas.
- Arena layout, the block format, growth and the embedder contract are in
  `../../../DESIGN.md`. This file is the API reference.

## 2. Key Concepts & Design Philosophy

- **Offset identity.** A persistent reference is a `dlmalloc_offset_t`, the
  arena-relative byte offset of user payload. Offset 0 means null/failure.
- **Process-local view.** `dlmalloc_offset_view` holds this process's segment 0
  mapping, the local base of every segment it has mapped, the last seen epoch,
  and optional embedder callbacks, plus a view-local copy of segment 0 that
  keeps a one-segment arena off the shared directory. It is never shared or
  persisted.
- **No local addresses, at all.** No code path writes a process address into the
  arena, so no translation pass exists. `dlmalloc_offset_resolve` turns an offset
  into a local pointer that is valid only in the calling process while the object
  is live.
- **Segmented offset space.** The offset space is split into segments; segment 0
  holds the header, the bucket table and the segment directory. Blocks never
  cross a segment boundary and blocks in different segments never coalesce. An
  arena starts as one segment and grows through the embedder: a range that
  continues the segment before it extends that segment in place, and a range that
  does not becomes a new segment, each twice the size of what came before. A
  reserved window's ranges always continue, so it stays one segment for life.
- **Embedder owns the mapping.** dlmalloc never opens files or calls mmap. The
  view carries `grow_segment`, `map_segment` and `unmap_segment` callbacks; with
  no callbacks the arena is fixed and `grow` fails. The shipped embedder maps one
  reserved window up front and grows into it arithmetically; a file-backed
  embedder is the variant for attaching by path. See `../../../DESIGN.md`.
- **Growth is noticed via an epoch.** Every operation takes the lock; `enter`
  compares the local epoch with the shared one and maps unseen segments before
  proceeding.
- **Synchronization.** One process-shared spin lock in the arena header
  serializes all operations. It has no owner tracking: a process that dies while
  holding it wedges the arena, and an interrupted operation can leave metadata
  inconsistent. There is no crash recovery; discard or reinitialize instead.
  `view->nolocking` turns the lock off for an arena the embedder states only one
  process and one thread touch, which is worth 13-15% of the hot path. It is a
  promise, not a check; a zeroed view locks.
- **Application data is not automatically relocatable.** The allocator cannot
  make arbitrary pointer-bearing payloads portable. Shared structures must store
  offsets (for example a `next` field) and resolve them locally on each access.

## 3. Core API & Data Structures

This project exposes its API from `src/dlmalloc.c`. There are no packaged public
header files in this repo; consumers include the source (it is an amalgamation).

### 3.1. Types

- `typedef uintptr_t dlmalloc_offset_t`: a persistent reference. Arena-relative
  byte offset to user payload; 0 means null/failure. An arena is an address
  space, so every coordinate and length in it is a `uintptr_t`; the on-arena
  format is therefore width-relative, and the configuration word refuses an
  arena built by the other width.
- `typedef struct dlmalloc_offset_view { ... }`: process-local attachment
  context. Its `base`/`capacity` describe segment 0; it also carries the local
  base of every mapped segment, the last seen `epoch`, and the optional
  `grow_segment` / `map_segment` / `unmap_segment` callbacks plus their
  `context`. Never share this struct or its contents.

### 3.2. Arena lifecycle

- `int dlmalloc_offset_arena_init(void *base, size_t capacity)`: initialize the
  arena once, in the creating process. `base` must be aligned to
  `DLMALLOC_OFFSET_ALIGN` (`2 * sizeof(uintptr_t)`: 8 on ILP32, 16 on LP64). Clears and rebuilds the region, so it may be
  called again as a reset when the arena is quiescent and exclusively held.
- `int dlmalloc_offset_arena_attach(dlmalloc_offset_view *view, void *base, size_t capacity)`:
  attach any process at any address. Checks the versioned header, the
  configuration word and the segment directory. It does not read the blocks: a
  segment is checked by the first offset that lands in it (blocks tile it, and
  every back offset names the block before it), so attaching stays cheap and a
  segment nobody uses is never read. A segment that fails that check fails the
  operation that found it instead of that operation misbehaving. Hold on to the
  view: it records which segments are already checked, so rebuilding it for every
  call turns that check into a per-operation walk. Clears any previous
  `corrupted` state. The per-operation validation -- this check, the payload and
  free-list ones, and the header fields that do not guard memory -- is compiled
  in only for a debug build, for the reason and the cost in *Validation: on, off
  and what it buys* in `DESIGN.md`.
- `int dlmalloc_offset_arena_adopt(dlmalloc_offset_view *view, void *base, size_t capacity)`:
  attach to a region this process has just built, so the segments that exist now
  are taken as checked. Use it after `arena_init`; walking back a region you just
  wrote only re-reads it.
- `int dlmalloc_offset_arena_verify(dlmalloc_offset_view *view)`: run the whole
  check now: the directory, every segment's blocks, and the free lists, which
  must describe exactly the free blocks once each. One pass over the arena under
  the lock; the segments it walks stay checked. This one is compiled into every
  build, so it is how a release build asks for the check.
- `void dlmalloc_offset_arena_detach(dlmalloc_offset_view *view)`: discard
  process-local state only. Destroys no shared metadata and no lock, so it is
  safe while other processes remain attached. Unmaps segments 1..n through
  `unmap_segment` when one is set.
- `int dlmalloc_offset_arena_grow(view, size_t min_size)`: add a segment, at
  least `min_size` and at least twice the previous segment size. Calls
  `grow_segment` to extend the backing store and map the new range, records it in
  the directory and bumps the epoch. Reports success *without* adding a segment
  when another process added one since this view's last look: the allocation that
  failed is about to be retried, and the segment already there is the room for
  it. Adding one anyway would answer a shortfall that is already covered, so
  processes that fail together would each grow the arena. Fails when no
  `grow_segment` callback is set or the directory is full
  (`DLMALLOC_OFFSET_MAX_SEGMENTS`), and *refuses* when the arena is already
  `corrupted`: growing a damaged arena only spends more shared window to reach
  the same failure.
- `int dlmalloc_offset_arena_corrupted(view)`: whether a segment failed its
  check. A failed allocation does not say which of the two reasons applies -- no
  room, or a segment that does not describe itself -- and a caller that retries
  by growing needs to tell them apart. A release build never checks a segment's
  tiling, so it never sets this: a failed allocation there always means no room.
- `int dlmalloc_offset_arena_refresh(view)`: map any segments added by another
  process since this view attached. Ordinary operations do this implicitly on
  entry; call it explicitly when you want determinism.

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
- `attach` maps every segment (through `map_segment`) and then reads only the
  header and the directory, so it costs O(segments) and does not touch the
  arena's blocks. In a one-segment arena -- the steady state of a reserved window
  -- a block access needs no lookup at all: segment 0 starts at offset 0 and
  never moves, so the block sits at `base + offset`. With more than one segment
  the segment is located by binary search over the directory and resolved through
  its own base. The first offset to land in a segment additionally checks that
  segment's tiling, once -- a bit test per block access after that, in a build
  that checks; `-DNDEBUG` compiles the walk and the bit test out and is direct
  from the first access.
- Ordinary allocation, free, realloc and resolve take the lock and are bounded by
  the size-class structure. `dlmalloc_offset_arena_verify` is where a full pass
  over the arena lives now, for a caller that wants it up front; attach does not
  do one.
- Per-block overhead is a two-word header (8 bytes on ILP32, 16 on LP64).
  Blocks tile the region exactly, so
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
  boundaries -- and in a release build only for zero and alignment, so a wrong
  offset there corrupts the arena rather than returning failure.

Common pitfalls:

- Persisting or exchanging resolved `void *` values (they are process-local).
- Assuming `realloc` keeps the offset: a move changes it.
- Expecting growth without embedder callbacks: with none set the arena is fixed.
- Expecting crash recovery: a dead lock owner wedges the arena.

## 6. Integration Example

```c
#include <sys/mman.h>
#include <stdint.h>
#include <string.h>

#include "mulle-dlmalloc/src/dlmalloc.c"   /* needs mulle-c11 on the include path */

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
- `DLMALLOC_OFFSET_ALIGN` is `2 * sizeof(uintptr_t)` (16 on LP64, 8 on ILP32);
  the arena header starts at offset 0 and the
  first block of segment 0 starts at an aligned offset after the header, bucket
  table and segment directory. Later segments start at their `offset_base`.
- `dlmalloc_offset_arena_init` doubles as a reset; it requires exclusive access
  and discards all previous allocations.
- A creating process pairs `dlmalloc_offset_arena_init` with
  `dlmalloc_offset_arena_adopt`, not `attach`: it built the region, so nothing
  about it needs checking. `attach` is for another process's region, and it
  checks each segment the first time that process uses it rather than reading the
  whole arena up front. `dlmalloc_offset_arena_verify` forces the whole check.
- Growth is a format feature, not a policy: call `dlmalloc_offset_arena_grow`
  and provide the callbacks. See `../../../DESIGN.md` for the full design.

-- End of TOC.md