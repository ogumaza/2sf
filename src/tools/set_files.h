// SPDX-License-Identifier: MIT

// The files that 2sfrip writes for a set of a game on the SDK's sound archive player or on DSE: the library, the
// libraries that some minis load after it, a mini for each sound, the playlist, and the game's files that a rip writes
// beside the set.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/ntr/rom.h"
#include "xsflib/cli/rip.h"
#include "xsflib/psf/psf.h"
#include "xsflib/rip/length.h"

#include "rip/set.h"
#include "twosf/twosf_file.h"

namespace twosf::tools
{

// A library that some minis load after the set's library, as `_lib2`, such as a stream's file. Its name is the base of
// the set's library, "_" and the title. `block` gives its place in the image.
struct SecondLibrary
{
    std::string title;
    Block block;
};

// A mini of a set: its title, the block that it writes over the library's image, the timing that gives its length tags
// (xsflib::rip::TagsOfTiming), and the index of its second library in SetParts::libraries.
struct MiniSpec
{
    std::string title;
    Block block;
    std::optional<xsflib::rip::Timing> timing;
    std::optional<std::size_t> library;
};

// A file of the game that a rip writes beside the set as the game stores it, such as a stream file (--stream-files):
// its title, its extension and its bytes.
struct GameFile
{
    std::string title;
    std::string extension;
    std::span<const uint8_t> bytes;
};

// The set of a download-play program of the game (rip/program_set.h), or of a second program of a game on DSE, written
// beside the game's set: its library, named after the game's library and `name`, its second libraries, named as the
// game's are, and its minis. The minis name that library. A mini's second library is an index into `libraries`.
struct ProgramParts
{
    std::string name;
    std::vector<uint8_t> image;
    std::vector<MiniSpec> minis;
    std::vector<SecondLibrary> libraries;
};

// What a set contains beside its library, and the sets of the game's programs.
struct SetParts
{
    std::vector<SecondLibrary> libraries;
    std::vector<MiniSpec> minis;
    std::vector<GameFile> files;
    std::vector<ProgramParts> programs;
};

// Writes the set into the folder that the options give or a folder next to `input`: the library `image`, the second
// libraries, a mini for each of `parts.minis`, then each program's library, second libraries and minis
// (`parts.programs`), all named "NN Title" in their order, the playlist of the minis, and the game's files. A library
// is written only with minis that name it. The library's name is the product code of a game that 2sfrip has been
// checked on, or "NTR-" and the game code. The minis take the options' tags, with the image's name for a game that no
// option names. The names of the libraries, the minis and the game's files go through `names`. Returns 1 if a file
// can't be written, or 0.
int WriteSet(const std::string& input, const ntdlib::ntr::Rom& rom, const std::vector<uint8_t>& image,
             const SetParts& parts, const xsflib::cli::RipOptions& o, xsflib::cli::FileNameRegistries& names);

} // namespace twosf::tools
