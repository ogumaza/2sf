# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)
project(twosf VERSION 0.3.0 LANGUAGES C CXX)

# xsflib is a library for players and rippers of the PSF family. It's a submodule. Its XsflibProject.cmake sets what a
# project built on it starts with: C++20, a release build by default, the static C runtime, the options that keep the
# output the same on every platform, and the warnings for 2sf's targets (TWOSF_WARNINGS). It refuses the compilers whose
# output would differ from other builds.
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

# 2sfplay renders 2SF tracks to WAV or FLAC files. It ships with 2sfrip. On Windows, its code page is UTF-8. On macOS, a
# droplet app takes files dropped on it in the Finder.
add_executable(2sfplay src/tools/2sfplay.cpp)
target_link_libraries(2sfplay PRIVATE twosf_player xsflib::cli)
target_compile_options(2sfplay PRIVATE ${TWOSF_WARNINGS})
xsflib_add_version(2sfplay VERSION ${twosf_VERSION} DEFINE TWOSF_VERSION DESCRIPTION "2SF renderer" PRODUCT 2sf
                   COPYRIGHT "Copyright (c) 2026 the 2sf authors. MIT licence.")
xsflib_add_utf8_manifest(2sfplay)
xsflib_add_droplet(2sfplay VERSION ${twosf_VERSION})

# foobar2000 component (Windows with clang-cl, or macOS): cmake -DFOOBAR2000_SDK_DIR=<unpacked SDK> ...
set(FOOBAR2000_SDK_DIR "" CACHE PATH "foobar2000 SDK (builds the foo_input_2sfplay component when set)")
if (FOOBAR2000_SDK_DIR)
    if (NOT MSVC AND NOT APPLE)
        message(FATAL_ERROR "the foobar2000 component requires clang-cl on Windows, or macOS")
    endif()
    add_subdirectory(plugins/foobar2000)
endif()

# XMPlay plugin (32-bit Windows with clang-cl): cmake -A Win32 -T ClangCL -DXMPLAY_SDK_DIR=<unpacked SDK> ...
set(XMPLAY_SDK_DIR "" CACHE PATH "XMPlay plugin SDK (builds the xmp-2sfplay plugin when set)")
if (XMPLAY_SDK_DIR AND WIN32)
    add_subdirectory(plugins/xmplay)
endif()

# The ripper's parts: the games' profiles, the stubs, the libraries' images, the files of a set, the time to a song's
# first tick, an SDAT game's streams, the runs of an SDAT game's minis on ntdlib's model of the DS, and the times of a
# mini in the render of 2sf's player. The files' names come from seqlib's SafeFileName.
add_library(twosf_rip STATIC
    src/rip/arm_code.cpp
    src/rip/arm_code.h
    src/rip/boots.cpp
    src/rip/boots.h
    src/rip/profile.cpp
    src/rip/dse_set.cpp
    src/rip/dse_set.h
    src/rip/game_start_table.h
    src/rip/game_streams.cpp
    src/rip/game_streams.h
    src/rip/library_name.cpp
    src/rip/library_name.h
    src/rip/mini_render.cpp
    src/rip/mini_render.h
    src/rip/profile.h
    src/rip/program_set.cpp
    src/rip/program_set.h
    src/rip/sadl.cpp
    src/rip/sadl.h
    src/rip/sdat_set.cpp
    src/rip/sdat_set.h
    src/rip/set.cpp
    src/rip/set.h
    src/rip/spd_set.cpp
    src/rip/spd_set.h
    src/rip/stream_set.cpp
    src/rip/stream_set.h
    src/rip/zelda_starts.h
)
target_include_directories(twosf_rip PUBLIC src)
target_link_libraries(twosf_rip PUBLIC twosf_format twosf_player ntdlib::ntr ntdlib::ntr_machine
                      ntdlib::arm_assembler ntdlib::universomo ntdlib::ntr_sound_routines ntdlib::ntr_rip
                      ntdlib::dse_lengths ntdlib::dse_rip ntdlib::spd PRIVATE seqlib::seqlib xsflib::zlib)
target_compile_options(twosf_rip PRIVATE ${TWOSF_WARNINGS})

# 2sfrip rips a game's music into a 2SF set. On Windows, its code page is UTF-8.
add_executable(2sfrip src/tools/2sfrip.cpp src/tools/dse_rip.cpp src/tools/dse_rip.h src/tools/hook_library.cpp
               src/tools/hook_library.h src/tools/messages.cpp src/tools/messages.h src/tools/sdat_rip.cpp
               src/tools/sdat_rip.h src/tools/set_files.cpp src/tools/set_files.h src/tools/spd_rip.cpp
               src/tools/spd_rip.h)
target_include_directories(2sfrip PRIVATE src)
# xsflib::rip comes through ntdlib::ntr_rip. A second mention makes ld warn of a duplicate library.
target_link_libraries(2sfrip PRIVATE twosf_rip ntdlib::ntr_profiles xsflib::console)
target_compile_options(2sfrip PRIVATE ${TWOSF_WARNINGS})
xsflib_add_version(2sfrip VERSION ${twosf_VERSION} DEFINE TWOSF_VERSION DESCRIPTION "2SF ripper" PRODUCT 2sf
                   COPYRIGHT "Copyright (c) 2026 the 2sf authors. MIT licence.")
xsflib_add_utf8_manifest(2sfrip)
# A rip of one of the larger DSE games uses about 2.3 GB. A 32-bit program gets 2 GB unless it is large-address-aware. A
# large-address-aware one gets 4 GB on 64-bit Windows. Only the tests use the 32-bit 2sfrip: the releases leave it out.
xsflib_large_address_aware(2sfrip)

option(TWOSF_TESTS "Build 2sf's tests and register them with CTest." ON)
if (TWOSF_TESTS)
    enable_testing()

    # The unit tests: twosf_<name>_test from tests/<name>_test.cpp, registered as twosf_<name>.
    xsflib_add_project_test(twosf twosf_file twosf_format)
    xsflib_add_project_test(twosf rip twosf_rip)
    xsflib_add_project_test(twosf boots twosf_rip)
    xsflib_add_project_test(twosf sdat_set twosf_rip)
    xsflib_add_project_test(twosf sdat_stub twosf_rip)
    xsflib_add_project_test(twosf sdat_archive twosf_rip)
    xsflib_add_project_test(twosf sdat_table twosf_rip)
    xsflib_add_project_test(twosf sdat_boots twosf_rip)
    xsflib_add_project_test(twosf dse_set twosf_rip)
    xsflib_add_project_test(twosf dse_stub twosf_rip)
    xsflib_add_project_test(twosf spd_set twosf_rip)
    xsflib_add_project_test(twosf stream_set twosf_rip)
    xsflib_add_project_test(twosf sadl twosf_rip)
    xsflib_add_project_test(twosf game_streams twosf_rip)
    xsflib_add_project_test(twosf program_set twosf_rip)
    xsflib_add_project_test(twosf mini_render twosf_rip)
    xsflib_add_project_test(twosf player twosf_player)

    # The files that 2sfrip writes for a set, from 2sfrip's source.
    xsflib_add_project_test(twosf set_files twosf_rip)
    target_sources(twosf_set_files_test PRIVATE src/tools/set_files.cpp)

    # The traits that the plugins share: the loader through the traits, and the tags that the plugins hide.
    xsflib_add_project_test(twosf plugin_traits twosf_player)
    target_include_directories(twosf_plugin_traits_test PRIVATE ${PROJECT_SOURCE_DIR}/plugins)
    target_compile_definitions(twosf_plugin_traits_test PRIVATE TWOSF_VERSION="${twosf_VERSION}")

    # 2sfplay reports the version it was built as.
    xsflib_add_version_test(2sfplay VERSION ${twosf_VERSION})
    xsflib_add_version_test(2sfrip VERSION ${twosf_VERSION})

    # Load local_tests.cmake from each immediate subdirectory to register tests that use locally supplied files.
    xsflib_include_local_tests()
endif()
