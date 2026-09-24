/*
  mulle-dlmalloc: a relocatable allocator for one fixed-capacity shared arena.

  The arena never stores a process-local address. Free-list links and every
  block's distance back to its predecessor are arena-relative offsets, so the
  same bytes work from a process that maps the arena at any address. A returned
  offset is the arena-relative address of user payload; adding the local base
  yields a pointer that is valid only in the calling process.

  This file is the whole library. Historically it was Doug Lea's dlmalloc with
  the mspace API; that pointer-based allocator is gone. The only allocator here
  is the offset arena, which shares no state or algorithms with the old code.

  Layout

     [ header ][ bucket table ][ blocks .................... ][ tail padding ]

  Blocks tile [blocks_offset, blocks_offset + blocks_size) exactly and each one
  starts on a DLMALLOC_OFFSET_ALIGN boundary with a 16 byte header:

     size       total block size in bytes, low bit set while the block is in use
     prev_size  distance back to the predecessor's header, 0 for the first block

  Tiling means the successor of a block sits at "block_offset + size", so
  coalescing forward needs only the size and coalescing backward needs only
  prev_size. Neither needs a footer, so an in-use block is never written past
  the end of its payload. Every size change writes the new size into the
  successor's prev_size, which is the one rule that keeps this coherent.

  A free block keeps its bucket links in the first 16 payload bytes:

     next  arena offset of the next free block in the class, 0 ends the list
     prev  arena offset of the previous free block in the class, 0 starts it

  Classes are the powers of two starting at DLMALLOC_OFFSET_MIN_BLOCK, so class
  k covers [min * 2^k, min * 2^(k+1)). A block always sits in the class of its
  own size, which means a request only scans its own class for a first fit: a
  block in any higher class is large enough by construction. A bitmap of
  non-empty classes makes finding that first non-empty class a shift, so
  allocation costs at most one list walk inside a single class.

  Not provided, deliberately: growth, trimming back to a system allocator,
  multiple segments, allocations outside the arena, and crash recovery. The lock
  is a process-shared word in the header; a process that dies while holding it
  wedges the arena, and an arena abandoned mid-operation must be discarded.
*/

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef DLMALLOC_EXPORT
#define DLMALLOC_EXPORT extern
#endif

typedef uint64_t dlmalloc_offset_t;

typedef struct dlmalloc_offset_view
{
   void    *base;        /* this process's mapping of the arena */
   size_t  capacity;     /* mapped length; must equal the arena's capacity */
} dlmalloc_offset_view;

/*
  Relocatable single-arena API. The caller supplies and maps the shared region;
  a view is process-local and must be attached separately in each process.

  Offset zero means null or failure. A non-zero offset is the arena-relative
  byte offset of user payload. User references stored in the arena must also be
  offsets, not raw pointers, and a resolved pointer is valid only in the calling
  process while the object stays live. A realloc that has to move an allocation
  returns a new offset, which is the caller's cue to publish it, because an
  offset is the allocation's persistent identity.

  dlmalloc_offset_arena_init may be called again on arena memory that is known
  to be quiescent, which clears and rebuilds it, so no separate reset entry
  point is required. The first format version is one fixed-capacity region with
  no growth, no trimming, and no multi-segment support, and it offers no crash
  recovery after a process dies while holding the arena lock.
*/
DLMALLOC_EXPORT int dlmalloc_offset_arena_init(void *base, size_t capacity);
DLMALLOC_EXPORT int dlmalloc_offset_arena_attach(dlmalloc_offset_view *view,
                                                  void *base,
                                                  size_t capacity);
DLMALLOC_EXPORT void dlmalloc_offset_arena_detach(dlmalloc_offset_view *view);
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

#define DLMALLOC_OFFSET_MAGIC        UINT64_C(0x444c4d4f46465332)
#define DLMALLOC_OFFSET_VERSION      2U
#define DLMALLOC_OFFSET_LOCKED       1U
#define DLMALLOC_OFFSET_INUSE_BIT    ((uint64_t)1)
#define DLMALLOC_OFFSET_BLOCK_SIZE   ((uint64_t)16)
#define DLMALLOC_OFFSET_MIN_BLOCK    (DLMALLOC_OFFSET_BLOCK_SIZE + 16)
#define DLMALLOC_OFFSET_BUCKETS      32
#define DLMALLOC_OFFSET_MAX_ALIGN    ((uint64_t)1 << 20)

/*
  Payload alignment. 16 covers every fundamental type on the supported targets
  and divides the block header, so "payload = block + DLMALLOC_OFFSET_BLOCK_SIZE"
  keeps the payload as aligned as the block itself.
*/
#define DLMALLOC_OFFSET_ALIGN        ((uint64_t)16)

struct dlmalloc_offset_block
{
   uint64_t size;       /* total block size, low bit = in use */
   uint64_t prev_size;  /* distance back to the predecessor, 0 = first */
};

struct dlmalloc_offset_links
{
   uint64_t next;       /* arena offset, 0 ends the class list */
   uint64_t prev;       /* arena offset, 0 starts the class list */
};

struct dlmalloc_offset_state
{
   uint64_t bucket_map;                            /* bit set = class not empty */
   uint64_t bucket_head[DLMALLOC_OFFSET_BUCKETS];
   uint64_t bucket_tail[DLMALLOC_OFFSET_BUCKETS];
};

struct dlmalloc_offset_header
{
   uint64_t magic;
   uint32_t version;
   uint32_t header_size;
   uint64_t capacity;
   uint64_t state_offset;
   uint64_t state_size;
   uint64_t configuration;
   uint32_t lock;
   uint32_t bucket_count;
   uint64_t blocks_offset;
   uint64_t blocks_size;
};

/* ---------------------------------------------------------------- helpers */

static uint64_t dlmalloc_offset_configuration(void)
{
   uint64_t value;

   value  = (uint64_t)sizeof(void *) << 32;
   value |= (uint64_t)sizeof(size_t) << 40;
   value |= (uint64_t)DLMALLOC_OFFSET_ALIGN << 48;
   value |= (uint64_t)sizeof(struct dlmalloc_offset_state) << 16;
   value |= (uint64_t)DLMALLOC_OFFSET_BLOCK_SIZE << 8;
   value |= (uint64_t)DLMALLOC_OFFSET_BUCKETS;
   return(value);
}

static uint64_t dlmalloc_offset_align_up(uint64_t value)
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

static struct dlmalloc_offset_block *dlmalloc_offset_block_at(
   dlmalloc_offset_view *view,
   uint64_t offset)
{
   return((struct dlmalloc_offset_block *)((char *)view->base + offset));
}

static struct dlmalloc_offset_links *dlmalloc_offset_links_at(
   dlmalloc_offset_view *view,
   uint64_t offset)
{
   return((struct dlmalloc_offset_links *)((char *)view->base + offset +
                                           DLMALLOC_OFFSET_BLOCK_SIZE));
}

static uint64_t dlmalloc_offset_block_size(struct dlmalloc_offset_block *block)
{
   return(block->size & ~DLMALLOC_OFFSET_INUSE_BIT);
}

static int dlmalloc_offset_block_inuse(struct dlmalloc_offset_block *block)
{
   return((block->size & DLMALLOC_OFFSET_INUSE_BIT) != 0);
}

static uint64_t dlmalloc_offset_blocks_end(dlmalloc_offset_view *view)
{
   struct dlmalloc_offset_header *header;

   header = dlmalloc_offset_header_for(view);
   return(header->blocks_offset + header->blocks_size);
}

/* Class of a block size. Monotonic in size, so a higher class always fits. */
static uint64_t dlmalloc_offset_bucket_for(uint64_t size)
{
   uint64_t bucket;
   uint64_t limit;

   bucket = 0;
   limit  = DLMALLOC_OFFSET_MIN_BLOCK * 2;
   while(bucket + 1 < DLMALLOC_OFFSET_BUCKETS && size >= limit)
   {
      ++bucket;
      limit *= 2;
   }
   return(bucket);
}

/*
  Set a block's size and in-use state, then push the size into the successor's
  prev_size. This is the only place allowed to change a block's size.
*/
static void dlmalloc_offset_set_size(dlmalloc_offset_view *view,
                                     uint64_t block_offset,
                                     uint64_t size,
                                     int inuse)
{
   struct dlmalloc_offset_block *block;

   block = dlmalloc_offset_block_at(view, block_offset);
   block->size = size | (inuse ? DLMALLOC_OFFSET_INUSE_BIT : 0);
   if(block_offset + size < dlmalloc_offset_blocks_end(view))
      dlmalloc_offset_block_at(view, block_offset + size)->prev_size = size;
}

static int dlmalloc_offset_link_target_valid(dlmalloc_offset_view *view,
                                             uint64_t offset)
{
   struct dlmalloc_offset_header *header;

   if( ! offset)
      return(1);
   header = dlmalloc_offset_header_for(view);
   return(offset >= header->blocks_offset &&
          offset < header->blocks_offset + header->blocks_size &&
          (offset & (DLMALLOC_OFFSET_ALIGN - 1)) == 0);
}

static void dlmalloc_offset_list_insert(dlmalloc_offset_view *view,
                                        uint64_t block_offset)
{
   struct dlmalloc_offset_state *state;
   struct dlmalloc_offset_links *links;
   uint64_t bucket;
   uint64_t tail;

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
   state->bucket_map |= (uint64_t)1 << bucket;
}

static void dlmalloc_offset_list_remove(dlmalloc_offset_view *view,
                                        uint64_t block_offset)
{
   struct dlmalloc_offset_state *state;
   struct dlmalloc_offset_links *links;
   uint64_t bucket;

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
      state->bucket_map &= ~((uint64_t)1 << bucket);
   links->next = 0;
   links->prev = 0;
}

/* --------------------------------------------------------------- geometry */

static int dlmalloc_offset_header_valid(dlmalloc_offset_view *view)
{
   struct dlmalloc_offset_header *header;
   uint64_t capacity;

   if( ! view || ! view->base ||
       ((uintptr_t)view->base & (DLMALLOC_OFFSET_ALIGN - 1)) != 0 ||
       view->capacity > (size_t)UINTPTR_MAX ||
       view->capacity < sizeof(struct dlmalloc_offset_header))
      return(0);
   header   = dlmalloc_offset_header_for(view);
   capacity = (uint64_t)view->capacity;
   if(header->magic != DLMALLOC_OFFSET_MAGIC ||
      header->version != DLMALLOC_OFFSET_VERSION ||
      header->capacity != capacity ||
      header->header_size != dlmalloc_offset_align_up(sizeof(*header)) ||
      header->state_offset != header->header_size ||
      header->state_size != sizeof(struct dlmalloc_offset_state) ||
      header->configuration != dlmalloc_offset_configuration() ||
      header->bucket_count != DLMALLOC_OFFSET_BUCKETS)
      return(0);
   if(header->blocks_offset !=
         dlmalloc_offset_align_up(header->state_offset + header->state_size) ||
      header->blocks_offset > capacity ||
      header->blocks_size !=
         ((capacity - header->blocks_offset) & ~(DLMALLOC_OFFSET_ALIGN - 1)) ||
      header->blocks_size < DLMALLOC_OFFSET_MIN_BLOCK ||
      header->blocks_size > capacity - header->blocks_offset)
      return(0);
   return(1);
}

/*
  Full integrity walk, used when a process attaches. Verifies that blocks tile
  the region, that every back offset agrees with the block before it, and that
  the free lists describe exactly the free blocks, each of them once. This costs
  one pass over the arena, which is acceptable for an operation that happens
  once per process instead of once per allocation.
*/
static int dlmalloc_offset_arena_valid_locked(dlmalloc_offset_view *view)
{
   struct dlmalloc_offset_header *header;
   struct dlmalloc_offset_state *state;
   struct dlmalloc_offset_block *block;
   struct dlmalloc_offset_block *other;
   struct dlmalloc_offset_links *links;
   uint64_t bucket;
   uint64_t end;
   uint64_t free_blocks;
   uint64_t linked;
   uint64_t offset;
   uint64_t prev_size;
   uint64_t size;
   uint64_t tail;

   header = dlmalloc_offset_header_for(view);
   state  = dlmalloc_offset_state_for(view);
   end    = header->blocks_offset + header->blocks_size;

   free_blocks = 0;
   prev_size   = 0;
   offset      = header->blocks_offset;
   while(offset < end)
   {
      block = dlmalloc_offset_block_at(view, offset);
      size  = dlmalloc_offset_block_size(block);
      if(size < DLMALLOC_OFFSET_MIN_BLOCK ||
         (size & (DLMALLOC_OFFSET_ALIGN - 1)) != 0 ||
         size > end - offset ||
         block->prev_size != prev_size)
         return(0);
      if( ! dlmalloc_offset_block_inuse(block))
         ++free_blocks;
      prev_size = size;
      offset += size;
   }
   if(offset != end)
      return(0);

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
         if(dlmalloc_offset_block_inuse(block) ||
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
            if(dlmalloc_offset_block_inuse(other))
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
      uint64_t bit;
      uint64_t mask;

      mask = (uint64_t)1 << bucket;
      bit  = state->bucket_head[bucket] ? mask : 0;
      if((state->bucket_map & mask) != bit)
         return(0);
   }
   return(1);
}

/* ------------------------------------------------------------ lock access */

static int dlmalloc_offset_lock(dlmalloc_offset_view *view)
{
   struct dlmalloc_offset_header *header;

   header = dlmalloc_offset_header_for(view);
#if defined(__GNUC__) || defined(__clang__)
   while(__atomic_exchange_n(&header->lock, DLMALLOC_OFFSET_LOCKED,
                             __ATOMIC_ACQUIRE) != 0)
      ;
   return(1);
#elif defined(_MSC_VER) && defined(WIN32)
   while(interlockedcompareexchange((volatile LONG *)&header->lock,
                                    (LONG)DLMALLOC_OFFSET_LOCKED, 0) != 0)
      SleepEx(0, FALSE);
   return(1);
#else
   (void)header;
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

static int dlmalloc_offset_enter(dlmalloc_offset_view *view)
{
   if( ! dlmalloc_offset_header_valid(view))
      return(0);
   return(dlmalloc_offset_lock(view));
}

static int dlmalloc_offset_leave(dlmalloc_offset_view *view)
{
   dlmalloc_offset_unlock(view);
   return(1);
}

/* --------------------------------------------------------------- payload */

/*
  Cheap plausibility check for a caller supplied offset: aligned, inside the
  block region, and describing a live block whose size keeps the payload inside
  that block. It does not prove the offset is a real block boundary, because
  that would need a walk over the whole arena on every call.
*/
static int dlmalloc_offset_payload_valid_locked(dlmalloc_offset_view *view,
                                                dlmalloc_offset_t offset)
{
   struct dlmalloc_offset_header *header;
   struct dlmalloc_offset_block *block;
   uint64_t block_offset;
   uint64_t end;
   uint64_t size;

   header = dlmalloc_offset_header_for(view);
   end    = header->blocks_offset + header->blocks_size;
   if( ! offset || (offset & (DLMALLOC_OFFSET_ALIGN - 1)) != 0 ||
       offset < header->blocks_offset + DLMALLOC_OFFSET_BLOCK_SIZE ||
       offset >= end)
      return(0);
   block_offset = (uint64_t)offset - DLMALLOC_OFFSET_BLOCK_SIZE;
   block        = dlmalloc_offset_block_at(view, block_offset);
   if( ! dlmalloc_offset_block_inuse(block))
      return(0);
   size = dlmalloc_offset_block_size(block);
   return(size >= DLMALLOC_OFFSET_MIN_BLOCK && size <= end - block_offset);
}

/* ------------------------------------------------------------ allocation */

/*
  Smallest usable block for a request: the request rounded up to the alignment,
  plus a header. Every block size is therefore a multiple of the alignment.
*/
static uint64_t dlmalloc_offset_needed_block(uint64_t bytes)
{
   uint64_t needed;

   needed = dlmalloc_offset_align_up(bytes) + DLMALLOC_OFFSET_BLOCK_SIZE;
   if(needed < DLMALLOC_OFFSET_MIN_BLOCK)
      needed = DLMALLOC_OFFSET_MIN_BLOCK;
   return(needed);
}

/*
  First fit inside the request's own class, otherwise the head of the first
  non-empty higher class, whose blocks are all large enough by construction.
*/
static uint64_t dlmalloc_offset_find_free(dlmalloc_offset_view *view,
                                          uint64_t size)
{
   struct dlmalloc_offset_state *state;
   struct dlmalloc_offset_block *block;
   struct dlmalloc_offset_links *links;
   uint64_t bucket;
   uint64_t candidates;
   uint64_t end;
   uint64_t offset;
   uint64_t total;

   state      = dlmalloc_offset_state_for(view);
   end        = dlmalloc_offset_blocks_end(view);
   bucket     = dlmalloc_offset_bucket_for(size);
   candidates = state->bucket_map >> bucket;
   while(candidates)
   {
      if(candidates & 1)
      {
         offset = state->bucket_head[bucket];
         while(offset)
         {
            if( ! dlmalloc_offset_link_target_valid(view, offset))
               return(0);
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
static uint64_t dlmalloc_offset_take(dlmalloc_offset_view *view,
                                     uint64_t block_offset,
                                     uint64_t size)
{
   uint64_t remainder;
   uint64_t total;

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
                                                      uint64_t bytes)
{
   struct dlmalloc_offset_header *header;
   uint64_t block_offset;
   uint64_t needed;

   header = dlmalloc_offset_header_for(view);
   needed = dlmalloc_offset_needed_block(bytes);
   if(needed > header->blocks_size)
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
                                        uint64_t block_offset,
                                        uint64_t size,
                                        uint64_t keep)
{
   struct dlmalloc_offset_block *successor;
   uint64_t end;
   uint64_t next_offset;
   uint64_t remainder;
   uint64_t tail;

   end       = dlmalloc_offset_blocks_end(view);
   remainder = block_offset + keep;
   tail      = size - keep;
   dlmalloc_offset_set_size(view, block_offset, keep, 1);
   dlmalloc_offset_set_size(view, remainder, tail, 0);
   if(remainder + tail < end)
   {
      next_offset = remainder + tail;
      successor   = dlmalloc_offset_block_at(view, next_offset);
      if( ! dlmalloc_offset_block_inuse(successor))
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
  a free successor. Every check happens before the first mutation, so a rejected
  call leaves the arena untouched.
*/
static int dlmalloc_offset_release_block(dlmalloc_offset_view *view,
                                         uint64_t block_offset)
{
   struct dlmalloc_offset_header *header;
   struct dlmalloc_offset_block *block;
   struct dlmalloc_offset_block *neighbour;
   uint64_t end;
   uint64_t merged;
   uint64_t next_offset;
   uint64_t next_size;
   uint64_t prev_size;
   uint64_t size;
   uint64_t start;

   header = dlmalloc_offset_header_for(view);
   end    = header->blocks_offset + header->blocks_size;
   block  = dlmalloc_offset_block_at(view, block_offset);
   size   = dlmalloc_offset_block_size(block);
   if(size < DLMALLOC_OFFSET_MIN_BLOCK || size > end - block_offset)
      return(0);

   start  = block_offset;
   merged = size;
   prev_size = block->prev_size;
   if(prev_size)
   {
      if(prev_size < DLMALLOC_OFFSET_MIN_BLOCK ||
         prev_size > block_offset - header->blocks_offset)
         return(0);
      neighbour = dlmalloc_offset_block_at(view, block_offset - prev_size);
      /* The predecessor's size is an invariant; only a free one merges. */
      if(dlmalloc_offset_block_size(neighbour) != prev_size)
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
      if( ! dlmalloc_offset_block_inuse(neighbour))
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
   dlmalloc_offset_view view;
   uint64_t blocks_offset;
   uint64_t blocks_size;
   uint64_t header_size;

   if( ! base || ((uintptr_t)base & (DLMALLOC_OFFSET_ALIGN - 1)) != 0 ||
       (uint64_t)capacity > (uint64_t)UINTPTR_MAX - (uintptr_t)base)
      return(0);
   header_size   = dlmalloc_offset_align_up(sizeof(struct dlmalloc_offset_header));
   blocks_offset = dlmalloc_offset_align_up(header_size +
                                            sizeof(struct dlmalloc_offset_state));
   if((uint64_t)capacity < blocks_offset + DLMALLOC_OFFSET_MIN_BLOCK)
      return(0);
   blocks_size = ((uint64_t)capacity - blocks_offset) &
                 ~(DLMALLOC_OFFSET_ALIGN - 1);

   memset(base, 0, capacity);
   header = (struct dlmalloc_offset_header *)base;
   header->version       = DLMALLOC_OFFSET_VERSION;
   header->header_size   = (uint32_t)header_size;
   header->capacity      = capacity;
   header->state_offset  = header_size;
   header->state_size    = sizeof(struct dlmalloc_offset_state);
   header->configuration = dlmalloc_offset_configuration();
   header->lock          = 0;
   header->bucket_count  = DLMALLOC_OFFSET_BUCKETS;
   header->blocks_offset = blocks_offset;
   header->blocks_size   = blocks_size;
   header->magic         = DLMALLOC_OFFSET_MAGIC;

   view.base     = base;
   view.capacity = capacity;
   dlmalloc_offset_set_size(&view, blocks_offset, blocks_size, 0);
   dlmalloc_offset_list_insert(&view, blocks_offset);
   return(1);
}

int dlmalloc_offset_arena_attach(dlmalloc_offset_view *view,
                                 void *base,
                                 size_t capacity)
{
   dlmalloc_offset_view candidate;
   int valid;

   if( ! view)
      return(0);
   candidate.base     = base;
   candidate.capacity = capacity;
   if( ! dlmalloc_offset_header_valid(&candidate) ||
       ! dlmalloc_offset_lock(&candidate))
      return(0);
   valid = dlmalloc_offset_arena_valid_locked(&candidate);
   dlmalloc_offset_unlock(&candidate);
   if( ! valid)
      return(0);
   *view = candidate;
   return(1);
}

void dlmalloc_offset_arena_detach(dlmalloc_offset_view *view)
{
   if(view)
   {
      view->base     = 0;
      view->capacity = 0;
   }
}

dlmalloc_offset_t dlmalloc_offset_malloc(dlmalloc_offset_view *view, size_t bytes)
{
   dlmalloc_offset_t result;

   result = 0;
   if( ! dlmalloc_offset_enter(view))
      return(0);
   if((uint64_t)bytes <= view->capacity)
      result = dlmalloc_offset_alloc_locked(view, (uint64_t)bytes);
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
   dlmalloc_offset_t result;
   uint64_t aligned;
   uint64_t block_offset;
   uint64_t needed;
   uint64_t payload;
   uint64_t preamble;
   uint64_t total;
   uintptr_t base;

   result = 0;
   if( ! dlmalloc_offset_enter(view))
      return(0);
   if(alignment <= DLMALLOC_OFFSET_ALIGN)
   {
      if((uint64_t)bytes <= view->capacity)
         result = dlmalloc_offset_alloc_locked(view, (uint64_t)bytes);
      dlmalloc_offset_leave(view);
      return(result);
   }
   if((alignment & (alignment - 1)) != 0 || alignment > DLMALLOC_OFFSET_MAX_ALIGN)
   {
      dlmalloc_offset_leave(view);
      return(0);
   }
   header = dlmalloc_offset_header_for(view);
   needed = dlmalloc_offset_needed_block((uint64_t)bytes);
   /*
      Reserve a header plus two alignment steps for the preamble: one step can
      be wasted because the first aligned payload leaves a preamble that is too
      small to be a block.
   */
   if(needed + DLMALLOC_OFFSET_BLOCK_SIZE + 2 * alignment <=
      header->blocks_size)
   {
      block_offset = dlmalloc_offset_find_free(view,
                          needed + DLMALLOC_OFFSET_BLOCK_SIZE + 2 * alignment);
      if(block_offset)
      {
         total = dlmalloc_offset_block_size(
                    dlmalloc_offset_block_at(view, block_offset));
         base  = (uintptr_t)view->base;
         payload = (uint64_t)((base + block_offset + DLMALLOC_OFFSET_BLOCK_SIZE +
                               alignment - 1) & ~(uintptr_t)(alignment - 1)) -
                   base;
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
   uint64_t total;

   if(bytes != 0 && count > SIZE_MAX / bytes)
      return(0);
   total = (uint64_t)count * (uint64_t)bytes;
   result = 0;
   if( ! dlmalloc_offset_enter(view))
      return(0);
   if(total <= view->capacity)
   {
      result = dlmalloc_offset_alloc_locked(view, total);
      if(result)
         memset((char *)view->base + result, 0, (size_t)total);
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
   struct dlmalloc_offset_block *block;
   dlmalloc_offset_t result;
   uint64_t block_offset;
   uint64_t end;
   uint64_t needed;
   uint64_t next_offset;
   uint64_t next_size;
   uint64_t size;
   uint64_t total;
   size_t copy;

   if( ! offset)
      return(dlmalloc_offset_malloc(view, bytes));
   result = 0;
   if( ! dlmalloc_offset_enter(view))
      return(0);
   if((uint64_t)bytes > view->capacity ||
      ! dlmalloc_offset_payload_valid_locked(view, offset))
   {
      dlmalloc_offset_leave(view);
      return(0);
   }
   block_offset = (uint64_t)offset - DLMALLOC_OFFSET_BLOCK_SIZE;
   block        = dlmalloc_offset_block_at(view, block_offset);
   size         = dlmalloc_offset_block_size(block);
   end          = dlmalloc_offset_blocks_end(view);
   needed       = dlmalloc_offset_needed_block((uint64_t)bytes);

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
      result = dlmalloc_offset_alloc_locked(view, (uint64_t)bytes);
      if(result)
      {
         total = size - DLMALLOC_OFFSET_BLOCK_SIZE;
         copy  = total < (uint64_t)bytes ? (size_t)total : bytes;
         memcpy((char *)view->base + result, (char *)view->base + offset, copy);
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
                                             (uint64_t)offset -
                                             DLMALLOC_OFFSET_BLOCK_SIZE);
   dlmalloc_offset_leave(view);
   return(result);
}

void *dlmalloc_offset_resolve(dlmalloc_offset_view *view, dlmalloc_offset_t offset)
{
   void *pointer;

   pointer = 0;
   if( ! dlmalloc_offset_enter(view))
      return(0);
   if(dlmalloc_offset_payload_valid_locked(view, offset))
      pointer = (char *)view->base + offset;
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
                             (uint64_t)offset - DLMALLOC_OFFSET_BLOCK_SIZE)) -
                        DLMALLOC_OFFSET_BLOCK_SIZE);
   dlmalloc_offset_leave(view);
   return(result);
}