/*
  Throughput and memory behaviour of the relocatable offset arena.

  The workload is deterministic and allocator independent, so every number on
  stdout is reproducible and can be compared against benchmark.stdout: counts,
  bytes, the allocator's own integrity walk, and the local address scan.
  Timings depend on the machine, so they are written to stderr and are not
  compared.

  Every coordinate and length in the arena is a uintptr_t, so its layout is
  width-relative and the byte counts -- and only those -- depend on the pointer
  width. benchmark.stdout is the 64-bit golden, benchmark.stdout.linux.i686 the
  32-bit one; the harness selects the second by MULLE_ARCH:

     mulle-sde -DMULLE_ARCH=i686 -DCFLAGS=-m32 test run benchmark.c

  Nothing else moves with the width: the counts, the integrity walk and the
  local address scan are the same at either one.

  Like every consumer of this project, the test includes the implementation
  source rather than a header, because the project is an amalgamation and ships
  no public headers.
*/

/* Needed here already for memfd_create. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/mman.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../../src/dlmalloc.c"

#define ARENA_CAPACITY   (8u * 1024u * 1024u)
#define CHURN_OPS        120000u
#define PAIR_OPS         40000u
#define LOOKUP_OPS       40000u
#define LIVE_TARGET      (256u * 1024u)
#define SLOTS            2048u

static dlmalloc_offset_view view;
static void                 *arena;
static int                  failures;

static struct
{
   dlmalloc_offset_t offset;
   size_t            size;
} slots[SLOTS];

static size_t   live_bytes;
static unsigned int rng;
static unsigned int checks_failed;

/* Keeps the lookup loop from being optimised away entirely. */
static volatile uintptr_t lookup_sink;

static void check(int ok, const char *what)
{
   if( ! ok)
      ++checks_failed;
   printf("  %-28s %s\n", what, ok ? "ok" : "FAIL");
}

static unsigned int next_random(void)
{
   rng = rng * 1103515245u + 12345u;
   return((rng >> 13) & 0x3ffffu);
}

static uint64_t now_ns(void)
{
   struct timespec ts;

   clock_gettime(CLOCK_MONOTONIC, &ts);
   return((uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec);
}

/* Touch a few bytes so the workload pays for the pages it really uses. */
static void touch(void *payload, size_t size)
{
   unsigned char *bytes;
   uintptr_t     offset;
   size_t        i;

   bytes  = (unsigned char *)payload;
   offset = (uintptr_t)payload - (uintptr_t)arena;
   bytes[0] = (unsigned char)offset;
   if(size > 1)
      bytes[size - 1] = (unsigned char)(offset >> 8);
   for(i = 0; i < size; i += 256)
      bytes[i] = (unsigned char)(offset + i);
}

static void release_slot(unsigned int slot)
{
   if(slots[slot].offset)
   {
      if( ! dlmalloc_offset_free(&view, slots[slot].offset))
         ++failures;
      live_bytes -= slots[slot].size;
      slots[slot].offset = 0;
      slots[slot].size   = 0;
   }
}

/*
  Mixed allocation churn, holding the live set under LIVE_TARGET. The decisions
  never consult the allocator, so the trace is the same on every run.
*/
static uint64_t churn(void)
{
   unsigned int      op;
   unsigned int      slot;
   unsigned int      other;
   unsigned int      pick;
   unsigned int      k;
   size_t            size;
   size_t            old_size;
   dlmalloc_offset_t offset;
   void              *payload;
   uint64_t          start;
   int               found;

   rng        = 0x12345678u;
   live_bytes = 0;
   memset(slots, 0, sizeof(slots));
   start = now_ns();
   for(op = 0; op < CHURN_OPS; ++op)
   {
      slot = next_random() % SLOTS;
      size = 32 + (next_random() % 2048);
      pick = next_random();
      offset = slots[slot].offset;

      if( ! offset && live_bytes + size > LIVE_TARGET)
      {
         found = 0;
         for(k = 1; k <= SLOTS; ++k)
         {
            other = (slot + k) % SLOTS;
            if(slots[other].offset)
            {
               found = 1;
               break;
            }
         }
         if(found)
         {
            release_slot(other);
            continue;
         }
      }
      if( ! offset)
      {
         if(pick & 8)
            offset = dlmalloc_offset_memalign(&view, 64, size);
         else
            offset = dlmalloc_offset_malloc(&view, size);
         if( ! offset)
         {
            ++failures;
            continue;
         }
         slots[slot].offset = offset;
         slots[slot].size   = size;
         live_bytes += size;
         payload = dlmalloc_offset_resolve(&view, offset);
         if(payload)
            touch(payload, size);
      }
      else if(pick & 1)
         release_slot(slot);
      else
      {
         old_size = slots[slot].size;
         payload  = dlmalloc_offset_resolve(&view, offset);
         if(payload)
            touch(payload, old_size < size ? old_size : size);
         offset = dlmalloc_offset_realloc(&view, offset, size);
         if( ! offset)
         {
            ++failures;
            continue;
         }
         slots[slot].offset = offset;
         slots[slot].size   = size;
         live_bytes = live_bytes - old_size + size;
      }
   }
   return(now_ns() - start);
}

static uint64_t measure_pairs(void)
{
   dlmalloc_offset_t offset;
   unsigned int      i;
   uint64_t          start;

   start = now_ns();
   for(i = 0; i < PAIR_OPS; ++i)
   {
      offset = dlmalloc_offset_malloc(&view, 64);
      if( ! offset)
      {
         ++failures;
         continue;
      }
      if( ! dlmalloc_offset_free(&view, offset))
         ++failures;
   }
   return(now_ns() - start);
}

static uint64_t measure_lookups(dlmalloc_offset_t offset)
{
   unsigned int i;
   uint64_t     start;
   uintptr_t    sink;

   sink  = 0;
   start = now_ns();
   for(i = 0; i < LOOKUP_OPS; ++i)
   {
      sink += (uintptr_t)dlmalloc_offset_resolve(&view, offset);
      sink += dlmalloc_offset_usable_size(&view, offset);
   }
   lookup_sink = sink;
   return(now_ns() - start);
}

/* Largest single allocation that still succeeds in the current layout. */
static uint64_t largest_remaining(uint64_t ceiling)
{
   dlmalloc_offset_t offset;
   uint64_t          low;
   uint64_t          high;
   uint64_t          mid;
   uint64_t          best;

   low  = 16;
   high = ceiling;
   best = 0;
   while(low < high)
   {
      mid    = low + (high - low + 1) / 2;
      offset = dlmalloc_offset_malloc(&view, (size_t)mid);
      if(offset)
      {
         best = mid;
         dlmalloc_offset_free(&view, offset);
         low = mid + 1;
      }
      else
         high = mid - 1;
   }
   return(best);
}

/*
  Any allocator word matching base + k for k < capacity is a leaked local
  address: an arena that names one of its own addresses is not relocatable.

  Only the allocator's own words are examined -- the fixed prefix (header,
  bucket table, directory) and, in every block, size and prev_size, plus the two
  list links a free block carries at its payload start.

  Payload bytes are deliberately left out, and that is what makes this sound at
  both widths. The workload writes arena-offset derived bytes into payloads (see
  touch), and on ILP32 the arena is one 8 MiB window inside a 4 GiB address
  space, so four payload bytes land inside that window by chance about once per
  512 words. A scan that includes payloads cannot tell a leak from the test's
  own data pattern, and it reported exactly that false positive before. Nothing
  the allocator does is missed by excluding them: the only payload an allocator
  writes is a free block's two links, which are covered below.
*/
static int scan_words(unsigned char *from, size_t bytes, uintptr_t base)
{
   uintptr_t value;
   size_t    i;

   for(i = 0; i + sizeof(uintptr_t) <= bytes; i += sizeof(uintptr_t))
   {
      memcpy(&value, from + i, sizeof(uintptr_t));
      if(value - base < ARENA_CAPACITY && value != 0)
      {
         printf("  leaked local address at arena offset %#zx\n",
                (size_t) (from + i - (unsigned char *) arena));
         return(1);
      }
   }
   return(0);
}

static size_t count_local_addresses(void)
{
   struct dlmalloc_offset_header  *header;
   struct dlmalloc_offset_segment *directory;
   struct dlmalloc_offset_block   *block;
   unsigned char                  *bytes;
   uintptr_t                       end;
   uintptr_t                       index;
   uintptr_t                       offset;
   uintptr_t                       size;
   uintptr_t                       base;

   base  = (uintptr_t)arena;
   bytes = (unsigned char *)arena;
   header = dlmalloc_offset_header_for(&view);

   /* header, bucket table and directory are one allocator-owned prefix run */
   if(scan_words(bytes, (size_t) header->blocks_offset, base))
      return(1);

   directory = dlmalloc_offset_directory_for(&view);
   for(index = 0; index < header->segment_count; ++index)
   {
      offset = directory[index].blocks_base;
      end    = directory[index].offset_base + directory[index].size;
      while(offset < end)
      {
         /* one mapping backs the whole arena, so an offset is an address here */
         block = (struct dlmalloc_offset_block *)(bytes + offset);
         size  = dlmalloc_offset_block_size(block);
         if( ! size || size > end - offset)
            break;
         if(scan_words((unsigned char *) block, 2 * sizeof(uintptr_t), base))
            return(1);
         if( ! dlmalloc_offset_block_inuse(block))
            if(scan_words((unsigned char *) block + DLMALLOC_OFFSET_BLOCK_SIZE,
                          2 * sizeof(uintptr_t), base))
               return(1);
         offset += size;
      }
   }
   return(0);
}

int main(void)
{
   dlmalloc_offset_view other_view;
   unsigned char       *shared;
   dlmalloc_offset_t   sample_offset;
   uint64_t      churn_ns;
   uint64_t      pair_ns;
   uint64_t      lookup_ns;
   uint64_t      largest;
   uint64_t      usable_total;
   size_t        live_count;
   size_t        leaks;
   size_t        i;
   unsigned int  slot;
   dlmalloc_offset_t sample;
   void          *payload;
   int           fd;
   int           integrity;

   fd = memfd_create("mulle-dlmalloc-benchmark", 0);
   if(fd < 0 || ftruncate(fd, ARENA_CAPACITY) != 0)
   {
      printf("cannot create the arena\n");
      return(1);
   }
   arena = mmap(NULL, ARENA_CAPACITY, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   if(arena == MAP_FAILED)
   {
      printf("cannot map the arena\n");
      return(1);
   }
   memset(&view, 0, sizeof(view));
   if( ! dlmalloc_offset_arena_init(arena, ARENA_CAPACITY) ||
       ! dlmalloc_offset_arena_attach(&view, arena, ARENA_CAPACITY))
   {
      printf("cannot initialize the arena\n");
      return(1);
   }

   churn_ns  = churn();
   pair_ns   = measure_pairs();

   usable_total  = 0;
   live_count    = 0;
   sample_offset = 0;
   for(slot = 0; slot < SLOTS; ++slot)
   {
      if( ! slots[slot].offset)
         continue;
      usable_total += dlmalloc_offset_usable_size(&view, slots[slot].offset);
      ++live_count;
      if( ! sample_offset)
         sample_offset = slots[slot].offset;
   }
   lookup_ns = measure_lookups(sample_offset);
   largest   = largest_remaining(ARENA_CAPACITY);

   integrity = dlmalloc_offset_lock(&view);
   if(integrity)
   {
      integrity = dlmalloc_offset_arena_valid_locked(&view);
      dlmalloc_offset_unlock(&view);
   }
   leaks = count_local_addresses();

   printf("offset arena benchmark\n");
   printf("  arena capacity               %u\n", ARENA_CAPACITY);
   printf("  churn operations             %u\n", CHURN_OPS);
   printf("  live allocations             %zu\n", live_count);
   printf("  requested bytes              %zu\n", live_bytes);
   printf("  usable bytes                 %llu\n", (unsigned long long)usable_total);
   printf("  internal overhead            %.2fx\n",
          live_bytes ? (double)usable_total / (double)live_bytes : 0.0);
   printf("  allocation failures          %d\n", failures);
   printf("  integrity walk               %s\n", integrity ? "ok" : "FAIL");
   printf("  local addresses in arena     %zu\n", leaks);
   printf("  largest remaining allocation %llu\n", (unsigned long long)largest);
   printf("  checks\n");
   check(failures == 0, "no allocation failed");
   check(integrity, "arena is internally consistent");
   check(leaks == 0, "arena holds no local address");

   /* The same arena at a second base, to show relocation still works here. */
   other_view.base     = 0;
   other_view.capacity = 0;
   shared = mmap(NULL, ARENA_CAPACITY, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   check(shared != MAP_FAILED && shared != arena,
         "second mapping at another base");
   check(dlmalloc_offset_arena_attach(&other_view, shared, ARENA_CAPACITY),
         "attach at another base");
   payload = dlmalloc_offset_resolve(&other_view, sample_offset);
   check(payload == (void *)(shared + sample_offset),
         "resolve through the other base");
   sample = dlmalloc_offset_malloc(&other_view, 128);
   check(sample != 0 && dlmalloc_offset_resolve(&view, sample) != 0,
         "allocate at one base, resolve at the other");
   check(dlmalloc_offset_free(&view, sample), "free it from the first base");

   /* Release everything and confirm the arena comes back to one free block. */
   for(i = 0; i < SLOTS; ++i)
      if(slots[i].offset)
         release_slot(i);
   check(largest_remaining(ARENA_CAPACITY) ==
         dlmalloc_offset_header_for(&view)->blocks_size -
            DLMALLOC_OFFSET_BLOCK_SIZE,
         "arena coalesces back into one block");

   fprintf(stderr, "timing: churn %.1f ns/op, pair %.1f ns/op, lookup %.1f ns/op\n",
           (double)churn_ns / (double)CHURN_OPS,
           (double)pair_ns / (double)PAIR_OPS,
           (double)lookup_ns / (double)LOOKUP_OPS);
   return(checks_failed == 0 ? 0 : 1);
}
