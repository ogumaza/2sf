// SPDX-License-Identifier: MIT

// The name of a set's library.

#pragma once

#include <span>
#include <string>
#include <string_view>

namespace twosf::rip
{

// A game that 2sfrip has been checked on, and its product code.
struct KnownGame
{
    std::string_view game_code;
    std::string_view product_code;
};

// The games that 2sfrip has been checked on, in the order of their game codes.
std::span<const KnownGame> KnownGames();

// The base of a library's name for the game with `game_code`: the game's product code, such as "NTR-AZEE-USA", for a
// known game, and "NTR-" with the game code for any other.
std::string LibraryBase(std::string_view game_code);

} // namespace twosf::rip
