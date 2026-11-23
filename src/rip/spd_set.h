// SPDX-License-Identifier: MIT

// The parts of a set for a game on Nintendo SPD's sound driver (Super Princess Peach): the game's routines, the stub,
// the library's image and the run of a mini on ntdlib's DS.
//
// The driver runs on the ARM7 (ntdlib/spd/player.h). The game's ARM9 code loads a block of the sound data as set B,
// queues a message that starts a sound, and sends the queued messages to the driver once a frame. The game's main
// function starts the sound driver, then shows its first screens. They wait for a touch. The hook is the instruction
// after the call that starts the driver. The stub goes there, over the rest of the main function, and never returns. It
// loads the sound's block as set B, starts the sound with no fade, sets the branch flag to 1 where the mini sets it,
// then waits for each vertical blank (the BIOS's VBlankIntrWait) and sends the frame's messages, as the game's loops
// do.
//
// The library is a compact image (ntdlib/ntr/compact_image.h): the files that the game's boot reads before the hook,
// whole, and the sound data's file. A mini stores the sound's number (from 1), its block's slot and the branch flag in
// the stub's three words.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/ntr/machine.h"
#include "ntdlib/ntr/rom.h"

#include "rip/arm_code.h"

namespace twosf::rip
{

// The routines of a game on Nintendo SPD's driver that a set calls. Each address is of ARM code in the ARM9 binary's
// static part.
struct SpdCode
{
    std::string_view game_code;

    // The hook, and the end of the code after it that a set never runs: the start of the function's literal pool.
    uint32_t hook = 0;
    uint32_t hook_end = 0;

    uint32_t load_set_b = 0;  // (sound, slot): loads the slot's block as set B, and returns 0 until it's loaded
    uint32_t play = 0;        // (sound, slot, fade): queues the message that starts the sound
    uint32_t sound_frame = 0; // sends the queued messages to the driver

    // Sends the driver command 10 with 1: the branch flag of player 0's 0xFC and 0xFD.
    uint32_t branch_flag_1 = 0;

    // The instruction words that FindSpdCode checks: the call before the hook, the branch before the literal pool, and
    // the first word of each routine.
    std::span<const GameWord> words;
};

// Whether FindSpdCode's table lists the game whose header gives `game_code`.
bool ListsSpdGame(std::string_view game_code);

// The routines of the game in `rom`, by its game code: Super Princess Peach (ASPE). Returns nothing, with `error` set,
// for another game, or when the ARM9 binary doesn't contain the table's instruction words.
std::optional<SpdCode> FindSpdCode(const ntdlib::ntr::Rom& rom, std::string& error);

// The sound that a set starts: its number in its block, from 1, the block's slot (0 for set A), and the branch flag.
// With the flag at 0, the stub leaves the driver's start-up value, 0.
struct SpdStart
{
    friend bool operator==(const SpdStart&, const SpdStart&) = default;

    uint32_t sound = 0;
    uint32_t slot = 0;
    uint32_t branch_flag = 0;
};

// The stub at `code.hook`, and the offset of its three words from its start: the sound, the slot and the branch flag.
struct SpdStub
{
    std::vector<uint8_t> bytes;
    uint32_t start_offset = 0;
};

SpdStub BuildSpdStub(const SpdCode& code, const SpdStart& start);

// The bytes of a mini's block for `start`: the sound, the slot and the branch flag.
std::vector<uint8_t> SpdStartWords(const SpdStart& start);

// The library's image of `rom`: the ARM9 binary with `stub` at the hook, and the files `files`. `start_word` gives the
// offset in the image of the stub's first word.
struct SpdSet
{
    std::vector<uint8_t> image;
    uint32_t start_word = 0;
};

// Throws std::runtime_error when the stub doesn't fit between the hook and the end of its code.
SpdSet MakeSpdSet(const ntdlib::ntr::Rom& rom, const SpdCode& code, const SpdStub& stub,
                  const std::map<uint32_t, std::vector<uint8_t>>& files);

// What a mini's run on ntdlib's DS gives: `start_ms` is the time from the boot to the stub's call of the game's start
// of the sound. `sounded` says whether the sound unit's output changed after the start. `error` says why the run
// doesn't count.
struct SpdMiniRun
{
    std::optional<std::string> error;
    double start_ms = 0;
    bool sounded = false;
};

// The time that a mini's run plays on after its sound starts.
constexpr double kSpdMiniPlaySeconds = 5.0;

// Boots `image`, a set's image with a mini in place, with `backup` in its card, until the stub starts the sound, for at
// most `seconds`. Then it plays on for kSpdMiniPlaySeconds. The run fails:
// - when the boot doesn't reach the hook, or the stub doesn't start a sound, in time;
// - when the stub starts another sound than `start`;
// - when a card read after `hook_clock`, the clock of the hook on the boot with `backup`, reaches neither the file
//   system's tables nor any of the image's files.
SpdMiniRun RunSpdMini(std::span<const uint8_t> image, const SpdCode& code, uint64_t hook_clock, const SpdStart& start,
                      double seconds, const ntdlib::ntr::BackupMemory& backup = {});

} // namespace twosf::rip
