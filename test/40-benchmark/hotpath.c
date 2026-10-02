/*
  What a hot path operation costs while the arena has one segment, and what it
  costs once it has two.

  One segment is the case that matters, because it is the one the shipped
  embedder lives in. A reserved window extends segment 0 in place, so a window
  arena stays at one segment for life, and then segment 0 starts at offset 0 and
  never moves: a flat offset is a displacement from this process's base and a
  block access is `view->base + offset`, no directory read and no index. That is
  the one-segment fast path, and this test exists to say whether it still holds.

  stdout is deterministic, so it is compared against hotpath.stdout:

    - the workload, and the arena shape before and after;
    - how many block accesses per pair fell back to the directory, which is 0
      while there is one segment and must not be 0 once there is a second one.
      The counter behind it is compiled into the allocator only for this test
      (it defines DLMALLOC_OFFSET_COUNT before including the source) and costs
      nothing in a build that does not ask for it;
    - the arena's own integrity walk, and whether any operation failed.

  stderr carries what depends on the machine or on the compiler, and is not
  compared:

    - nanoseconds per pair, for a 64 byte malloc/free pair;
    - instructions retired per pair, counted by callgrind. That number is what
      moves with the *code*, where nanoseconds move with the machine, so it is
      the one to watch when the hot path changes. It is measured by running this
      program again as a child, twice, with two operation counts: the difference
      between the two runs is the loop, and process startup cancels in it. A
      machine without valgrind reports "not measured" and the compared half of
      this test is unaffected.

  It is on stderr rather than in the golden because it depends on the compiler
  as well -- another gcc, another optimization, another count -- so pinning it
  in a file that every machine compares would pin the toolchain instead of the
  design. Neither number is guarded by a threshold: they are reported, and the
  fallback count is what the suite can fail on.

  Like every consumer of this project, the test includes the implementation
  source rather than a header, because the project is an amalgamation and ships
  no public headers.
*/

/* Needed here already for MAP_ANONYMOUS. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/mman.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
  Before the include, because that is what makes the allocator keep the count of
  block accesses that had to fall back to the directory.
*/
#define DLMALLOC_OFFSET_COUNT  1
#include "../../src/dlmalloc.c"

#define ARENA_CAPACITY   (1024u * 1024u)
#define FIRST_SEGMENT    (64u * 1024u)
#define SMALL            64u
#define PAIR_OPS         200000u
#define MEASURE_LOW      1000u
#define MEASURE_HIGH     4000u

static dlmalloc_offset_view view;
static void                *arena;
static unsigned int         failures;
static unsigned int         checks_failed;

static void check(int ok, const char *what)
{
   if( ! ok)
      ++checks_failed;
   printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
}

static uint64_t now_ns(void)
{
   struct timespec ts;

   clock_gettime(CLOCK_MONOTONIC, &ts);
   return((uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec);
}

/*
  The workload both arenas get: a 64 byte malloc/free pair, timed, with the
  fallback counter read either side. Reading it around the loop rather than for
  the whole run is the point: establishing an arena is allowed to fall back (a
  checked build walks segment 0 on its first use), the steady state is not.
*/
static uint64_t measure_pairs(unsigned int ops,
                              unsigned long *fallbacks)
{
   unsigned long     before;
   dlmalloc_offset_t offset;
   unsigned int      i;
   uint64_t          start;

   before = dlmalloc_offset_directory_fallbacks;
   start  = now_ns();
   for(i = 0; i < ops; ++i)
   {
      offset = dlmalloc_offset_malloc(&view, SMALL);
      if( ! offset)
      {
         ++failures;
         continue;
      }
      if( ! dlmalloc_offset_free(&view, offset))
         ++failures;
   }
   if(fallbacks)
      *fallbacks = dlmalloc_offset_directory_fallbacks - before;
   return(now_ns() - start);
}

/*
  A range that deliberately does not continue the segment before it, so that
  grow appends a second segment rather than extending segment 0: the arena shape
  that must *not* take the fast path.
*/
struct scatter
{
   void   *segment[ 2];
   size_t  size[ 2];
   int     count;
};

static int scatter_grow_segment(void *context, size_t min_size,
                                uintptr_t *backing_offset, void **base,
                                uintptr_t *size)
{
   struct scatter *s;
   void           *p;

   s = (struct scatter *) context;
   if(s->count >= 2 || min_size < DLMALLOC_OFFSET_MIN_BLOCK)
      return(0);
   p = mmap(NULL, min_size, PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
   if(p == MAP_FAILED)
      return(0);
   s->segment[s->count] = p;
   s->size[s->count]    = min_size;
   *backing_offset      = (uintptr_t) (s->count + 1) * (uintptr_t) 0x1000000;
   *base                = p;
   *size                = min_size;
   ++s->count;
   return(1);
}

static int integrity_of(void)
{
   int ok;

   ok = dlmalloc_offset_lock(&view);
   if(ok)
   {
      ok = dlmalloc_offset_arena_valid_locked(&view);
      dlmalloc_offset_unlock(&view);
   }
   return(ok);
}

/* A fixed arena: one segment, no callbacks, so it can never grow. */
static int setup_one_segment(void)
{
   arena = mmap(NULL, ARENA_CAPACITY, PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
   if(arena == MAP_FAILED)
      return(0);
   if( ! dlmalloc_offset_arena_init(arena, ARENA_CAPACITY))
      return(0);
   memset(&view, 0, sizeof(view));
   view.base     = arena;
   view.capacity = ARENA_CAPACITY;
   if( ! dlmalloc_offset_arena_attach(&view, arena, ARENA_CAPACITY))
      return(0);
   /* Warm the free lists, and pay the first access of segment 0 here. */
   if( ! dlmalloc_offset_free(&view, dlmalloc_offset_malloc(&view, SMALL)))
      return(0);
   return(dlmalloc_offset_view_segments(&view) == 1);
}

static int setup_two_segments(struct scatter *scatter)
{
   memset(scatter, 0, sizeof(*scatter));
   arena = mmap(NULL, FIRST_SEGMENT, PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
   if(arena == MAP_FAILED)
      return(0);
   if( ! dlmalloc_offset_arena_init(arena, FIRST_SEGMENT))
      return(0);
   memset(&view, 0, sizeof(view));
   view.base         = arena;
   view.capacity     = FIRST_SEGMENT;
   view.grow_segment = scatter_grow_segment;
   view.context      = scatter;
   if( ! dlmalloc_offset_arena_attach(&view, arena, FIRST_SEGMENT))
      return(0);
   if( ! dlmalloc_offset_arena_grow(&view, 2 * FIRST_SEGMENT))
      return(0);
   if( ! dlmalloc_offset_free(&view, dlmalloc_offset_malloc(&view, SMALL)))
      return(0);
   return(dlmalloc_offset_view_segments(&view) == 2);
}

/*
  The instruction count, from callgrind. `ops` is the operation count of one
  run; the caller subtracts two of them. The child is this same binary, so what
  is counted is exactly the workload above and nothing else.

  /proc/self/exe has to be resolved here rather than inside the command, because
  system() runs a shell and /proc/self/exe would then mean the shell.
*/
static int callgrind_instructions(unsigned int ops, unsigned long *ir)
{
   char  self[ 4096];
   char  path[ 256];
   char  command[ 4608];
   char  line[ 256];
   long  length;
   FILE *file;

   *ir = 0;
   length = readlink("/proc/self/exe", self, sizeof(self) - 1);
   if(length <= 0)
      return(0);
   self[length] = 0;

   snprintf(path, sizeof(path), "/tmp/mulle-dlmalloc-hotpath-%ld.out",
            (long) getpid());
   snprintf(command, sizeof(command),
            "valgrind --tool=callgrind --callgrind-out-file=%s %s --measure %u "
            ">/dev/null 2>/dev/null",
            path, self, ops);
   if(system(command) != 0)
      return(0);

   file = fopen(path, "r");
   if(file)
   {
      while(fgets(line, sizeof(line), file))
         if(strncmp(line, "summary:", 8) == 0)
         {
            *ir = strtoul(line + 8, 0, 10);
            break;
         }
      fclose(file);
   }
   unlink(path);
   return(*ir != 0);
}

static int report_instructions(void)
{
   unsigned long low;
   unsigned long high;
   double        per_pair;

   if(getenv("LD_PRELOAD") && strstr(getenv("LD_PRELOAD"), "vgpreload"))
   {
      fprintf(stderr, "instructions: not measured (already under valgrind)\n");
      return(0);
   }
   if(system("valgrind --version >/dev/null 2>&1") != 0)
   {
      fprintf(stderr, "instructions: not measured (no valgrind)\n");
      return(0);
   }
   if( ! callgrind_instructions(MEASURE_LOW, &low) ||
       ! callgrind_instructions(MEASURE_HIGH, &high) ||
       high <= low)
   {
      fprintf(stderr, "instructions: not measured (callgrind gave nothing)\n");
      return(0);
   }
   per_pair = (double) (high - low) / (double) (MEASURE_HIGH - MEASURE_LOW);
   fprintf(stderr,
           "instructions: %.0f retired per malloc/free pair "
           "(callgrind, %u and %u pairs, this build)\n",
           per_pair, MEASURE_LOW, MEASURE_HIGH);
   return(1);
}

int main(int argc, char *argv[])
{
   struct scatter scatter;
   unsigned long  one_fallbacks;
   unsigned long  two_fallbacks;
   uint64_t       one_ns;
   uint64_t       two_ns;
   unsigned int   i;

   /* The child of the callgrind run: the workload, and nothing else. */
   if(argc > 2 && strcmp(argv[1], "--measure") == 0)
   {
      if( ! setup_one_segment())
         return(1);
      measure_pairs((unsigned int) strtoul(argv[2], 0, 10), 0);
      return(failures ? 1 : 0);
   }

   printf("hot path benchmark\n");

   if( ! setup_one_segment())
   {
      printf("cannot set up the one-segment arena\n");
      return(1);
   }
   failures = 0;
   one_ns   = measure_pairs(PAIR_OPS, &one_fallbacks);

   printf("  one segment\n");
   printf("  arena capacity                        %u\n", ARENA_CAPACITY);
   printf("  segments                              %llu\n",
          (unsigned long long) dlmalloc_offset_view_segments(&view));
   printf("  malloc/free pairs                     %u\n", PAIR_OPS);
   printf("  block size                            %u\n", SMALL);
   printf("  accesses that fell back to directory  %lu\n", one_fallbacks);
   printf("  fallbacks per pair                    %.2f\n",
          (double) one_fallbacks / (double) PAIR_OPS);
   printf("  allocation failures                   %u\n", failures);
   printf("  checks\n");
   check(dlmalloc_offset_view_segments(&view) == 1,
         "the arena has a single segment");
   check(failures == 0, "no operation failed");
   check(one_fallbacks == 0,
         "no access falls back to the directory");
   check(integrity_of(), "the arena is internally consistent");

   /* The same workload with the fast path turned off by the arena's shape. */
   if( ! setup_two_segments(&scatter))
   {
      printf("cannot set up the two-segment arena\n");
      munmap(arena, ARENA_CAPACITY);
      return(1);
   }
   failures = 0;
   two_ns   = measure_pairs(PAIR_OPS, &two_fallbacks);

   /*
     The two-segment count is not in the compared half: a checked build reaches
     block_at once more per free than an unchecked one does, through
     payload_valid_locked, so the raw number is a build question where the
     0 above is not. What is compared is that it is not 0, which is what says
     the fast path is the arena's shape and not a habit.
   */
   printf("  two segments\n");
   printf("  segments                              %llu\n",
          (unsigned long long) dlmalloc_offset_view_segments(&view));
   printf("  malloc/free pairs                     %u\n", PAIR_OPS);
   printf("  block size                            %u\n", SMALL);
   printf("  allocation failures                   %u\n", failures);
   printf("  checks\n");
   check(dlmalloc_offset_view_segments(&view) == 2,
         "the arena has two segments");
   check(failures == 0, "no operation failed");
   check(two_fallbacks > 0, "every access falls back to the directory");
   check(integrity_of(), "the two-segment arena is internally consistent");

   munmap(arena, FIRST_SEGMENT);
   for(i = 0; i < (unsigned int) scatter.count; ++i)
      munmap(scatter.segment[i], scatter.size[i]);

   fprintf(stderr, "timing: %.1f ns per malloc/free pair, one segment; "
                   "%.1f with two\n",
           (double) one_ns / (double) PAIR_OPS,
           (double) two_ns / (double) PAIR_OPS);
   fprintf(stderr, "fallbacks: %lu with two segments (%.2f per pair), "
                   "%lu with one\n",
           two_fallbacks, (double) two_fallbacks / (double) PAIR_OPS,
           one_fallbacks);
   report_instructions();

   return(checks_failed == 0 ? 0 : 1);
}
