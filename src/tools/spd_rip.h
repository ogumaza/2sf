// SPDX-License-Identifier: MIT

// 2sfrip's rip of a game on Nintendo SPD's sound driver (rip/spd_set.h): the sounds of the set, the confirmation of
// every mini on ntdlib's DS, the lengths, the tags and the files.

#pragma once

#include <string>

#include "ntdlib/ntr/rom.h"
#include "xsflib/cli/rip.h"

namespace twosf::tools
{

// Whether 2sfrip rips the game in `rom` as a game on Nintendo SPD's driver: whether its table of the driver's routines
// lists the game.
bool IsSpdGame(const ntdlib::ntr::Rom& rom);

// Rips `rom`, read from `input`, into a set of its music: the sounds of player 0. `effects` selects the sound effects
// too. 2sfrip doesn't rip them from a game on this driver yet. Returns 1 if the rip fails, or 0.
int RipSpd(const std::string& input, const ntdlib::ntr::Rom& rom, const xsflib::cli::RipOptions& o, bool effects,
           xsflib::cli::FileNameRegistries& names);

} // namespace twosf::tools
