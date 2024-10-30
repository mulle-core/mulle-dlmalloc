# mulle-dlmalloc

#### Doug Lea's malloc.c extended for shared memory use.


# mulle-dlmalloc

Doug Lea's malloc.c extended for shared memory use.

**mulle-dlmalloc** assumes you compile it with `-DMSPACE_ONLY`.

mulle-dlmalloc is used by the [mulle-mmapallocator](//github.com/mulle-core/mulle-mmapallocator) to manage shared memory and seperate memory arenas.





| Release Version                                       | Release Notes
|-------------------------------------------------------|--------------
| ![Mulle kybernetiK tag](https://img.shields.io/github/tag/mulle-core/mulle-dlmalloc.svg?branch=release) [![Build Status](https://github.com/mulle-core/mulle-dlmalloc/workflows/CI/badge.svg?branch=release)](//github.com/mulle-core/mulle-dlmalloc/actions) | [RELEASENOTES](RELEASENOTES.md) |






### You are here

![Overview](overview.dot.svg)





## Add

**This project is a component of the [mulle-core](//github.com/mulle-core/mulle-core) library. As such you usually will *not* add or install it
individually, unless you specifically do not want to link against
`mulle-core`.**


### Add as an individual component

Use [mulle-sde](//github.com/mulle-sde) to add mulle-dlmalloc to your project:

``` sh
mulle-sde add github:mulle-core/mulle-dlmalloc
```

To only add the sources of mulle-dlmalloc with dependency
sources use [clib](https://github.com/clibs/clib):


``` sh
clib install --out src/mulle-core mulle-core/mulle-dlmalloc
```

Add `-isystem src/mulle-core` to your `CFLAGS` and compile all the sources that were downloaded with your project.


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
cmake -B build \
      -DCMAKE_INSTALL_PREFIX=/usr/local \
      -DCMAKE_PREFIX_PATH=/usr/local \
      -DCMAKE_BUILD_TYPE=Release &&
cmake --build build --config Release &&
cmake --install build --config Release
```


## Author

[Nat!](https://mulle-kybernetik.com/weblog) for Mulle kybernetiK  



