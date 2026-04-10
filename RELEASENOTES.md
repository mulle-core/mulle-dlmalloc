## 0.1.0








* fix `MMAP_DEFAULT` macro parenthesis to prevent macro expansion errors on Windows builds
* remove accidental static qualifiers from FORCEINLINE platform helpers (win32mmap, `win32direct_mmap,` win32munmap, x86 lock helpers, recursive lock functions) — **BREAKING**: changes internal linkage and may expose new symbols to linkers
