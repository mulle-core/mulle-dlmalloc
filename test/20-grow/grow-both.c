/*
  Two processes grow one shared arena.

  Segment 0 is filled to exhaustion *before* forking, so every later allocation
  must come from a segment that one of the two processes adds. Parent and child
  then each grow the arena twice, concurrently.

  A grow that finds the arena already carries a segment the other process added
  in the meantime adds nothing, so how many segments exist depends on who grew
  when and is not the same number on every run. What does not depend on that,
  and what this pins, is the shape:

     every call adds at most one segment,
     every segment added doubles the one before it, so the total is
     (2^segments - 1) MB, and
     at least ROUNDS segments are added: one append by either process can cause
     at most one skipped call in the other, so appends cannot be fewer than the
     rounds.

  Each process records the offsets it allocated in a small control block that
  lives in segment 0, so the other process can resolve them. After a barrier
  both processes resolve and verify every item, which proves the grown segments
  are mapped, shared and addressable from both sides.

  Nothing in the printed output depends on the interleaving, so stdout is
  compared against grow-both.stdout.

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

#define ARENA_CAPACITY   (1024 * 1024)
#define CHUNK_SIZE       4096
#define MAX_CHUNKS       1024
#define ROUNDS           2
#define ITEM_SIZE        (64 * 1024)
#define PROCESSES        2

struct backing
{
   int   fd;
};

struct control
{
   dlmalloc_offset_t   item[PROCESSES][ROUNDS];
};

static struct backing backing;

static int checks_failed;
static int checks_run;

static void report(int ok, const char *what)
{
   ++checks_run;
   if( ! ok)
      ++checks_failed;
   printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
}

static int grow_segment(void *context, size_t min_size,
                        uintptr_t *backing_offset, void **base, uintptr_t *size)
{
   struct backing *b;
   off_t          old;
   size_t         want;
   void           *p;

   b    = (struct backing *) context;
   want = min_size;
   if(want < ARENA_CAPACITY)
      want = ARENA_CAPACITY;

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

static unsigned char item_byte(unsigned int process, unsigned int round,
                               size_t index)
{
   return((unsigned char)((process * 37 + round * 11 + index) & 0xff));
}

static void write_item(void *pointer, unsigned int process, unsigned int round)
{
   unsigned char *bytes;
   size_t        i;

   bytes = (unsigned char *) pointer;
   for(i = 0; i < ITEM_SIZE; ++i)
      bytes[i] = item_byte(process, round, i);
}

static int item_holds(void *pointer, unsigned int process, unsigned int round)
{
   unsigned char *bytes;
   size_t        i;

   bytes = (unsigned char *) pointer;
   for(i = 0; i < ITEM_SIZE; ++i)
      if(bytes[i] != item_byte(process, round, i))
         return(0);
   return(1);
}

/* Both sides arrive before either continues. */
static int sync_barrier(int write_fd, int read_fd)
{
   char c;

   c = 'x';
   if(write(write_fd, &c, 1) != 1)
      return(0);
   if(read(read_fd, &c, 1) != 1)
      return(0);
   return(1);
}

/* One grow followed by one allocation that must fit the new segment. */
static int grow_and_allocate(dlmalloc_offset_view *view,
                             struct control *control,
                             unsigned int process,
                             unsigned int round)
{
   dlmalloc_offset_t offset;
   void              *pointer;

   if( ! dlmalloc_offset_arena_grow(view, 0))
      return(0);
   offset = dlmalloc_offset_malloc(view, ITEM_SIZE);
   if( ! offset)
      return(0);
   pointer = dlmalloc_offset_resolve(view, offset);
   if( ! pointer)
      return(0);
   write_item(pointer, process, round);
   control->item[process][round] = offset;
   return(1);
}

/* Verify every item, from this process's own mapping of the segments. */
static int verify_all(dlmalloc_offset_view *view, struct control *control)
{
   unsigned int process;
   unsigned int round;
   void         *pointer;

   if( ! dlmalloc_offset_arena_refresh(view))
      return(0);
   for(process = 0; process < PROCESSES; ++process)
      for(round = 0; round < ROUNDS; ++round)
      {
         pointer = dlmalloc_offset_resolve(view, control->item[process][round]);
         if( ! pointer || ! item_holds(pointer, process, round))
            return(0);
      }
   return(1);
}

static int arena_still_valid(dlmalloc_offset_view *view)
{
   int ok;

   if( ! dlmalloc_offset_lock(view))
      return(0);
   ok = dlmalloc_offset_arena_valid_locked(view);
   dlmalloc_offset_unlock(view);
   return(ok);
}

int main(void)
{
   dlmalloc_offset_view view;
   dlmalloc_offset_view child_view;
   struct control       *control;
   dlmalloc_offset_t    chunks[MAX_CHUNKS];
   dlmalloc_offset_t    control_offset;
   void                 *base0;
   void                 *child_base;
   unsigned int         chunk_count;
   unsigned int         round;
   uint64_t             segments;
   uint64_t             total;
   int                  ready[2];
   int                  back[2];
   int                  wstatus;
   int                  fd;
   pid_t                pid;

   fd = memfd_create("dlmalloc-grow-both", 0);
   if(fd < 0 || ftruncate(fd, ARENA_CAPACITY) != 0)
   {
      printf("  this platform has no memfd support\n");
      return(1);
   }
   base0 = mmap(NULL, ARENA_CAPACITY, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   if(base0 == MAP_FAILED)
   {
      printf("  mmap of segment 0 failed\n");
      return(1);
   }
   backing.fd = fd;

   printf("two-process arena growth\n");
   if( ! dlmalloc_offset_arena_init(base0, ARENA_CAPACITY))
   {
      printf("  init failed\n");
      return(1);
   }
   setup_view(&view);
   if( ! dlmalloc_offset_arena_attach(&view, base0, ARENA_CAPACITY))
   {
      printf("  attach failed\n");
      return(1);
   }

   /* Control block first, then fill segment 0 to exhaustion. */
   control_offset = dlmalloc_offset_calloc(&view, 1, sizeof(struct control));
   control        = (struct control *) dlmalloc_offset_resolve(&view,
                                                               control_offset);
   if( ! control)
   {
      printf("  could not allocate the control block\n");
      return(1);
   }

   chunk_count = 0;
   while(chunk_count < MAX_CHUNKS)
   {
      dlmalloc_offset_t offset;

      offset = dlmalloc_offset_malloc(&view, CHUNK_SIZE);
      if( ! offset)
         break;
      chunks[chunk_count++] = offset;
   }
   report(chunk_count > 0 && chunk_count < MAX_CHUNKS,
          "segment 0 is filled to exhaustion before the fork");

   if(pipe(ready) != 0 || pipe(back) != 0)
   {
      printf("  pipe setup failed\n");
      return(1);
   }
   fflush(stdout);
   pid = fork();
   if(pid == 0)
   {
      close(ready[0]);
      close(back[1]);

      child_base = mmap(NULL, ARENA_CAPACITY, PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, 0);
      setup_view(&child_view);
      if(child_base == MAP_FAILED ||
         ! dlmalloc_offset_arena_attach(&child_view, child_base, ARENA_CAPACITY))
         _exit(1);

      for(round = 0; round < ROUNDS; ++round)
         if( ! grow_and_allocate(&child_view, control, 1, round))
            _exit(2);

      if( ! sync_barrier(ready[1], back[0]))
         _exit(3);
      if( ! verify_all(&child_view, control))
         _exit(5);
      if( ! arena_still_valid(&child_view))
         _exit(6);
      _exit(0);
   }
   close(ready[1]);
   close(back[0]);

   for(round = 0; round < ROUNDS; ++round)
      if( ! grow_and_allocate(&view, control, 0, round))
      {
         printf("  the parent could not grow and allocate\n");
         return(1);
      }

   if( ! sync_barrier(back[1], ready[0]))
   {
      printf("  barrier failed\n");
      return(1);
   }

   report(verify_all(&view, control), "the parent resolves every item from both processes");

   waitpid(pid, &wstatus, 0);
   report(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0,
          "the child grew, verified and exited cleanly");

   segments = dlmalloc_offset_header_for(&view)->segment_count;
   total    = dlmalloc_offset_header_for(&view)->total_offset_space;
   report(segments >= (1 + ROUNDS) && segments <= (1 + PROCESSES * ROUNDS),
          "growth added one segment per call, at most");
   report(total == ((uint64_t) ARENA_CAPACITY << segments) -
                    (uint64_t) ARENA_CAPACITY,
          "and every segment added doubles the one before it");
   report(arena_still_valid(&view), "the grown arena is internally consistent");

   /* Give everything back and confirm the arena coalesces per segment. */
   while(chunk_count)
      dlmalloc_offset_free(&view, chunks[--chunk_count]);
   for(round = 0; round < ROUNDS; ++round)
   {
      dlmalloc_offset_free(&view, control->item[0][round]);
      dlmalloc_offset_free(&view, control->item[1][round]);
   }
   dlmalloc_offset_free(&view, control_offset);
   report(arena_still_valid(&view), "the arena stays consistent after freeing everything");

   printf("  %d/%d checks passed\n", checks_run - checks_failed, checks_run);
   return(checks_failed == 0 ? 0 : 1);
}
