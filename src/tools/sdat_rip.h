// SPDX-License-Identifier: MIT

// 2sfrip's rip of a game on the SDK's sound archive player (rip/sdat_set.h): the sounds of the set, the confirmation
// of every mini on ntdlib's DS, the lengths, the tags and the files.

#pragma once

#include <string>

#include "ntdlib/ntr/rom.h"
#include "xsflib/cli/rip.h"

namespace twosf::tools
{

// Rips `rom`, read from `input`, into a set. `effects` adds the sound effects. Returns 1 if the rip fails, or 0.
int RipSdat(const std::string& input, const ntdlib::ntr::Rom& rom, const xsflib::cli::RipOptions& o, bool effects,
            xsflib::cli::FileNameRegistries& names);

} // namespace twosf::tools
