# Keyboard 67: arm64 port of libgl2jni.so

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
- [x] **Real arm64 phone (Pixel 9a, Android 17)**: works, and was profiled there (below).

## Performance (Pixel 9a, Android 17)

Profiled with `simpleperf` on device; fixes so far:

| Fix | Effect |
|---|---|
| Native code built `-O2` even in debug APKs (`CMakeLists.txt`) | dynarmic at `-O0` was the biggest cost |
| PLT entries rewritten to direct branches at load (`loader.cpp`) | dynarmic's arm64 backend has no fast dispatch, so every PLT call went through a C++ hash lookup |
| dynarmic patch: 16 KB instead of 256 KB IR pool per block (`patches/`) | each compiled block was doing an `mmap`/`munmap` round trip in scudo |
| `/proc/self/stat` served from a 250 ms snapshot via a virtual fd (`stdio.cpp`) | the engine reads it ~30 times per frame |
| Lock-free SVC dispatch (`cpu.cpp`) | one mutex per import call removed |
| dynarmic patch: optional code-range index, disabled | saves host heap; the guest never modifies its code |
| dynarmic patch: skip the debug-only `VerificationPass` | compile time |
| Private dlmalloc for all C++ allocations in the library (`host_heap.c`, `host_new.cpp`) | scudo was ~1 s of a cold start (JIT compile churn); native heap 330 MB → ~50 MB |
| Native library preloaded on a background thread from `SoftKeyboard.onCreate` | guest static constructors overlap view/EGL creation |
| Guest threads (with their JITs) are pooled instead of destroyed (`cpu.cpp`) | the preload thread's warm JIT is inherited by the GL thread, and by a new GL thread after the view is recreated |
| Block-profile warm-up (`warmup.cpp`, dynarmic patch `Jit::Precompile`) | compiled block addresses are saved to `files/ninbridge-blocks.bin`; the next start precompiles them in the background (~2 s for ~130k blocks) |

Tap-to-keyboard: ~40 s → **~0.6 s** when the keyboard process is alive (the usual case, since
Android starts and restarts IME processes in the background). A first run after install pays for
Android's own startup (~1.2 s) and compilation. Typing costs ~10 ms of engine time per touch, from
the first word on. Frames average 6–9 ms. `ninbridge: perf:` lines in logcat report frame and touch
timings every 5 s. Memory is ~420–540 MB PSS, depending on how much code is compiled.

**On-demand rendering** (`NINView.kt`). The original rendered continuously at 60 fps. The view now
renders when dirty: while a finger is down, for 1.5 s after any touch or text event (the engine's
timers, e.g. autospace), and while frames keep changing. Otherwise it only steps the engine at 4 Hz.
"Changed" comes from a fingerprint of each frame's GL commands and data computed in the bridge
(`gles.cpp`, `NINLib.frameChanged()`). The engine's own `onRoenSignalDirty` fires nearly every
frame, so it is useless for this. Idle GL-thread CPU went from ~50% to ~4%. On 32-bit devices
(original library, no `frameChanged`) the view stays continuous.

## Fixed: memory leak and crash after a few minutes of typing

The guest library has two `STT_GNU_IFUNC` symbols (`__atomic_fetch_add_4`, `__atomic_add_fetch_4`),
whose values are resolver functions. The loader bound the resolvers themselves, so every atomic
decrement of a libstdc++ string refcount returned a pointer instead of the old count, and no string
was ever freed (~400 KB per keystroke, guest heap exhausted after ~10 minutes, then a crash on a
failed allocation). `loader.cpp` now calls IFUNC resolvers during relocation. The guest heap size is
in logcat's `ninbridge: perf:` lines. Headless builds track live guest allocations per call site
(`NB_LEAK_ROUNDS=<n>` keeps typing and prints the top allocators).

## Known issues

- **Black keyboard after a phone call** (also on the original app and old phones). After the call
  the surface is recreated, one frame renders, then the GL thread stops drawing. Switching apps does
  not reproduce it; the screen turning off during the call (proximity sensor), and possibly EGL
  context loss, are the suspects. Debug builds log `EXSurfaceView` lifecycle and GL-thread wait
  states to catch the next occurrence.

## Building

- NDK r27+ (`nin.ndkPath=...` in `local.properties`, or install NDK 27.2.12479018 in the SDK).
- `git submodule update --init --recursive` (dynarmic and its externals). CMake applies the
  patches in `app/src/main/cpp/patches/` to the dynarmic checkout, so it shows as modified.
- Boost headers: found in `/usr/include` if installed, otherwise downloaded once into
  `app/src/main/cpp/.deps`.
- Gradle 8.0 needs JDK 17 (Kotlin 1.8 cannot target JVM 21).

## Known gaps

- `popen`/`fork`/`system`/`dlopen` are refused (return failure); not hit in testing.
- pthread key destructors are not run; `RegisterNatives` from the guest is unsupported.
- File `mmap` at offsets not aligned to the host page size fails on 16 KiB-page devices.
