# Headless cross-backend check

Runs the guest engine with a fake JNI, a recording GLES2 and a deterministic clock, printing a
hash of each frame's GL command stream (including vertex/texture/uniform data and shader text).

```sh
NDK=~/Android/ndk/android-ndk-r27c
for abi in x86_64 arm64-v8a; do
  cmake -S tools/headless -B build/headless-$abi -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=$abi -DANDROID_PLATFORM=android-29
  ninja -C build/headless-$abi ninheadless
done
A=app/src/main/assets
build/headless-x86_64/ninheadless $A /tmp/h1 60 > x86.txt
qemu-aarch64 build/headless-arm64-v8a/ninheadless $A /tmp/h2 60 > arm64.txt
diff x86.txt arm64.txt
```

`FAKEGL_TRACE_FRAME=<n>` dumps every GL call of frame n to stderr.

Result (2026-10-21): identical except frame 7, which also differs between two x86_64 runs
(host noise), over init + 180 frames with ~45 taps.
