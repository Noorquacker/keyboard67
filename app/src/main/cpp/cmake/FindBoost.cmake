# Dynarmic only needs Boost's header-only libraries (icl, variant). This replaces CMake's
# FindBoost so cross-compiles can use the host's headers (exposing *only* the boost/ directory,
# never the rest of the host include path) or fall back to downloading a release once.
if(TARGET Boost::boost)
    set(Boost_FOUND TRUE)
    return()
endif()

set(NB_BOOST_ROOT "" CACHE PATH "Directory containing boost/icl/interval_set.hpp")
set(_nb_boost_dir "${CMAKE_BINARY_DIR}/nb-boost-include")

find_path(_nb_boost_inc boost/icl/interval_set.hpp
    PATHS ${NB_BOOST_ROOT} /usr/include /usr/local/include /opt/homebrew/include
    NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)

if(_nb_boost_inc)
    file(MAKE_DIRECTORY "${_nb_boost_dir}")
    if(NOT EXISTS "${_nb_boost_dir}/boost")
        file(CREATE_LINK "${_nb_boost_inc}/boost" "${_nb_boost_dir}/boost" SYMBOLIC)
    endif()
    set(_nb_boost_include "${_nb_boost_dir}")
else()
    # Shared between ABIs so the archive is fetched only once.
    set(_nb_deps "${CMAKE_CURRENT_LIST_DIR}/../.deps")
    set(_nb_src "${_nb_deps}/boost_1_86_0")
    if(NOT EXISTS "${_nb_src}/boost/icl/interval_set.hpp")
        message(STATUS "Downloading Boost headers (one-time)")
        file(DOWNLOAD https://archives.boost.io/release/1.86.0/source/boost_1_86_0.tar.bz2
            "${_nb_deps}/boost.tar.bz2"
            EXPECTED_HASH SHA256=1bed88e40401b2cb7a1f76d4bab499e352fa4d0c5f31c0dbae64e24d34d7513b
            SHOW_PROGRESS)
        file(ARCHIVE_EXTRACT INPUT "${_nb_deps}/boost.tar.bz2" DESTINATION "${_nb_deps}"
            PATTERNS "boost_1_86_0/boost/*")
        file(REMOVE "${_nb_deps}/boost.tar.bz2")
    endif()
    set(_nb_boost_include "${_nb_src}")
endif()

add_library(Boost::boost INTERFACE IMPORTED GLOBAL)
target_include_directories(Boost::boost SYSTEM INTERFACE "${_nb_boost_include}")
add_library(Boost::headers INTERFACE IMPORTED GLOBAL)
target_link_libraries(Boost::headers INTERFACE Boost::boost)
set(Boost_FOUND TRUE)
set(Boost_VERSION 1.86.0)
