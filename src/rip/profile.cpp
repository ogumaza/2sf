// SPDX-License-Identifier: MIT

#include "rip/profile.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "xsflib/bytes.h"

namespace twosf::rip
{

namespace
{

// Beat City (USA), NTR-BEYE. Every address is in its ARM9 binary, which the cartridge stores uncompressed.
const UniversomoProfile kBeatCity = {
    .game_code = "BEYE",
    .product_code = "NTR-BEYE-USA",
    .game = "Beat City",
    // The first release, in North America.
    .year = "2010-04-20",
    .copyright = "Universomo, THQ",
    .engine = 0x020E0DBC,
    .main_loop = {0x020350E0, {0x10, 0x40, 0x2D, 0xE9, 0x20, 0xD0, 0x4D, 0xE2}},
    .main_loop_size = 0x2B0,
    .skipped = {0x020344B4, {0x38, 0x40, 0x2D, 0xE9, 0x28, 0xD0, 0x4D, 0xE2}},
    .load = {0x0202C17C, {0xF0, 0x4F, 0x2D, 0xE9, 0x47, 0xDF, 0x4D, 0xE2}},
    .start = {0x0202CB58, {0xF0, 0x41, 0x2D, 0xE9, 0x00, 0x70, 0xA0, 0xE1}},
    .update = {0x0202CE98, {0xF8, 0x43, 0x2D, 0xE9, 0x00, 0x40, 0xA0, 0xE1}},
    .wait_vblank = {0x02004580, {0x08, 0x40, 0x2D, 0xE9, 0x01, 0x00, 0xA0, 0xE3}},
    .receive_reply = {0x02005124, {0x70, 0x40, 0x2D, 0xE9, 0x00, 0x50, 0xA0, 0xE1}},
    .flush = {0x020052F4, {0xF8, 0x4F, 0x2D, 0xE9, 0x00, 0xA0, 0xA0, 0xE1}},
    // The game reads the message for a removed card before its main loop. Without the file's bytes, a set boots in
    // melonDS but plays nothing.
    .kept_suffixes = {".ums", "/samplebank.usb", "/card-removed-message.png"},
    .sound_table = "audio",
    .title_song = 105,
};

const UniversomoProfile* const kProfiles[] = {&kBeatCity};

// The first two words of a decrypted secure area.
constexpr uint32_t kDecryptedMarker = 0xE7FFDEFF;

// Whether the ARM9 binary holds `routine`'s first bytes at its address.
bool Matches(const ntdlib::ntr::Rom& rom, const Routine& routine)
{
    const ntdlib::ntr::Binary& arm9 = rom.Header().arm9;
    const std::span<const uint8_t> binary = rom.Arm9();
    if (binary.size() < routine.start.size() || routine.address < arm9.ram_address ||
        routine.address - arm9.ram_address > binary.size() - routine.start.size())
    {
        return false;
    }

    const std::span<const uint8_t> bytes = binary.subspan(routine.address - arm9.ram_address, routine.start.size());

    return std::equal(bytes.begin(), bytes.end(), routine.start.begin());
}

// Whether the ARM9 binary starts with 0xE7FFDEFF twice, as it does once the secure area is decrypted. The stub runs the
// game's start-up, which the encrypted area would crash.
bool SecureAreaDecrypted(const ntdlib::ntr::Rom& rom)
{
    const std::span<const uint8_t> binary = rom.Arm9();

    return binary.size() >= 8 && xsflib::LoadLe32(binary.data()) == kDecryptedMarker &&
           xsflib::LoadLe32(binary.data() + 4) == kDecryptedMarker;
}

} // namespace

const UniversomoProfile* FindProfile(const ntdlib::ntr::Rom& rom, std::string& error)
{
    for (const UniversomoProfile* profile : kProfiles)
    {
        if (rom.Header().game_code != profile->game_code)
        {
            continue;
        }

        if (!SecureAreaDecrypted(rom))
        {
            error = "the image's secure area is encrypted. Decrypt the image, then rip it.";
            return nullptr;
        }

        for (const Routine* routine :
             {&profile->main_loop, &profile->skipped, &profile->load, &profile->start, &profile->update,
              &profile->wait_vblank, &profile->receive_reply, &profile->flush})
        {
            if (!Matches(rom, *routine))
            {
                error = "the game's code differs from the profile of " + std::string(profile->game) +
                        ". Only the first US release is supported.";
                return nullptr;
            }
        }

        return profile;
    }

    error = "2sfrip doesn't support this game (game code " + rom.Header().game_code + ")";
    return nullptr;
}

std::vector<std::string_view> SupportedGames()
{
    std::vector<std::string_view> codes;
    for (const UniversomoProfile* profile : kProfiles)
    {
        codes.push_back(profile->game_code);
    }

    return codes;
}

} // namespace twosf::rip
