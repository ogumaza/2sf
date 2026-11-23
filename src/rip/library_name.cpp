// SPDX-License-Identifier: MIT

#include "rip/library_name.h"

#include <algorithm>
#include <span>
#include <string>
#include <string_view>

namespace twosf::rip
{

namespace
{

constexpr KnownGame kGames[] = {
    {"APHE", "NTR-APHE-USA"}, {"AZEE", "NTR-AZEE-USA"}, {"BEYE", "NTR-BEYE-USA"}, {"BKIE", "NTR-BKIE-USA"},
    {"BKIP", "NTR-BKIP-EUR"}, {"C2SE", "NTR-C2SE-USA"}, {"KQ9E", "TWL-KQ9E-USA"}, {"KQ9V", "TWL-KQ9V-EUR"},
    {"VI2J", "TWL-VI2J-JPN"}, {"YFEE", "NTR-YFEE-USA"}, {"YFTE", "NTR-YFTE-USA"}, {"YFYE", "NTR-YFYE-USA"},
};

} // namespace

std::span<const KnownGame> KnownGames()
{
    return kGames;
}

std::string LibraryBase(std::string_view game_code)
{
    const auto game = std::ranges::find(kGames, game_code, &KnownGame::game_code);
    return game != std::end(kGames) ? std::string(game->product_code) : "NTR-" + std::string(game_code);
}

} // namespace twosf::rip
