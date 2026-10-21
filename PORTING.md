# arm64 port of libgl2jni.so — progress notes

## Approach

`libgl2jni.so` is ~20 MB of stripped ARMv7 C++ (CryptoPP, unqlite, a full GL UI engine, …).
Decompiling it to recompilable source is not realistic, and all of its data structures assume
4-byte pointers. Instead the port uses **in-process binary translation**, the way `libhoudini`
and box86 work:

- A new native `libgl2jni.so` (arm64-v8a / x86_64) is built from `app/src/main/cpp`.
- It embeds the original 32-bit library (`app/src/main/jniLibs/armeabi-v7a/libgl2jni.so`, via
  `.incbin`), loads it into a private 4 GiB guest address space, applies its relocations, and
  runs it with the **dynarmic** ARM32 JIT (`third_party/dynarmic`, git submodule, 0BSD,
  azahar-emu fork).
- Each of the 338 imports (libc, libm, GLES2, liblog, libandroid, plus the JNI function tables)
  is bound to an ARM stub `svc #n; bx lr`. The SVC handler marshals AAPCS soft-float arguments,
  translates guest pointers, and calls the real host function (`bridge/*.cpp`).
- Java is unchanged. The same 15 `Java_com_jormy_nin_NINLib_*` symbols are exported and
  forwarded into the guest.
- 32-bit ARM devices keep using the original library natively (armeabi-v7a).

## Layout

| File | Purpose |
|---|---|
| `bridge/nb.h` | guest memory model, AAPCS argument cursor, auto-marshalling templates |
| `bridge/memory.cpp`, `guest_heap.c` | 4 GiB reservation, page bitmap, mmap area, dlmalloc on a guest sbrk arena (16 KiB-page aware) |
| `bridge/cpu.cpp` | dynarmic per-thread/per-nesting-depth JITs, SVC dispatch, kuser helpers (0xffff0fc0 cmpxchg etc.), CP15 |
| `bridge/loader.cpp` | ELF32 loader, REL relocations (bionic semantics), init_array, symbolizer for crash dumps |
| `bridge/libc.cpp` | libc/libm/libdl incl. 32↔64 struct conversions (stat, tm, rusage, sysinfo, flock, hostent, msghdr…) |
| `bridge/stdio.cpp` | FILE emulation (bionic 84-byte layout), printf/scanf engines on guest varargs/va_list |
| `bridge/pthread.cpp` | guest threads = host threads; 4-byte mutex/cond/sem words hold handles to host objects |
| `bridge/gles.cpp` | GLES2 (VBO-aware glVertexAttribPointer, glShaderSource, glGetString) |
| `bridge/jni_bridge.cpp` | guest JNIEnv/JavaVM tables, 32-bit reference handles with local frames, signature-driven calls |
| `bridge/android.cpp` | liblog, AAsset (desktop fallback reads `$NIN_ASSETS`) |
| `bridge/entry.cpp` | JNI_OnLoad + NINLib exports |

## Status (2026-10-21)

- [x] Bridge builds for x86_64 Linux and arm64-v8a (NDK r27c at `~/Android/ndk`).
- [x] **Desktop harness works** (`tools/hosttest/run.sh`): real JVM + EGL. Static constructors
  run, init loads the dictionary (77k words), the keyboard renders correctly, taps move through
  the tutorial. About 15 ms/frame steady state.
- [x] Gradle integration (externalNativeBuild, abiFilters arm64-v8a/x86_64/armeabi-v7a, `nin.ndkPath`).
- [x] **Android 14 x86_64 emulator**: keyboard loads (~4 s cold start), renders at 60 fps
  (~6 ms/frame), and typing commits text through InputConnection, with dictionary suggestions.
  See `docs/port/android14-x86_64-typing.png`.
- [x] Headless check: arm64 JIT output matches x86_64 frame-for-frame (`tools/headless`).
- [x] **arm64-v8a build on Android**: installed with `adb install --abi arm64-v8a` on the same
  emulator (which runs arm64 apps through Google's ndk_translation). The keyboard loads and renders
  (`docs/port/android14-arm64-via-ndk-translation.png`), at ~46 ms/frame because of the double
  translation. A real arm64 phone runs the JIT output natively.
- [ ] Real arm64 phone (no device was available).

## Building

- NDK r27+ (`nin.ndkPath=...` in `local.properties`, or install NDK 27.2.12479018 in the SDK).
- `git submodule update --init --recursive` (dynarmic and its externals).
- Boost headers: found in `/usr/include` if installed, otherwise downloaded once into
  `app/src/main/cpp/.deps`.
- Gradle 8.0 needs JDK 17 (Kotlin 1.8 cannot target JVM 21).

## Known gaps

- `popen`/`fork`/`system`/`dlopen` are refused (return failure); not hit in testing.
- pthread key destructors are not run; `RegisterNatives` from the guest is unsupported.
- File `mmap` at offsets not aligned to the host page size fails on 16 KiB-page devices.
