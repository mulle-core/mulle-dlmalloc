# mulle-dlmalloc design

mulle-dlmalloc is a single-file, relocatable allocator for one shared arena. All
of its metadata lives in the arena as arena-relative offsets instead of
process-local pointers, so the same arena can be mapped at a different virtual
address in every process that attaches it. There is no trimming and no
allocation outside the arena.

The offset space is split into segments. An arena starts as one fixed segment and
grows through the embedder's callbacks; the backing store and the actual mappings
are the embedder's job. A growth that continues the last segment -- same address,
same backing offset -- extends that segment in place instead of publishing
another one, which is what a reserved window produces: the whole window is one
mapping, so the range that comes back is simply the next bytes of segment 0. A
growth that lands anywhere else appends a segment. dlmalloc never opens a file
and never calls mmap. Without callbacks the arena stays fixed.

The implementation lives in `src/dlmalloc.c` and needs no compile-time flags.
Its one dependency is [mulle-c11](//github.com/mulle-c/mulle-c11), for
`mulle_c_log2_floor`: the size class of a block is a floor(log2), and mulle-c11
is where that compiler branch belongs rather than in every library that needs
it. The library ships no public header -- it is an amalgamation, so consumers
compile the source in place, as mulle-mmapallocator does. The historic Doug Lea
`dlmalloc`/`mspace` allocator is gone.

## Arena layout and the block format

The first bytes of segment 0 hold everything that is not a block: the header,
the free-list bucket table and the segment directory. Blocks follow and tile
every segment exactly.

A block starts on a `DLMALLOC_OFFSET_ALIGN` boundary with a two-word header:

```
     size       total block size in bytes, low bit set while the block is in use
     prev_size  distance back to the predecessor's header, 0 = first in segment
```

Both words are `uintptr_t`, which makes the header exactly one alignment unit and
`DLMALLOC_OFFSET_ALIGN` equal to `2 * sizeof(uintptr_t)`: 16 on LP64, 8 on ILP32.
8 covers every fundamental type on the 32-bit targets; a 16-byte vector type
wants more, and that is what `memalign` is for.

Because blocks tile a segment exactly, the successor of a block sits at
`block_offset + size`, so coalescing needs only `size` and `prev_size` -- no
footer, and an in-use block is never written past the end of its payload.
`payload = block + DLMALLOC_OFFSET_BLOCK_SIZE` is as aligned as the block,
because the header is one alignment unit wide.

A free block keeps its bucket links in the first two payload words: `next` and
`prev` are arena offsets, 0 ending or starting a list. There are 32 power-of-two
classes starting at `DLMALLOC_OFFSET_MIN_BLOCK`; a request scans its own class
for a first fit, otherwise the head of the first non-empty higher class, which is
large enough by construction. A bitmap of non-empty classes makes finding that
class a shift, so allocation costs at most one list walk inside a single class.

## API surface

Types: `dlmalloc_offset_t` (a `uintptr_t` arena-relative byte offset of user
payload; 0 means null/failure) and `dlmalloc_offset_view` (the process-local
attachment context; never shared or persisted). An arena is an address space,
so every coordinate and length in it is a `uintptr_t`: the format is
width-relative but not width-portable, and the configuration word refuses the
wrong width.

Arena lifecycle:

- `dlmalloc_offset_arena_init(void *base, size_t capacity)` -- initialize once,
  in the creating process; also doubles as a reset when the arena is quiescent.
- `dlmalloc_offset_arena_attach(view, void *base, size_t capacity)` -- attach any
  process at any address; checks the versioned header and the segment directory.
  The blocks are checked per segment, on first use (see *Checking a segment on
  first use*), in a build that checks -- a release build by default does not (see
  *Validation: on, off and what it buys*).
- `dlmalloc_offset_arena_adopt(view, void *base, size_t capacity)` -- attach to a
  region this process has just built, taking the segments that exist as already
  checked. `init` + `adopt` is what a creator wants.
- `dlmalloc_offset_arena_verify(view)` -- run the whole check now instead of on
  first use: the directory, every segment's blocks and the free lists. It is
  compiled into every build, so this is how a release build asks for the check.
- `dlmalloc_offset_arena_detach(view)` -- discard process-local state only.
- `dlmalloc_offset_arena_grow(view, size_t min_size)` -- add a segment.
- `dlmalloc_offset_arena_refresh(view)` -- map any segments another process added.

Allocation, offsets in and offsets out: `dlmalloc_offset_malloc`,
`dlmalloc_offset_memalign` (power-of-two alignment up to 1 MiB),
`dlmalloc_offset_calloc`, `dlmalloc_offset_realloc`, `dlmalloc_offset_free`.

Inspection: `dlmalloc_offset_resolve` (offset to a process-local pointer, or NULL
when the offset is not a live allocation) and `dlmalloc_offset_usable_size`.

A `void *` from `resolve` is valid only in the calling process and only while the
object is live. It must never be written into shared memory or sent to another
process -- store the offset instead. A `realloc` that has to move an allocation
returns a new offset, which is the caller's cue to publish it.

## Building and testing

This is a library project, so `mulle-sde run` does not apply to it. Build it the
way any mulle-sde library is built, or with the cmake commands below.

The tests live under `test/`; each one includes `../../src/dlmalloc.c` directly
and is compiled and run by mulle-sde:

```sh
mulle-sde test --platform linux run test/30-fixed/fixed.c   # one test
mulle-sde test --platform linux run                         # all of them
```

- `test/10-relocate` -- one arena at two bases, plus a forked process at a third.
- `test/20-grow` -- filling a segment, growing, and two processes growing one
  shared arena; a forked process notices the growth on refresh.
- `test/30-fixed` -- the single-segment fast path, that a growth which continues
  segment 0 extends it while one that does not appends a segment, both an
  extension that merges into a free tail block and one that has to follow a live
  one, that a two-segment arena uses the directory and not the view's copy, and
  that a segment is checked by its first use rather than by `attach`.
- `test/40-benchmark` -- allocation churn and offset lookup timings, plus
  `hotpath.c`, which pins the one-segment fast path: nanoseconds and callgrind's
  instruction count per malloc/free pair go to stderr, where nothing is
  compared, and the number of block accesses that had to fall back to the
  segment directory goes to stdout, where it is: 0 while the arena has one
  segment, and not 0 once it has two.

The suite asserts the validation behaviour, so it is run against a checked
build: mulle-sde's Debug configuration does not define `NDEBUG`, which is what
`DLMALLOC_OFFSET_CHECKED` defaults from. Compiling the same tests with `-DNDEBUG`
makes seven of `test/30-fixed`'s checks fail on purpose; they are the ones that
assert a corrupt arena is noticed. Everything else, the refused offsets
included, holds in both builds.

`--platform` must come before the subcommand; without it the platform list comes
from the environment. The multi-process tests and the benchmark are the ones
worth running under valgrind.

## Growth: what it must guarantee

Let an arena grow beyond its initial mapping while keeping `dlmalloc_offset_t`
a plain flat offset. Growth must be triggerable by **any** attached process, and
every other attached process must notice the new space. It must work on Windows
as well as POSIX.

## Implementation status

| Layer | State |
| ----- | ----- |
| `src/dlmalloc.c` | format v5: segment directory, per-segment block rules, epoch + refresh, `arena_grow` extending the last segment when the range continues it and appending when it does not, the three callbacks, single-segment fast path, a segment checked by its first use (`attach` / `adopt` / `verify`). Fixed arenas behave exactly as before. Per-operation validation is compiled in only for a debug build. |
| `mulle-mmapallocator` | the **reserved-window** embedder implements the callbacks and `mulle_mmap_allocator_grow`. Growth is arithmetic over a window mapped at creation. `test/30-shared` passes on Linux and on Windows (`grow.stdout.linux`, `grow.stdout.windows`). |
| file-backed embedder | **not built.** It stays the option for attaching by path, or when the ceiling cannot be named at creation. |

The two layers are decoupled on purpose: dlmalloc only ever sees callbacks, so
the window and the file are both just embedders of the same format. What is
described below as *the* design is the format and the dlmalloc contract; the
choice of backing store belongs to the embedder and is covered in
[Embedders](#embedders).

## Design decisions

1. **Flat offsets, multiple segments.** `dlmalloc_offset_t` stays a single
   pointer-sized number (`uintptr_t`). The offset space is split into segments
   that are mapped independently. Segment 0 holds the global header, the free-list bucket table
   and the segment directory. There is no `(segment_id, offset)` pair.
2. **The backing store is the embedder's business.** dlmalloc never opens files
   or calls mmap; it asks the view for a segment. The shipped embedder maps a
   reserved window once and grows into it arithmetically, which makes a segment
   addition a few stores under the lock. A file-backed embedder — a file has a
   path every process can open, can be extended in place
   (`ftruncate` / `SetEndOfFile`), and each process can map any byte range — is
   the variant for a process that must discover the arena by path, or that
   cannot afford to map the whole ceiling up front.
3. **Any process may grow.** Growth is a normal, lock-protected operation.
4. **Doubling segment sizes.** Segment 0 starts at `S0`; each new segment is
   twice the previous one, floored at `DLMALLOC_OFFSET_MIN_SEGMENT` (1 MB) and
   at the caller's `min_size`. Doubling keeps the segment count tiny — 27
   segments cover 2^48 from `S0 = 2 MB` — which matters because every segment
   is one entry in *every* process's local base table, and in a file-backed
   embedder one more mapping.
5. **Epoch + refresh on enter.** Shared metadata carries an `epoch` counter.
   Every operation already takes the arena lock; `enter()` compares the local
   epoch with the shared one and maps any segments it has not seen yet.
6. **dlmalloc stays platform-agnostic.** The process-local view carries
   callbacks that the embedder implements. If no callbacks are set, the arena
   is fixed and `grow` fails — existing single-segment behaviour is unchanged.

## Offset space and the segment directory

The offset space is the concatenation of all mapped segments. Offsets are flat
and never change meaning, even as segments are appended:

```
 flat offset    0              S0            S0+S1         S0+S1+S2
            +--------------+--------------+--------------+--------------+
            |  segment 0   |  segment 1   |  segment 2   |   ...
            | hdr+buckets  |   blocks     |   blocks     |
            | + directory  |              |              |
            +--------------+--------------+--------------+--------------
             offset_base=0  offset_base=S0  offset_base=S0+S1
             size=S0        size=S1         size=S2
```

Segment `i` has a flat `offset_base`, a `size`, and a `backing` byte offset in
the backing store. Offsets are assigned monotonically, so
`offset_base[i+1] = offset_base[i] + size[i]`.

The first 4 KB of segment 0 hold everything that is not a block -- 1616 bytes on
ILP32, 3200 on LP64:

```
 byte 0   +--------------------------------------+
          | dlmalloc_offset_header               |  magic, version, epoch,
          |                                      |  segment_count, total_offset_space, ...
          +--------------------------------------+
          | free-list bucket table               |  32 classes: head, tail, bitmap
          +--------------------------------------+
          | segment directory                    |  64 entries * 40 B = 2560 B
          |  [0] [1] [2] [3] ........... [63]    |
          +--------------------------------------+  <- blocks_offset
          | blocks of segment 0                  |
          | [blk][blk][free.............][blk]   |
          +--------------------------------------+  <- byte S0 = start of segment 1
```

Each directory entry is a flat range, where blocks start in it, and a backing
range:

```
        offset_base     size        blocks_base      backing
      +--------------+--------------+--------------+--------------+
 [0]  |      0       |      S0      | blocks_offset|      0       |  segment 0
 [1]  |      S0      |      S1      |      S0      |      S0      |  segment 1
 [2]  |    S0+S1     |      S2      |    S0+S1     |    S0+S1     |  segment 2
      +--------------+--------------+--------------+--------------+
         \____ flat offsets ____/                   \__ backing __/
```

In the simple "append at the end" layout `backing == offset_base`. They are
separate fields so a segment could also live in a different backing object.
`blocks_base` is `offset_base` for every later segment; it differs only for
segment 0, where the header, bucket table and directory come first.

Header additions (format version 4; the directory entry gained `tail_size` in
version 5):

```c
uintptr_t epoch;              /* bumped on every growth */
uintptr_t segment_count;
uintptr_t directory_offset;   /* inside segment 0 */
uintptr_t directory_capacity; /* DLMALLOC_OFFSET_MAX_SEGMENTS */
uintptr_t total_offset_space; /* high-water offset */
```

Directory entry (20 bytes on ILP32, 40 on LP64):

```c
struct dlmalloc_offset_segment
{
   uintptr_t offset_base;  /* flat offset of the first mapped byte */
   uintptr_t size;         /* mapped bytes, multiple of DLMALLOC_OFFSET_ALIGN.
                                 Grows when the segment is extended in place */
   uintptr_t blocks_base;  /* flat offset where blocks start */
   uintptr_t backing;      /* byte offset in the backing store */
   uintptr_t tail_size;    /* total size of the block that ends the segment */
};
```

`tail_size` is what makes an in-place growth constant time (see the growth
section below). Blocks record the size of the block *before* them, so the last
block of a segment has nowhere to record its own, and `set_size` keeps it here
instead. A segment whose tiling does not end in a block of exactly that size
fails its check.

Segment 0 reserves `DLMALLOC_OFFSET_MAX_SEGMENTS * sizeof(entry)`. With a max of
64 that is 1280 bytes on ILP32 and 2560 on LP64, which still leaves the whole
header, bucket table and directory inside the first 4 KB page -- 3232 bytes on
LP64, of the 4096 there are.

## The process-local view

```c
typedef struct dlmalloc_offset_view
{
   void     *base;        /* this process's mapping of segment 0 */
   size_t    capacity;    /* mapped length of segment 0 */
   uintptr_t epoch;       /* last seen shared epoch */
   uintptr_t segment_count;
   uintptr_t total_offset_space;  /* high-water offset as last seen */
   uint64_t  verified;    /* one bit per segment: its tiling is checked.
                             Fixed 64 bits, because MAX_SEGMENTS is 64 */
   int       corrupted;   /* a segment failed its check; grow then refuses */
   int       grew;        /* the last refresh saw the arena grow */
   int       nolocking;   /* embedder guarantees exclusive access */
   void     *local_base[DLMALLOC_OFFSET_MAX_SEGMENTS];

   /* single-segment fast path: a copy of directory[0] that follows the
      published entry when the epoch moves. 0 means "not built yet" */
   struct dlmalloc_offset_segment segment0;

   /* embedder supplied, optional; growth is disabled when NULL */
   int     (*grow_segment)( void *context, size_t min_size,
                            uintptr_t *backing, void **base, uintptr_t *size);
   int     (*map_segment)( void *context, uintptr_t backing, size_t size,
                           void **base);
   void     (*unmap_segment)( void *context, void *base, size_t size);
   void     *context;
} dlmalloc_offset_view;
```

- `offset -> ptr`: find the segment whose range contains the offset, then
  `local_base[i] + (offset - offset_base[i])`; with a single segment that is
  simply `base + offset`, which is what the fast path below takes.
- The local base table is embedded (no libc allocation). A zeroed view still
  means a single segment described by `base`/`capacity`, so existing call sites
  keep working.
- `base` is `local_base[0]`, and segment 0 is always answered from `base`.

### Single-segment fast path

`segment_find` sits under every block primitive — `block_at`, `links_at`,
`blocks_end` and `set_size` all reach for it — so a fixed arena would otherwise
search the shared directory on every operation. A one-segment arena has nothing
to search:
`header_valid` has already pinned `directory[0]` to `offset_base == 0`,
`blocks_base == header->blocks_offset` and `size >= view->capacity`, so the copy
of that entry is enough to describe the whole arena. `segment_find` takes the
copy into `segment0` on first use and returns a pointer to it; no directory read
and no search, and `set_size` resolves the segment once instead of three times.

Two things about the copy are worth stating. Its `size` is the segment's *current*
size and not `view->capacity`, which is only the length segment 0 was created
with -- a reserved window grows the segment past it, so a copy that still said
`capacity` would refuse offsets in the part that was added. And the copy is a
cache, not a constant: `refresh_locked` puts the published entry back into it
when the epoch moves, and drops the segment's check bit when the size moved with
it, because the bytes that changed are the ones this process did not write.

Only a view that reports a single segment (`segment_count == 1`, or `0` for a
zeroed view) takes this path. Everything else binary searches the directory,
which is sorted by `offset_base`.

#### The lookup is not the whole cost, so it goes too

That fast path answers *from the view*, but every access still entered
`segment_find`, got an index and an entry back, and computed
`local_base[i] + (offset - offset_base[i])`. Measured, that segment plumbing
was 702 instructions per 64 byte malloc/free pair against a fixed 1 MiB arena,
more than the whole pre-segment allocator spends on a pair.
`dlmalloc_offset_direct_locked` and the four primitives under it (`block_at`,
`links_at`, `blocks_end`, `set_size`) take it out. One segment starts at offset
0 and never moves, so a flat offset *is* a displacement from this process's
base and the block is `view->base + offset`: no index, no entry, no directory.
`set_size`, which used to resolve once and pass the entry down, now only needs
one to record `tail_size` on the segment's last block.

The predicate is a question for the checked build, not a correctness one. An
unchecked build is direct from the first access, because there is nothing to
check. A checked build is direct once the segment has passed its tiling check --
the walk runs on the first access and the verified bit answers every one after
it -- and goes back to the slow path when a growth clears that bit, which is the
case the bit exists for. The functions that validate an offset a *caller* handed
in (`payload_valid_locked`, `link_target_valid`, `resolve`, `usable_size`) keep
calling `segment_find` and keep their bounds check; only the primitives, which
address offsets they took from the arena itself, skip it.

On the same 1 MiB pair: **988 instructions in an unchecked build** (from 1364,
against 662 for a stub that takes the segment as given and cannot report a
missing one) and **1287 in a checked one** (from 1769). Wall clock in the same
harness, medians of 60 interleaved samples: 66 ns unchecked, from 93, against
the stub's 49 and the flat arena's 49. What remains above the stub is mostly the
predicate itself, which every primitive still tests.

Those numbers come from scratch harnesses built for the comparison, and they are
not what is guarded. `test/40-benchmark/hotpath.c` is: it walks the same 64 byte
pair on a one-segment and a two-segment arena and counts the block accesses that
fell back to the directory -- the counter is compiled in only for that test, by
defining `DLMALLOC_OFFSET_COUNT` before it includes the source -- and compares
the count, which is 0 with one segment and 17 per pair with two. It reports its
own instruction count for its own loop on stderr (1292 checked, 843 unchecked on
this compiler, against 1287 and 988 above: same quantity, different harness and
inlining), where a human can see it and no golden depends on the toolchain.
Disabling the fast path moves the one-segment count from 0 to those same 17 per
pair, fails the check, and fails the test.

Two processes, one arena, unrelated local addresses. The offsets are shared;
the local bases are not:

```
                 shared bytes (one backing)
                 offset space
               +----------------+   segment 0
               | header + dir   |
               | blocks         |
               +----------------+   segment 1
               | blocks         |
               +----------------+   segment 2
               | blocks         |
               +----------------+

   process A                                  process B
   base_A      = 0x50000000                   base_B      = 0x7f200000
   local[0]    = 0x50000000                   local[0]    = 0x7f200000
   local[1]    = 0x60000000                   local[1]    = 0x7f400000
   local[2]    = 0x68000000                   local[2]    = 0x7f800000

   offset 0     -> A: 0x50000000   B: 0x7f200000
   offset S0    -> A: 0x60000000   B: 0x7f400000
   offset S0+S1 -> A: 0x68000000   B: 0x7f800000
```

Resolving one offset:

```
   offset = S0 + 0x240

   segment_find:  S0 <= offset < S0 + S1        ->  index 1

   local pointer  = local_base[1] + (offset - offset_base[1])
                  = local_base[1] + 0x240
```

### Checking a segment on first use

**Everything in this section is a checked build feature.** The check it
describes, the header and directory validation above it, and the free list and
payload checks below it are compiled in only when `DLMALLOC_OFFSET_CHECKED` is
1, which defaults to `NDEBUG` being absent. A release build runs unchecked. See
*Validation: on, off and what it buys*.

Attaching to an arena is not where the blocks get read. `attach` checks the
header and the directory and stops there: every bit in `verified` is clear, and
the first offset that lands in a segment is what checks that segment's tiling --
the blocks start at `blocks_base`, are aligned, at least
`DLMALLOC_OFFSET_MIN_BLOCK` long, never overrun the segment, each records the
size of the block before it, and the last of them is the one the entry's
`tail_size` names. The bit is then set, and the segment is not read for checking
again -- unless the arena extends it. `refresh_locked` clears the bit when a
published size moved, because the bytes that changed are the ones this process
did not write, so the next use of the segment walks it again. The process that
performed the growth keeps its bit: it laid the new bytes out itself.

**The bit lives in the view, so the view has to be reused.** That is a
requirement on the embedder, not a property of the check. An embedder that builds
a fresh view for every operation clears `verified` every time, and then the check
is a walk of a segment on every operation rather than a one-off -- measured at
650x slower for 40000 64 byte allocations, and quadratic in the number of them.
It hits a fixed arena too, because its single segment is checked down the same
path. Hold one view per arena for as long as the arena is in use;
mulle-mmapallocator keeps it in the allocator.

That is a change from checking everything at attach, which cost one pass over the
whole arena while holding the lock every process shares:

- a process that uses one segment of a grown arena reads only that segment;
- attaching cannot stall the other processes for the length of a walk over a
  region that may be cold, or swapped out;
- the check still happens before any block header in that segment is
  dereferenced, because `segment_find` is the only way to reach one.

A segment that fails the check fails the operation that found it: `segment_find`
reports "no such segment", and callers only go on to `block_at` or `links_at` an
offset after a `segment_find` for the same segment returned 1. `test/30-fixed`
pins it: a block size scribbled to an impossible value makes the next allocation
in that segment fail rather than misbehave.

Reporting it as "no such segment" is right for those callers and wrong for the
embedder. A failed allocation is the signal an embedder retries by growing, and
the two things that cause one -- no room, and a segment that failed its check --
want opposite answers. Conflated, one scribbled block header makes a retry loop
add segments until the window is used up: a fault of a few bytes spends the whole
shared arena, and reports itself as out of memory. So a failed check also sets
`corrupted` in the view, and `dlmalloc_offset_arena_grow` refuses once it is set.
The caller fails once and can say why: `dlmalloc_offset_arena_corrupted`.
`attach` and `adopt` clear the flag, so the way out stays the documented one --
discard the arena, rebuild it, attach again.

All of this is checked-mode behaviour. An unchecked build never checks a segment
tiling, so it never sets `corrupted`, so it never refuses a growth for one: a
failed allocation there means no room, as it did before the segment checks
existed. `attach` and `adopt` still validate the header and the directory, and
`verify` still checks everything, on demand.

Two entry points cover what the lazy check does not:

- `dlmalloc_offset_arena_adopt` is `attach` for a region the calling process has
  just built. `arena_init` has no view to record that on, so the next call says
  it: the segments that exist are taken as checked, and a fresh arena is never
  walked back over what was just written to it. mulle-mmapallocator's `_init`,
  `_init_growable` and `_reset` use it.
- `dlmalloc_offset_arena_verify` runs the whole check now -- the directory, every
  segment's tiling, and the free lists, which must describe exactly the free
  blocks once each. That last part is the one check that cannot be made per
  segment, because it counts across all of them, so it lives here. One pass over
  the arena under the lock, like any other operation.

A grown segment is marked checked by the process that grew it, because that
process laid the segment out itself.

## Growth: extending or adding a segment

`grow` asks the embedder for a range and then publishes it in one of two ways.

If the range continues the segment before it -- the same address and the same
backing offset, that is, literally the bytes after it -- that segment is
**extended**: `size` grows, the new bytes become its last block, and the segment
count does not move. A reserved window always takes this path, because the whole
window is one mapping. Otherwise the range becomes a **new** directory entry,
which is the path for an embedder whose segments are separate objects and whose
range lands wherever the system put it, like the file-backed one.

Extending is worth going out of the way for. A second segment costs the
single-segment fast path on every later operation, and it puts a boundary in the
middle of the arena: a block never crosses one, so a request the arena has room
for can still fail because the room lies on both sides of that boundary, and
`grow` answers by doubling the arena. A segment that is extended has neither
problem, and `DLMALLOC_OFFSET_MAX_SEGMENTS` stops being a ceiling for it.

What makes it cheap is `tail_size`. The extension has to merge with the block
that ends the segment when that block is free -- that is what turns a request
spanning the former end into one ordinary free block -- and blocks record the
size of the block *before* them, so the last one has nowhere to record its own.
Without the field, every growth would walk the segment to find it.

Sizes double, floored at `DLMALLOC_OFFSET_MIN_SEGMENT` and at the caller's
`min_size`. Appended, each segment is twice its predecessor:

```
  segment 0        segment 1          segment 2             segment 3
 +----------+   +--------------+   +----------------+   +----------------------+
 |   S0     |   |    S1 = 2*S0 |   |  S2 = 4*S0     |   |    S3 = 8*S0         | ...
 +----------+   +--------------+   +----------------+   +----------------------+
 offset: 0        S0                 S0+S1 (3*S0)         S0+S1+S2 (7*S0)
 total:  S0       3*S0               7*S0                 15*S0

 cumulative after n segments = S0 * (2^n - 1)     ~27 segments cover 2^48 from 2 MB
```

Extended, the doubling is on the arena itself -- `size = 2 * entry[0].size` -- so
the totals run `S0`, `3*S0`, `9*S0`, `27*S0`. Three times per growth instead of
two, still geometric, still amortized.

Neither ladder is a closed form. A large `min_size`, an embedder that cannot fill
a request (a window's last growth is cut to whatever is left of it), a floor, and
a segment that ends in a live block all shorten a step, so **sizes come out of
the directory**. Anything that needs to locate a segment must read the directory;
the binary search is the generic path.

Why doubling and not a fixed step: every segment is one more mapping (and, on
Windows, one more mapped file range) in *every* process, so a small segment count
matters more than a small last segment.

`int dlmalloc_offset_arena_grow( view, size_t min_size)`:

1. take the lock, refresh if the shared epoch moved
2. **refuse to grow if that refresh saw the arena grow already** -- report
   success and let the caller retry, see below
3. compute the next size with the doubling policy, at least `min_size` and at
   least `DLMALLOC_OFFSET_MIN_SEGMENT`
4. call `grow_segment( context, size, &backing, &base, &size)`; the embedder
   makes the new range and returns where it landed (it may return less than
   asked for)
5. if that range continues segment 0, extend it: check that `tail_size` names a
   real block ending at the segment's end, merge the new bytes into that block if
   it is free or put a new free block after it if it is not, and grow the size
6. otherwise append a directory entry `{ offset_base = high_water, size, backing }`
   and seed it as one free block (`prev_size = 0`, `size = segment size`)
7. `total_offset_space` to the new end in both cases, `segment_count++` in the
   appended one, `epoch++`
8. unlock

### One shortfall, one growth

`grow` is only reached through a failed allocation, and every allocation
refreshes on entry, so the refresh in step 1 knows what the allocation that
failed did not: whether the arena grew in the meantime. If it did, that growth is
the room the caller is about to retry for, so growing here spends the arena on a
shortfall that is already covered. Four processes that hit a full arena together
would otherwise add 2S, 4S, 8S and 16S for one request -- address space for the
window embedder, and real disk for the file-backed one.

This is why `refresh_locked` compares `total_offset_space` and not just the
segment count: a window arena grows without ever gaining a segment, so a
segment count that did not move does not mean nothing happened.

So `grow` reports success without growing, and the caller's retry finds what its
sibling added. If the retry still fails, nothing appeared in the
meantime, the flag the refresh set is clear, and the next call to `grow` adds one
as usual. That is also what bounds the embedder's retry loop.

The number of segments is therefore a function of who grew when, not of how many
times `grow` was called. What stays fixed is the shape: a call adds at most one
segment, and each segment added doubles the one before it.

```
  dlmalloc                         embedder (mulle-mmapallocator)        window
  --------                         -----------------------------        ------
  lock
  refresh if needed
  size = 2 * last            --->
                                   base = window + high_water       ->  no syscall
                                   return(backing=high_water, base, size)
  directory[n] = { high_water, size, backing }
  seed one free block
  segment_count++, epoch++
  unlock

  another process: next enter() sees epoch changed, calls map_segment for the
  new directory entry, then allocates in it
```

`grow_segment` runs under the lock. The embedder must not take the arena lock
again. dlmalloc never grows on its own: an allocation that fails is retried by
the embedder after it calls `grow`, so the retry policy lives with the code that
owns the backing store.

## Noticing growth: epoch + refresh

In `dlmalloc_offset_enter()`, after acquiring the lock -- which is skipped when
`view->nolocking` says the embedder is the only one in the arena:

```c
if( header->epoch != view->epoch)
   dlmalloc_offset_refresh_locked( view);
```

Two processes, one grower and one that notices on its next call:

```
  process A (grows)                         process B (idle, view->epoch = 4)
  ------------------------------------      ------------------------------------
  lock
  grow_segment(...)  -> new segment 4
  directory[4] = {...}
  segment_count = 5
  epoch = 5
  unlock

                                            lock
                                              header->epoch (5)
                                                 != view->epoch (4)
                                              refresh_locked:
                                                map_segment(directory[4])
                                                view->epoch = 5
                                              allocate -> lands in segment 4
                                            unlock
```

`refresh_locked` re-reads `segment_count` and, for every segment with
`local_base[i] == NULL`, calls `map_segment` and stores the result, then updates
`view->epoch`. The header's `segment_count` and `epoch` are only ever written
together, in `arena_init` and `arena_grow`, so they always agree. Consequences:

- A process can never allocate in a segment it has not mapped: it refreshes
  before use.
- Concurrent growers are serialized by the lock; each appends one entry and
  bumps the epoch.
- An idle process picks the change up on its next call. An explicit
  `dlmalloc_offset_arena_refresh( view)` is provided for callers that want
  determinism.
- Mapping while holding the lock is a slow critical section. With the window
  embedder `map_segment` is arithmetic, so it is not; with a file-backed one it
  is, and the two-phase "notice under the lock, drop it, map, re-lock" variant
  is the escape hatch.

## Allocator rules across segments

A block never crosses a segment boundary and blocks in different segments never
coalesce. `blocks_end` becomes per-segment:

- `set_size` writes the successor's `prev_size` only when
  `block_offset + size < segment_end`.
- `release_block` merges forward only when the successor is in the same segment.
  Backward merge is already safe: the first block of a segment has
  `prev_size == 0`.
- `find_free`, `payload_valid`, `usable_size`, `memalign`, `realloc` bound
  themselves by the segment end.

Segment 0's blocks start after the header, directory and bucket table. Every
later segment's blocks start at `offset_base`.

## Embedders

### Reserved window (shipped)

`mulle-mmapallocator` under `_mulle_mmap_allocator_init_growable()` maps the
whole ceiling at initialization — `mulle_mmap_alloc_shared_memory( growth_limit)` in
shared mode, `mulle_mmap_alloc_pages( growth_limit)` otherwise — and starts the
arena as a smaller first segment. The callbacks are then arithmetic:

```c
grow_segment : base = p->base + total_offset_space; return that range
map_segment  : base = p->base + backing;            return that range
unmap_segment: no-op; the window is one mapping, released as a whole
```

- A segment is a range of the one window, so growing is a few stores. Nothing
  blocking happens under the arena lock.
- Every process maps the entire ceiling, and the ceiling is fixed at creation.
  That is the price; 1 TB of reserved address space is free on 64-bit, and on
  shared memory the pages commit when touched.
- It does not matter that an unrelated process cannot see a *later* segment:
  there are none. The whole window exists from the start, so an attacher that
  has the handle (fork, or `CreateProcess` with the inherited handle) maps the
  same window. Later segments are discovered through the directory, exactly as
  in the multi-segment model above.

### File-backed (optional)

One file. The creator decides the path (or a temporary file) and publishes it.
Segment 0 is `[0, S0)` of the file, segment `i>0` is
`[backing_i, backing_i + size_i)`, and growth extends the file by `size_i`,
where `backing_i` is the file size before the extension.

```
  one backing file, grown by ftruncate
 +---------+---------+--------------+----------------------+
 |  seg 0  |  seg 1  |   seg 2      |       seg 3          | ...
 +---------+---------+--------------+----------------------+
 file: 0        S0      S0+S1           S0+S1+S2
        ^        ^         ^                 ^
        |        |         |                 |
     backing[0] backing[1] backing[2]     backing[3]

  a new process:
     open(path) -> map(backing[0], S0)   /* read header + directory */
                -> map(backing[1], S1)
                -> map(backing[2], S2)   /* one mapping per segment, any address */
                ...
```

On Windows this is a regular file mapped with `mulle_mmap_file_open` +
`mulle_mmap_memory_map`; on POSIX the same, or a `memfd` when only local
processes are involved.

What it buys over the window: a process that only has a path can attach, a
segment can be mapped only once it exists instead of reserving the whole
ceiling, and the ceiling is no longer chosen at creation (it is bounded by the
flat offset space instead). What it costs: `grow_segment` now does
`ftruncate` + `mmap` **while holding a process-shared, non-robust lock**, so a
grower that dies mid-callback wedges the arena and a multi-GB extension stalls
every other process. It also changes the threat model — mode 0600, a
non-world-writable directory, and validation of a directory entry against the
file size on refresh — and on NTFS the file must be marked sparse or the
doubling ladder allocates real disk.

Pick it only when attach-by-path or a bounded-address-space attach is the actual
requirement. The format and dlmalloc need no change either way.

## Validation: on, off and what it buys

Every operation enters through four checks: `header_valid` (the shared metadata
still describes an arena of this build), `link_target_valid` (the free list a
search walks is self consistent), `payload_valid_locked` (the offset a caller
hands back names a live block of the size it claims) and `segment_check` (one
segment's block tiling, once per segment per process).
`DLMALLOC_OFFSET_CHECKED` compiles them in, and defaults to `NDEBUG` being
absent -- the convention `MULLE_C_ASSERT` uses in mulle-c11, so a debug build
checks and a release build does not.

```sh
cc -DNDEBUG ............................... unchecked (release)
cc ......................................... checked   (debug)
cc -DDLMALLOC_OFFSET_CHECKED=1 -DNDEBUG .... checked in a release build
cc -DDLMALLOC_OFFSET_CHECKED=0 ............. unchecked in a debug build
```

What an unchecked build still does: `attach` and `adopt` check the header and
the whole directory, and `verify` checks every block tiling and every free list
on demand. What it no longer does: notice a change to the arena between those
checks, and refuse an offset that does not name a live block. Freeing an offset
twice, freeing one the arena never handed out, or using an arena another process
is halfway through changing corrupts it instead of failing. Two offsets stay
refused in both builds, because freeing either has to leave the arena alone:
zero, and a misaligned one. `test/30-fixed` pins that floor in both builds, and
pins the double free under `DLMALLOC_OFFSET_CHECKED`; seven of its checks are
the ones a release build cannot make, and they are all the lazy segment check
and what follows from it.

Measured as instructions retired per 64-byte malloc/free pair against a fixed
1 MiB arena -- valgrind counts, which no code layout can flatter:

| build | instructions per pair | ns/op |
| ----- | --------------------- | ----- |
| flat arena, before the segment directory (format v2) | 851 | 49 |
| checked (debug, or `DLMALLOC_OFFSET_CHECKED=1`) | 1769 | 111 |
| unchecked (`-DNDEBUG`) | 1364 | 93 |

The ns/op column is the median of 48 interleaved samples of 500000 operations,
pinned to one core; it is the least trustworthy number here, because two
binaries differ by 10-15% whatever they contain. Trust the instruction counts.
They say validation is 405 of the 918 instructions the segment directory added,
about 44% of the overhead, and that turning it off stops well short of the flat
arena: the other 513 are the segment lookup and the segment-relative addressing
themselves -- `segment_find` per block access, and a base of
`local_base[index] + offset - offset_base` instead of `base + offset`.

Those are not a switch's business, but they are not immovable either: they are
exactly what the one-segment addressing fast path removes (see *The lookup is
not the whole cost, so it goes too*). With it, the same pair costs **988
instructions unchecked** and **1287 checked**, at or below the flat arena's 851
once the checks are off as well.

## Crash and locking

Unchanged: a single process-shared lock, no owner tracking, no crash recovery.

`view->nolocking` turns that lock off for an arena the embedder states no other
process and no other thread touches. It is worth having because the lock is not
free: an atomic exchange and its release per operation, measured at 17-29 ns
aside a 120 ns hot path, 13-15% of it. A zeroed view still locks, so this is
opt-in and nothing that existed before changed behaviour. On a target whose
atomics are not available, `dlmalloc_offset_lock` cannot succeed at all, and
`nolocking` is then the only way a private arena can be used. It is a promise,
not a check: taking it wrongly is a data race.
Growth adds one failure mode — a process dying between making the new range and
publishing the directory entry leaves a harmless orphan range; a process dying
after publishing but before the epoch bump leaves a segment nobody maps until
the next grow. Both are covered by the existing "discard or reinitialize after a
crash" policy. The directory is only mutated under the lock, and **a segment is
never removed**. That invariant is what makes the design safe: `local_base[i]`
is fill-once, published directory entries are immutable, so no resolved pointer
can go stale and there is no ABA hazard. A future shrink would turn every
resolved pointer in every process into a landmine, so it is off the table.

## Staged plan

1. **[done]** Format v3 in `src/dlmalloc.c` (the header additions below bumped
   it to v4, and extending a segment in place to v5): directory, per-segment
   block rules,
   epoch, refresh, `grow` with callbacks. Single-segment arenas behave exactly as
   before; the existing suite stays green.
2. **[done]** Growth tests: `test/20-grow/grow.c` fills segment 0, grows,
   attaches a second view at another base, and has a forked process notice the
   growth on refresh and allocate in the new segment; `grow-both.c` has two
   processes grow one shared arena and pins that a call adds at most one segment
   and that the added ones keep doubling. `test/30-fixed/fixed.c` pins the
   single-segment fast path, that a reserved window's growth extends segment 0
   while a discontiguous embedder's appends a second one, and -- with two views
   of one arena, one of them stale -- that a grow which finds the shortfall
   already covered adds nothing.
3. **[done]** mulle-mmapallocator: the reserved-window embedder implements the
   three callbacks and exposes `mulle_mmap_allocator_grow`;
   `test/30-shared/grow.c` covers it.
4. **[done]** Windows: `test/30-shared/grow.stdout.windows` is the cross-compiled
   run of the same test, with the same arithmetic callbacks.
5. **[optional, not started]** File-backed embedder, if a use case needs
   attach-by-path or refuses to reserve the ceiling.
6. **[done]** The format is width-relative rather than 64-bit-only. Every
   coordinate and length in the arena is a `uintptr_t`, so ILP32 lays out an
   arena at half LP64's size: block header 8 vs 16, directory entry 20 vs 40,
   arena header 72 vs 112. A 32-bit process creates and uses an arena of its
   own; what it cannot do is attach to one built by the other width, and not
   because a `size_t` is too narrow. `header_valid` compares
   `header->configuration`, which pins `sizeof(void *)` and `sizeof(size_t)`, and
   stays a fixed 64-bit word precisely so two widths can disagree about it. The
   refusal is fail-closed and happens before a single byte is mapped (measured,
   `zz-width.c`). An arena is at most one address space wide, which was already
   true because segments stay mapped for the life of the view.

## Implementation notes

- `dlmalloc_offset_view` grew a fixed `local_base[DLMALLOC_OFFSET_MAX_SEGMENTS]`
  table, a `segment0` cache, the two process-local flags (`corrupted`, `grew`) and
  the callbacks. A zeroed view still means a single segment described by
  `base`/`capacity`, so existing callers that build a view on the fly keep
  working (they simply cannot grow).
- `dlmalloc_offset_view` is process-local and never persisted, so its layout is
  free to change; only the header, the directory and the block format are on the
  wire. The format version guards those.
- `DLMALLOC_OFFSET_MAX_SEGMENTS` is 64 and a directory entry is five
  `uintptr_t`, so the directory is 1280 bytes on ILP32 and 2560 on LP64; either
  way a single 4 KB header page holds header, bucket table and directory (1616
  and 3200 bytes respectively before the blocks start). Neither the 64 entries
  nor the arithmetic of `offset_base + size` is the binding constraint, and
  `grow` needs no overflow check: that sum is the high-water flat offset, and
  every segment stays mapped for the life of the arena, so it cannot exceed one
  address space. The doubling ladder would pass `uintptr_t` at segment 43 with
  `S0 = 2 MB` on LP64, and at segment 11 on ILP32 -- which is the narrower
  pointer reaching the ceiling *sooner*, not later -- but no arena can be mapped
  that far in the first place. And an extended segment does not even add a
  directory entry to run out of.
- The arena is append-only in *offset space*: segments are never removed,
  `local_base[i]` is written once, and a published entry's `size` is the only
  field that ever moves.
- The lock is held while `grow_segment`/`map_segment` run. The embedder
  callbacks must not re-enter the arena.
- The format is width-relative: everything that measures the arena is a
  `uintptr_t`, so ILP32 halves the per-block, directory and header overhead.
  LP64 is byte-identical to what it was before, because `uintptr_t` is
  `uint64_t` there. Two things stay fixed 64-bit on purpose: the `configuration`
  word, which has to be comparable across widths, and the `verified` mask, which
  holds one bit per segment with 64 segments.
- The width pin is the `configuration` word, not a `size_t` bound. A narrow
  process never reaches `map_segment` when it meets a wide arena:
  `header->configuration` disagrees and `header_valid` returns 0 first. There is
  no narrowing left to worry about either, now that segment sizes are `uintptr_t`
  and `size_t` is pointer-sized on every ABI this runs on. The
  `view->capacity > (size_t)UINTPTR_MAX` test in `header_valid` is dead at both
  widths and could be deleted.
