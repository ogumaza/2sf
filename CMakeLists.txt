# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)
project(twosf VERSION 0.1.0 LANGUAGES C CXX)

# xsflib is a library for players and rippers of the PSF family. It's a submodule. Its XsflibProject.cmake sets what a
# project built on it starts with: C++20, a release build by default, the static C runtime, the options that keep the
# output the same on every platform, and the warnings for 2sf's targets (TWOSF_WARNINGS). It refuses the compilers
# whose output would differ from other builds.
if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/externals/xsflib/CMakeLists.txt")
    message(FATAL_ERROR "externals/xsflib is missing. Clone with git clone --recursive, or run "
                        "git submodule update --init --recursive in a clone.")
endif()
include(externals/xsflib/cmake/XsflibProject.cmake)
xsflib_project(CHECK_COMPILERS NAME 2sf PREFIX TWOSF)

# ntdlib is a library for the hardware and formats of Nintendo's consoles, as a second submodule. It builds on xsflib.
# 2sf takes the DS from it: the cartridge image, the sound archives, the model of the console with both CPUs and the
# parts of a rip. The player's programs use xsflib's playback framework and its command-line frame. The framework
# requires zlib. The frame's FLAC output requires libFLAC.
set(XSFLIB_PSF ON)
set(XSFLIB_CLI ON)
xsflib_add_libraries(xsflib ntdlib)

# The 2SF format: its files, and the loading of a track with its libraries into a cartridge image.
add_library(twosf_format STATIC
    src/twosf/twosf_file.cpp
    src/twosf/twosf_file.h
    src/twosf/twosf_write.cpp
)
target_include_directories(twosf_format PUBLIC src)
target_link_libraries(twosf_format PUBLIC xsflib::common xsflib::psf)
target_compile_options(twosf_format PRIVATE ${TWOSF_WARNINGS})

# The player: a loaded track booted on ntdlib's model of the DS, on xsflib's playback framework.
add_library(twosf_player STATIC
    src/twosf/player.cpp
    src/twosf/player.h
)
target_link_libraries(twosf_player PUBLIC twosf_format ntdlib::ntr_machine xsflib::player)
target_compile_options(twosf_player PRIVATE ${TWOSF_WARNINGS})

# 2sfplay renders 2SF tracks to WAV or FLAC files. On Windows, its code page is UTF-8. On macOS, a droplet app takes
# files dropped on it in the Finder.
add_executable(2sfplay src/tools/2sfplay.cpp)
target_link_libraries(2sfplay PRIVATE twosf_player xsflib::cli)
target_compile_options(2sfplay PRIVATE ${TWOSF_WARNINGS})
xsflib_add_version(2sfplay VERSION ${twosf_VERSION} DEFINE TWOSF_VERSION DESCRIPTION "2SF renderer" PRODUCT 2sf
                   COPYRIGHT "Copyright (c) 2026 the 2sf authors. MIT licence.")
xsflib_add_utf8_manifest(2sfplay)
xsflib_add_droplet(2sfplay VERSION ${twosf_VERSION})

# The ripper's parts: the games' profiles, the stub, the library's image, the files of a set and the time to a song's
# first tick on ntdlib's model of the DS.
add_library(twosf_rip STATIC
    src/rip/profile.cpp
    src/rip/profile.h
    src/rip/set.cpp
    src/rip/set.h
)
target_include_directories(twosf_rip PUBLIC src)
target_link_libraries(twosf_rip PUBLIC twosf_format ntdlib::ntr ntdlib::ntr_machine ntdlib::arm_assembler
                      ntdlib::universomo)
target_compile_options(twosf_rip PRIVATE ${TWOSF_WARNINGS})

# 2sfrip rips a game's music into a 2SF set. On Windows, its code page is UTF-8.
add_executable(2sfrip src/tools/2sfrip.cpp)
target_link_libraries(2sfrip PRIVATE twosf_rip xsflib::rip xsflib::console)
target_compile_options(2sfrip PRIVATE ${TWOSF_WARNINGS})
xsflib_add_version(2sfrip VERSION ${twosf_VERSION} DEFINE TWOSF_VERSION DESCRIPTION "2SF ripper" PRODUCT 2sf
                   COPYRIGHT "Copyright (c) 2026 the 2sf authors. MIT licence.")
xsflib_add_utf8_manifest(2sfrip)

option(TWOSF_TESTS "Build 2sf's tests and register them with CTest." ON)
if (TWOSF_TESTS)
    enable_testing()

    # The unit tests: twosf_<name>_test from tests/<name>_test.cpp, registered as twosf_<name>.
    xsflib_add_project_test(twosf twosf_file twosf_format)
    xsflib_add_project_test(twosf rip twosf_rip)

    # 2sfplay reports the version it was built as.
    xsflib_add_version_test(2sfplay VERSION ${twosf_VERSION})
    xsflib_add_version_test(2sfrip VERSION ${twosf_VERSION})

    # Load local_tests.cmake from each immediate subdirectory to register tests that use locally supplied files.
    xsflib_include_local_tests()
endif()
