/*
  Growth of the relocatable arena.

  The arena starts as one file-backed segment. When it is exhausted,
  dlmalloc_offset_arena_grow adds a segment twice as large, through the
  embedder's grow_segment callback. The new segment gets the next range of the
  flat offset space, so offsets stay valid and unique.

  This test proves:

  1. the first segment can be filled to exhaustion;
  2. grow adds a segment and allocation resumes in it;
  3. a second view, mapped at a different address, attaches and maps every
     segment automatically, and resolves an offset from the grown segment;
  4. a forked process attaches before the parent grows, then notices the growth
     on refresh and allocates in the new segment, which the parent reads.

  Every number printed is reproducible, so stdout is compared against grow.stdout.

  Like every consumer of this project, the test includes the implementation
  source rather than a header, because the project is an amalgamation and ships
  no public headers.
*/

/* Needed here already for memfd_create. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/mman.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../../src/dlmalloc.c"

#define INITIAL_CAPACITY  (64 * 1024)
#define FILL_SIZE         4096
#define MAX_SLOTS         256

struct backing
{
   int   fd;
};

static struct backing backing;

static int checks_run;
static int checks_failed;

static void report(int ok, const char *what)
{
   ++checks_run;
   if( ! ok)
      ++checks_failed;
   printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
}

/* Extend the backing file and map the new range anywhere. */
static int grow_segment(void *context, size_t min_size,
                        uintptr_t *backing_offset, void **base, uintptr_t *size)
{
   struct backing *b;
   off_t          old;
   size_t         want;
   void           *p;

   b    = (struct backing *) context;
   want = min_size;
   if(want < INITIAL_CAPACITY)
      want = INITIAL_CAPACITY;

   old = lseek(b->fd, 0, SEEK_END);
   if(old < 0 || ftruncate(b->fd, old + (off_t)want) != 0)
      return(0);
   p = mmap(NULL, want, PROT_READ | PROT_WRITE, MAP_SHARED, b->fd, old);
   if(p == MAP_FAILED)
      return(0);
   *backing_offset = (uintptr_t) old;
   *base           = p;
   *size           = want;
   return(1);
}

/* Map a range of the backing file that another process added. */
static int map_segment(void *context, uintptr_t backing_offset, size_t size,
                       void **base)
{
   struct backing *b;
   void           *p;

   b = (struct backing *) context;
   p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, b->fd,
            (off_t) backing_offset);
   if(p == MAP_FAILED)
      return(0);
   *base = p;
   return(1);
}

static void unmap_segment(void *context, void *base, size_t size)
{
   (void) context;
   munmap(base, size);
}

static void setup_view(dlmalloc_offset_view *view)
{
   memset(view, 0, sizeof(*view));
   view->grow_segment  = grow_segment;
   view->map_segment   = map_segment;
   view->unmap_segment = unmap_segment;
   view->context       = &backing;
}

/* The arena lock and integrity walk are internal, so use them as an oracle. */
static int arena_still_valid(dlmalloc_offset_view *view)
{
   int ok;

   if( ! dlmalloc_offset_lock(view))
      return(0);
   ok = dlmalloc_offset_arena_valid_locked(view);
   dlmalloc_offset_unlock(view);
   return(ok);
}

static void fill_pattern(char *s, size_t length, unsigned int seed)
{
   size_t i;

   for(i = 0; i + 1 < length; ++i)
      s[i] = (char)('a' + ((seed + i) % 26));
   s[length ? length - 1 : 0] = 0;
}

int main(void)
{
   dlmalloc_offset_view view;
   dlmalloc_offset_view other;
   dlmalloc_offset_t    offsets[MAX_SLOTS];
   dlmalloc_offset_t    grown;
   dlmalloc_offset_t    child_offset;
   dlmalloc_offset_t    offset;
   char                 *text;
   void                 *base0;
   void                 *base1;
   void                 *child_base;
   unsigned int         n;
   int                  ready[2];
   int                  go[2];
   int                  result[2];
   int                  wstatus;
   int                  fd;
   pid_t                pid;
   char                 signal;

   fd = memfd_create("dlmalloc-grow-arena", 0);
   if(fd < 0 || ftruncate(fd, INITIAL_CAPACITY) != 0)
   {
      printf("  this platform has no memfd support\n");
      return(1);
   }
   base0 = mmap(NULL, INITIAL_CAPACITY, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   if(base0 == MAP_FAILED)
   {
      printf("  mmap of segment 0 failed\n");
      return(1);
   }
   backing.fd = fd;

   printf("offset arena growth\n");
   report(dlmalloc_offset_arena_init(base0, INITIAL_CAPACITY),
          "init the first segment");

   setup_view(&view);
   report(dlmalloc_offset_arena_attach(&view, base0, INITIAL_CAPACITY),
          "attach the first segment");

   n = 0;
   for(;;)
   {
      offset = dlmalloc_offset_malloc(&view, FILL_SIZE);
      if( ! offset)
         break;
      if(n >= MAX_SLOTS)
         break;
      text = (char *)dlmalloc_offset_resolve(&view, offset);
      fill_pattern(text, FILL_SIZE, n);
      offsets[n++] = offset;
   }
   report(n > 0 && n < MAX_SLOTS, "the first segment fills up");
   report(dlmalloc_offset_malloc(&view, FILL_SIZE) == 0,
          "the exhausted first segment refuses further allocations");

   report(dlmalloc_offset_arena_grow(&view, 0), "grow adds a segment");
   grown = dlmalloc_offset_malloc(&view, FILL_SIZE);
   report(grown != 0, "allocation resumes after growth");
   report(grown >= INITIAL_CAPACITY, "the new allocation is in the grown segment");
   text = (char *)dlmalloc_offset_resolve(&view, grown);
   if(text)
      strcpy(text, "grown");
   report(arena_still_valid(&view), "the arena is still internally consistent");

   /* A second view at a different address must map both segments. */
   base1 = mmap(NULL, INITIAL_CAPACITY, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   setup_view(&other);
   report(base1 != MAP_FAILED && base1 != base0,
          "a second mapping of segment 0 lands elsewhere");
   report(dlmalloc_offset_arena_attach(&other, base1, INITIAL_CAPACITY),
          "a second view attaches and maps every segment");
   text = (char *)dlmalloc_offset_resolve(&other, grown);
   report(text && strcmp(text, "grown") == 0,
          "the second view resolves the grown segment");
   report(arena_still_valid(&other), "the second view validates the arena");

   /*
     Cross-process notice: the child attaches before the parent grows, so it
     must learn about the new segment from the epoch on its next call.
   */
   if(pipe(ready) != 0 || pipe(go) != 0 || pipe(result) != 0)
   {
      printf("  pipe setup failed\n");
      return(1);
   }
   fflush(stdout);
   pid = fork();
   if(pid == 0)
   {
      dlmalloc_offset_view child_view;

      close(ready[0]);
      close(go[1]);
      close(result[0]);

      child_base = mmap(NULL, INITIAL_CAPACITY, PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, 0);
      setup_view(&child_view);
      if(child_base == MAP_FAILED ||
         ! dlmalloc_offset_arena_attach(&child_view, child_base, INITIAL_CAPACITY))
         _exit(1);
      if(write(ready[1], "r", 1) != 1)
         _exit(2);

      if(read(go[0], &signal, 1) != 1)
         _exit(3);
      if( ! dlmalloc_offset_arena_refresh(&child_view))
         _exit(4);

      child_offset = dlmalloc_offset_malloc(&child_view, FILL_SIZE);
      if( ! child_offset)
         _exit(5);
      text = (char *)dlmalloc_offset_resolve(&child_view, child_offset);
      if( ! text)
         _exit(6);
      strcpy(text, "from-child");
      if(write(result[1], &child_offset, sizeof(child_offset)) !=
         (ssize_t)sizeof(child_offset))
         _exit(7);
      _exit(0);
   }
   close(ready[1]);
   close(go[0]);
   close(result[1]);

   if(read(ready[0], &signal, 1) != 1)
   {
      printf("  the child did not attach\n");
      return(1);
   }
   report(dlmalloc_offset_arena_grow(&view, 0), "the parent grows again");
   if(write(go[1], "g", 1) != 1)
   {
      printf("  could not signal the child\n");
      return(1);
   }
   child_offset = 0;
   report(read(result[0], &child_offset, sizeof(child_offset)) ==
          (ssize_t)sizeof(child_offset),
          "the parent received the child offset");
   text = (char *)dlmalloc_offset_resolve(&view, child_offset);
   report(text && strcmp(text, "from-child") == 0,
          "the parent reads the child allocation in the newly grown segment");
   waitpid(pid, &wstatus, 0);
   report(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0,
          "the child process exited cleanly");
   report(arena_still_valid(&view), "the arena survives the cross-process growth");

   while(n)
      dlmalloc_offset_free(&view, offsets[--n]);
   dlmalloc_offset_free(&view, grown);
   dlmalloc_offset_free(&view, child_offset);
   report(arena_still_valid(&view), "the arena is consistent after freeing everything");

   printf("  %d/%d checks passed\n", checks_run - checks_failed, checks_run);
   return(checks_failed == 0 ? 0 : 1);
}
