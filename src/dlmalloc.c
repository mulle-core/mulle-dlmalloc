/*
  mulle-dlmalloc: a relocatable allocator for a growable shared arena.

  The arena never stores a process-local address. Free-list links and every
  block's distance back to its predecessor are arena-relative offsets, so the
  same bytes work from a process that maps the arena at any address. A returned
  offset is the arena-relative address of user payload; adding the local base
  yields a pointer that is valid only in the calling process.

  This file is the whole library. Historically it was Doug Lea's dlmalloc with
  the mspace API; that pointer-based allocator is gone. The only allocator here
  is the offset arena, which shares no state or algorithms with the old code.

  Layout

     [ header ][ bucket table ][ segment directory ][ blocks .... ][ padding ]

  The offset space is split into segments. Segment 0 holds the header, the
  free-list bucket table and the segment directory. Every segment is an
  independently mapped range of one backing store; segment 0 is mapped by the
  caller, later segments are mapped through the view's map_segment callback.
  Offsets stay flat and monotonic: offset_base[i + 1] == offset_base[i] + size.

  Blocks within a segment tile [blocks_base, offset_base + size) exactly and
  each one starts on a DLMALLOC_OFFSET_ALIGN boundary with a 16 byte header:

     size       total block size in bytes, low bit set while the block is in use
     prev_size  distance back to the predecessor's header, 0 = first in segment

  A block never crosses a segment boundary and blocks in different segments
  never coalesce. The first block of a segment has prev_size 0, so coalescing
  backward stops there by construction; coalescing forward stops at the
  segment end.

  A free block keeps its bucket links in the first 16 payload bytes:

     next  arena offset of the next free block in the class, 0 ends the list
     prev  arena offset of the previous free block in the class, 0 starts it

  Classes are the powers of two starting at DLMALLOC_OFFSET_MIN_BLOCK, so class
  k covers [min * 2^k, min * 2^(k+1)). A block always sits in the class of its
  own size, which means a request only scans its own class for a first fit: a
  block in any higher class is large enough by construction. A bitmap of
  non-empty classes makes finding that first non-empty class a shift, so
  allocation costs at most one list walk inside a single class.

  Growth: the shared header carries an epoch. Every operation takes the arena
  lock; dlmalloc_offset_enter compares the local epoch with the shared one and
  maps any segments it has not seen yet before proceeding. dlmalloc never opens
  files or calls mmap itself; the process-local view carries map_segment /
  grow_segment callbacks supplied by the embedder. Without them the arena is
  fixed and dlmalloc_offset_arena_grow fails.

  When grow_segment hands back a range that continues the segment before it, the
  segment is extended in place instead of a second one being published. A
  reserved window always does -- the whole window is one mapping, so what comes
  back is the next bytes of segment 0 -- and then a window arena stays at one
  segment for life, which keeps the fast path in segment_find and keeps the
  offset space from acquiring a boundary in the middle of the arena.

  Not provided, deliberately: trimming back to a system allocator and crash
  recovery. The lock is a process-shared word in the header; a process that dies
  while holding it wedges the arena, and an arena abandoned mid-operation must
  be discarded.

  Per operation validation is a debug build feature: see DLMALLOC_OFFSET_CHECKED
  below for what a release build leaves out.
*/

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <mulle-c11/mulle-c11-builtin.h>

#ifndef DLMALLOC_EXPORT
#define DLMALLOC_EXPORT extern
#endif

/*
  Per operation validation.

  Every operation enters through a check that the shared metadata still
  describes an arena of this build (header_valid), that the free list a search
  walks is self consistent (link_target_valid), and that the offset a caller
  hands back names a live block of the size it claims (payload_valid_locked); a
  segment is checked against its own blocks the first time this process touches
  it (segment_check). That is what turns a corrupt, foreign or half written
  arena into a failed call instead of into corruption, and what lets
  dlmalloc_offset_free reject an offset that was never allocated or was already
  freed. It is also most of what an operation costs.

  Define DLMALLOC_OFFSET_CHECKED to 0 to compile it out. The default follows
  NDEBUG, the convention MULLE_C_ASSERT uses in mulle-c11: a debug build checks
  every operation, a release build does not.

    cc -DNDEBUG ............................... unchecked (release)
    cc ......................................... checked   (debug)
    cc -DDLMALLOC_OFFSET_CHECKED=1 -DNDEBUG .... checked in a release build
    cc -DDLMALLOC_OFFSET_CHECKED=0 ............. unchecked in a debug build

  What an unchecked build still does: arena_attach and arena_bind check the
  header and the whole directory before a process starts using an arena, and
  arena_verify checks every block tiling and every free list on demand. A caller
  that wants the guarantee can verify an arena once and then run unchecked, or
  build with DLMALLOC_OFFSET_CHECKED=1.

  What it no longer does: notice a change to the arena between verifications,
  and refuse an offset that does not name a live block. Freeing an offset twice,
  freeing one the arena never handed out, or operating on an arena another
  process is halfway through changing will corrupt it instead of failing. Offset
  zero and a misaligned offset are still refused in both builds, because freeing
  either has to stay a no-op (see payload_valid_locked).
*/
#if ! defined( DLMALLOC_OFFSET_CHECKED)
# if defined( NDEBUG)
#  define DLMALLOC_OFFSET_CHECKED 0
# else
#  define DLMALLOC_OFFSET_CHECKED 1
# endif
#endif

/*
  Development instrument: how many block accesses could not be answered from the
  view and had to be located in the segment directory. Nothing maintains it
  unless DLMALLOC_OFFSET_COUNT is set to 1 before this file is included, which
  is what test/40-benchmark/hotpath.c does; the count is then read back from
  `dlmalloc_offset_directory_fallbacks`.

  A fixed arena answers every access from the view once its segment has been
  established, so this is the number that says whether the one-segment fast path
  still holds: 0 per operation while the arena has one segment, and one per
  access as soon as a second one exists. It counts work not done, which is why
  it is an instrument and not a metric.
*/
#if ! defined( DLMALLOC_OFFSET_COUNT)
#define DLMALLOC_OFFSET_COUNT 0
#endif
#if DLMALLOC_OFFSET_COUNT
unsigned long  dlmalloc_offset_directory_fallbacks;
#define DLMALLOC_OFFSET_COUNT_FALLBACK()  \
   (++dlmalloc_offset_directory_fallbacks)
#else
#define DLMALLOC_OFFSET_COUNT_FALLBACK()  ((void) 0)
#endif

typedef uintptr_t dlmalloc_offset_t;

#define DLMALLOC_OFFSET_MAGIC        UINT64_C(0x444c4d4f46465333)
#define DLMALLOC_OFFSET_VERSION      5U
#define DLMALLOC_OFFSET_LOCKED       1U
#define DLMALLOC_OFFSET_INUSE_BIT    ((uintptr_t)1)
/*
  Payload alignment, and with it the width of a block header. An arena is an
  address space, so every coordinate and length in it is a uintptr_t: 8 bytes
  wide on ILP32 and 16 on LP64, never more and never the other one. A block
  header is two such words (size and prev_size), which is exactly one alignment
  unit, so "payload = block + DLMALLOC_OFFSET_BLOCK_SIZE" keeps the payload as
  aligned as the block itself. 2 * sizeof(uintptr_t) is dlmalloc's own
  MALLOC_ALIGNMENT and covers every fundamental type on these targets; a
  16-byte vector type wants more, and that is what memalign is for.
*/
#define DLMALLOC_OFFSET_ALIGN        (2 * sizeof(uintptr_t))
#define DLMALLOC_OFFSET_BLOCK_SIZE   DLMALLOC_OFFSET_ALIGN
#define DLMALLOC_OFFSET_MIN_BLOCK    (DLMALLOC_OFFSET_BLOCK_SIZE + DLMALLOC_OFFSET_ALIGN)
#define DLMALLOC_OFFSET_BUCKETS      32
#define DLMALLOC_OFFSET_MAX_ALIGN    ((uintptr_t)1 << 20)
#define DLMALLOC_OFFSET_MAX_SEGMENTS 64
#define DLMALLOC_OFFSET_MIN_SEGMENT  ((uintptr_t)1024 * 1024)

struct dlmalloc_offset_block
{
   uintptr_t size;       /* total block size, low bit = in use */
   uintptr_t prev_size;  /* distance back to the predecessor, 0 = first */
};

typedef char dlmalloc_offset_block_size_covers_the_header[
   DLMALLOC_OFFSET_BLOCK_SIZE == sizeof(struct dlmalloc_offset_block) ? 1 : -1];

struct dlmalloc_offset_links
{
   uintptr_t next;       /* arena offset, 0 ends the class list */
   uintptr_t prev;       /* arena offset, 0 starts the class list */
};

struct dlmalloc_offset_state
{
   uintptr_t bucket_map;                            /* bit set = class not empty */
   uintptr_t bucket_head[DLMALLOC_OFFSET_BUCKETS];
   uintptr_t bucket_tail[DLMALLOC_OFFSET_BUCKETS];
};

struct dlmalloc_offset_segment
{
   uintptr_t offset_base;   /* flat offset of the first mapped byte */
   uintptr_t size;          /* mapped bytes, multiple of the alignment */
   uintptr_t blocks_base;   /* flat offset where blocks start */
   uintptr_t backing;       /* byte offset of these bytes in the backing store */
   /*
     Total size of the block that ends this segment, which has no successor to
     record it. set_size keeps it; arena_grow reads it to find the block an
     in-place extension has to merge with or follow, in constant time. A segment
     whose tiling does not end in a block of exactly this size is inconsistent.
   */
   uintptr_t tail_size;
};

struct dlmalloc_offset_header
{
   uint64_t magic;
   uint32_t version;
   uint32_t header_size;
   uintptr_t capacity;             /* segment 0 length at creation */
   uintptr_t state_offset;
   uintptr_t state_size;
   uintptr_t directory_offset;
   uintptr_t directory_capacity;
   uint64_t configuration;
   uint32_t lock;
   uint32_t bucket_count;
   uintptr_t blocks_offset;        /* segment 0 blocks base */
   uintptr_t blocks_size;          /* segment 0 block region size */
   uintptr_t epoch;                /* bumped on every growth */
   uintptr_t segment_count;
   uintptr_t total_offset_space;   /* high-water flat offset */
};

typedef struct dlmalloc_offset_view
{
   void     *base;        /* this process's mapping of segment 0 */
   size_t    capacity;    /* mapped length of segment 0 */
   uintptr_t  epoch;       /* last seen shared epoch */
   uintptr_t  segment_count;
   /*
     High-water flat offset as this view last saw it. The arena can gain space
     without gaining a segment, by growing the segment it has, so this is what
     refresh_locked compares to tell whether somebody else got there first.
   */
   uintptr_t  total_offset_space;
   /*
     One bit per segment, set once this process has checked that segment's
     block tiling. attach and adopt reset it; after that a segment is checked
     the first time segment_find resolves an offset in it. Only ever written
     with the lock held. The directory holds DLMALLOC_OFFSET_MAX_SEGMENTS (64)
     entries, which is exactly the width of this word.
   */
   uint64_t  verified;

   /*
     Sticky and process-local: set when a segment fails its check. A segment
     that fails is not space to be grown around, so this is what keeps the
     embedder's allocation retry loop from answering a failed check by adding
     segments until the window is used up. One scribbled block header must not
     read as "out of space" and consume the arena. attach and adopt clear it;
     written under the lock, like verified.
   */
   int       corrupted;

   /*
     Set by refresh_locked when the arena gained segments this view had not
     accounted for, and consumed by arena_grow. A failed allocation is the only
     thing that leads to a grow and every allocation refreshes on entry, so this
     answers exactly what grow needs to know: did another process add a segment
     between the allocation that failed and this call? If it did, that segment
     is the room the caller is about to retry for. Written under the lock, like
     verified.
   */
   int       grew;

   void     *local_base[DLMALLOC_OFFSET_MAX_SEGMENTS];

   /*
     Process-local cache of directory[0], built on first use when the arena has
     only that one segment. It lives in the view because segment_find hands out
     a pointer to it, and it is what keeps a one-segment arena off the directory
     on every call. Its size is the one field that changes under it -- a
     reserved window grows segment 0 in place -- so refresh_locked puts the new
     size back. A zero size means "not built yet".
   */
   struct dlmalloc_offset_segment segment0;

   /*
     Embedder supplied. Zero means every operation takes the arena lock, which
     is what a shared arena needs and what a zeroed view gets. Non-zero says the
     embedder guarantees that this process is the only one touching the arena
     and that a single thread does, so the lock is never taken. Only sound for a
     private arena.
   */
   int       nolocking;

   /*
     Embedder supplied, optional. Without grow_segment the arena is fixed.
     map_segment is called to map a segment another process added; unmap_segment
     releases a mapping on detach.
   */
   int     (*grow_segment)( void *context, size_t min_size,
                            uintptr_t *backing, void **base, uintptr_t *size);
   int     (*map_segment)( void *context, uintptr_t backing, size_t size,
                           void **base);
   void     (*unmap_segment)( void *context, void *base, size_t size);
   void     *context;
} dlmalloc_offset_view;

/*
  `verified` is one bit per segment and the directory holds
  DLMALLOC_OFFSET_MAX_SEGMENTS of them, so the two widths have to agree. A
  directory wider than 64 would shift a uint64_t by 64 or more, which is
  undefined behaviour rather than a wrong answer, and it would happen silently
  the day someone raises the limit. Make it a compile error instead. A typedef
  is used rather than _Static_assert because this file compiles as C99 as well.
*/
typedef char dlmalloc_offset_verified_covers_all_segments[
   DLMALLOC_OFFSET_MAX_SEGMENTS <= 64 ? 1 : -1];

/*
  Relocatable arena API. The caller supplies and maps the first segment; a view
  is process-local and must be attached separately in each process.

  Offset zero means null or failure. A non-zero offset is the flat byte offset
  of user payload. User references stored in the arena must also be offsets, not
  raw pointers, and a resolved pointer is valid only in the calling process
  while the object stays live. A realloc that has to move an allocation returns
  a new offset, which is the caller's cue to publish it.

  dlmalloc_offset_arena_init may be called again on arena memory that is known
  to be quiescent, which clears and rebuilds it, so no separate reset entry
  point is required. There is no crash recovery after a process dies while
  holding the arena lock.

  attach checks the header and the segment directory, and leaves the block
  tiling and the free lists alone: a segment is checked the first time an offset
  lands in it, so attaching does not read the arena and a segment nobody touches
  is never read at all. adopt is attach for a region the caller built itself and
  has just written, so nothing needs checking. verify runs the whole check on
  demand.
*/
DLMALLOC_EXPORT int dlmalloc_offset_arena_init(void *base, size_t capacity);
DLMALLOC_EXPORT int dlmalloc_offset_arena_attach(dlmalloc_offset_view *view,
                                                  void *base,
                                                  size_t capacity);
DLMALLOC_EXPORT int dlmalloc_offset_arena_adopt(dlmalloc_offset_view *view,
                                                 void *base,
                                                 size_t capacity);
DLMALLOC_EXPORT int dlmalloc_offset_arena_verify(dlmalloc_offset_view *view);
DLMALLOC_EXPORT int dlmalloc_offset_arena_corrupted(dlmalloc_offset_view *view);
DLMALLOC_EXPORT void dlmalloc_offset_arena_detach(dlmalloc_offset_view *view);
DLMALLOC_EXPORT int dlmalloc_offset_arena_grow(dlmalloc_offset_view *view,
                                               size_t min_size);
DLMALLOC_EXPORT int dlmalloc_offset_arena_refresh(dlmalloc_offset_view *view);
DLMALLOC_EXPORT dlmalloc_offset_t dlmalloc_offset_malloc(dlmalloc_offset_view *view,
                                                         size_t bytes);
DLMALLOC_EXPORT dlmalloc_offset_t dlmalloc_offset_memalign(dlmalloc_offset_view *view,
                                                           size_t alignment,
                                                           size_t bytes);
DLMALLOC_EXPORT dlmalloc_offset_t dlmalloc_offset_calloc(dlmalloc_offset_view *view,
                                                         size_t count,
                                                         size_t bytes);
DLMALLOC_EXPORT dlmalloc_offset_t dlmalloc_offset_realloc(dlmalloc_offset_view *view,
                                                          dlmalloc_offset_t offset,
                                                          size_t bytes);
DLMALLOC_EXPORT int dlmalloc_offset_free(dlmalloc_offset_view *view,
                                         dlmalloc_offset_t offset);
DLMALLOC_EXPORT void *dlmalloc_offset_resolve(dlmalloc_offset_view *view,
                                              dlmalloc_offset_t offset);
DLMALLOC_EXPORT size_t dlmalloc_offset_usable_size(dlmalloc_offset_view *view,
                                                   dlmalloc_offset_t offset);

/* ---------------------------------------------------------------- helpers */

/*
  The width and ABI fingerprint, and the one number in the arena that is NOT a
  uintptr_t. It has to stay a fixed 64 bits for the same reason a hash does: it
  exists to be compared between two processes that may disagree about how wide
  a pointer is. A uintptr_t here would be 32 bits in the narrow process and the
  top half -- MAX_SEGMENTS and ALIGN, shifted to 48 and 56 -- would be shifted
  out of existence before anything could compare it.
*/
static uint64_t dlmalloc_offset_configuration(void)
{
   uint64_t value;

   value  = (uint64_t)sizeof(void *) << 32;
   value |= (uint64_t)sizeof(size_t) << 40;
   value |= (uint64_t)DLMALLOC_OFFSET_ALIGN << 48;
   value |= (uint64_t)DLMALLOC_OFFSET_MAX_SEGMENTS << 56;
   value |= (uint64_t)sizeof(struct dlmalloc_offset_state) << 16;
   value |= (uint64_t)DLMALLOC_OFFSET_BLOCK_SIZE << 8;
   value |= (uint64_t)DLMALLOC_OFFSET_BUCKETS;
   return(value);
}

static uintptr_t dlmalloc_offset_align_up(uintptr_t value)
{
   return((value + DLMALLOC_OFFSET_ALIGN - 1) & ~(DLMALLOC_OFFSET_ALIGN - 1));
}

static struct dlmalloc_offset_header *dlmalloc_offset_header_for(
   dlmalloc_offset_view *view)
{
   return((struct dlmalloc_offset_header *)view->base);
}

static struct dlmalloc_offset_state *dlmalloc_offset_state_for(
   dlmalloc_offset_view *view)
{
   struct dlmalloc_offset_header *header;

   header = dlmalloc_offset_header_for(view);
   return((struct dlmalloc_offset_state *)((char *)view->base +
                                           header->state_offset));
}

static struct dlmalloc_offset_segment *dlmalloc_offset_directory_for(
   dlmalloc_offset_view *view)
{
   struct dlmalloc_offset_header *header;

   header = dlmalloc_offset_header_for(view);
   return((struct dlmalloc_offset_segment *)((char *)view->base +
                                             header->directory_offset));
}

/* A zeroed view means a single segment described by base/capacity. */
static uintptr_t dlmalloc_offset_view_segments(dlmalloc_offset_view *view)
{
   return(view->segment_count ? view->segment_count : 1);
}

static void *dlmalloc_offset_view_base(dlmalloc_offset_view *view,
                                       uintptr_t index)
{
   return(index ? view->local_base[index] : view->base);
}

/*
  Segment 0 of a single-segment arena, described from the directory once and
  cached in the view. The caller has already passed header_valid, which
  guarantees directory[0] is segment 0. The size is read from the directory
  rather than from view->capacity because the window embedder grows segment 0
  past the length it was created with; refresh_locked puts a change back.
*/
static struct dlmalloc_offset_segment *
dlmalloc_offset_segment0(dlmalloc_offset_view *view)
{
   struct dlmalloc_offset_segment *entry;
   struct dlmalloc_offset_segment *published;

   entry = &view->segment0;
   if(entry->size == 0)
   {
      published          = dlmalloc_offset_directory_for(view);
      entry->offset_base = published[0].offset_base;
      entry->size        = published[0].size;
      entry->blocks_base = published[0].blocks_base;
      entry->backing     = published[0].backing;
      entry->tail_size   = published[0].tail_size;
   }
   return(entry);
}

static int dlmalloc_offset_segment_valid_locked(
   dlmalloc_offset_view *view,
   uintptr_t index,
   struct dlmalloc_offset_segment *entry,
   uintptr_t *free_blocks);

/* Check a segment the first time it is used; see the note in segment_find. */
static int dlmalloc_offset_segment_check(dlmalloc_offset_view *view,
                                         uintptr_t index,
                                         struct dlmalloc_offset_segment *entry)
{
#if ! DLMALLOC_OFFSET_CHECKED
   /*
     Unchecked: a segment is taken as it is found. Neither the verified mask
     nor the corrupted flag is maintained, so a process that attaches to an
     arena does not walk it before it uses it. arena_verify is the way to ask
     for that walk in this build.
   */
   (void) view;
   (void) index;
   (void) entry;
   return(1);
#else
   if(view->verified & ((uint64_t)1 << index))
      return(1);
   if( ! dlmalloc_offset_segment_valid_locked(view, index, entry, 0))
   {
      view->corrupted = 1;
      return(0);
   }
   view->verified |= (uint64_t)1 << index;
   return(1);
#endif
}

/*
  Locate the segment containing a flat offset. Returns 0 when no mapped segment
  covers it. A one-segment arena is answered from the view, so the fixed case
  never reads the directory. Otherwise the directory is sorted by offset_base,
  so this is a binary search.

  The block primitives do not normally get here at all any more: with one
  segment a flat offset is a displacement from the view's base, and block_at,
  links_at, blocks_end and set_size address it directly (see direct_locked).
  This is the path for an arena with more than one segment, and the bounds check
  the functions that validate a caller supplied offset still want.

  This is also where an attached arena gets its integrity checking. Attaching
  reads the header and the directory only; the first offset that lands in a
  segment checks that segment's block tiling, before any caller dereferences a
  block header in it. A process therefore reads only the segments it uses, and
  no attach holds the arena lock across a walk of the whole arena.

  Reporting a failed check as "no such segment" is safe: callers only go on to
  block_at or links_at an offset after a segment_find for the same segment
  returned 1, so a segment that fails here is never dereferenced.
*/
static int dlmalloc_offset_segment_find(dlmalloc_offset_view *view,
                                        uintptr_t offset,
                                        uintptr_t *index,
                                        struct dlmalloc_offset_segment **entry)
{
   struct dlmalloc_offset_segment *directory;
   uintptr_t high;
   uintptr_t low;
   uintptr_t mid;

   if(dlmalloc_offset_view_segments(view) == 1)
   {
      *entry = dlmalloc_offset_segment0(view);
      if(offset >= (*entry)->size)
         return(0);
      *index = 0;
      return(dlmalloc_offset_segment_check(view, 0, *entry));
   }

   directory = dlmalloc_offset_directory_for(view);
   low       = 0;
   high      = dlmalloc_offset_view_segments(view);
   while(low < high)
   {
      mid = low + (high - low) / 2;
      if(offset < directory[mid].offset_base)
         high = mid;
      else
         if(offset >= directory[mid].offset_base + directory[mid].size)
            low = mid + 1;
         else
         {
            *index = mid;
            *entry = &directory[mid];
            return(dlmalloc_offset_segment_check(view, mid, *entry));
         }
   }
   return(0);
}

/*
  Is this process's base an address base for the whole arena?

  It is while the arena has one segment, and only then: segment 0 starts at
  offset 0 in every arena (header_valid and directory_valid_locked both require
  offset_base 0 for index 0) and a segment never moves, so for that one segment
  a flat offset *is* a displacement from this process's base and addressing is
  `view->base + offset` -- no directory read, no search, no index, no entry.
  A reserved-window arena is one segment for life, so this is the steady state
  of the shipped embedder; the file-backed one appends a second segment and
  leaves the fast path at that point.

  A checked build adds one condition: the segment has to have passed its tiling
  check (see segment_find), which is what the verified bit records. The check
  runs on the first access and every access after it addresses directly; an
  in-place growth of segment 0 drops the bit again, because the bytes that
  changed are the ones this process did not write. An unchecked build has no
  check to pass and is direct from the first access.

  Only the internal primitives below ask: they address offsets they took from
  the arena itself. The functions that validate an offset a caller handed in --
  payload_valid_locked, link_target_valid, resolve, usable_size -- keep going
  through segment_find, so they keep their bounds check.
*/
static int dlmalloc_offset_direct_locked(dlmalloc_offset_view *view)
{
#if DLMALLOC_OFFSET_CHECKED
   return(dlmalloc_offset_view_segments(view) == 1 &&
          (view->verified & ((uint64_t)1 << 0)) != 0);
#else
   return(dlmalloc_offset_view_segments(view) == 1);
#endif
}

static struct dlmalloc_offset_block *dlmalloc_offset_block_at(
   dlmalloc_offset_view *view,
   uintptr_t offset)
{
   struct dlmalloc_offset_segment *entry;
   uintptr_t index;

   if(dlmalloc_offset_direct_locked(view))
      return((struct dlmalloc_offset_block *)((char *)view->base + offset));
   DLMALLOC_OFFSET_COUNT_FALLBACK();
   if( ! dlmalloc_offset_segment_find(view, offset, &index, &entry))
      return(0);
   return((struct dlmalloc_offset_block *)((char *)dlmalloc_offset_view_base(view,
                                                                             index) +
                                           (offset - entry->offset_base)));
}

static struct dlmalloc_offset_links *dlmalloc_offset_links_at(
   dlmalloc_offset_view *view,
   uintptr_t offset)
{
   struct dlmalloc_offset_segment *entry;
   uintptr_t index;

   if(dlmalloc_offset_direct_locked(view))
      return((struct dlmalloc_offset_links *)((char *)view->base + offset +
                                             DLMALLOC_OFFSET_BLOCK_SIZE));
   DLMALLOC_OFFSET_COUNT_FALLBACK();
   if( ! dlmalloc_offset_segment_find(view, offset, &index, &entry))
      return(0);
   return((struct dlmalloc_offset_links *)((char *)dlmalloc_offset_view_base(view,
                                                                             index) +
                                           (offset - entry->offset_base) +
                                           DLMALLOC_OFFSET_BLOCK_SIZE));
}

static uintptr_t dlmalloc_offset_block_size(struct dlmalloc_offset_block *block)
{
   return(block->size & ~DLMALLOC_OFFSET_INUSE_BIT);
}

static int dlmalloc_offset_block_inuse(struct dlmalloc_offset_block *block)
{
   return((block->size & DLMALLOC_OFFSET_INUSE_BIT) != 0);
}

static uintptr_t dlmalloc_offset_blocks_end(dlmalloc_offset_view *view,
                                           uintptr_t offset)
{
   struct dlmalloc_offset_segment *entry;
   uintptr_t index;
   uintptr_t end;

   if(dlmalloc_offset_direct_locked(view))
   {
      /*
        The fast path keeps segment_find's contract, bounds check included,
        because the callers use the 0 to refuse the offset rather than to
        dereference it.
      */
      entry = dlmalloc_offset_segment0(view);
      end   = entry->offset_base + entry->size;
      if(offset >= end)
         return(0);
      return(end);
   }
   DLMALLOC_OFFSET_COUNT_FALLBACK();
   if( ! dlmalloc_offset_segment_find(view, offset, &index, &entry))
      return(0);
   return(entry->offset_base + entry->size);
}/*
  Class of a block size: the largest k with size >= DLMALLOC_OFFSET_MIN_BLOCK *
  2^k (>= 2^k, since MIN_BLOCK * 2^k is just 2^(k + 5)), capped at the last
  class. Monotonic in size, so a block in a higher class is always large enough.

  This used to step up to the class in a loop with a multiply, once to search,
  once to unlink and once to relink -- three times per allocation, up to 31
  iterations each. A floor(log2) and a subtraction do the same thing, and are
  the same function. mulle_c_log2_floor carries the compiler branch for it.
*/
static uintptr_t dlmalloc_offset_bucket_for(uintptr_t size)
{
   uintptr_t bucket;

   if(size < DLMALLOC_OFFSET_MIN_BLOCK * 2)
      return(0);
   /* MIN_BLOCK * 2 is 64, so the class is log2(size) - 6 plus the one class
      that spans everything below 64. */
   bucket = mulle_c_log2_floor(size) - 5;
   if(bucket >= DLMALLOC_OFFSET_BUCKETS)
      bucket = DLMALLOC_OFFSET_BUCKETS - 1;
   return(bucket);
}

/*
  Set a block's size and in-use state, then push the size into the successor's
  prev_size. This is the only place allowed to change a block's size. The last
  block of a segment has no successor, so nothing is written past the segment:
  its size goes into the segment's tail_size instead, which is how arena_grow
  finds the block an extension has to merge with.
*/
static void dlmalloc_offset_set_size(dlmalloc_offset_view *view,
                                     uintptr_t block_offset,
                                     uintptr_t size,
                                     int inuse)
{
   struct dlmalloc_offset_segment *entry;
   struct dlmalloc_offset_block *block;
   char *base;
   uintptr_t end;

   if(dlmalloc_offset_direct_locked(view))
   {
      entry = dlmalloc_offset_segment0(view);
      base  = (char *)view->base;
   }
   else
   {
      uintptr_t index;

      DLMALLOC_OFFSET_COUNT_FALLBACK();
      if( ! dlmalloc_offset_segment_find(view, block_offset, &index, &entry))
         return;
      base = (char *)dlmalloc_offset_view_base(view, index);
   }
   end   = entry->offset_base + entry->size;
   block = (struct dlmalloc_offset_block *)
              (base + (block_offset - entry->offset_base));
   block->size = size | (inuse ? DLMALLOC_OFFSET_INUSE_BIT : 0);
   if(block_offset + size < end)
      ((struct dlmalloc_offset_block *)
          (base + (block_offset + size - entry->offset_base)))->prev_size = size;
   else
   {
      entry->tail_size = size;
      /*
        A one-segment arena is answered from the view's cache, so for that case
        the cache and the published entry are two copies of one description and
        the record belongs in both: the directory is what arena_grow and every
        other process read.
      */
      if(entry == &view->segment0)
         dlmalloc_offset_directory_for(view)[0].tail_size = size;
   }
}

static int dlmalloc_offset_link_target_valid(dlmalloc_offset_view *view,
                                             uintptr_t offset)
{
#if DLMALLOC_OFFSET_CHECKED
   struct dlmalloc_offset_segment *entry;
   uintptr_t index;
#endif

   /* A zero link ends the class list; it is not a target to check. */
   if( ! offset)
      return(1);
#if DLMALLOC_OFFSET_CHECKED
   if((offset & (DLMALLOC_OFFSET_ALIGN - 1)) != 0)
      return(0);
   if( ! dlmalloc_offset_segment_find(view, offset, &index, &entry))
      return(0);
   return(offset >= entry->blocks_base &&
          offset < entry->offset_base + entry->size);
#else
   /*
     Unchecked: a link is followed wherever it points. A list that is intact
     behaves exactly as it does in a checked build; a list that is not takes the
     walk into memory the segment does not own.
   */
   (void) view;
   return(1);
#endif
}

static void dlmalloc_offset_list_insert(dlmalloc_offset_view *view,
                                        uintptr_t block_offset)
{
   struct dlmalloc_offset_state *state;
   struct dlmalloc_offset_links *links;
   uintptr_t bucket;
   uintptr_t tail;

   state  = dlmalloc_offset_state_for(view);
   bucket = dlmalloc_offset_bucket_for(
               dlmalloc_offset_block_size(dlmalloc_offset_block_at(view,
                                                                   block_offset)));
   links  = dlmalloc_offset_links_at(view, block_offset);
   tail   = state->bucket_tail[bucket];
   links->next = 0;
   links->prev = tail;
   if(tail)
      dlmalloc_offset_links_at(view, tail)->next = block_offset;
   else
      state->bucket_head[bucket] = block_offset;
   state->bucket_tail[bucket] = block_offset;
   state->bucket_map |= (uintptr_t)1 << bucket;
}

static void dlmalloc_offset_list_remove(dlmalloc_offset_view *view,
                                        uintptr_t block_offset)
{
   struct dlmalloc_offset_state *state;
   struct dlmalloc_offset_links *links;
   uintptr_t bucket;

   state  = dlmalloc_offset_state_for(view);
   bucket = dlmalloc_offset_bucket_for(
               dlmalloc_offset_block_size(dlmalloc_offset_block_at(view,
                                                                   block_offset)));
   links  = dlmalloc_offset_links_at(view, block_offset);
   if(links->prev)
      dlmalloc_offset_links_at(view, links->prev)->next = links->next;
   else
      state->bucket_head[bucket] = links->next;
   if(links->next)
      dlmalloc_offset_links_at(view, links->next)->prev = links->prev;
   else
      state->bucket_tail[bucket] = links->prev;
   if(state->bucket_head[bucket] == 0)
      state->bucket_map &= ~((uintptr_t)1 << bucket);
   links->next = 0;
   links->prev = 0;
}

/* --------------------------------------------------------------- geometry */

static int dlmalloc_offset_header_valid(dlmalloc_offset_view *view)
{
   struct dlmalloc_offset_header *header;
   uintptr_t capacity;
#if DLMALLOC_OFFSET_CHECKED
   struct dlmalloc_offset_segment *directory;
   uintptr_t expected;
#endif

   if( ! view || ! view->base ||
       ((uintptr_t)view->base & (DLMALLOC_OFFSET_ALIGN - 1)) != 0 ||
       view->capacity < sizeof(struct dlmalloc_offset_header))
      return(0);
   header   = dlmalloc_offset_header_for(view);
   capacity = (uintptr_t)view->capacity;
   /*
     The half that every build pays for: this is our format, at this pointer
     width, and it describes this view. It reads the header cache line, which
     an operation reads anyway, and it is what keeps a foreign arena, or one
     built by a process with a different pointer width, from being used as if
     it were ours.
   */
   if(header->magic != DLMALLOC_OFFSET_MAGIC ||
      header->version != DLMALLOC_OFFSET_VERSION ||
      header->capacity != capacity ||
      header->configuration != dlmalloc_offset_configuration() ||
      header->segment_count < 1 ||
      header->segment_count > DLMALLOC_OFFSET_MAX_SEGMENTS)
      return(0);
#if DLMALLOC_OFFSET_CHECKED
   /*
     The half only a checked build pays for: the derived offsets, the geometry
     of the directory and segment 0's own description. Attaching checks the
     directory on its own account (see arena_attach), so nothing is lost at
     attach time by turning this off. What is lost is noticing a change to it
     afterwards, which is the case a checked build exists for.
   */
   if(header->header_size != dlmalloc_offset_align_up(sizeof(*header)) ||
      header->state_offset != header->header_size ||
      header->state_size != sizeof(struct dlmalloc_offset_state) ||
      header->bucket_count != DLMALLOC_OFFSET_BUCKETS ||
      header->directory_capacity != DLMALLOC_OFFSET_MAX_SEGMENTS)
      return(0);
   expected = dlmalloc_offset_align_up(header->state_offset + header->state_size);
   if(header->directory_offset != expected ||
      header->directory_offset > capacity)
      return(0);
   expected = dlmalloc_offset_align_up(header->directory_offset +
                                       header->directory_capacity *
                                       sizeof(struct dlmalloc_offset_segment));
   if(header->blocks_offset != expected ||
      header->blocks_offset > capacity ||
      header->blocks_size !=
         ((capacity - header->blocks_offset) & ~(DLMALLOC_OFFSET_ALIGN - 1)) ||
      header->blocks_size < DLMALLOC_OFFSET_MIN_BLOCK ||
      header->blocks_size > capacity - header->blocks_offset)
      return(0);
   directory = dlmalloc_offset_directory_for(view);
   /*
     Segment 0 is at least the length it was created with: header->capacity and
     the view's capacity are that length, and a reserved window grows the
     segment past it in place, so it is compared as a floor and not as an
     equality. header->capacity itself stays the creation length, which is what
     makes it a constant this process can check the view against.
   */
   if(directory[0].offset_base != 0 ||
      directory[0].size < capacity ||
      directory[0].blocks_base != header->blocks_offset ||
      directory[0].backing != 0 ||
      header->total_offset_space < directory[0].size)
      return(0);
#endif
   return(1);
}

/*
  Directory consistency: sizes sane, the segments contiguous in offset space,
  and agreement with the header. O(segments), so attaching can afford it.
*/
static int dlmalloc_offset_directory_valid_locked(dlmalloc_offset_view *view)
{
   struct dlmalloc_offset_header *header;
   struct dlmalloc_offset_segment *directory;
   struct dlmalloc_offset_segment *entry;
   uintptr_t count;
   uintptr_t index;
   uintptr_t prev_end;

   header    = dlmalloc_offset_header_for(view);
   directory = dlmalloc_offset_directory_for(view);
   count     = header->segment_count;

   prev_end = 0;
   for(index = 0; index < count; ++index)
   {
      entry = &directory[index];
      if(entry->size < DLMALLOC_OFFSET_MIN_BLOCK ||
         (entry->size & (DLMALLOC_OFFSET_ALIGN - 1)) != 0)
         return(0);
      if(index == 0)
      {
         if(entry->offset_base != 0 ||
            entry->size < header->capacity ||
            entry->blocks_base != header->blocks_offset)
            return(0);
      }
      else
         if(entry->offset_base != prev_end ||
            entry->blocks_base != entry->offset_base)
            return(0);
      prev_end = entry->offset_base + entry->size;
   }
   if(header->total_offset_space != prev_end)
      return(0);
   return(1);
}

/*
  One segment's block tiling: the blocks start at blocks_base, are aligned and
  at least DLMALLOC_OFFSET_MIN_BLOCK long, never overrun the segment, and each
  records the size of the block before it. Counting the free ones here saves the
  free-list check below a second pass over the tiling.

  The segment's base and description come from the caller, so this never goes
  back through segment_find and cannot recurse into the lazy check there.
*/
static int dlmalloc_offset_segment_valid_locked(
   dlmalloc_offset_view *view,
   uintptr_t index,
   struct dlmalloc_offset_segment *entry,
   uintptr_t *free_blocks)
{
   struct dlmalloc_offset_segment *published;
   struct dlmalloc_offset_block *block;
   char *segment;
   uintptr_t end;
   uintptr_t free_count;
   uintptr_t offset;
   uintptr_t prev_size;
   uintptr_t size;

   segment = (char *)dlmalloc_offset_view_base(view, index);
   if( ! segment)
      return(0);

   end        = entry->offset_base + entry->size;
   free_count = 0;
   prev_size  = 0;
   offset     = entry->blocks_base;
   while(offset < end)
   {
      block = (struct dlmalloc_offset_block *)(segment +
                                               (offset - entry->offset_base));
      size  = dlmalloc_offset_block_size(block);
      if(size < DLMALLOC_OFFSET_MIN_BLOCK ||
         (size & (DLMALLOC_OFFSET_ALIGN - 1)) != 0 ||
         size > end - offset ||
         block->prev_size != prev_size)
         return(0);
      if( ! dlmalloc_offset_block_inuse(block))
         ++free_count;
      prev_size = size;
      offset   += size;
   }
   if(offset != end)
      return(0);
   /*
     The walk already knows the size of the last block, so the segment's record
     of it is checked here for free. That record is read from the directory and
     not from the entry the caller handed in, because a one-segment arena is
     described by a copy of directory[0] that changes its size without the
     epoch moving. A segment that disagrees with the record is one arena_grow
     cannot safely extend.
   */
   published = dlmalloc_offset_directory_for(view) + index;
   if(published->tail_size != prev_size)
      return(0);
   if(free_blocks)
      *free_blocks += free_count;
   return(1);
}

/*
  The whole check: the directory, every segment's block tiling, and the free
  lists, which must describe exactly the free blocks once each. This reads the
  whole arena, so it is not what attach does any more -- attach checks the
  directory and each segment is checked by the first offset that lands in it.
  This runs from dlmalloc_offset_arena_verify. It also sets the view's checked
  bits, because a segment it just walked really is checked.
*/
static int dlmalloc_offset_arena_valid_locked(dlmalloc_offset_view *view)
{
   struct dlmalloc_offset_header *header;
   struct dlmalloc_offset_state *state;
   struct dlmalloc_offset_segment *directory;
   struct dlmalloc_offset_segment *entry;
   struct dlmalloc_offset_block *block;
   struct dlmalloc_offset_block *other;
   struct dlmalloc_offset_links *links;
   uintptr_t bucket;
   uintptr_t count;
   uintptr_t free_blocks;
   uintptr_t index;
   uintptr_t linked;
   uintptr_t offset;
   uintptr_t tail;

   header    = dlmalloc_offset_header_for(view);
   state     = dlmalloc_offset_state_for(view);
   directory = dlmalloc_offset_directory_for(view);
   count     = header->segment_count;

   if( ! dlmalloc_offset_directory_valid_locked(view))
      return(0);

   free_blocks = 0;
   for(index = 0; index < count; ++index)
   {
      entry = &directory[index];
      if( ! dlmalloc_offset_segment_valid_locked(view, index, entry,
                                                 &free_blocks))
         return(0);
      view->verified |= (uint64_t)1 << index;
   }

   linked = 0;
   for(bucket = 0; bucket < DLMALLOC_OFFSET_BUCKETS; ++bucket)
   {
      tail   = 0;
      offset = state->bucket_head[bucket];
      while(offset)
      {
         if( ! dlmalloc_offset_link_target_valid(view, offset))
            return(0);
         block = dlmalloc_offset_block_at(view, offset);
         if( ! block || dlmalloc_offset_block_inuse(block) ||
             dlmalloc_offset_bucket_for(dlmalloc_offset_block_size(block)) !=
                bucket)
            return(0);
         links = dlmalloc_offset_links_at(view, offset);
         if(links->prev != tail)
            return(0);
         if(links->next)
         {
            if( ! dlmalloc_offset_link_target_valid(view, links->next) ||
                dlmalloc_offset_links_at(view, links->next)->prev != offset)
               return(0);
            other = dlmalloc_offset_block_at(view, links->next);
            if( ! other || dlmalloc_offset_block_inuse(other))
               return(0);
         }
         tail   = offset;
         offset = links->next;
         ++linked;
         if(linked > free_blocks)
            return(0);
      }
      if(tail != state->bucket_tail[bucket])
         return(0);
   }
   /* Each free block must sit in exactly one list: no more and no fewer. */
   if(linked != free_blocks)
      return(0);

   for(bucket = 0; bucket < DLMALLOC_OFFSET_BUCKETS; ++bucket)
   {
      uintptr_t bit;
      uintptr_t mask;

      mask = (uintptr_t)1 << bucket;
      bit  = state->bucket_head[bucket] ? mask : 0;
      if((state->bucket_map & mask) != bit)
         return(0);
   }
   return(1);
}

/* ------------------------------------------------------------ lock access */

/*
  Giving the CPU up, declared rather than included: this is an amalgamation and
  takes no headers. On Windows the embedder has already pulled in <windows.h>
  (mulle-mmap.h does), which is what WINAPI is standing in for.
*/
#if (defined(_WIN32) || defined(WIN32)) && defined(WINAPI)
#  define DLMALLOC_OFFSET_YIELD()   ((void) SleepEx(0, FALSE))
#elif defined(__unix__) || defined(__APPLE__) || defined(__MACH__)
   extern int sched_yield(void);
#  define DLMALLOC_OFFSET_YIELD()   ((void) sched_yield())
#else
#  define DLMALLOC_OFFSET_YIELD()   ((void) 0)
#endif

/* Tell the CPU we are spinning. There is no portable C for this. */
#if defined(__GNUC__) || defined(__clang__)
#  if defined(__i386__) || defined(__x86_64__)
#     define DLMALLOC_OFFSET_PAUSE()  __asm__ __volatile__( "pause" ::: "memory")
#  elif defined(__aarch64__) || defined(__arm__)
#     define DLMALLOC_OFFSET_PAUSE()  __asm__ __volatile__( "yield" ::: "memory")
#  else
#     define DLMALLOC_OFFSET_PAUSE()  ((void) 0)
#  endif
#else
#  define DLMALLOC_OFFSET_PAUSE()     ((void) 0)
#endif

/*
  Spins before the first yield: a pause is tens of cycles and the critical
  section is about a hundred nanoseconds, so this covers the usual case.
*/
#define DLMALLOC_OFFSET_SPIN_LIMIT  256u

/*
  Take the lock: one word in the shared header, held for a few stores.

  A waiter does not spin on the exchange. That is a read-modify-write, so every
  waiter takes the cache line exclusively on every iteration and the line
  ping-pongs between them; spinning on a plain load instead leaves the line
  shared and only reaches for the exchange once the lock looks free
  (test-and-test-and-set). After a bounded spin it gives the CPU up, because the
  holder has to be scheduled for anybody to make progress at all -- without
  that, N processes on one arena spend nearly all their time stealing the line
  from each other: 16 of them measured 57 us per operation.
*/
static int dlmalloc_offset_lock(dlmalloc_offset_view *view)
{
   struct dlmalloc_offset_header *header;
   unsigned spins;

   header = dlmalloc_offset_header_for(view);
   spins  = 0;
#if defined(__GNUC__) || defined(__clang__)
   for(;;)
   {
      if(__atomic_load_n(&header->lock, __ATOMIC_RELAXED) == 0 &&
         __atomic_exchange_n(&header->lock, DLMALLOC_OFFSET_LOCKED,
                             __ATOMIC_ACQUIRE) == 0)
         return(1);
      if(++spins < DLMALLOC_OFFSET_SPIN_LIMIT)
         DLMALLOC_OFFSET_PAUSE();
      else
      {
         spins = 0;
         DLMALLOC_OFFSET_YIELD();
      }
   }
#elif defined(_MSC_VER) && defined(WIN32)
   for(;;)
   {
      if(*(volatile LONG *) &header->lock == 0 &&
         interlockedcompareexchange((volatile LONG *)&header->lock,
                                    (LONG)DLMALLOC_OFFSET_LOCKED, 0) == 0)
         return(1);
      if(++spins < DLMALLOC_OFFSET_SPIN_LIMIT)
         DLMALLOC_OFFSET_PAUSE();
      else
      {
         spins = 0;
         DLMALLOC_OFFSET_YIELD();
      }
   }
#else
   (void)header;
   (void)spins;
   return(0);
#endif
}

static void dlmalloc_offset_unlock(dlmalloc_offset_view *view)
{
   struct dlmalloc_offset_header *header;

   header = dlmalloc_offset_header_for(view);
#if defined(__GNUC__) || defined(__clang__)
   __atomic_store_n(&header->lock, 0, __ATOMIC_RELEASE);
#elif defined(_MSC_VER) && defined(WIN32)
   interlockedexchange((volatile LONG *)&header->lock, 0);
#else
   (void)header;
#endif
}

/*
  Map any segments another process added since the last call. The lock is held,
  so the directory and epoch cannot change underneath us. Caller supplied
  callbacks do the actual mapping; dlmalloc stays platform-agnostic.
*/
static int dlmalloc_offset_refresh_locked(dlmalloc_offset_view *view)
{
   struct dlmalloc_offset_header *header;
   struct dlmalloc_offset_segment *entry;
   void *base;
   uintptr_t count;
   uintptr_t index;

   header     = dlmalloc_offset_header_for(view);
   view->grew = 0;
   if(header->epoch == view->epoch)
      return(1);
   count = header->segment_count;
   /*
     Either the arena has segments this view has not mapped yet, or it has
     grown the segment it has -- a reserved window extends segment 0 rather than
     publishing a second one -- and in both cases the room a failed allocation
     is about to retry for may already be there.
   */
   if(count > dlmalloc_offset_view_segments(view) ||
      header->total_offset_space > view->total_offset_space)
      view->grew = 1;
   for(index = dlmalloc_offset_view_segments(view); index < count; ++index)
   {
      if(view->local_base[index])
         continue;
      if( ! view->map_segment)
         return(0);
      entry = &dlmalloc_offset_directory_for(view)[index];
      if( ! view->map_segment(view->context, entry->backing, entry->size, &base) ||
          ! base)
         return(0);
      view->local_base[index] = base;
   }
   view->segment_count      = count;
   view->epoch              = header->epoch;
   view->total_offset_space = header->total_offset_space;

   /*
     Segments are appended, but segment 0 can also be grown in place, so its
     description is not a constant of the view. Put the published entry back
     into the cache, and drop the segment's check bit when its size moved: what
     the segment was checked against has changed, and the bytes that changed
     are the ones this process did not write. A cache that was never built is
     left alone -- it is built from the directory anyway.
   */
   entry = dlmalloc_offset_directory_for(view);
   if(view->segment0.size)
   {
      if(view->segment0.size != entry[0].size)
         view->verified &= ~(uint64_t)1;
      view->segment0 = entry[0];
   }
   return(1);
}

/*
  The per-operation prologue. The header's geometry is re-checked on every call
  rather than once at attach. That was tried the other way round and measured:
  moving the check to bind changed nothing, here or under contention, because
  those fields sit in the same line as the lock and taking the lock fetches that
  line anyway. Since it is free, it stays -- it is what turns a region that
  somebody corrupted into a failed operation instead of a walk into it.

  Everything that does change between calls, the epoch and the segment count,
  is read by refresh below.
*/
static int dlmalloc_offset_enter(dlmalloc_offset_view *view)
{
   if( ! dlmalloc_offset_header_valid(view))
      return(0);
   if( ! view->nolocking && ! dlmalloc_offset_lock(view))
      return(0);
   if( ! dlmalloc_offset_refresh_locked(view))
   {
      if( ! view->nolocking)
         dlmalloc_offset_unlock(view);
      return(0);
   }
   return(1);
}

static int dlmalloc_offset_leave(dlmalloc_offset_view *view)
{
   if( ! view->nolocking)
      dlmalloc_offset_unlock(view);
   return(1);
}

/* --------------------------------------------------------------- payload */

/*
  Cheap plausibility check for a caller supplied offset: aligned, inside a
  segment's block region, and describing a live block whose size keeps the
  payload inside that block. It does not prove the offset is a real block
  boundary, because that would need a walk over the whole arena on every call.
*/
static int dlmalloc_offset_payload_valid_locked(dlmalloc_offset_view *view,
                                                dlmalloc_offset_t offset)
{
#if DLMALLOC_OFFSET_CHECKED
   struct dlmalloc_offset_segment *entry;
   struct dlmalloc_offset_block *block;
   uintptr_t block_offset;
   uintptr_t end;
   uintptr_t index;
   uintptr_t size;
#endif

   /*
     These two are contract rather than validation: offset zero means "no
     allocation", and a misaligned offset cannot name a payload, so freeing
     either must leave the arena untouched. Both builds refuse them.
   */
   if( ! offset || (offset & (DLMALLOC_OFFSET_ALIGN - 1)) != 0)
      return(0);
#if DLMALLOC_OFFSET_CHECKED
   if( ! dlmalloc_offset_segment_find(view, offset, &index, &entry))
      return(0);
   end = entry->offset_base + entry->size;
   if(offset < entry->blocks_base + DLMALLOC_OFFSET_BLOCK_SIZE || offset >= end)
      return(0);
   block_offset = (uintptr_t)offset - DLMALLOC_OFFSET_BLOCK_SIZE;
   block        = dlmalloc_offset_block_at(view, block_offset);
   if( ! block || ! dlmalloc_offset_block_inuse(block))
      return(0);
   size = dlmalloc_offset_block_size(block);
   return(size >= DLMALLOC_OFFSET_MIN_BLOCK && size <= end - block_offset);
#else
   /*
     Unchecked: the offset is taken at its word, so an offset this arena never
     handed out, or handed out and got back already, is released again.
   */
   (void) view;
   return(1);
#endif
}

/* ------------------------------------------------------------ allocation */

/*
  Smallest usable block for a request: the request rounded up to the alignment,
  plus a header. Every block size is therefore a multiple of the alignment.
*/
static uintptr_t dlmalloc_offset_needed_block(uintptr_t bytes)
{
   uintptr_t needed;

   needed = dlmalloc_offset_align_up(bytes) + DLMALLOC_OFFSET_BLOCK_SIZE;
   if(needed < DLMALLOC_OFFSET_MIN_BLOCK)
      needed = DLMALLOC_OFFSET_MIN_BLOCK;
   return(needed);
}

/*
  First fit inside the request's own class, otherwise the head of the first
  non-empty higher class, whose blocks are all large enough by construction.
*/
static uintptr_t dlmalloc_offset_find_free(dlmalloc_offset_view *view,
                                          uintptr_t size)
{
   struct dlmalloc_offset_state *state;
   struct dlmalloc_offset_segment *entry;
   struct dlmalloc_offset_block *block;
   struct dlmalloc_offset_links *links;
   uintptr_t bucket;
   uintptr_t candidates;
   uintptr_t end;
   uintptr_t index;
   uintptr_t offset;
   uintptr_t total;

   state      = dlmalloc_offset_state_for(view);
   bucket     = dlmalloc_offset_bucket_for(size);
   candidates = state->bucket_map >> bucket;
   while(candidates)
   {
      if(candidates & 1)
      {
         offset = state->bucket_head[bucket];
         while(offset)
         {
            if( ! dlmalloc_offset_segment_find(view, offset, &index, &entry))
               return(0);
            end   = entry->offset_base + entry->size;
            block = dlmalloc_offset_block_at(view, offset);
            total = dlmalloc_offset_block_size(block);
            if(dlmalloc_offset_block_inuse(block) ||
               total < DLMALLOC_OFFSET_MIN_BLOCK || total > end - offset)
               return(0);
            links = dlmalloc_offset_links_at(view, offset);
            if( ! dlmalloc_offset_link_target_valid(view, links->next) ||
                ! dlmalloc_offset_link_target_valid(view, links->prev))
               return(0);
            if(total >= size)
               return(offset);
            offset = links->next;
         }
      }
      candidates >>= 1;
      ++bucket;
   }
   return(0);
}

/* Claim size bytes from the front of a free block that is already large enough. */
static uintptr_t dlmalloc_offset_take(dlmalloc_offset_view *view,
                                     uintptr_t block_offset,
                                     uintptr_t size)
{
   uintptr_t remainder;
   uintptr_t total;

   total = dlmalloc_offset_block_size(dlmalloc_offset_block_at(view,
                                                               block_offset));
   dlmalloc_offset_list_remove(view, block_offset);
   if(total >= size + DLMALLOC_OFFSET_MIN_BLOCK)
   {
      remainder = block_offset + size;
      dlmalloc_offset_set_size(view, block_offset, size, 1);
      dlmalloc_offset_set_size(view, remainder, total - size, 0);
      dlmalloc_offset_list_insert(view, remainder);
   }
   else
      dlmalloc_offset_set_size(view, block_offset, total, 1);
   return(block_offset);
}

static dlmalloc_offset_t dlmalloc_offset_alloc_locked(dlmalloc_offset_view *view,
                                                      uintptr_t bytes)
{
   struct dlmalloc_offset_header *header;
   uintptr_t block_offset;
   uintptr_t needed;

   header = dlmalloc_offset_header_for(view);
   needed = dlmalloc_offset_needed_block(bytes);
   if(needed > header->total_offset_space)
      return(0);
   block_offset = dlmalloc_offset_find_free(view, needed);
   if( ! block_offset)
      return(0);
   return((dlmalloc_offset_t)(dlmalloc_offset_take(view, block_offset, needed) +
                              DLMALLOC_OFFSET_BLOCK_SIZE));
}

/*
  Shrink a block to keep bytes and hand the tail back to the free lists, merging
  it with a free successor so the arena does not accumulate adjacent free blocks.
*/
static void dlmalloc_offset_trim_locked(dlmalloc_offset_view *view,
                                        uintptr_t block_offset,
                                        uintptr_t size,
                                        uintptr_t keep)
{
   struct dlmalloc_offset_block *successor;
   uintptr_t end;
   uintptr_t next_offset;
   uintptr_t remainder;
   uintptr_t tail;

   end       = dlmalloc_offset_blocks_end(view, block_offset);
   remainder = block_offset + keep;
   tail      = size - keep;
   dlmalloc_offset_set_size(view, block_offset, keep, 1);
   dlmalloc_offset_set_size(view, remainder, tail, 0);
   if(remainder + tail < end)
   {
      next_offset = remainder + tail;
      successor   = dlmalloc_offset_block_at(view, next_offset);
      if(successor && ! dlmalloc_offset_block_inuse(successor))
      {
         dlmalloc_offset_list_remove(view, next_offset);
         tail += dlmalloc_offset_block_size(successor);
         dlmalloc_offset_set_size(view, remainder, tail, 0);
      }
   }
   dlmalloc_offset_list_insert(view, remainder);
}

/*
  Return a block to the free lists after merging it with a free predecessor and
  a free successor inside the same segment. Every check happens before the first
  mutation, so a rejected call leaves the arena untouched.
*/
static int dlmalloc_offset_release_block(dlmalloc_offset_view *view,
                                         uintptr_t block_offset)
{
   struct dlmalloc_offset_segment *entry;
   struct dlmalloc_offset_block *block;
   struct dlmalloc_offset_block *neighbour;
   uintptr_t end;
   uintptr_t index;
   uintptr_t merged;
   uintptr_t next_offset;
   uintptr_t next_size;
   uintptr_t prev_size;
   uintptr_t size;
   uintptr_t start;

   if( ! dlmalloc_offset_segment_find(view, block_offset, &index, &entry))
      return(0);
   end   = entry->offset_base + entry->size;
   block = dlmalloc_offset_block_at(view, block_offset);
   size  = dlmalloc_offset_block_size(block);
   if(size < DLMALLOC_OFFSET_MIN_BLOCK || size > end - block_offset)
      return(0);

   start  = block_offset;
   merged = size;
   prev_size = block->prev_size;
   if(prev_size)
   {
      if(prev_size < DLMALLOC_OFFSET_MIN_BLOCK ||
         prev_size > block_offset - entry->blocks_base)
         return(0);
      neighbour = dlmalloc_offset_block_at(view, block_offset - prev_size);
      /* The predecessor's size is an invariant; only a free one merges. */
      if( ! neighbour || dlmalloc_offset_block_size(neighbour) != prev_size)
         return(0);
      if( ! dlmalloc_offset_block_inuse(neighbour))
      {
         start = block_offset - prev_size;
         merged += prev_size;
      }
   }

   /*
      The successor always sits at block_offset + size, whichever way the merge
      went, so its back offset must still name this block's pre-merge size.
   */
   next_offset = start + merged;
   next_size   = 0;
   if(next_offset < end)
   {
      neighbour = dlmalloc_offset_block_at(view, next_offset);
      if(neighbour && ! dlmalloc_offset_block_inuse(neighbour))
      {
         next_size = dlmalloc_offset_block_size(neighbour);
         if(neighbour->prev_size != size ||
            next_size < DLMALLOC_OFFSET_MIN_BLOCK ||
            next_size > end - next_offset)
            return(0);
         merged += next_size;
      }
   }

   if(start != block_offset)
      dlmalloc_offset_list_remove(view, start);
   if(next_size)
      dlmalloc_offset_list_remove(view, next_offset);
   dlmalloc_offset_set_size(view, start, merged, 0);
   dlmalloc_offset_list_insert(view, start);
   return(1);
}

/* ----------------------------------------------------------- public API */

int dlmalloc_offset_arena_init(void *base, size_t capacity)
{
   struct dlmalloc_offset_header *header;
   struct dlmalloc_offset_segment *directory;
   dlmalloc_offset_view view;
   uintptr_t blocks_offset;
   uintptr_t blocks_size;
   uintptr_t header_size;

   if( ! base || ((uintptr_t)base & (DLMALLOC_OFFSET_ALIGN - 1)) != 0 ||
       (uintptr_t)capacity > (uintptr_t)UINTPTR_MAX - (uintptr_t)base)
      return(0);
   header_size      = dlmalloc_offset_align_up(sizeof(struct dlmalloc_offset_header));
   blocks_offset    = dlmalloc_offset_align_up(header_size +
                                               sizeof(struct dlmalloc_offset_state));
   blocks_offset    = dlmalloc_offset_align_up(blocks_offset +
                                               DLMALLOC_OFFSET_MAX_SEGMENTS *
                                               sizeof(struct dlmalloc_offset_segment));
   if((uintptr_t)capacity < blocks_offset + DLMALLOC_OFFSET_MIN_BLOCK)
      return(0);
   blocks_size = ((uintptr_t)capacity - blocks_offset) &
                 ~(DLMALLOC_OFFSET_ALIGN - 1);

   memset(base, 0, capacity);
   header = (struct dlmalloc_offset_header *)base;
   header->version            = DLMALLOC_OFFSET_VERSION;
   header->header_size        = (uint32_t)header_size;
   header->capacity           = capacity;
   header->state_offset       = header_size;
   header->state_size         = sizeof(struct dlmalloc_offset_state);
   header->directory_offset   = dlmalloc_offset_align_up(header_size +
                                                         sizeof(struct dlmalloc_offset_state));
   header->directory_capacity = DLMALLOC_OFFSET_MAX_SEGMENTS;
   header->configuration      = dlmalloc_offset_configuration();
   header->lock               = 0;
   header->bucket_count       = DLMALLOC_OFFSET_BUCKETS;
   header->blocks_offset      = blocks_offset;
   header->blocks_size        = blocks_size;
   header->epoch              = 1;
   header->segment_count      = 1;
   header->total_offset_space = capacity;
   header->magic              = DLMALLOC_OFFSET_MAGIC;

   memset(&view, 0, sizeof(view));
   view.base               = base;
   view.capacity           = capacity;
   view.epoch              = header->epoch;
   view.segment_count      = 1;
   view.total_offset_space = capacity;
   view.verified           = 1;   /* built right here, so segment 0 is known good */
   view.local_base[0]      = base;

   directory = dlmalloc_offset_directory_for(&view);
   directory[0].offset_base = 0;
   directory[0].size        = capacity;
   directory[0].blocks_base = blocks_offset;
   directory[0].backing     = 0;

   dlmalloc_offset_set_size(&view, blocks_offset, blocks_size, 0);
   dlmalloc_offset_list_insert(&view, blocks_offset);
   return(1);
}

/*
  Bind a view to an arena and map the segments this process does not have yet.
  trusted says the caller built the region itself, so the segments that exist
  now need no checking; otherwise each one is checked the first time it is used.
*/
static int dlmalloc_offset_arena_bind(dlmalloc_offset_view *view,
                                      void *base,
                                      size_t capacity,
                                      int trusted)
{
   struct dlmalloc_offset_header *header;
   struct dlmalloc_offset_segment *entry;
   void *mapped;
   uintptr_t count;
   uintptr_t index;
   int locked;
   int valid;

   if( ! view || ! base)
      return(0);
   view->base          = base;
   view->capacity      = capacity;
   view->epoch         = 0;
   view->segment_count = 1;
   view->verified      = 0;
   view->corrupted     = 0;
   view->grew          = 0;
   view->local_base[0] = base;
   view->segment0.size = 0;   /* rebuilt from the newly bound base/capacity */

   /*
     nolocking has to hold here too: an arena that never takes the lock during
     its operations should not take it once at attach either, and on a target
     without an accessible atomic exchange that is the difference between a
     usable private arena and one that cannot be attached at all.
   */
   locked = ! view->nolocking;

   if( ! dlmalloc_offset_header_valid(view) ||
       (locked && ! dlmalloc_offset_lock(view)))
      return(0);

   header = dlmalloc_offset_header_for(view);
   for(index = 1; index < header->segment_count; ++index)
   {
      entry = &dlmalloc_offset_directory_for(view)[index];
      mapped = 0;
      if( ! view->map_segment ||
          ! view->map_segment(view->context, entry->backing, entry->size,
                              &mapped) ||
          ! mapped)
      {
         if(locked)
            dlmalloc_offset_unlock(view);
         return(0);
      }
      view->local_base[index] = mapped;
   }
   view->segment_count = header->segment_count;
   valid = dlmalloc_offset_directory_valid_locked(view);
   if(valid)
   {
      view->epoch              = header->epoch;
      view->total_offset_space = header->total_offset_space;
      if(trusted)
      {
         count = header->segment_count;
         view->verified = (count >= DLMALLOC_OFFSET_MAX_SEGMENTS) ?
                             ~(uint64_t)0 : (((uint64_t)1 << count) - 1);
      }
   }
   if(locked)
      dlmalloc_offset_unlock(view);
   return(valid);
}

/*
  Attach to an arena another process may have built, at any address. Checks the
  header and the directory and nothing else: the block tiling of a segment is
  checked by the first offset that lands in it (see segment_find), so attaching
  does not read the arena and cannot stall the other processes on the lock more
  than a directory scan. Use verify to have the whole arena checked here.
*/
int dlmalloc_offset_arena_attach(dlmalloc_offset_view *view,
                                 void *base,
                                 size_t capacity)
{
   return(dlmalloc_offset_arena_bind(view, base, capacity, 0));
}

/*
  Attach to a region this process has just built, taking the segments that exist
  now as already checked -- the caller wrote them, so walking them back would
  only re-read what it just wrote. This is what an init-then-attach sequence
  wants: dlmalloc_offset_arena_init has no view to say so on, so it is said
  here. Do not use it for a region another process could have corrupted.
*/
int dlmalloc_offset_arena_adopt(dlmalloc_offset_view *view,
                                void *base,
                                size_t capacity)
{
   return(dlmalloc_offset_arena_bind(view, base, capacity, 1));
}

void dlmalloc_offset_arena_detach(dlmalloc_offset_view *view)
{
   struct dlmalloc_offset_segment *entry;
   uintptr_t index;

   if( ! view)
      return;
   if(view->unmap_segment)
      for(index = 1; index < view->segment_count; ++index)
         if(view->local_base[index])
         {
            entry = &dlmalloc_offset_directory_for(view)[index];
            view->unmap_segment(view->context, view->local_base[index],
                                entry->size);
         }
   memset(view, 0, sizeof(*view));
}

/*
  Grow a segment in place instead of publishing another one. This is for an
  embedder whose grow_segment hands back a range that continues the segment
  before it, which a reserved window always does: the whole window is one
  mapping, so what comes back is the next bytes of segment 0.

  It is worth doing for two reasons. A second segment costs the segment0 fast
  path on every later call, because segment_find falls back to searching the
  directory; and a block cannot cross a segment boundary, so a request the arena
  already has room for can fail because the room is spread over segments. An
  arena that extends keeps one segment for life and keeps both.

  entry->tail_size, which set_size maintains, is what makes this constant time:
  the block that ends the segment is the one the new bytes must merge with or
  follow, and finding it without a record would mean walking the segment.

  Segment 0 only, and only while it is the arena's only segment: the segment0
  cache, the fast path and the check bit this keeps are all about that one
  segment. Called with the lock held, and it leaves with the same view the
  caller handed in. Returns 0 when the arena does not describe a usable tail
  block, and arena_grow appends a segment instead.
*/
static int dlmalloc_offset_extend_locked(dlmalloc_offset_view *view,
                                         struct dlmalloc_offset_segment *entry,
                                         uintptr_t size)
{
   struct dlmalloc_offset_header *header;
   struct dlmalloc_offset_block *block;
   uintptr_t end;
   uintptr_t start;
   uintptr_t tail;
   uintptr_t total;

   /*
     The tail record is a claim about the arena, so check it before acting on
     it: a real block size, aligned, inside the segment. A wrong one costs a
     segment append, never a corrupt arena.
   */
   tail = entry->tail_size;
   if(tail < DLMALLOC_OFFSET_MIN_BLOCK ||
      tail > entry->size ||
      (tail & (DLMALLOC_OFFSET_ALIGN - 1)) != 0)
      return(0);

   end   = entry->offset_base + entry->size;
   start = end - tail;
   block = dlmalloc_offset_block_at(view, start);
   if( ! block || dlmalloc_offset_block_size(block) != tail)
      return(0);

   /*
     A segment has to be known good before it is changed: from here on it is
     half written, and a lookup that ran the lazy check now would walk exactly
     that. block_at above has already had the segment checked in the ordinary
     case -- the allocation that failed went through it -- so this costs
     nothing when it is not needed, and it is what makes growing an arena this
     process has only just attached safe.
   */
   if( ! dlmalloc_offset_segment_check(view, 0, entry))
      return(0);

   header      = dlmalloc_offset_header_for(view);
   entry->size = entry->size + size;

   /*
     segment_find answers a one-segment arena from the view's copy of entry 0,
     so that copy has to describe the extended segment before the first lookup
     in the new bytes: the block_at and set_size calls below go through it, and
     a copy that still said the old size would refuse the offset. The lookups
     above are what built it; if it does not exist, it will be built from the
     directory, which already carries the new size.
   */
   if(view->segment0.size)
      view->segment0 = *entry;

   if(dlmalloc_offset_block_inuse(block))
   {
      /*
        The segment ended in a live block, so the extension is a block of its
        own. It records the distance back to that block itself: set_size writes
        a block's size into its successor, and the last block of a segment has
        no successor.
      */
      start = end;
      total = size;
      dlmalloc_offset_block_at(view, start)->prev_size = tail;
   }
   else
   {
      /* The segment ended in a free block: the new bytes are its new tail. */
      dlmalloc_offset_list_remove(view, start);
      total = tail + size;
   }
   dlmalloc_offset_set_size(view, start, total, 0);
   dlmalloc_offset_list_insert(view, start);

   header->total_offset_space = end + size;
   header->epoch              = header->epoch + 1;
   view->epoch                = header->epoch;
   view->total_offset_space   = header->total_offset_space;

   /*
     This process wrote the new bytes and checked the rest, so it keeps its
     check bit for the segment. Every other process drops its own when refresh
     sees the new size, and checks the segment again.
   */
   return(1);
}

int dlmalloc_offset_arena_grow(dlmalloc_offset_view *view, size_t min_size)
{
   struct dlmalloc_offset_header *header;
   struct dlmalloc_offset_segment *directory;
   struct dlmalloc_offset_segment *entry;
   struct dlmalloc_offset_block *block;
   void *base;
   uintptr_t backing;
   uintptr_t count;
   uintptr_t last;
   uintptr_t offset_base;
   uintptr_t size;

   if( ! view || ! view->grow_segment)
      return(0);
   if( ! dlmalloc_offset_enter(view))
      return(0);

   header    = dlmalloc_offset_header_for(view);
   directory = dlmalloc_offset_directory_for(view);
   count     = header->segment_count;

   /*
     A segment of this arena failed its check, so the failure that led here was
     not lack of space. Adding a segment would not repair it and would consume
     the window doing it; refuse, so the caller fails once and honestly.
   */
   if(view->corrupted)
   {
      dlmalloc_offset_leave(view);
      return(0);
   }

   /*
     Another process added a segment between the allocation that failed and
     this call: enter() refreshed and saw it, and that segment is the room the
     caller is about to retry for. Adding one here would answer a shortfall that
     is already covered. Processes that hit a full arena together would
     otherwise add 2S, 4S, 8S and 16S for one request -- address space for the
     window embedder, and actual disk for the file-backed one.

     Report success and let the caller retry. If the retry still fails, no
     segment appeared in the meantime, refresh cleared this, and the next call
     here adds one as usual. Consuming it here also bounds the retry loop of a
     caller that cannot get back into the arena at all.
   */
   if(view->grew)
   {
      view->grew = 0;
      dlmalloc_offset_leave(view);
      return(1);
   }

   if(count >= DLMALLOC_OFFSET_MAX_SEGMENTS)
   {
      dlmalloc_offset_leave(view);
      return(0);
   }

   last = directory[count - 1].size;
   size = last * 2;
   if(size < last || size < (uintptr_t)min_size)
      size = (uintptr_t)min_size;
   if(size < DLMALLOC_OFFSET_MIN_SEGMENT)
      size = DLMALLOC_OFFSET_MIN_SEGMENT;
   size = dlmalloc_offset_align_up(size);

   backing = 0;
   base    = 0;
   if( ! view->grow_segment(view->context, (size_t)size, &backing, &base,
                            &size) ||
       ! base || size < DLMALLOC_OFFSET_MIN_BLOCK)
   {
      dlmalloc_offset_leave(view);
      return(0);
   }
   size = dlmalloc_offset_align_up(size);

   /*
     An embedder can hand back a range that continues the previous segment. Both
     the address and the backing offset have to continue, because the entry is
     what another process maps from: a range that only looks adjacent in this
     process's address space would resolve offsets to the wrong bytes there.

     Only the last segment can be extended, and only while it is the only one.
   */
   if(count == 1 &&
      (uintptr_t)base == (uintptr_t)view->base + directory[0].size &&
      backing == directory[0].backing + directory[0].size &&
      dlmalloc_offset_extend_locked(view, &directory[0], size))
   {
      dlmalloc_offset_leave(view);
      return(1);
   }

   offset_base = header->total_offset_space;
   entry       = &directory[count];
   entry->offset_base = offset_base;
   entry->size        = size;
   entry->blocks_base = offset_base;
   entry->backing     = backing;

   header->segment_count      = count + 1;
   header->total_offset_space = offset_base + size;
   view->segment_count        = count + 1;
   view->local_base[count]    = base;
   /* This process just laid the segment out, so it is known good. */
   view->verified            |= (uint64_t)1 << count;

   view->total_offset_space = header->total_offset_space;

   /* The whole new segment starts as one free block. */
   block = (struct dlmalloc_offset_block *)base;
   block->prev_size = 0;
   dlmalloc_offset_set_size(view, offset_base, size, 0);
   dlmalloc_offset_list_insert(view, offset_base);

   header->epoch = header->epoch + 1;
   view->epoch   = header->epoch;

   dlmalloc_offset_leave(view);
   return(1);
}

int dlmalloc_offset_arena_refresh(dlmalloc_offset_view *view)
{
   if( ! dlmalloc_offset_enter(view))
      return(0);
   dlmalloc_offset_leave(view);
   return(1);
}

/*
  Check the whole arena on demand: the directory, every segment's block tiling
  and the free lists. Attach leaves this to first use, so this is how a caller
  asks for it up front -- a verification pass, a test oracle, or a debug build.
  Costs one pass over the arena, under the lock like any other operation; the
  segments it walks stay checked afterwards.
*/
int dlmalloc_offset_arena_verify(dlmalloc_offset_view *view)
{
   int result;

   if( ! dlmalloc_offset_enter(view))
      return(0);
   result = dlmalloc_offset_arena_valid_locked(view);
   dlmalloc_offset_leave(view);
   return(result);
}


/*
  Whether this process has found a segment that fails its check. Because a
  failed check surfaces as a failed allocation, which an embedder is entitled
  to retry after growing, this is how that retry can tell "the arena is out of
  space" from "the arena is damaged" and report the second one as itself.
  Once set, dlmalloc_offset_arena_grow refuses; attach and adopt clear it.
*/
int dlmalloc_offset_arena_corrupted(dlmalloc_offset_view *view)
{
   int result;

   if( ! view)
      return(0);
   if( ! dlmalloc_offset_enter(view))
      return(0);
   result = view->corrupted;
   dlmalloc_offset_leave(view);
   return(result);
}

dlmalloc_offset_t dlmalloc_offset_malloc(dlmalloc_offset_view *view, size_t bytes)
{
   dlmalloc_offset_t result;

   result = 0;
   if( ! dlmalloc_offset_enter(view))
      return(0);
   if((uintptr_t)bytes <= dlmalloc_offset_header_for(view)->total_offset_space)
      result = dlmalloc_offset_alloc_locked(view, (uintptr_t)bytes);
   dlmalloc_offset_leave(view);
   return(result);
}

/*
  Aligned allocation. Alignments up to the natural payload alignment come
  straight from the free lists. Larger ones carve the block: the aligned payload
  starts somewhere inside the block and the part in front of it goes back to the
  free lists, merging with a free predecessor if there is one. A preamble too
  small to be a block is skipped by stepping one more alignment unit forward.
*/
dlmalloc_offset_t dlmalloc_offset_memalign(dlmalloc_offset_view *view,
                                           size_t alignment,
                                           size_t bytes)
{
   struct dlmalloc_offset_header *header;
   struct dlmalloc_offset_segment *entry;
   dlmalloc_offset_t result;
   uintptr_t aligned;
   uintptr_t block_offset;
   uintptr_t index;
   uintptr_t local_block;
   uintptr_t local_payload;
   uintptr_t needed;
   uintptr_t payload;
   uintptr_t preamble;
   uintptr_t total;
   uintptr_t local_base;

   result = 0;
   if( ! dlmalloc_offset_enter(view))
      return(0);
   if(alignment <= DLMALLOC_OFFSET_ALIGN)
   {
      if((uintptr_t)bytes <= dlmalloc_offset_header_for(view)->total_offset_space)
         result = dlmalloc_offset_alloc_locked(view, (uintptr_t)bytes);
      dlmalloc_offset_leave(view);
      return(result);
   }
   if((alignment & (alignment - 1)) != 0 || alignment > DLMALLOC_OFFSET_MAX_ALIGN)
   {
      dlmalloc_offset_leave(view);
      return(0);
   }
   header = dlmalloc_offset_header_for(view);
   needed = dlmalloc_offset_needed_block((uintptr_t)bytes);
   /*
      Reserve a header plus two alignment steps for the preamble: one step can
      be wasted because the first aligned payload leaves a preamble that is too
      small to be a block.
   */
   if(needed + DLMALLOC_OFFSET_BLOCK_SIZE + 2 * alignment <=
      header->total_offset_space)
   {
      block_offset = dlmalloc_offset_find_free(view,
                          needed + DLMALLOC_OFFSET_BLOCK_SIZE + 2 * alignment);
      if(block_offset &&
         dlmalloc_offset_segment_find(view, block_offset, &index, &entry))
      {
         total = dlmalloc_offset_block_size(
                    dlmalloc_offset_block_at(view, block_offset));
         local_base = (uintptr_t)dlmalloc_offset_view_base(view, index);
         local_block = local_base + (block_offset - entry->offset_base);
         local_payload = (local_block + DLMALLOC_OFFSET_BLOCK_SIZE + alignment - 1) &
                         ~(uintptr_t)(alignment - 1);
         payload = block_offset + (local_payload - local_block);
         preamble = payload - DLMALLOC_OFFSET_BLOCK_SIZE - block_offset;
         if(preamble && preamble < DLMALLOC_OFFSET_MIN_BLOCK)
         {
            payload += alignment;
            preamble = payload - DLMALLOC_OFFSET_BLOCK_SIZE - block_offset;
         }
         if(payload + needed <= block_offset + total)
         {
            aligned = payload - DLMALLOC_OFFSET_BLOCK_SIZE;
            dlmalloc_offset_list_remove(view, block_offset);
            if(preamble)
               dlmalloc_offset_set_size(view, block_offset, preamble, 1);
            /* Claim the whole remainder first so its geometry is coherent. */
            dlmalloc_offset_set_size(view, aligned, total - preamble, 1);
            if(preamble && ! dlmalloc_offset_release_block(view, block_offset))
               result = 0;
            else
            {
               /* Anything beyond the request goes back to the free lists. */
               if(total - preamble - needed >= DLMALLOC_OFFSET_MIN_BLOCK)
                  dlmalloc_offset_trim_locked(view, aligned,
                                              total - preamble, needed);
               result = (dlmalloc_offset_t)payload;
            }
         }
      }
   }
   dlmalloc_offset_leave(view);
   return(result);
}

dlmalloc_offset_t dlmalloc_offset_calloc(dlmalloc_offset_view *view,
                                         size_t count,
                                         size_t bytes)
{
   dlmalloc_offset_t result;
   uintptr_t total;

   if(bytes != 0 && count > SIZE_MAX / bytes)
      return(0);
   total = (uintptr_t)count * (uintptr_t)bytes;
   result = 0;
   if( ! dlmalloc_offset_enter(view))
      return(0);
   if(total <= dlmalloc_offset_header_for(view)->total_offset_space)
   {
      result = dlmalloc_offset_alloc_locked(view, total);
      if(result)
      {
         struct dlmalloc_offset_segment *entry;
         uintptr_t index;

         if(dlmalloc_offset_segment_find(view, result, &index, &entry))
            memset((char *)dlmalloc_offset_view_base(view, index) +
                      (result - entry->offset_base), 0, (size_t)total);
      }
   }
   dlmalloc_offset_leave(view);
   return(result);
}

/*
  Resize an allocation. The offset stays valid when the block can shrink in
  place or absorb a free successor. Otherwise the payload moves, the returned
  offset differs, and the caller must publish the new one: an offset is the
  allocation's persistent identity, so a move cannot be hidden.
*/
dlmalloc_offset_t dlmalloc_offset_realloc(dlmalloc_offset_view *view,
                                          dlmalloc_offset_t offset,
                                          size_t bytes)
{
   struct dlmalloc_offset_segment *entry;
   struct dlmalloc_offset_block *block;
   dlmalloc_offset_t result;
   uintptr_t block_offset;
   uintptr_t end;
   uintptr_t index;
   uintptr_t needed;
   uintptr_t next_offset;
   uintptr_t next_size;
   uintptr_t size;
   uintptr_t total;
   size_t copy;

   if( ! offset)
      return(dlmalloc_offset_malloc(view, bytes));
   result = 0;
   if( ! dlmalloc_offset_enter(view))
      return(0);
   if((uintptr_t)bytes > dlmalloc_offset_header_for(view)->total_offset_space ||
      ! dlmalloc_offset_payload_valid_locked(view, offset))
   {
      dlmalloc_offset_leave(view);
      return(0);
   }
   block_offset = (uintptr_t)offset - DLMALLOC_OFFSET_BLOCK_SIZE;
   if( ! dlmalloc_offset_segment_find(view, block_offset, &index, &entry))
   {
      dlmalloc_offset_leave(view);
      return(0);
   }
   block  = dlmalloc_offset_block_at(view, block_offset);
   size   = dlmalloc_offset_block_size(block);
   end    = entry->offset_base + entry->size;
   needed = dlmalloc_offset_needed_block((uintptr_t)bytes);

   if(needed <= size)
   {
      if(size - needed >= DLMALLOC_OFFSET_MIN_BLOCK)
         dlmalloc_offset_trim_locked(view, block_offset, size, needed);
      result = offset;
   }
   else
   {
      next_offset = block_offset + size;
      if(next_offset < end)
      {
         next_size = dlmalloc_offset_block_size(
                        dlmalloc_offset_block_at(view, next_offset));
         if( ! dlmalloc_offset_block_inuse(
                 dlmalloc_offset_block_at(view, next_offset)) &&
             size + next_size >= needed)
         {
            dlmalloc_offset_list_remove(view, next_offset);
            total = size + next_size;
            if(total - needed >= DLMALLOC_OFFSET_MIN_BLOCK)
            {
               dlmalloc_offset_set_size(view, block_offset, needed, 1);
               dlmalloc_offset_set_size(view, block_offset + needed,
                                        total - needed, 0);
               dlmalloc_offset_list_insert(view, block_offset + needed);
            }
            else
               dlmalloc_offset_set_size(view, block_offset, total, 1);
            result = offset;
         }
      }
   }
   if( ! result)
   {
      result = dlmalloc_offset_alloc_locked(view, (uintptr_t)bytes);
      if(result)
      {
         struct dlmalloc_offset_segment *dest;
         uintptr_t dest_index;

         total = size - DLMALLOC_OFFSET_BLOCK_SIZE;
         copy  = total < (uintptr_t)bytes ? (size_t)total : bytes;
         if(dlmalloc_offset_segment_find(view, result, &dest_index, &dest))
            memcpy((char *)dlmalloc_offset_view_base(view, dest_index) +
                      (result - dest->offset_base),
                   (char *)dlmalloc_offset_view_base(view, index) +
                      (offset - entry->offset_base),
                   copy);
         dlmalloc_offset_release_block(view, block_offset);
      }
   }
   dlmalloc_offset_leave(view);
   return(result);
}

int dlmalloc_offset_free(dlmalloc_offset_view *view, dlmalloc_offset_t offset)
{
   int result;

   result = 0;
   if( ! dlmalloc_offset_enter(view))
      return(0);
   if(dlmalloc_offset_payload_valid_locked(view, offset))
      result = dlmalloc_offset_release_block(view,
                                             (uintptr_t)offset -
                                             DLMALLOC_OFFSET_BLOCK_SIZE);
   dlmalloc_offset_leave(view);
   return(result);
}

void *dlmalloc_offset_resolve(dlmalloc_offset_view *view, dlmalloc_offset_t offset)
{
   struct dlmalloc_offset_segment *entry;
   void *pointer;
   uintptr_t index;

   pointer = 0;
   if( ! dlmalloc_offset_enter(view))
      return(0);
   if(dlmalloc_offset_payload_valid_locked(view, offset) &&
      dlmalloc_offset_segment_find(view, offset, &index, &entry))
      pointer = (char *)dlmalloc_offset_view_base(view, index) +
                (offset - entry->offset_base);
   dlmalloc_offset_leave(view);
   return(pointer);
}

size_t dlmalloc_offset_usable_size(dlmalloc_offset_view *view,
                                   dlmalloc_offset_t offset)
{
   size_t result;

   result = 0;
   if( ! dlmalloc_offset_enter(view))
      return(0);
   if(dlmalloc_offset_payload_valid_locked(view, offset))
      result = (size_t)(dlmalloc_offset_block_size(
                          dlmalloc_offset_block_at(view,
                             (uintptr_t)offset - DLMALLOC_OFFSET_BLOCK_SIZE)) -
                        DLMALLOC_OFFSET_BLOCK_SIZE);
   dlmalloc_offset_leave(view);
   return(result);
}