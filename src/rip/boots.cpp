// SPDX-License-Identifier: MIT

#include "rip/boots.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>

#include "ntdlib/ntr/code.h"
#include "ntdlib/ntr/compact_image.h"
#include "xsflib/text.h"

namespace twosf::rip
{

namespace ntr = ntdlib::ntr;

std::vector<ntr::BackupMemory> BootBackups(std::string_view game_code)
{
    const bool infrared = game_code.starts_with('I');

    return {
        {},
        {.address_bytes = 1, .size = 512, .infrared = infrared},
        {.address_bytes = 2, .size = 0x10000, .infrared = infrared},
        {.address_bytes = 3, .size = 0x80000, .infrared = infrared},
    };
}

namespace
{

// The boot's limit.
constexpr uint64_t kBootClock = 30ull * ntr::kSystemClockHz;

// The CPSR's mode bits, its I bit, and the modes in which a stub may run: user and system.
constexpr uint32_t kModeBits = 0x1F;
constexpr uint32_t kIrqDisabled = 0x80;
constexpr uint32_t kUserMode = 0x10;
constexpr uint32_t kSystemMode = 0x1F;

} // namespace

HookBoot BootTo(const ntr::Rom& rom, uint32_t hook, const ntr::BackupMemory& backup)
{
    HookBoot state;
    const auto machine = std::make_unique<ntr::Machine>(rom, backup);
    machine->on_card_read = [&](uint32_t address, uint32_t bytes)
    {
        for (const uint32_t id : rom.FilesIn(address, address + bytes))
        {
            state.files.insert(id);
            const ntr::RomFile& file = rom.Files()[id];
            AddRange(state.reads[id], std::max(address, file.start) - file.start,
                     std::min(address + bytes, file.end) - file.start);
        }
    };
    machine->Arm9().SetStopAddress(hook);
    std::vector<ntr::SoundOutput> out;
    state.reached = machine->Run(kBootClock, out);
    state.clock = machine->Clock();
    state.seconds = static_cast<double>(machine->Clock()) / ntr::kSystemClockHz;
    state.cpsr = machine->Arm9().Cpsr();

    return state;
}

void AddRange(FileRanges& ranges, uint32_t first, uint32_t end)
{
    if (first >= end)
    {
        return;
    }

    // The ranges that end before `first` stay, and so do the ranges that start after `end`. The rest merge.
    const auto before = [first](const std::pair<uint32_t, uint32_t>& range)
    {
        return range.second < first;
    };
    const auto low = std::ranges::find_if_not(ranges, before);
    auto high = low;
    while (high != ranges.end() && high->first <= end)
    {
        first = std::min(first, high->first);
        end = std::max(end, high->second);
        ++high;
    }
    const auto at = ranges.erase(low, high);
    ranges.insert(at, {first, end});
}

void AddCardPages(FileRanges& ranges, const ntr::RomFile& file, uint32_t first, uint32_t end)
{
    const uint32_t low = (file.start + first) / kCardPage * kCardPage;
    const uint32_t high = (file.start + end + kCardPage - 1) / kCardPage * kCardPage;
    AddRange(ranges, std::max(low, file.start) - file.start, std::min(high, file.end) - file.start);
}

std::map<uint32_t, std::vector<uint8_t>> KeptFiles(const ntr::Rom& rom, const std::set<uint32_t>& whole,
                                                   const std::map<uint32_t, FileRanges>& partial)
{
    std::map<uint32_t, std::vector<uint8_t>> files = WholeFiles(rom, whole);
    for (const auto& [id, ranges] : partial)
    {
        const std::span<const uint8_t> data = rom.FileData(id);
        std::vector<uint8_t>& bytes = files[id];
        bytes.assign(data.size(), 0);
        for (const auto& [first, end] : ranges)
        {
            const auto last = static_cast<uint32_t>(std::min<std::size_t>(end, data.size()));
            if (first < last)
            {
                std::copy(data.begin() + first, data.begin() + last, bytes.begin() + first);
            }
        }
    }

    return files;
}

HookBoot MergedBoot(std::span<const HookBoot> boots)
{
    if (boots.empty())
    {
        return {};
    }

    HookBoot merged = boots.front();
    for (const HookBoot& boot : boots.subspan(1))
    {
        merged.files.insert(boot.files.begin(), boot.files.end());
        for (const auto& [id, ranges] : boot.reads)
        {
            for (const auto& [first, end] : ranges)
            {
                AddRange(merged.reads[id], first, end);
            }
        }
    }

    return merged;
}

std::set<uint32_t> LibraryFiles(const HookBoot& game, const HookBoot& full, const std::set<uint32_t>& songs)
{
    std::set<uint32_t> kept = game.files;
    kept.insert(full.files.begin(), full.files.end());
    kept.insert(songs.begin(), songs.end());

    return kept;
}

bool LetsStubRun(const HookBoot& boot)
{
    const uint32_t mode = boot.cpsr & kModeBits;

    return boot.reached && (mode == kSystemMode || mode == kUserMode) && (boot.cpsr & kIrqDisabled) == 0;
}

std::vector<std::pair<uint32_t, uint32_t>> KeptRanges(const ntr::Rom& rom,
                                                      const std::map<uint32_t, FileRanges>& partial)
{
    const ntr::RomHeader& header = rom.Header();
    std::vector<std::pair<uint32_t, uint32_t>> kept = {{header.fnt_offset, header.fnt_offset + header.fnt_size},
                                                       {header.fat_offset, header.fat_offset + header.fat_size}};
    for (const ntr::RomFile& file : rom.Files())
    {
        if (const auto ranges = partial.find(file.id); ranges != partial.end())
        {
            for (const auto& [first, end] : ranges->second)
            {
                kept.emplace_back(file.start + first, file.start + end);
            }
        }
        else if (file.end > file.start)
        {
            kept.emplace_back(file.start, file.end);
        }
    }

    return kept;
}

bool Reaches(std::span<const std::pair<uint32_t, uint32_t>> ranges, uint32_t address, uint32_t bytes)
{
    return std::ranges::any_of(ranges, [&](const std::pair<uint32_t, uint32_t>& range)
                               { return range.first < uint64_t{address} + bytes && address < range.second; });
}

std::string ReadOutsideSetError(uint32_t address)
{
    return "the game reads " + xsflib::Hex(address, 8, true) + ", outside the files that the set keeps";
}

std::set<uint32_t> EveryFile(const ntr::Rom& rom)
{
    std::set<uint32_t> ids;
    for (const ntr::RomFile& file : rom.Files())
    {
        ids.insert(file.id);
    }

    return ids;
}

std::map<uint32_t, std::vector<uint8_t>> WholeFiles(const ntr::Rom& rom, const std::set<uint32_t>& ids)
{
    std::map<uint32_t, std::vector<uint8_t>> files;
    for (const uint32_t id : ids)
    {
        const std::span<const uint8_t> data = rom.FileData(id);
        files[id].assign(data.begin(), data.end());
    }

    return files;
}

std::vector<uint8_t> PatchedImage(const ntr::Rom& rom, std::span<const GamePatch> patches)
{
    if (patches.empty())
    {
        return {rom.Image().begin(), rom.Image().end()};
    }

    ntr::CompactParts parts{.arm9 = ntr::UncompressedArm9(rom), .files = WholeFiles(rom, EveryFile(rom))};
    for (const GamePatch& patch : patches)
    {
        ApplyPatch(parts.arm9, rom.Header().arm9.ram_address, patch);
    }

    return ntr::WriteCompactImage(rom, parts);
}

std::string BootName(const ntr::BackupMemory& backup)
{
    if (backup.address_bytes == 0)
    {
        return "with no backup memory";
    }

    const std::string size =
        backup.size % 1024 == 0 ? std::to_string(backup.size / 1024) + " KiB" : std::to_string(backup.size) + " bytes";

    return "with a blank backup memory of " + size;
}

std::string OnBoot(const ntr::BackupMemory& backup)
{
    return backup.address_bytes == 0 ? "" : " (the boot " + BootName(backup) + ")";
}

std::vector<ntr::BackupMemory> BackupsOf(std::span<const SetBoot> boots)
{
    std::vector<ntr::BackupMemory> backups;
    for (const SetBoot& boot : boots)
    {
        backups.push_back(boot.backup);
    }

    return backups;
}

void OutputWatch::Add(std::span<const ntr::SoundOutput> samples)
{
    for (const ntr::SoundOutput& sample : samples)
    {
        if (!first_)
        {
            first_ = sample;
        }
        else if (sample != *first_)
        {
            sounded_ = true;
            return;
        }
    }
}

bool OutputWatch::Sounded() const
{
    return sounded_;
}

std::optional<std::string> BootsError(std::span<const ntr::BackupMemory> backups, std::span<const BootRun> runs)
{
    for (std::size_t i = 0; i < runs.size() && i < backups.size(); i++)
    {
        if (runs[i].error)
        {
            return *runs[i].error + " (the boot " + BootName(backups[i]) + ")";
        }

        if (i > 0 && runs[0].sounded && !runs[i].sounded)
        {
            return "the mini stays silent " + BootName(backups[i]) + ", where it sounds " + BootName(backups[0]);
        }
    }

    return std::nullopt;
}

} // namespace twosf::rip
