## 0.3.0


* Reference `GROWTH-SCHEME.md` instead of `DESIGN.md` throughout the API table of contents



feature: add growable, segmented shared-memory arenas

* arenas can now grow: ``dlmalloc_offset_arena_grow`` adds segments through embedder-supplied callbacks, each new segment twice the size of the previous
* every attached process notices growth automatically via an epoch counter and maps new segments on its next operation `(`dlmalloc_offset_arena_refresh`` for explicit control)
* new API: ``dlmalloc_offset_arena_adopt`` (attach to a just-built region), ``dlmalloc_offset_arena_verify`` (full on-demand check), ``dlmalloc_offset_arena_corrupted`,` ``dlmalloc_offset_arena_grow`,` ``dlmalloc_offset_arena_refresh``
* a growth that continues the previous segment extends it in place instead of appending, so reserved-window arenas stay single-segment and keep the fast addressing path
* **BREAKING**: ``dlmalloc_offset_t`` is now ``uintptr_t`,` making the on-arena format width-relative (format v5, incompatible with v2)
* **BREAKING**: per-operation validation is now compiled in only for debug builds `(`DLMALLOC_OFFSET_CHECKED`,` defaulting from `NDEBUG`); a release build corrupts rather than fails on a stale or foreign offset
* `nolocking` lets a private single-process arena skip the shared lock


## 0.2.0



* ``add_subdirectory`` consumers now get all-load dependencies propagated with ``WHOLE_ARCHIVE`` link semantics, and force-all-load linker fragments are no longer exported as ordinary CMake targets
* build-interface include dir is only advertised when the project actually ships an `include/` directory, avoiding a phantom `-I` for consumers


feature: support cmake `add_subdirectory` consumption

* dependencies now resolve to cmake targets when the library is pulled in via `add_subdirectory,` enabling link-time include/definition inheritance
* object libraries pick up dependency usage requirements (amalgamated include dir, build order) automatically
* reflect output can live in a config-specific directory (multi-reflect) via ``MULLE_SOURCETREE_CONFIG_NAME``



* API TOC moved to asset/dox/api/toc/index.md for better organization
* added documentation section to README with link to API summary


### 0.1.1

Various small improvements
