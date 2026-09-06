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
