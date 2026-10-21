#!/usr/bin/env bash
# Builds the bridge for the desktop (x86_64 dynarmic backend) and runs the original ARM32
# keyboard engine inside a real JVM with an offscreen GLES2 context.
#   tools/hosttest/run.sh [build-dir]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
build="${1:-$root/build/hosttest}"
mkdir -p "$build/classes" "$build/out" "$build/home"

cmake -S "$root/app/src/main/cpp" -B "$build/native" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ >/dev/null
ninja -C "$build/native" gl2jni

jinc="$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")/include"
clang -shared -fPIC -O2 -I"$jinc" -I"$jinc/linux" "$here/hostgl.c" -lEGL -lGLESv2 -o "$build/native/libhostgl.so"

javac -d "$build/classes" $(find "$here/java" -name '*.java')

NIN_ASSETS="$root/app/src/main/assets" java -Xss8m \
    -Djava.library.path="$build/native" -Dnin.home="$build/home" -Dnin.out="$build/out" \
    -cp "$build/classes" com.jormy.nin.HostTest
