# Headless deterministic test executable: the bridge + fake JNI + recording GL + fake clock.
# Build it for x86_64 and for aarch64 (static Android executable, runnable under qemu-aarch64)
# and diff the per-frame GL hashes to validate the arm64 JIT backend against x86_64.
cmake_minimum_required(VERSION 3.22)
project(ninheadless C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(NB_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../app/src/main/cpp")
set(NB_GUEST_LIB "${NB_SRC}/../jniLibs/armeabi-v7a/libgl2jni.so")
list(PREPEND CMAKE_MODULE_PATH "${NB_SRC}/cmake")

set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
foreach(patch dynarmic-small-ir-pool.patch dynarmic-optional-range-tracking.patch dynarmic-skip-verification.patch dynarmic-precompile-api.patch)
    set(_p "${NB_SRC}/patches/${patch}")
    execute_process(COMMAND git apply --reverse --check "${_p}" WORKING_DIRECTORY "${NB_SRC}/third_party/dynarmic"
        RESULT_VARIABLE _already OUTPUT_QUIET ERROR_QUIET)
    if(NOT _already EQUAL 0)
        execute_process(COMMAND git apply "${_p}" WORKING_DIRECTORY "${NB_SRC}/third_party/dynarmic")
    endif()
endforeach()
add_subdirectory("${NB_SRC}/third_party/dynarmic" dynarmic EXCLUDE_FROM_ALL)

configure_file("${NB_SRC}/bridge/guest_image.S.in" "${CMAKE_CURRENT_BINARY_DIR}/guest_image.S" @ONLY)

file(GLOB NB_BRIDGE "${NB_SRC}/bridge/*.cpp" "${NB_SRC}/bridge/*.c")
add_executable(ninheadless main.cpp fake_jni.cpp fake_gl.cpp ${NB_BRIDGE}
    "${CMAKE_CURRENT_BINARY_DIR}/guest_image.S")
target_compile_definitions(ninheadless PRIVATE NB_HEADLESS=1)
target_include_directories(ninheadless PRIVATE "${NB_SRC}/bridge")
target_link_libraries(ninheadless PRIVATE dynarmic)
# Always built with the NDK as a static bionic executable (x86_64 runs natively on Linux,
# arm64 under qemu-aarch64), so both variants share libc/libm and jni.h.
if(NOT ANDROID)
    message(FATAL_ERROR "configure with the NDK toolchain (ANDROID_ABI=x86_64 or arm64-v8a)")
endif()
target_link_options(ninheadless PRIVATE -static)
