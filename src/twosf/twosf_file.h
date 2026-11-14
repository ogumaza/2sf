// SPDX-License-Identifier: MIT

// 2SF files: the PSF container with version 0x24. A file's program, once inflated, is one block for a DS cartridge
// image: the block's offset in the image, its size and its bytes. The reserved area is unused. WriteTwosf makes a
// file. LoadTwosf loads a track with its libraries into one image, as a player does before it boots the image.
//
// The layout follows "2sf specifications, revision 7" (2009-02-16).

#pragma once

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

// Loads the file at `path` and its libraries: a file's `_lib` first, then the file, then its `_lib2`, `_lib3` and on.
// A later block replaces the bytes of an earlier one where they overlap. `separators` are the characters that end a
// directory in a path. Returns the error, or nothing.
std::optional<std::string> LoadTwosf(const std::string& path, const xsflib::psf::FileReader& reader,
                                     std::string_view separators, LoadedTwosf& out);

} // namespace twosf
