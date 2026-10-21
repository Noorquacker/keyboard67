cmake_minimum_required(VERSION 3.22)
project(ninbridge C CXX ASM)

# libgl2jni.so is a closed-source 32-bit ARM library. On 32-bit ARM devices it is packaged
# as-is (src/main/jniLibs/armeabi-v7a). Everywhere else (arm64, x86_64) this project builds a
# replacement libgl2jni.so that runs the original under an ARM32 JIT (dynarmic).
set(NB_GUEST_LIB "${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/armeabi-v7a/libgl2jni.so"
    CACHE FILEPATH "Original 32-bit ARM libgl2jni.so")

if(ANDROID AND ANDROID_ABI STREQUAL "armeabi-v7a")
    return()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# The JIT is far too slow unoptimized, so debug builds (e.g. Gradle assembleDebug) still use -O2.
set(CMAKE_C_FLAGS_DEBUG "-O2 -g")
set(CMAKE_CXX_FLAGS_DEBUG "-O2 -g")

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
# Local fixes to dynarmic, applied to the submodule checkout (idempotent).
foreach(patch dynarmic-small-ir-pool.patch dynarmic-optional-range-tracking.patch dynarmic-skip-verification.patch dynarmic-precompile-api.patch)
    set(_p "${CMAKE_CURRENT_SOURCE_DIR}/patches/${patch}")
    execute_process(COMMAND git apply --reverse --check "${_p}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/third_party/dynarmic"
        RESULT_VARIABLE _already OUTPUT_QUIET ERROR_QUIET)
    if(NOT _already EQUAL 0)
        execute_process(COMMAND git apply "${_p}"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/third_party/dynarmic"
            RESULT_VARIABLE _applied)
        if(NOT _applied EQUAL 0)
            message(FATAL_ERROR "failed to apply ${patch} to dynarmic")
        endif()
    endif()
endforeach()
add_subdirectory(third_party/dynarmic EXCLUDE_FROM_ALL)

configure_file(bridge/guest_image.S.in "${CMAKE_CURRENT_BINARY_DIR}/guest_image.S" @ONLY)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/guest_image.S" PROPERTIES OBJECT_DEPENDS "${NB_GUEST_LIB}")

add_library(gl2jni SHARED
    bridge/android.cpp
    bridge/cpu.cpp
    bridge/entry.cpp
    bridge/gles.cpp
    bridge/guest_heap.c
    bridge/host_heap.c
    bridge/host_new.cpp
    bridge/jni_bridge.cpp
    bridge/libc.cpp
    bridge/loader.cpp
    bridge/memory.cpp
    bridge/pthread.cpp
    bridge/stdio.cpp
    bridge/warmup.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/guest_image.S"
)
target_link_libraries(gl2jni PRIVATE dynarmic)
target_compile_options(gl2jni PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fvisibility=hidden -fvisibility-inlines-hidden>
    $<$<COMPILE_LANGUAGE:C>:-fvisibility=hidden -w>)

if(ANDROID)
    target_link_libraries(gl2jni PRIVATE GLESv2 android log)
    # 16 KiB page devices (Android 15+)
    target_link_options(gl2jni PRIVATE "-Wl,-z,max-page-size=16384" "-Wl,--exclude-libs,ALL")
else()
    # Desktop test build: real JVM + Mesa GLES.
    find_package(JNI REQUIRED)
    target_include_directories(gl2jni PRIVATE ${JNI_INCLUDE_DIRS})
    target_link_libraries(gl2jni PRIVATE GLESv2)
endif()
