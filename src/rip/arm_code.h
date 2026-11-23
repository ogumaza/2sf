// SPDX-License-Identifier: MIT

// Reads and writes of a game's ARM9 code that the sets share: the words that a table expects, branches and calls.

#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "ntdlib/arm/assembler.h"
#include "ntdlib/ntr/code.h"

namespace twosf::rip
{

// An instruction word of the game's code that a table expects: the word at `address` of the ARM9 binary, or of the ARM9
// overlay `overlay`, decompressed.
struct GameWord
{
    uint32_t address = 0;
    uint32_t value = 0;
    int overlay = -1; // -1 for the ARM9 binary
};

// The word at `address` of `arm9`, or nothing past either end.
std::optional<uint32_t> WordAt(const ntdlib::ntr::LoadedCode& arm9, uint32_t address);

// The first of `words` that `arm9` doesn't contain, or nothing when it contains every one. Each word must be one of the
// ARM9 binary (overlay -1).
std::optional<GameWord> MissingWord(const ntdlib::ntr::LoadedCode& arm9, std::span<const GameWord> words);

// A change of the game's code that a set makes: the word or halfword (`bytes`, 4 or 2) at `address` of the ARM9 binary,
// `expected` in the game, becomes `value`. A set patches a game's code only where the game can't reach its music
// otherwise, such as a save check that stops the boot without a backup memory.
struct GamePatch
{
    friend bool operator==(const GamePatch&, const GamePatch&) = default;

    uint32_t address = 0;
    uint32_t expected = 0;
    uint32_t value = 0;
    uint32_t bytes = 4;
};

// Whether `arm9`, the ARM9 binary decompressed, contains `patch.expected` at the patch's address, as a word or a
// halfword at a multiple of its size.
bool PatchMatches(const ntdlib::ntr::LoadedCode& arm9, const GamePatch& patch);

// `patch` as a patch that expects its value. It checks a set's library, where the word is already patched.
inline GamePatch PatchedWord(const GamePatch& patch)
{
    GamePatch patched = patch;
    patched.expected = patch.value;

    return patched;
}

// Writes `patch` into `arm9`, the ARM9 binary decompressed. `arm9` starts at `base`.
void ApplyPatch(std::span<uint8_t> arm9, uint32_t base, const GamePatch& patch);

// b <target> at `address`, in ARM code.
uint32_t Branch(uint32_t address, uint32_t target);

// bl <target> at `address`, in ARM code.
uint32_t BranchLink(uint32_t address, uint32_t target);

// A call of the routine at `address`, in ARM code or, with bit 0 set, in Thumb code. It uses r12.
void Call(ntdlib::arm::ArmAssembler& a, uint32_t address);

// Call for code that runs only on the ARM9 (ARMv5TE): a Thumb routine takes blx r12, one instruction in place of Call's
// veneer. A stub with room for little more than its calls fits. An ARM routine takes Call's code. Call keeps its
// veneer: the sets that call Thumb routines through it keep their bytes.
void CallOnArm9(ntdlib::arm::ArmAssembler& a, uint32_t address);

} // namespace twosf::rip
