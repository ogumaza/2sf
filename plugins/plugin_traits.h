// SPDX-License-Identifier: MIT

// The traits that the foobar2000 component and the XMPlay plugin share, for xsflib's plugin templates
// (xsflib/foobar2000/input.h and xsflib/xmplay/input.h): 2SF's player, names, extensions, limits, version and about
// text. Each plugin's traits derive from PluginTraits. The plugin's CMake target defines TWOSF_VERSION.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "xsflib/psf/psf.h"

#include "twosf/player.h"
#include "twosf/twosf_file.h"

namespace twosf
{

struct PluginTraits
{
    using Player = twosf::Player;
    using Set = LoadedTwosf;

    static constexpr const char* kName = "2SF";
    static constexpr std::array<const char*, 2> kExtensions = {"mini2sf", "2sf"};
    static constexpr uint8_t kPsfVersion = kTwosfVersion;
    static constexpr uint64_t kMaxFileSize = uint64_t{256} << 20; // largest file read into memory

    static constexpr std::array<const char*, twosf::kPlaybackTags.size()> kPlaybackTags = twosf::kPlaybackTags;

    static constexpr const char* kVersion = TWOSF_VERSION;
    static constexpr const char* kAbout =
        "Plays 2SF (Nintendo DS music) rips. The game's code runs on a model of the DS with both CPUs and the sound "
        "unit.\n\n"
        "MIT licence. Uses zlib.";

    static std::optional<std::string> LoadSet(const std::string& path, const xsflib::psf::FileReader& reader, Set& out,
                                              std::string_view separators)
    {
        return LoadTwosf(path, reader, separators, out);
    }
};

} // namespace twosf
