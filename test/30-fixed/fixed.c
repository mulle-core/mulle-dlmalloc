/*
  A single-segment arena is served from the view, not the segment directory.

  Every block primitive funnels through dlmalloc_offset_segment_find: block_at,
  links_at and blocks_end all call it, and set_size resolves once and reaches
  the successor through block_in. For a one-segment arena there is nothing to
  search, so segment_find answers from the view's copy of the directory entry
  instead, which is built on first use and whose size follows the segment when
  it is grown in place.

  This test pins both halves of that choice, and where a segment gets checked:

  1. a fixed arena takes the fast path, and its view-local copy of segment 0
     matches the shared directory;
  2. everything that consumes a segment description stays correct while it does
     so: malloc, calloc, memalign, realloc, free with coalescing, resolve and
     usable_size, checked against the arena's own integrity walk;
  3. a contiguous growth extends segment 0 instead of adding a segment, and a
     view that did not grow it takes the new length from the directory and
     checks the segment again, while an arena whose segments do not continue
     each other appends, keeps no view-local copy, and still resolves offsets
     in both segments through the directory;
  4. a segment's block tiling is checked by the first offset that lands in it
     and not by attach: attaching checks neither segment, and the segment an
     allocation lands in is checked by the time that allocation returns;
  5. the offsets neither build may release -- zero and a misaligned one -- leave
     the arena alone, and the double free that only a checked build can notice
     is pinned under DLMALLOC_OFFSET_CHECKED.

  Every number printed is reproducible, so stdout is compared against
  fixed.stdout.

  Like every consumer of this project, the test includes the implementation
  source rather than a header, because the project is an amalgamation and ships
  no public headers.
*/

/* Needed here already for MAP_ANONYMOUS. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/mman.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#undef DLMALLOC_OFFSET_CHECKED
#define DLMALLOC_OFFSET_CHECKED 1

#include "../../src/dlmalloc.c"

#define ARENA_CAPACITY   (256 * 1024)
#define WINDOW_CAPACITY  (1024 * 1024)
#define RACE_CAPACITY    (2048 * 1024)
#define FIRST_SEGMENT    (64 * 1024)

static int checks_run;
static int checks_failed;

static void report(int ok, const char *what)
{
   ++checks_run;
   if( ! ok)
      ++checks_failed;
   printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
}

/* The lock and the integrity walk are internal, so use them as an oracle. */
static int arena_is_valid(dlmalloc_offset_view *view)
{
   int ok;

   if( ! dlmalloc_offset_lock(view))
      return(0);
   ok = dlmalloc_offset_arena_valid_locked(view);
   dlmalloc_offset_unlock(view);
   return(ok);
}

/* Does the view-local copy of segment 0 agree with the shared directory? */
static int segment_zero_matches_directory(dlmalloc_offset_view *view)
{
   struct dlmalloc_offset_segment *entry;

   entry = &dlmalloc_offset_directory_for(view)[0];
   return(view->segment0.offset_base == entry->offset_base &&
          view->segment0.size        == entry->size &&
          view->segment0.blocks_base == entry->blocks_base &&
          view->segment0.backing     == entry->backing &&
          view->segment0.tail_size   == entry->tail_size);
}

/* Which segment does an offset live in? */
static uint64_t segment_of(dlmalloc_offset_view *view,
                           dlmalloc_offset_t offset)
{
   struct dlmalloc_offset_header *header;
   struct dlmalloc_offset_segment *directory;
   uint64_t index;

   header    = dlmalloc_offset_header_for(view);
   directory = dlmalloc_offset_directory_for(view);
   for(index = 0; index < header->segment_count; ++index)
      if(offset >= directory[index].offset_base &&
         offset < directory[index].offset_base + directory[index].size)
         return(index);
   return(DLMALLOC_OFFSET_MAX_SEGMENTS);
}

/*
  A fixed arena has no callbacks, so it can never grow and always has one
  segment: the case the fast path exists for.
*/
static void test_fixed_arena(void)
{
   dlmalloc_offset_view view;
   dlmalloc_offset_t    block[8];
   dlmalloc_offset_t    aligned;
   dlmalloc_offset_t    zeroed;
   dlmalloc_offset_t    original;
   dlmalloc_offset_t    moved;
   dlmalloc_offset_t    large;
   void                *mapping;
   char                *text;
   size_t               i;
   int                  ok;

   mapping = mmap(NULL, ARENA_CAPACITY, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
   if(mapping == MAP_FAILED)
   {
      report(0, "the fixed arena is mapped");
      return;
   }
   report(dlmalloc_offset_arena_init(mapping, ARENA_CAPACITY),
          "arena_init on a fixed arena");

   memset(&view, 0, sizeof(view));
   view.base     = mapping;
   view.capacity = ARENA_CAPACITY;
   report(dlmalloc_offset_arena_attach(&view, mapping, ARENA_CAPACITY),
          "attach without growth callbacks");
   report(dlmalloc_offset_view_segments(&view) == 1,
          "the fixed arena has a single segment");

   /*
     Attaching stopped walking the arena: it checks the header and the segment
     directory and leaves each segment's block tiling to the first offset that
     lands in it, so nothing is checked and nothing is read yet.
   */
   report(view.verified == 0,
          "attach checks no segment");
   report(view.segment0.size == 0,
          "attach does not build the view copy of segment 0");

   ok = 1;
   for(i = 0; i < 8; ++i)
   {
      block[i] = dlmalloc_offset_malloc(&view, 1000 + i);
      text     = block[i] ? (char *)dlmalloc_offset_resolve(&view, block[i]) : 0;
      ok      &= (text == (char *)mapping + block[i]);
      if(text)
         memset(text, (int) ('a' + i), 1000 + i);
   }
   report(ok, "every malloc resolves to base plus its offset");

   /* The first offset in the segment is what checked it, and the fast path ran. */
   report(view.verified == 1,
          "the first allocation checks the segment it lands in");
   report(view.segment0.size == ARENA_CAPACITY,
          "segment 0 is answered from the view, not the directory");
   report(segment_zero_matches_directory(&view),
          "the view copy of segment 0 matches the directory");
   report(dlmalloc_offset_usable_size(&view, block[0]) >= 1000,
          "usable_size covers the request");

   aligned = dlmalloc_offset_memalign(&view, 256, 777);
   text    = aligned ? (char *)dlmalloc_offset_resolve(&view, aligned) : 0;
   report(text != 0 && ((uintptr_t)text & 255) == 0,
          "memalign resolves to the alignment it promised");

   zeroed = dlmalloc_offset_calloc(&view, 4, 100);
   text   = zeroed ? (char *)dlmalloc_offset_resolve(&view, zeroed) : 0;
   ok     = text != 0;
   if(text)
      for(i = 0; i < 400; ++i)
         ok &= (text[i] == 0);
   report(ok, "calloc hands back zeroed memory");

   original = dlmalloc_offset_malloc(&view, 1010);
   text     = original ? (char *)dlmalloc_offset_resolve(&view, original) : 0;
   if(text)
      memset(text, 'z', 1010);
   report(text != 0, "a block is allocated for realloc");

   moved = dlmalloc_offset_realloc(&view, original, 64 * 1024);
   text  = moved ? (char *)dlmalloc_offset_resolve(&view, moved) : 0;
   ok    = text != 0;
   if(text)
      for(i = 0; i < 1010; ++i)
         ok &= (text[i] == 'z');
   report(ok, "realloc keeps the payload and resolves at its new offset");

   report(arena_is_valid(&view), "the arena is consistent after the churn");

   ok = 1;
   for(i = 0; i < 8; ++i)
      ok &= dlmalloc_offset_free(&view, block[i]);
   ok &= dlmalloc_offset_free(&view, aligned);
   ok &= dlmalloc_offset_free(&view, zeroed);
   ok &= dlmalloc_offset_free(&view, moved);
   report(ok, "every block is freed");
   report(arena_is_valid(&view), "the arena is consistent after freeing");

   /* Everything is free again, so one large request has to fit. */
   large = dlmalloc_offset_malloc(&view, ARENA_CAPACITY / 2);
   text  = large ? (char *)dlmalloc_offset_resolve(&view, large) : 0;
   report(text == (char *)mapping + large,
          "a large request reuses the coalesced segment");
   dlmalloc_offset_free(&view, large);

   report(segment_zero_matches_directory(&view),
          "the view copy survives the whole run");

   munmap(mapping, ARENA_CAPACITY);
}

/*
  The reserved-window embedder: the whole ceiling is mapped once and a segment
  is a range of it, so the callbacks are arithmetic.
*/
struct window
{
   char   *base;
   size_t  capacity;
};

static int grow_segment(void *context, size_t min_size,
                        uintptr_t *backing_offset, void **base, uintptr_t *size)
{
   struct window                 *w;
   struct dlmalloc_offset_header *header;
   uintptr_t                      high;
   uintptr_t                      available;
   uintptr_t                      want;

   w      = (struct window *) context;
   header = (struct dlmalloc_offset_header *) w->base;
   high   = header->total_offset_space;
   if(high >= (uintptr_t) w->capacity)
      return(0);
   available = (uintptr_t) w->capacity - high;
   want      = min_size ? (uintptr_t) min_size : DLMALLOC_OFFSET_MIN_SEGMENT;
   if(want > available)
      want = available;
   want &= ~((uintptr_t) DLMALLOC_OFFSET_ALIGN - 1);
   if(want < DLMALLOC_OFFSET_MIN_BLOCK)
      return(0);
   *backing_offset = high;
   *base           = w->base + high;
   *size           = want;
   return(1);
}

static int map_segment(void *context, uintptr_t backing_offset, size_t size,
                       void **base)
{
   struct window *w;

   w = (struct window *) context;
   if(backing_offset + (uintptr_t) size > (uintptr_t) w->capacity)
      return(0);
   *base = w->base + backing_offset;
   return(1);
}

static void unmap_segment(void *context, void *base, size_t size)
{
   (void) context;
   (void) base;
   (void) size;
}

/*
  A contiguous embedder does not need a second segment.

  The reserved window is one mapping, so grow_segment hands back the bytes that
  follow the segment before it. The arena extends that segment in place rather
  than publishing another one: the fast path in segment_find survives the
  growth, DLMALLOC_OFFSET_MAX_SEGMENTS stops being a ceiling, and no request is
  refused because the space it wanted was split by a segment boundary.

  Two views are in play. The one that grows keeps its own copy of segment 0 in
  step. The one that does not has to take the new length from the directory and
  drop its check bit for that segment, because the bytes that changed are the
  ones it did not write.
*/
static void test_extended_arena(void)
{
   struct dlmalloc_offset_header *header;
   struct window                 window;
   dlmalloc_offset_view           first;
   dlmalloc_offset_view           second;
   dlmalloc_offset_view           fresh;
   dlmalloc_offset_t              early;
   dlmalloc_offset_t              late;
   void                          *mapping;
   char                          *text;
   uintptr_t                      grown;
   unsigned int                   i;

   mapping = mmap(NULL, WINDOW_CAPACITY, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
   if(mapping == MAP_FAILED)
   {
      report(0, "the growable window is mapped");
      return;
   }
   window.base     = mapping;
   window.capacity = WINDOW_CAPACITY;
   header          = (struct dlmalloc_offset_header *) mapping;

   report(dlmalloc_offset_arena_init(mapping, FIRST_SEGMENT),
          "arena_init with one small segment");

   memset(&first, 0, sizeof(first));
   first.base          = mapping;
   first.capacity      = FIRST_SEGMENT;
   first.grow_segment  = grow_segment;
   first.map_segment   = map_segment;
   first.unmap_segment = unmap_segment;
   first.context       = &window;
   report(dlmalloc_offset_arena_attach(&first, mapping, FIRST_SEGMENT),
          "attach the growable window");

   /* This view is attached before the growth, so the growth has to reach it. */
   memset(&second, 0, sizeof(second));
   second.base          = mapping;
   second.capacity      = FIRST_SEGMENT;
   second.grow_segment  = grow_segment;
   second.map_segment   = map_segment;
   second.unmap_segment = unmap_segment;
   second.context       = &window;
   report(dlmalloc_offset_arena_attach(&second, mapping, FIRST_SEGMENT),
          "a second view attaches before the growth");
   early = dlmalloc_offset_malloc(&second, 8 * 1024);
   report(early != 0 && second.segment0.size == FIRST_SEGMENT,
          "it caches segment 0 at the length it was created with");

   report(dlmalloc_offset_arena_grow(&first, 2 * FIRST_SEGMENT),
          "grow extends the segment");
   grown = header->total_offset_space;
   report(grown > FIRST_SEGMENT, "the extension is in the offset space");
   report(dlmalloc_offset_view_segments(&first) == 1,
          "the arena still has one segment");
   report(first.segment0.size == grown,
          "the view that grew it keeps its segment 0 copy in step");

   report(dlmalloc_offset_arena_refresh(&second),
          "the other view refreshes");
   report(second.segment0.size == grown,
          "and takes the new length from the directory");
   report((second.verified & (uint64_t)1) == 0,
          "and drops the check bit for the segment that changed");

   /*
     Now it has to allocate past the length segment 0 was created with to get
     out of it at all, which only works if the growth reached this view.
   */
   late = 0;
   for(i = 0; i < 32 && ! late; ++i)
   {
      early = dlmalloc_offset_malloc(&second, 8 * 1024);
      if( ! early)
         break;
      if(early >= FIRST_SEGMENT)
         late = early;
   }
   text = late ? (char *)dlmalloc_offset_resolve(&second, late) : 0;
   report(late != 0, "an allocation lands past the first segment");
   report(text == (char *)mapping + late,
          "the extension resolves in a view that did not grow it");
   report((second.verified & (uint64_t)1) != 0,
          "and the segment is checked again on the next allocation");

   /*
     A view attached after the growth has no copy to refresh: it reads the
     directory the first time it needs segment 0, so it starts out grown.
   */
   memset(&fresh, 0, sizeof(fresh));
   fresh.base          = mapping;
   fresh.capacity      = FIRST_SEGMENT;
   fresh.grow_segment  = grow_segment;
   fresh.map_segment   = map_segment;
   fresh.unmap_segment = unmap_segment;
   fresh.context       = &window;
   report(dlmalloc_offset_arena_attach(&fresh, mapping, FIRST_SEGMENT),
          "a view attached after the growth");
   report(fresh.segment0.size == 0,
          "has no segment 0 copy before its first allocation");
   report(dlmalloc_offset_malloc(&fresh, 64) != 0 &&
          fresh.segment0.size == grown,
          "and takes the grown length from the directory");

   report(arena_is_valid(&second), "the extended arena is consistent");

   munmap(mapping, WINDOW_CAPACITY);
}

/*
  A discontiguous embedder still gets a second segment.

  Every segment is its own mapping here, so a growth cannot continue the one
  before it and the arena appends -- the file-backed case, and the reason the
  append path stays. The fast path is for one segment only, so with two the view
  keeps its segment 0 copy out of the way and each segment is checked by the
  first offset that lands in it.
*/
struct scatter
{
   void   *segment[ 4];
   size_t  size[ 4];
   int     count;
};

static int scatter_grow_segment(void *context, size_t min_size,
                                uintptr_t *backing_offset, void **base,
                                uintptr_t *size)
{
   struct scatter *s;
   void           *p;

   s = (struct scatter *) context;
   if(s->count >= 4 || min_size < DLMALLOC_OFFSET_MIN_BLOCK)
      return(0);
   p = mmap(NULL, min_size, PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
   if(p == MAP_FAILED)
      return(0);
   s->segment[s->count] = p;
   s->size[s->count]    = min_size;
   /*
     A backing offset deliberately not the one after the previous segment's, so
     that the arena cannot read the range as a continuation of it even if the
     system happened to place the mapping next to the one before.
   */
   *backing_offset = (uintptr_t) (s->count + 1) * (uintptr_t) 0x1000000;
   *base           = p;
   *size           = min_size;
   ++s->count;
   return(1);
}

static void test_appended_arena(void)
{
   struct scatter       scatter;
   dlmalloc_offset_view view;
   dlmalloc_offset_t    one;
   dlmalloc_offset_t    two;
   void                *mapping;
   unsigned int         i;

   memset(&scatter, 0, sizeof(scatter));
   mapping = mmap(NULL, FIRST_SEGMENT, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
   if(mapping == MAP_FAILED)
   {
      report(0, "the discontiguous arena is mapped");
      return;
   }

   report(dlmalloc_offset_arena_init(mapping, FIRST_SEGMENT),
          "arena_init with one small segment");
   memset(&view, 0, sizeof(view));
   view.base         = mapping;
   view.capacity     = FIRST_SEGMENT;
   view.grow_segment = scatter_grow_segment;
   view.context      = &scatter;
   report(dlmalloc_offset_arena_attach(&view, mapping, FIRST_SEGMENT),
          "attach the discontiguous arena");
   report(view.verified == 0 && view.segment0.size == 0,
          "attach checks and describes nothing yet");

   report(dlmalloc_offset_arena_grow(&view, 2 * FIRST_SEGMENT),
          "a range that does not continue segment 0 is appended");
   report(dlmalloc_offset_view_segments(&view) == 2,
          "the arena now has two segments");
   report(view.segment0.size == 0,
          "a two-segment arena keeps no copy of segment 0");
   report((view.verified & (uint64_t)1) == 0,
          "the segment the grow laid out is checked, segment 0 is not");

   one = dlmalloc_offset_malloc(&view, 48 * 1024);
   report(one != 0 && dlmalloc_offset_resolve(&view, one) != 0,
          "an allocation in segment 0 resolves");
   report((view.verified & ((uint64_t)1 << segment_of(&view, one))) != 0,
          "the segment an allocation lands in is checked");
   report(view.segment0.size == 0,
          "the two-segment view never falls back to the copy");

   two = dlmalloc_offset_malloc(&view, 32 * 1024);
   report(two >= FIRST_SEGMENT,
          "a second allocation lands in the appended segment");
   report((view.verified & ((uint64_t)1 << segment_of(&view, two))) != 0,
          "and the appended segment is checked when it is first used");
   report(arena_is_valid(&view), "the two-segment arena is consistent");

   munmap(mapping, FIRST_SEGMENT);
   for(i = 0; i < (unsigned int) scatter.count; ++i)
      munmap(scatter.segment[i], scatter.size[i]);
}

/*
  A failed segment check is not "out of space".

  Both arrive at the embedder as a failed allocation, and an embedder is
  entitled to retry one after growing. If it could not tell them apart, the
  retry loop would answer a corrupt segment by adding segments until the window
  ran out -- one scribbled block header, and the arena is finished. grow
  refuses once a check has failed, and this pins that, plus the way back out:
  rebuilding the region clears it.
*/
static void test_corrupt_arena_does_not_grow(void)
{
   struct dlmalloc_offset_header *header;
   struct dlmalloc_offset_block  *block;
   struct window                  window;
   dlmalloc_offset_view           view;
   void                          *mapping;
   uint64_t                       consumed;
   uint64_t                       segments;

   mapping = mmap(NULL, WINDOW_CAPACITY, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
   if(mapping == MAP_FAILED)
   {
      report(0, "the corrupt-window arena is mapped");
      return;
   }
   window.base     = mapping;
   window.capacity = WINDOW_CAPACITY;

   report(dlmalloc_offset_arena_init(mapping, FIRST_SEGMENT) &&
          FIRST_SEGMENT < WINDOW_CAPACITY,
          "a growable arena starts small, with window to spare");

   memset(&view, 0, sizeof(view));
   view.base          = mapping;
   view.capacity      = FIRST_SEGMENT;
   view.grow_segment  = grow_segment;
   view.map_segment   = map_segment;
   view.unmap_segment = unmap_segment;
   view.context       = &window;
   report(dlmalloc_offset_arena_attach(&view, mapping, FIRST_SEGMENT),
          "attach the growable arena");

   /* an impossible block size at the head of segment 0's tiling */
   header = (struct dlmalloc_offset_header *) mapping;
   block  = (struct dlmalloc_offset_block *)((char *) mapping +
                                             header->blocks_offset);
   block->size = 0;

   report(dlmalloc_offset_malloc(&view, 64) == 0,
          "an allocation that must use the broken segment fails");
   report(dlmalloc_offset_arena_corrupted(&view),
          "the arena is marked corrupt, not out of space");

   segments = dlmalloc_offset_view_segments(&view);
   consumed = header->total_offset_space;
   report( ! dlmalloc_offset_arena_grow(&view, 64),
          "grow refuses, instead of adding a segment around it");
   report(dlmalloc_offset_view_segments(&view) == segments &&
          header->total_offset_space == consumed,
          "so the window is left alone");

   /* the documented way out: rebuild the region and attach again */
   report(dlmalloc_offset_arena_init(mapping, FIRST_SEGMENT),
          "arena_init rebuilds the region");
   report(dlmalloc_offset_arena_attach(&view, mapping, FIRST_SEGMENT) &&
          ! dlmalloc_offset_arena_corrupted(&view),
          "attaching again clears the corrupt mark");
   report(dlmalloc_offset_arena_grow(&view, 64),
          "and the rebuilt arena grows again");

   munmap(mapping, WINDOW_CAPACITY);
}

/*
  A shortfall seen by several processes at once is still one shortfall.

  Each of them fails its allocation, and each is entitled to retry after
  growing, so the obvious loop adds a segment per process. Four processes that
  hit a full arena together add 2S, 4S, 8S and 16S for one request -- address
  space for the window embedder, and disk for the file-backed one. So grow looks
  at what its own refresh just learned: if a segment appeared between the
  allocation that failed and this call, that segment is the room the caller is
  about to retry for, and there is nothing to add.

  Both views below are attached while the arena has one segment, so the second
  one is in exactly the state the first was in before it grew. The race is made
  sequential, and therefore deterministic, without any threads.
*/
static void test_one_shortfall_adds_one_segment(void)
{
   struct dlmalloc_offset_header *header;
   struct window                  window;
   dlmalloc_offset_view           grown;
   dlmalloc_offset_view           stale;
   void                          *mapping;
   uint64_t                       consumed;
   uint64_t                       segments;
   size_t                         request;

   /*
     A window with room to spare, so that a second grow could succeed. A
     DLMALLOC_OFFSET_MIN_SEGMENT of 1 MiB means one grow into a 1 MiB window
     uses it up, and then "adds nothing" would hold whether or not grow
     noticed the segment it did not add.
   */
   mapping = mmap(NULL, RACE_CAPACITY, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
   if(mapping == MAP_FAILED)
   {
      report(0, "the racing arena is mapped");
      return;
   }
   window.base     = mapping;
   window.capacity = RACE_CAPACITY;
   request         = FIRST_SEGMENT + 32 * 1024;

   report(dlmalloc_offset_arena_init(mapping, FIRST_SEGMENT),
          "arena_init with one small segment");

   memset(&grown, 0, sizeof(grown));
   grown.base          = mapping;
   grown.capacity      = FIRST_SEGMENT;
   grown.grow_segment  = grow_segment;
   grown.map_segment   = map_segment;
   grown.unmap_segment = unmap_segment;
   grown.context       = &window;
   report(dlmalloc_offset_arena_attach(&grown, mapping, FIRST_SEGMENT),
          "a first process attaches");

   memset(&stale, 0, sizeof(stale));
   stale.base          = mapping;
   stale.capacity      = FIRST_SEGMENT;
   stale.grow_segment  = grow_segment;
   stale.map_segment   = map_segment;
   stale.unmap_segment = unmap_segment;
   stale.context       = &window;
   report(dlmalloc_offset_arena_attach(&stale, mapping, FIRST_SEGMENT),
          "a second process attaches");

   header   = (struct dlmalloc_offset_header *) mapping;
   consumed = header->total_offset_space;

   report(dlmalloc_offset_malloc(&grown, request) == 0,
          "the request does not fit the first segment");
   report(dlmalloc_offset_malloc(&stale, request) == 0,
          "and the second process fails it the same way");

   report(dlmalloc_offset_arena_grow(&grown, request),
          "the first process grows the arena");
   segments = dlmalloc_offset_view_segments(&grown);
   report(segments == 1 && header->total_offset_space > consumed,
          "which is one growth, of the segment the window already had");
   consumed = header->total_offset_space;

   report(dlmalloc_offset_arena_grow(&stale, request),
          "the second process, still seeing one segment, retries too");
   report(dlmalloc_offset_view_segments(&stale) == segments &&
          header->total_offset_space == consumed,
          "but adds nothing: the shortfall was already covered");
   report(dlmalloc_offset_malloc(&stale, request) != 0,
          "and its retry finds the segment the first process added");
   report(arena_is_valid(&stale), "the arena is consistent afterwards");

   munmap(mapping, RACE_CAPACITY);
}

/*
  An arena that no other process and no other thread touches does not need its
  lock, and the lock is not free: an atomic exchange and a store on every
  operation, measured at 17-29 ns aside a 120 ns hot path, 13-15% of it.
  nolocking is the embedder saying "I am the only one in here".

  That the lock is then not merely cheap but untouched is shown by planting a
  value that means "held" first: an operation that took the lock would spin on
  it forever, and this checks the word is exactly as it was left. A build that
  stops honouring nolocking therefore does not fail this check, it hangs on it;
  the runner's timeout is what reports that.
*/
static void test_nolocking_arena(void)
{
   struct dlmalloc_offset_header *header;
   dlmalloc_offset_view           view;
   dlmalloc_offset_t              offset;
   void                          *mapping;
   uint32_t                       planted;
   int                            ok;

   mapping = mmap(NULL, ARENA_CAPACITY, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
   if(mapping == MAP_FAILED)
   {
      report(0, "the arena that does not lock is mapped");
      return;
   }
   report(dlmalloc_offset_arena_init(mapping, ARENA_CAPACITY),
          "arena_init for an arena that does not lock");

   memset(&view, 0, sizeof(view));
   view.base      = mapping;
   view.capacity  = ARENA_CAPACITY;
   view.nolocking = 1;
   report(dlmalloc_offset_arena_attach(&view, mapping, ARENA_CAPACITY),
          "attach, and attaching does not take the lock either");

   header       = dlmalloc_offset_header_for(&view);
   planted      = header->lock;
   header->lock = DLMALLOC_OFFSET_LOCKED;   /* as if someone held it */

   offset = dlmalloc_offset_malloc(&view, 64);
   ok     = (offset != 0);
   if(ok)
      dlmalloc_offset_free(&view, offset);
   ok &= (header->lock == DLMALLOC_OFFSET_LOCKED);
   report(ok, "it allocates and frees without touching the lock word");

   header->lock = planted;   /* free again before anything else locks */
   report(dlmalloc_offset_arena_verify(&view),
          "and the arena it filled is consistent");

   munmap(mapping, ARENA_CAPACITY);
}

/*
  attach and adopt differ in exactly one thing: whether the segments that are
  already there are taken as checked. And a segment that fails its check must
  fail the operation that found it, not misbehave.
*/
static void test_lazy_check(void)
{
   struct dlmalloc_offset_header *header;
   struct dlmalloc_offset_block  *block;
   dlmalloc_offset_view           view;
   void                          *mapping;
   int                            ok;

   mapping = mmap(NULL, ARENA_CAPACITY, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
   if(mapping == MAP_FAILED)
   {
      report(0, "the lazy-check arena is mapped");
      return;
   }

   /* A region this process just built: nothing about it needs checking. */
   report(dlmalloc_offset_arena_init(mapping, ARENA_CAPACITY),
          "arena_init on a fresh region");
   memset(&view, 0, sizeof(view));
   report(dlmalloc_offset_arena_adopt(&view, mapping, ARENA_CAPACITY),
          "adopt takes a region this process built");
   report(view.verified == 1,
          "adopt leaves the built segment checked");
   report(dlmalloc_offset_malloc(&view, 64) != 0,
          "and it allocates without checking it again");
   report(dlmalloc_offset_arena_verify(&view),
          "verify agrees the arena is consistent");

   /*
     Now the same region with a block size that cannot be one: the tiling no
     longer describes the segment. Only the block header is touched, so the
     directory check in attach still passes and the corruption is what the
     segment check has to find.
   */
   header = (struct dlmalloc_offset_header *) mapping;
   block  = (struct dlmalloc_offset_block *)((char *) mapping +
                                             header->blocks_offset);
   block->size = 0;

   memset(&view, 0, sizeof(view));
   report(dlmalloc_offset_arena_attach(&view, mapping, ARENA_CAPACITY),
          "attach accepts it, because attach does not read the blocks");
   report(view.verified == 0,
          "attach trusts no segment of it");
   report(dlmalloc_offset_malloc(&view, 64) == 0,
          "allocating in the corrupt segment fails, it does not misbehave");
   report( ! dlmalloc_offset_arena_verify(&view),
          "verify rejects the corrupt arena");

   report(dlmalloc_offset_arena_init(mapping, ARENA_CAPACITY),
          "arena_init rebuilds it");
   memset(&view, 0, sizeof(view));
   ok = dlmalloc_offset_arena_adopt(&view, mapping, ARENA_CAPACITY);
   ok &= (dlmalloc_offset_malloc(&view, 64) != 0);
   report(ok, "and it allocates again after the rebuild");

   munmap(mapping, ARENA_CAPACITY);
}

/*
  An extension that lands after a live block.

  An extension either merges into the free block that ends the segment or
  follows it, and only the second case has to write the predecessor size into a
  block of its own. The tail is normally free -- splitting a free block leaves
  the remainder behind -- so the setup that exercises the other branch is an
  allocation that consumes the last free block whole, which is what an allocator
  does when what is left over is too small to hold a block.
*/
static void test_extended_after_live_tail(void)
{
   struct dlmalloc_offset_segment *entry;
   struct dlmalloc_offset_block   *block;
   struct window                   window;
   dlmalloc_offset_view            view;
   dlmalloc_offset_t               live;
   dlmalloc_offset_t               added;
   void                           *mapping;
   char                           *text;
   uintptr_t                       tail;

   mapping = mmap(NULL, WINDOW_CAPACITY, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
   if(mapping == MAP_FAILED)
   {
      report(0, "the growable window is mapped");
      return;
   }
   window.base     = mapping;
   window.capacity = WINDOW_CAPACITY;

   report(dlmalloc_offset_arena_init(mapping, FIRST_SEGMENT),
          "arena_init with one small segment");
   memset(&view, 0, sizeof(view));
   view.base          = mapping;
   view.capacity      = FIRST_SEGMENT;
   view.grow_segment  = grow_segment;
   view.map_segment   = map_segment;
   view.unmap_segment = unmap_segment;
   view.context       = &window;
   report(dlmalloc_offset_arena_attach(&view, mapping, FIRST_SEGMENT),
          "attach the growable window");

   entry = dlmalloc_offset_directory_for(&view);
   tail  = entry[0].tail_size;
   block = dlmalloc_offset_block_at(&view,
                                    entry[0].offset_base + entry[0].size - tail);
   report(tail >= 2 * DLMALLOC_OFFSET_BLOCK_SIZE &&
          block != 0 && ! dlmalloc_offset_block_inuse(block),
          "a fresh segment ends in one free block");

   /*
     Take all of it: the request plus a header is exactly the block, so there is
     no remainder and the segment now ends in a block that is in use.
   */
   live = dlmalloc_offset_malloc(&view, tail - DLMALLOC_OFFSET_BLOCK_SIZE);
   text = live ? (char *)dlmalloc_offset_resolve(&view, live) : 0;
   report(text != 0 && text[tail - DLMALLOC_OFFSET_BLOCK_SIZE - 1] == 0,
          "an allocation takes the whole of it");
   report(dlmalloc_offset_block_inuse(dlmalloc_offset_block_at(&view,
             entry[0].offset_base + entry[0].size - tail)),
          "so the segment ends in a live block");

   report(dlmalloc_offset_arena_grow(&view, FIRST_SEGMENT),
          "grow extends the segment past it");
   report(dlmalloc_offset_view_segments(&view) == 1,
          "without adding a segment");
   report(entry[0].size > FIRST_SEGMENT, "and the segment is bigger for it");
   report(arena_is_valid(&view), "the extension left the arena consistent");

   /*
     What it added starts at the old end, so an allocation from it is above the
     block that used to end the segment.
   */
   added = dlmalloc_offset_malloc(&view, FIRST_SEGMENT / 2);
   report(added > live, "and it can be allocated from");
   report(arena_is_valid(&view), "which leaves the arena consistent too");

   munmap(mapping, WINDOW_CAPACITY);
}

/*
  The two offsets every build refuses, checked or not: zero, because it means
  "no allocation", and a misaligned one, because it cannot name a payload.
  Neither may touch the arena, and that floor is what an unchecked (release)
  build keeps, so it is pinned here rather than among the checks that come and
  go with DLMALLOC_OFFSET_CHECKED. The double free at the end is the opposite:
  the one guarantee only the checked half can still make.
*/
static void test_refused_offsets(void)
{
   dlmalloc_offset_view view;
   dlmalloc_offset_t    offset;
   void                *mapping;
   int                  ok;

   mapping = mmap(NULL, ARENA_CAPACITY, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
   if(mapping == MAP_FAILED)
   {
      report(0, "the arena for the refused offsets is mapped");
      return;
   }
   report(dlmalloc_offset_arena_init(mapping, ARENA_CAPACITY),
          "arena_init for the refused offsets");

   memset(&view, 0, sizeof(view));
   view.base     = mapping;
   view.capacity = ARENA_CAPACITY;
   report(dlmalloc_offset_arena_attach(&view, mapping, ARENA_CAPACITY),
          "attach for the refused offsets");

   report( ! dlmalloc_offset_free(&view, 0),
          "freeing offset zero is refused");
   report( ! dlmalloc_offset_free(&view, 1),
          "freeing a misaligned offset is refused");

   offset = dlmalloc_offset_malloc(&view, 64);
   ok     = (offset != 0) && ((offset & (DLMALLOC_OFFSET_ALIGN - 1)) == 0);
   report(ok, "a live allocation to pit them against is made");
   if( ! ok)
   {
      munmap(mapping, ARENA_CAPACITY);
      return;
   }

   report( ! dlmalloc_offset_free(&view, 0),
          "freeing zero while an allocation is live is refused");
   report( ! dlmalloc_offset_free(&view, offset + 1),
          "and a misaligned view of a live allocation is too");
   report(arena_is_valid(&view),
          "neither refusal touched the arena");

   report(dlmalloc_offset_free(&view, offset),
          "the live allocation still frees normally");
#if DLMALLOC_OFFSET_CHECKED
   report( ! dlmalloc_offset_free(&view, offset),
          "freeing the same offset twice is refused");
#endif
   report(arena_is_valid(&view), "the arena is consistent afterwards");

   munmap(mapping, ARENA_CAPACITY);
}

int main(void)
{
   printf("single-segment fast path\n");
   test_fixed_arena();
   test_extended_arena();
   test_extended_after_live_tail();
   test_appended_arena();
   test_corrupt_arena_does_not_grow();
   test_one_shortfall_adds_one_segment();
   test_nolocking_arena();
   test_lazy_check();
   test_refused_offsets();
   printf("  %d/%d checks passed\n", checks_run - checks_failed, checks_run);
   return(checks_failed == 0 ? 0 : 1);
}
