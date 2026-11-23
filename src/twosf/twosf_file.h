// SPDX-License-Identifier: MIT

// 2SF files: the PSF container with version 0x24. A file's program, once inflated, is one block for a DS cartridge
// image: the block's offset in the image, its size and its bytes. The reserved area is unused. WriteTwosf makes a file.
// LoadTwosf loads a track with its libraries into one image, as a player does before it boots the image.
//
// The layout follows "2sf specifications, revision 7" (2009-02-16).

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xsflib/psf/psf.h"

namespace twosf
{

inline constexpr uint8_t kTwosfVersion = 0x24;

// The largest cartridge image: the DS's card addresses 512 MiB.
inline constexpr uint32_t kMaxImageSize = 0x20000000;

// The tags that set how a set plays, not what it is. The plugins don't show them as metadata, and they keep them when
// the other tags are edited. `_arm9clk` and `_arm7clk` set the clock rate of an emulated CPU, and `_dsi` marks a
// DSiWare set (the specification). The other tags are vio2sf's settings. Sets made for vio2sf contain them. 2sf's
// player runs both CPUs as fast as the game's code requires and reads none of these tags. It runs a set in DSi mode
// when the image's unit code is the DSi's (3). 2sfrip writes only `_dsi`, in the minis of a game that runs only on a
// DSi.
inline constexpr std::array<const char*, 7> kPlaybackTags = {"_arm9clk",
                                                             "_arm7clk",
                                                             "_dsi",
                                                             "_clockdown",
                                                             "_vio2sf_sync_type",
                                                             "_vio2sf_arm9_clockdown_level",
                                                             "_vio2sf_arm7_clockdown_level"};

// The block of one file: `data` for the image at `offset`.
struct Block
{
    friend bool operator==(const Block&, const Block&) = default;

    uint32_t offset = 0;
    std::vector<uint8_t> data;
};

// A program for `block`: the offset and the size as little-endian words, then the bytes.
std::vector<uint8_t> WriteProgram(const Block& block);

// Parses a program. An empty program gives an empty block at offset 0. Bytes after the block's size are ignored.
// Returns the error for a program that ends inside the two words or inside the block, or for a block that ends past
// kMaxImageSize. Otherwise returns nothing.
std::optional<std::string> ParseProgram(std::span<const uint8_t> program, Block& out);

// A whole file: the header with version 0x24, an empty reserved area, the program of `block` and the tags. In
// twosf_write.
std::vector<uint8_t> WriteTwosf(const Block& block, const xsflib::psf::Tags& tags);

// A track loaded with its libraries.
struct LoadedTwosf
{
    // The cartridge image, up to the highest byte that a block wrote. Bytes that no block wrote hold zeros.
    std::vector<uint8_t> image;

    // The tags of the file that was opened.
    xsflib::psf::Tags tags;

    // The files loaded, in loading order.
    std::vector<std::string> sources;
};

// Loads the file at `path` and its libraries: a file's `_lib` first, then the file, then its `_lib2`, `_lib3` and on. A
// later block replaces the bytes of an earlier one where they overlap. `separators` are the characters that end a
// directory in a path. Returns the error, or nothing.
std::optional<std::string> LoadTwosf(const std::string& path, const xsflib::psf::FileReader& reader,
                                     std::string_view separators, LoadedTwosf& out);

} // namespace twosf
