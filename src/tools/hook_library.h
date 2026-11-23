// SPDX-License-Identifier: MIT

// The library of a set whose stub replaces the game's code at a hook and keeps the game's files whole, such as a set of
// a game on DSE (rip/dse_set.h) or on Nintendo SPD's driver (rip/spd_set.h): the files that it keeps, and its boots.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ntdlib/ntr/rom.h"

#include "rip/boots.h"

namespace twosf::tools
{

// A set's image with the stub in place and the files given, each whole, by ID.
using ImageOf = std::function<std::vector<uint8_t>(const std::map<uint32_t, std::vector<uint8_t>>& files)>;

struct HookLibrary
{
    std::vector<uint8_t> image;
    std::set<uint32_t> files;                    // the IDs of the files that the image keeps whole
    std::map<uint32_t, rip::FileRanges> partial; // the files that the image keeps in part, with their ranges
    std::vector<rip::SetBoot> boots;             // in the order of rip::BootBackups
};

// The library of `rom`'s set, from `image_of`, with a stub at `hook`. It keeps every file that any boot of the game
// (rip::BootBackups) or of the full image (the set's image with every file) reads before the hook, and `song_files`.
// Each boot of the game and of the full image must reach the hook where a stub can run (rip::LetsStubRun). Each boot of
// the library must match the full image's, the clock included (rip::DifferentBoot). A mini's sound must start within
// `start_limit_seconds` of the hook on each boot. Prints the hook's time, the library's size and the files that only a
// boot with a backup memory or the full image reads. Returns nothing, with `error` set, when a check fails. Each file
// of `partial`, such as a pack of songs, stays at its size with the ranges that `partial` gives and the ranges that the
// boots read, and zeros elsewhere (rip::KeptFiles).
std::optional<HookLibrary> MakeHookLibrary(const ntdlib::ntr::Rom& rom, uint32_t hook,
                                           const std::set<uint32_t>& song_files, const ImageOf& image_of,
                                           double start_limit_seconds, std::string& error,
                                           const std::map<uint32_t, rip::FileRanges>& partial = {});

} // namespace twosf::tools
