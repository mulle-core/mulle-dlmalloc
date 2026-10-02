# mulle-dlmalloc

#### A relocatable allocator for one shared-memory arena.

**mulle-dlmalloc** keeps all of its allocator metadata in the region as
arena-relative offsets instead of process-local pointers, so the same arena can
be mapped at a different virtual address in every process that attaches it.
There is no trimming and no allocation outside the arena.

An arena starts as one fixed segment and grows by adding segments through the
embedder's callbacks. The backing store and the mappings are the embedder's job,
so dlmalloc never opens a file or calls mmap itself; without callbacks the arena
stays fixed. The API is offset based: `dlmalloc_offset_arena_init` /
`dlmalloc_offset_arena_attach`, and `dlmalloc_offset_malloc` and friends return
and accept `dlmalloc_offset_t` values. A pointer from
`dlmalloc_offset_resolve` is valid only in the calling process while the object
is live, so anything persistent must be an offset. The implementation is in
`src/dlmalloc.c` and needs no compile-time flags. Its one dependency is
[mulle-c11](//github.com/mulle-c/mulle-c11), for the size-class `floor(log2)`.

mulle-dlmalloc is used by the [mulle-mmapallocator](//github.com/mulle-core/mulle-mmapallocator) to manage shared memory and separate memory arenas.

See [DESIGN.md](DESIGN.md) for the arena layout, the block format, the growth
scheme and the embedder contract.





| Release Version                                       | Release Notes  | AI Documentation
|-------------------------------------------------------|----------------|---------------
| ![Mulle kybernetiK tag](https://img.shields.io/github/tag/mulle-core/mulle-dlmalloc.svg) [![Build Status](https://github.com/mulle-core/mulle-dlmalloc/workflows/CI/badge.svg)](//github.com/mulle-core/mulle-dlmalloc/actions) | [RELEASENOTES](RELEASENOTES.md) | [DeepWiki for mulle-dlmalloc](https://deepwiki.com/mulle-core/mulle-dlmalloc)




## Documentation & Guides

* [API Summary](asset/dox/api/toc)
* [Design](DESIGN.md)




### You are here

![Overview](overview.dot.svg)





## Add

mulle-dlmalloc is a component of the [mulle-core](//github.com/mulle-core/mulle-core) library. So in your code include the mulle-core umbrella header:

``` c
#include <mulle-core/mulle-core.h>
```

### Add mulle-core to a cmake and git project

``` bash
git submodule add https://github.com/mulle-core/mulle-core.git mulle-core
```

Add this to your `CMakeLists.txt`:

``` cmake
add_subdirectory( mulle-core)
target_link_libraries( ${PROJECT_NAME} PRIVATE mulle-core)
```


### Add mulle-core to a mulle-sde project

``` sh
mulle-sde add github:mulle-core/mulle-core
```

### Embed mulle-dlmalloc with clib

``` sh
clib install --out src mulle-core/mulle-dlmalloc
```

Append `src` to your include path (e.g. add `-isystem src`  to your `CFLAGS`)
and compile all the sources that were downloaded.

## Install

Use [mulle-sde](//github.com/mulle-sde) to build and install mulle-dlmalloc:

``` sh
mulle-sde install --prefix /usr/local \
   https://github.com/mulle-core/mulle-dlmalloc/archive/latest.tar.gz
```

### Legacy Installation


Download the latest [tar](https://github.com/mulle-core/mulle-dlmalloc/archive/refs/tags/latest.tar.gz) or [zip](https://github.com/mulle-core/mulle-dlmalloc/archive/refs/tags/latest.zip) archive and unpack it.

Install **mulle-dlmalloc** into `/usr/local` with [cmake](https://cmake.org):

``` sh
PREFIX_DIR="/usr/local"
cmake -B build                               \
      -DMULLE_SDK_PATH="${PREFIX_DIR}"       \
      -DCMAKE_INSTALL_PREFIX="${PREFIX_DIR}" \
      -DCMAKE_PREFIX_PATH="${PREFIX_DIR}"    \
      -DCMAKE_BUILD_TYPE=Release &&
cmake --build build --config Release &&
cmake --install build --config Release
```


## Author

[Nat!](https://mulle-kybernetik.com/weblog) for Mulle kybernetiK  



