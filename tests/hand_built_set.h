// SPDX-License-Identifier: MIT

// Hand-built 2SF sets for the tests: a DS cartridge image whose two binaries halt, and a set of one library with an
// image and a mini that loads it. No game data.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ntdlib/testing/nds_files.h"
#include "xsflib/bytes.h"
#include "xsflib/psf/psf.h"

#include "twosf/twosf_file.h"

namespace twosf::testing
{

// Files in memory, by path.
using Files = std::map<std::string, std::vector<uint8_t>>;

// A binary that halts for good: swi 0x060000 (Halt), then a branch back to it.
inline std::vector<uint8_t> Halts()
{
    std::vector<uint8_t> bytes;
    xsflib::AppendLe32(bytes, 0xEF060000);
    xsflib::AppendLe32(bytes, 0xEAFFFFFD);

    return bytes;
}

// A whole image: both binaries halt, and the file system has one file.
inline std::vector<uint8_t> HaltingImage()
{
    ntdlib::testing::nds_files::Image spec;
    spec.arm9 = Halts();
    spec.arm7 = Halts();
    spec.files = {{"sound/sound_data.sdat", std::vector<uint8_t>(0x400, 0x5A)}};

    return ntdlib::testing::nds_files::BuildImage(spec).first;
}

// A set in `folder` (empty, or ending in a separator): the library test.2sflib with `image`, and test.mini2sf. The mini
// loads the library and has `tags` besides `_lib`.
inline Files SetOf(const std::vector<uint8_t>& image, xsflib::psf::Tags tags = {}, const std::string& folder = {})
{
    tags["_lib"] = "test.2sflib";

    return {{folder + "test.2sflib", WriteTwosf(Block{0, image}, {})},
            {folder + "test.mini2sf", WriteTwosf(Block{}, tags)}};
}

} // namespace twosf::testing
