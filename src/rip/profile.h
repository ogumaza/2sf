// SPDX-License-Identifier: MIT

// The profiles of the games that 2sfrip rips: where a game's sound driver and the routines that the stub calls are in
// its ARM9 binary, and which files of the cartridge the music reads. A game matches a profile by its game code and by
// the bytes at the start of each of those routines.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/ntr/rom.h"

namespace twosf::rip
{

// A routine of the game that a profile names, with the first 8 bytes that the game has there.
struct Routine
{
    uint32_t address = 0;
    std::array<uint8_t, 8> start = {};
};

// A game whose music plays on Universomo's sound driver. The stub replaces the game's main loop: the game's main
// function calls it once the SDK, the file system and the sound are set up. It loads one song with the game's loader,
// starts it, then calls the driver's update once a frame as the main loop does.
struct UniversomoProfile
{
    std::string_view game_code;    // such as "BEYE"
    std::string_view product_code; // the base of the library's name, such as "NTR-BEYE-USA"
    std::string_view game;         // the game's name for the tags
    std::string_view year;
    std::string_view copyright; // "Developer, Publisher"

    // The driver's object, whose address each of the driver's routines takes first.
    uint32_t engine = 0;

    // The game's main loop, which the stub replaces, and the bytes that the stub may take there.
    Routine main_loop;
    uint32_t main_loop_size = 0;

    // A routine that the main function calls before the main loop, which the set doesn't require. It returns at once
    // in the set.
    Routine skipped;

    // The driver's loader (engine, path, number), its start (engine, index, loop, first channel), and its update
    // (engine).
    Routine load;
    Routine start;
    Routine update;

    // The SDK's routines that the main loop calls each frame: OS_WaitVBlankIntr, SND_RecvCommandReply and
    // SND_FlushCommand.
    Routine wait_vblank;
    Routine receive_reply;
    Routine flush;

    // The files that the set reads, by the end of their paths.
    std::vector<std::string_view> kept_suffixes;

    // The path of the table of sounds, and the number of the song that the game plays at its title screen.
    std::string_view sound_table;
    uint32_t title_song = 0;
};

// The profile of `rom`, or null with `error` set when 2sfrip supports no game with its code, or when its code differs
// from the profile's.
const UniversomoProfile* FindProfile(const ntdlib::ntr::Rom& rom, std::string& error);

// Whether the ARM9 binary starts with 0xE7FFDEFF twice, as it does once the secure area is decrypted. A set runs the
// game's start-up, which the encrypted area would crash.
bool SecureAreaDecrypted(const ntdlib::ntr::Rom& rom);

// The game codes of the games that 2sfrip supports.
std::vector<std::string_view> SupportedGames();

} // namespace twosf::rip
