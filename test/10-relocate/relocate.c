/*
  Relocation correctness of the offset arena.

  The point of storing offsets instead of pointers is that the same arena can be
  mapped at different addresses, in one process and in several. This test proves
  that, and it is the reason the offset format exists at all:

  1. One process maps the same shared object twice at two different bases. An
     allocation made through base A is resolved, verified and freed through
     base B, and a block freed through B is reused by A.
  2. 6000 rounds of randomized churn alternate both bases, and every payload is
     written through one view and read back through the other.
  3. A forked process maps the same object at a third base, allocates a block
     the parent then reads and frees, and frees a block the parent allocated.
  4. After all of that the arena still contains no process-local address: an
     offset is small, so any word landing inside the mapping is a leaked
     pointer. The old dlmalloc-translating design failed exactly here.

  Every number printed is reproducible, so stdout is compared against
  relocate.stdout.

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
#define STRESS_SLOTS     96
#define STRESS_ROUNDS    6000

static int checks_run;
static int checks_failed;

static void report(int ok, const char *what)
{
   ++checks_run;
   if( ! ok)
      ++checks_failed;
   printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
}

/*
  Any arena word that equals base + k for some k < capacity is a leaked
  process-local address. Real offsets are far smaller than a mapping base.
*/
static size_t count_local_slots(void *p, size_t length, void *base,
                                size_t capacity)
{
   unsigned char *bytes;
   size_t        count;
   size_t        i;
   uintptr_t     value;

   count = 0;
   bytes = (unsigned char *)p;
   for(i = 0; i + sizeof(uintptr_t) <= length; i += sizeof(uintptr_t))
   {
      memcpy(&value, bytes + i, sizeof(uintptr_t));
      if(value - (uintptr_t)base < capacity && value != 0)
      {
         printf("  leaked local address at arena offset %#zx\n",
                (size_t)(bytes + i - (unsigned char *)base));
         ++count;
      }
   }
   return(count);
}

/*
  The whole arena must be free of local addresses, not merely the parts the
  allocator dereferences. This is the check the dlmalloc-translating design
  could not pass.
*/
static void check_arena_hygiene(void *base, size_t capacity, const char *label)
{
   size_t      leaks;
   static char message[96];

   leaks = count_local_slots(base, capacity, base, capacity);
   snprintf(message, sizeof(message), "arena holds no local address %s", label);
   report(leaks == 0, message);
}

static void fill_pattern(char *s, size_t length, unsigned int seed)
{
   size_t i;

   for(i = 0; i + 1 < length; ++i)
      s[i] = (char)('a' + ((seed + i) % 26));
   s[length ? length - 1 : 0] = 0;
}

static unsigned int rng_state = 0x12345678u;

static unsigned int next_random(void)
{
   rng_state = rng_state * 1103515245u + 12345u;
   return((rng_state >> 16) & 0x7fffu);
}

static unsigned char pattern_byte(unsigned int slot, size_t index)
{
   return((unsigned char)((slot * 31u + index) & 0xffu));
}

/* Returns 0 when the whole run of bytes matches the slot's pattern. */
static int pattern_holds(void *pointer, unsigned int slot, size_t size)
{
   unsigned char *bytes;
   size_t        i;

   bytes = (unsigned char *)pointer;
   for(i = 0; i < size; ++i)
      if(bytes[i] != pattern_byte(slot, i))
      {
         printf("  slot %u byte %zu is %#x, want %#x\n",
                slot, i, bytes[i], pattern_byte(slot, i));
         return(0);
      }
   return(1);
}

static void write_pattern(void *pointer, unsigned int slot, size_t size)
{
   unsigned char *bytes;
   size_t        i;

   bytes = (unsigned char *)pointer;
   for(i = 0; i < size; ++i)
      bytes[i] = pattern_byte(slot, i);
}

/* Run the allocator's own full integrity walk; used as a test oracle. */
static int arena_still_valid(dlmalloc_offset_view *view)
{
   int ok;

   if( ! dlmalloc_offset_lock(view))
      return(0);
   ok = dlmalloc_offset_arena_valid_locked(view);
   dlmalloc_offset_unlock(view);
   return(ok);
}

/*
  Alternates allocation and release between two views of one arena and reads
  every payload back through the non-owning view. A wrong link offset or a lost
  block shows up as a bad byte or as a failed integrity walk.
*/
static void stress_two_views(dlmalloc_offset_view *views)
{
   dlmalloc_offset_t offsets[STRESS_SLOTS];
   size_t            sizes[STRESS_SLOTS];
   dlmalloc_offset_t offset;
   unsigned int      round;
   unsigned int      slot;
   unsigned int      which;
   unsigned int      live;
   size_t            size;
   void              *pointer;
   int               bad;

   bad = 0;
   round = 0;
   for(slot = 0; slot < STRESS_SLOTS; ++slot)
   {
      offsets[slot] = 0;
      sizes[slot]   = 0;
   }
   live = 0;
   for(round = 0; round < STRESS_ROUNDS && ! bad; ++round)
   {
      slot  = next_random() % STRESS_SLOTS;
      which = next_random() & 1;
      if(offsets[slot] == 0)
      {
         size = 1 + (next_random() % 4000);
         if(next_random() & 8)
            offset = dlmalloc_offset_memalign(&views[which], 256, size);
         else
            offset = dlmalloc_offset_malloc(&views[which], size);
         if( ! offset || ! arena_still_valid(&views[which]))
         {
            printf("  round %u: allocating %zu bytes failed or broke the arena\n",
                   round, size);
            bad = 1;
            break;
         }
         pointer = dlmalloc_offset_resolve(&views[ ! which], offset);
         if( ! pointer)
         {
            printf("  round %u: the other view cannot resolve a fresh offset\n",
                   round);
            bad = 1;
            break;
         }
         write_pattern(pointer, slot, size);
         offsets[slot] = offset;
         sizes[slot]   = size;
         ++live;
      }
      else
      {
         pointer = dlmalloc_offset_resolve(&views[ ! which], offsets[slot]);
         if( ! pointer || ! pattern_holds(pointer, slot, sizes[slot]))
         {
            printf("  round %u: payload of slot %u did not survive\n",
                   round, slot);
            bad = 1;
            break;
         }
         if(next_random() & 1)
         {
            if( ! dlmalloc_offset_free(&views[which], offsets[slot]) ||
                ! arena_still_valid(&views[which]))
            {
               printf("  round %u: free of slot %u failed\n", round, slot);
               bad = 1;
               break;
            }
            offsets[slot] = 0;
            --live;
         }
         else
         {
            size   = 1 + (next_random() % 4000);
            offset = dlmalloc_offset_realloc(&views[which], offsets[slot], size);
            if( ! offset || ! arena_still_valid(&views[which]))
            {
               printf("  round %u: realloc of slot %u to %zu bytes failed\n",
                      round, slot, size);
               bad = 1;
               break;
            }
            pointer = dlmalloc_offset_resolve(&views[ ! which], offset);
            if( ! pointer)
            {
               printf("  round %u: reallocated offset does not resolve\n", round);
               bad = 1;
               break;
            }
            write_pattern(pointer, slot, size);
            offsets[slot] = offset;
            sizes[slot]   = size;
         }
      }
   }
   for(slot = 0; slot < STRESS_SLOTS; ++slot)
      if(offsets[slot])
      {
         pointer = dlmalloc_offset_resolve(&views[ ! (slot & 1)], offsets[slot]);
         if( ! pointer || ! pattern_holds(pointer, slot, sizes[slot]))
            bad = 1;
         if( ! dlmalloc_offset_free(&views[slot & 1], offsets[slot]))
            bad = 1;
      }
   printf("  %u churn rounds, %u slots still live at the end\n", round, live);
   report( ! bad, "two-view churn keeps every payload intact");
   check_arena_hygiene(views[0].base, views[0].capacity, "after the churn");
}

static int map_arena(int fd, void **base_out)
{
   void *base;

   base = mmap(NULL, ARENA_CAPACITY, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   if(base == MAP_FAILED)
      return(0);
   *base_out = base;
   return(1);
}

static int child_process(int fd,
                         int child_to_parent,
                         int parent_to_child,
                         void *parent_base_a,
                         void *parent_base_b)
{
   dlmalloc_offset_view view = { 0 };
   dlmalloc_offset_t   offset;
   dlmalloc_offset_t   parent_offset;
   char                *text;
   char                *resolved;
   void                *base;
   char                status;
   unsigned char       base_bytes[sizeof(void *)];

   if( ! map_arena(fd, &base))
      return(1);
   if(base == parent_base_a || base == parent_base_b)
   {
      printf("  child mapping did not relocate\n");
      return(1);
   }
   if( ! dlmalloc_offset_arena_attach(&view, base, ARENA_CAPACITY))
   {
      printf("  child attach failed\n");
      return(1);
   }
   if(read(parent_to_child, &parent_offset, sizeof(parent_offset)) !=
      (ssize_t)sizeof(parent_offset))
   {
      printf("  child did not receive the parent offset\n");
      return(1);
   }

   offset = dlmalloc_offset_malloc(&view, 64);
   if( ! offset)
   {
      printf("  child allocation failed\n");
      return(1);
   }
   text = (char *)dlmalloc_offset_resolve(&view, offset);
   if( ! text)
   {
      printf("  child cannot resolve its own allocation\n");
      return(1);
   }
   strcpy(text, "from-child");

   memcpy(base_bytes, &base, sizeof(base));
   if(write(child_to_parent, base_bytes, sizeof(base)) != (ssize_t)sizeof(base))
      return(1);
   if(write(child_to_parent, &offset, sizeof(offset)) != (ssize_t)sizeof(offset))
      return(1);

   resolved = (char *)dlmalloc_offset_resolve(&view, parent_offset);
   if( ! resolved || strcmp(resolved, "from-parent") != 0)
   {
      printf("  child could not read the parent allocation\n");
      return(1);
   }
   if( ! dlmalloc_offset_free(&view, parent_offset))
   {
      printf("  child could not free the parent allocation\n");
      return(1);
   }
   status = 1;
   if(write(child_to_parent, &status, 1) != 1)
      return(1);
   return(0);
}

int main(void)
{
   dlmalloc_offset_view view_a = { 0 };
   dlmalloc_offset_view view_b = { 0 };
   dlmalloc_offset_t   first;
   dlmalloc_offset_t   second;
   dlmalloc_offset_t   reused;
   dlmalloc_offset_t   aligned_offset;
   dlmalloc_offset_t   grown;
   dlmalloc_offset_t   child_offset;
   dlmalloc_offset_view views[2];
   void                *child_gone;
   void                *base_a;
   void                *base_b;
   void                *child_base;
   char                *text;
   int                 child_to_parent[2];
   int                 parent_to_child[2];
   int                 child_status;
   int                 fd;
   pid_t               pid;
   char                status;

   printf("offset arena relocation\n");
   fd = memfd_create("dlmalloc-offset-arena", 0);
   if(fd < 0 || ftruncate(fd, ARENA_CAPACITY) != 0)
   {
      printf("  this platform has no memfd support\n");
      return(1);
   }
   if( ! map_arena(fd, &base_a))
   {
      printf("  mmap of the arena failed\n");
      return(1);
   }
   if(((uintptr_t)base_a & (DLMALLOC_OFFSET_ALIGN - 1)) != 0)
   {
      printf("  the mapping is not DLMALLOC_OFFSET_ALIGN aligned\n");
      return(1);
   }
   report(dlmalloc_offset_arena_init(base_a, ARENA_CAPACITY),
          "arena_init on a fresh mapping");
   report(dlmalloc_offset_arena_attach(&view_a, base_a, ARENA_CAPACITY),
          "attach the initializing base");

   if( ! map_arena(fd, &base_b))
   {
      printf("  second mmap of the arena failed\n");
      return(1);
   }
   report(base_a != base_b, "second mapping lands at a different base");
   report(dlmalloc_offset_arena_attach(&view_b, base_b, ARENA_CAPACITY),
          "attach the relocated base");

   first = dlmalloc_offset_malloc(&view_a, 32);
   report(first != 0, "allocate through base A");
   text = (char *)dlmalloc_offset_resolve(&view_a, first);
   report(text == (char *)base_a + first, "resolve matches base A");
   if(text)
      strcpy(text, "alpha");

   text = (char *)dlmalloc_offset_resolve(&view_b, first);
   report(text == (char *)base_b + first && text && strcmp(text, "alpha") == 0,
          "base B sees the data written through base A");

   second = dlmalloc_offset_malloc(&view_b, 4096);
   report(second != 0, "allocate through base B");
   text = (char *)dlmalloc_offset_resolve(&view_b, second);
   if(text)
      fill_pattern(text, 4096, 7);
   report(dlmalloc_offset_usable_size(&view_b, second) >= 4096,
          "usable_size through base B");
   report(dlmalloc_offset_usable_size(&view_a, second) >= 4096,
          "usable_size of a base-B allocation seen from base A");

   report(dlmalloc_offset_free(&view_b, second), "free a base-B allocation");
   report( ! dlmalloc_offset_resolve(&view_b, second),
           "a freed offset no longer resolves");
   reused = dlmalloc_offset_malloc(&view_a, 1024);
   report(reused == second, "base A reuses the block freed through base B");
   report(dlmalloc_offset_free(&view_a, reused), "free the reused block");

   aligned_offset = dlmalloc_offset_memalign(&view_b, 64, 100);
   report(aligned_offset != 0, "memalign through base B");
   text = (char *)dlmalloc_offset_resolve(&view_a, aligned_offset);
   report(text != 0 && ((uintptr_t)text % 64) == 0,
          "base A resolves the memaligned block aligned");
   report(dlmalloc_offset_usable_size(&view_a, aligned_offset) >= 100,
          "usable_size of the memaligned block");
   report(dlmalloc_offset_free(&view_b, aligned_offset),
          "free the memaligned block");

   grown = dlmalloc_offset_realloc(&view_a, first, 8192);
   report(grown != 0, "realloc through base A");
   text = (char *)dlmalloc_offset_resolve(&view_b, grown);
   report(text != 0 && text[0] == 'a' && text[1] == 'l',
          "the reallocated block is readable through base B");
   report(dlmalloc_offset_free(&view_a, grown), "free the reallocated block");

   report( ! dlmalloc_offset_free(&view_a, 0), "free(0) is rejected");
   report( ! dlmalloc_offset_free(&view_a, ARENA_CAPACITY + 4096),
           "free of an out-of-range offset is rejected");
   report( ! dlmalloc_offset_resolve(&view_a, 3),
           "resolve of a bogus offset fails");

   check_arena_hygiene(base_a, ARENA_CAPACITY, "after the single-base checks");

   views[0] = view_a;
   views[1] = view_b;
   stress_two_views(views);

   if(pipe(child_to_parent) != 0 || pipe(parent_to_child) != 0)
   {
      printf("  pipe setup failed\n");
      return(1);
   }
   first = dlmalloc_offset_malloc(&view_a, 64);
   if(first)
      strcpy((char *)dlmalloc_offset_resolve(&view_a, first), "from-parent");

   fflush(stdout);   /* the child must not inherit buffered parent output */
   pid = fork();
   if(pid == 0)
   {
      int rc;

      close(child_to_parent[0]);
      close(parent_to_child[1]);
      rc = child_process(fd, child_to_parent[1], parent_to_child[0],
                         base_a, base_b);
      fflush(stdout);
      close(child_to_parent[1]);
      close(parent_to_child[0]);
      _exit(rc);
   }
   close(child_to_parent[1]);
   close(parent_to_child[0]);
   if(write(parent_to_child[1], &first, sizeof(first)) != (ssize_t)sizeof(first))
   {
      printf("  the parent could not publish its offset\n");
      return(1);
   }
   close(parent_to_child[1]);

   child_base   = 0;
   child_offset = 0;
   report(read(child_to_parent[0], &child_base, sizeof(void *)) ==
          (ssize_t)sizeof(void *),
          "the parent received the child mapping base");
   report(read(child_to_parent[0], &child_offset, sizeof(child_offset)) ==
          (ssize_t)sizeof(child_offset),
          "the parent received the child allocation offset");
   report(child_base != 0 && child_base != base_a && child_base != base_b,
          "the child attached at a third base");

   text = (char *)dlmalloc_offset_resolve(&view_b, child_offset);
   report(text == (char *)base_b + child_offset && text &&
          strcmp(text, "from-child") == 0,
          "the parent reads the child allocation through its own base");
   report(dlmalloc_offset_free(&view_b, child_offset),
          "the parent frees the child allocation");

   status = 0;
   report(read(child_to_parent[0], &status, 1) == 1 && status == 1,
          "the child freed the parent allocation");
   waitpid(pid, &child_status, 0);
   report(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0,
          "the child process exited cleanly");

   child_gone = dlmalloc_offset_resolve(&view_a, first);
   report(child_gone == 0, "the parent observes the child's free");
   check_arena_hygiene(base_a, ARENA_CAPACITY, "after the cross-process work");

   printf("  %d/%d checks passed\n", checks_run - checks_failed, checks_run);
   return(checks_failed == 0 ? 0 : 1);
}
