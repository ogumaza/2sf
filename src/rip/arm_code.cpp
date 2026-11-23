// SPDX-License-Identifier: MIT

#include "rip/arm_code.h"

#include <algorithm>

#include "xsflib/bytes.h"

namespace twosf::rip
{

namespace
{

// The register that a call uses for the routine's address.
constexpr int kScratch = 12;

} // namespace

std::optional<uint32_t> WordAt(const ntdlib::ntr::LoadedCode& arm9, uint32_t address)
{
    if (address < arm9.address || address - arm9.address + 4 > arm9.bytes.size())
    {
        return std::nullopt;
    }

    return xsflib::LoadLe32(&arm9.bytes[address - arm9.address]);
}

std::optional<GameWord> MissingWord(const ntdlib::ntr::LoadedCode& arm9, std::span<const GameWord> words)
{
    const auto missing =
        std::ranges::find_if(words, [&arm9](const GameWord& word) { return WordAt(arm9, word.address) != word.value; });
    if (missing == words.end())
    {
        return std::nullopt;
    }

    return *missing;
}

bool PatchMatches(const ntdlib::ntr::LoadedCode& arm9, const GamePatch& patch)
{
    const std::optional<uint32_t> word = WordAt(arm9, patch.address & ~3u);
    const uint32_t shift = 8 * (patch.address & 3);
    const uint32_t mask = patch.bytes == 4 ? 0xFFFFFFFF : 0xFFFFu;

    return word && (patch.bytes == 4 || patch.bytes == 2) && (patch.address % patch.bytes) == 0 &&
           ((*word >> shift) & mask) == patch.expected;
}

void ApplyPatch(std::span<uint8_t> arm9, uint32_t base, const GamePatch& patch)
{
    uint8_t* at = &arm9[patch.address - base];
    if (patch.bytes == 2)
    {
        xsflib::StoreLe16(at, static_cast<uint16_t>(patch.value));
    }
    else
    {
        xsflib::StoreLe32(at, patch.value);
    }
}

uint32_t Branch(uint32_t address, uint32_t target)
{
    constexpr uint32_t kB = 0xEA000000;
    constexpr uint32_t kOffsetBits = 0x00FFFFFF;
    return kB | (((target - (address + 8)) >> 2) & kOffsetBits);
}

uint32_t BranchLink(uint32_t address, uint32_t target)
{
    constexpr uint32_t kLinkBit = 0x01000000;
    return Branch(address, target) | kLinkBit;
}

void Call(ntdlib::arm::ArmAssembler& a, uint32_t address)
{
    a.LoadConstant(kScratch, address);
    if ((address & 1) != 0)
    {
        a.CallThumb(kScratch);
    }
    else
    {
        a.CallRegister(kScratch);
    }
}

void CallOnArm9(ntdlib::arm::ArmAssembler& a, uint32_t address)
{
    if ((address & 1) == 0)
    {
        Call(a, address);
        return;
    }

    constexpr uint32_t kBlxScratch = 0xE12FFF30 | kScratch; // blx r12
    a.LoadConstant(kScratch, address);
    a.Word(kBlxScratch);
}

} // namespace twosf::rip
