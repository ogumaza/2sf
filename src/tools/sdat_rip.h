// SPDX-License-Identifier: MIT

// 2sfrip's rip of a game on the SDK's sound archive player (rip/sdat_set.h): the sounds of the set, the confirmation of
// every mini on ntdlib's DS, the lengths, the tags and the files.

#pragma once

#include <string>

#include "ntdlib/ntr/rom.h"
#include "xsflib/cli/rip.h"

namespace twosf::tools
{

// The options of an SDAT game's rip beyond those of every rip tool. `effects` adds the sound effects. `stream_files`
// writes the game's stream files beside the set, in place of the streams' libraries and minis.
struct SdatOptions
{
    bool effects = false;
    bool stream_files = false;
};

// Rips `rom`, read from `input`, into a set. Returns 1 if the rip fails, or 0.
int RipSdat(const std::string& input, const ntdlib::ntr::Rom& rom, const xsflib::cli::RipOptions& o,
            const SdatOptions& sdat, xsflib::cli::FileNameRegistries& names);

} // namespace twosf::tools
