// SPDX-License-Identifier: MIT

// The boots on which a ripper reads a game and checks its minis.
//
// A cartridge's header doesn't give the kind of its backup memory, and a game may take another path through its boot
// with a backup memory than without one. A Naruto game from Japan (NTR-AREJ) shows an error screen without one, and
// loads its logo screen's files with a 4-kbit EEPROM. A set must contain the files of both paths. 2sf's player boots a
// set with no backup memory, and a DS and melonDS boot it with the cartridge's. A ripper therefore boots each game with
// no backup memory and with a blank one for each size of address, keeps the files that any of those boots reads before
// the hook, and runs each mini on each of them.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iterator>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/ntr/machine.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/ntr/sound.h"

#include "rip/arm_code.h"

namespace twosf::rip
{

// The backup memories of the boots for the game with `game_code`: none first, then a blank one with 1, 2 and 3 address
// bytes (512 bytes, 64 KiB and 512 KiB). For a game code that starts with I, each has the infrared chip in front of it.
std::vector<ntdlib::ntr::BackupMemory> BootBackups(std::string_view game_code);

// How a message names the boot with `backup`, such as "with no backup memory" or "with a blank backup memory of 512
// bytes".
std::string BootName(const ntdlib::ntr::BackupMemory& backup);

// What ends a message about the boot with `backup`, such as " (the boot with a blank backup memory of 64 KiB)". Nothing
// for the boot with no backup memory.
std::string OnBoot(const ntdlib::ntr::BackupMemory& backup);

// Calls `run`, a boot or a run of a mini on the boot with `backup`, and returns what it returns. An exception from it
// comes back as a std::runtime_error whose message ends with OnBoot(backup). The rip still stops. A std::bad_alloc
// passes unchanged: xsflib's AnalyzeLengths catches it alone and reports it as out of memory.
template <typename Run>
auto RunOnBoot(const ntdlib::ntr::BackupMemory& backup, Run run)
{
    try
    {
        return run();
    }
    catch (const std::bad_alloc&)
    {
        throw;
    }
    catch (const std::exception& e)
    {
        throw std::runtime_error(e.what() + OnBoot(backup));
    }
}

// The IDs of every file of `rom`.
std::set<uint32_t> EveryFile(const ntdlib::ntr::Rom& rom);

// The files `ids` of `rom`, each whole, by ID: the files of a compact image (ntdlib/ntr/compact_image.h).
std::map<uint32_t, std::vector<uint8_t>> WholeFiles(const ntdlib::ntr::Rom& rom, const std::set<uint32_t>& ids);

// The image that the rippers boot as the game: `rom`'s image, or with `patches` the image with the ARM9 binary
// decompressed and patched, and every file kept whole.
std::vector<uint8_t> PatchedImage(const ntdlib::ntr::Rom& rom, std::span<const GamePatch> patches);

// How a library's boot differs from a reference boot with the same backup memory, at the stop.
struct BootDifference
{
    friend bool operator==(const BootDifference&, const BootDifference&) = default;

    int64_t clock = 0;                 // the library's clock less the reference's
    std::set<uint32_t> only_reference; // the files that only the reference's boot reads before the stop
    std::set<uint32_t> only_library;   // the files that only the library's boot reads before it
};

// How `library` differs from `reference`. `Boot` has `clock` and `files`.
template <typename Boot>
BootDifference DifferenceOf(const Boot& reference, const Boot& library)
{
    BootDifference difference;
    difference.clock = static_cast<int64_t>(library.clock) - static_cast<int64_t>(reference.clock);
    std::ranges::set_difference(reference.files, library.files,
                                std::inserter(difference.only_reference, difference.only_reference.end()));
    std::ranges::set_difference(library.files, reference.files,
                                std::inserter(difference.only_library, difference.only_library.end()));
    return difference;
}

// The first boot on which the library's boot differs at all from the reference's, from `reference` and `library`, the
// boots of each in the order of BootBackups: it misses the stop, comes to it at another clock or reads other files.
// Nothing when every boot matches. Every ripper compares a library with the full image: the set's image with every file
// of the game kept whole. The two images have the same stub and store the same binaries. They differ only in the files
// that the library leaves out, and in an SDAT game's sound archive. The library cuts that archive, and each file that
// the cut keeps stays at its place in its card page (CutArchive). A file that the library leaves out has no bytes in
// the library's image. A read of it therefore reaches no card, and the boot reads other files or reaches the stop at
// another clock. `Boot` has `reached`, `clock` and `files`.
template <typename Boot>
std::optional<std::size_t> DifferentBoot(const std::vector<Boot>& reference, const std::vector<Boot>& library)
{
    for (std::size_t i = 0; i < reference.size(); i++)
    {
        if (!reference[i].reached || !library[i].reached || DifferenceOf(reference[i], library[i]) != BootDifference{})
        {
            return i;
        }
    }

    return std::nullopt;
}

// Byte ranges [first, end) of a file, in order, apart and not empty.
using FileRanges = std::vector<std::pair<uint32_t, uint32_t>>;

// Adds [first, end) to `ranges`, merged with the ranges that it touches.
void AddRange(FileRanges& ranges, uint32_t first, uint32_t end);

// The size of a card page: the card reads 512 bytes at a time, and the SDK reads the card through a cache of one page.
inline constexpr uint32_t kCardPage = 0x200;

// Adds to `ranges`, ranges of `file`, the card pages of the image that bytes [first, end) of `file` reach, cut to the
// file. A card read reaches whole pages.
void AddCardPages(FileRanges& ranges, const ntdlib::ntr::RomFile& file, uint32_t first, uint32_t end);

// The files of a library by ID: each of `whole` as it is, and each of `partial` at its size, with the ranges that
// `partial` gives and zeros elsewhere. No file is in both.
std::map<uint32_t, std::vector<uint8_t>> KeptFiles(const ntdlib::ntr::Rom& rom, const std::set<uint32_t>& whole,
                                                   const std::map<uint32_t, FileRanges>& partial);

// What a boot of an image gives at a hook, the place where a stub replaces the game's code: whether it reached it, the
// time, the ARM9's CPSR, the files that the card's reads reached, and the ranges of each file that they reached.
struct HookBoot
{
    bool reached = false;
    uint64_t clock = 0; // the machine's clock, in cycles of the system clock
    double seconds = 0;
    uint32_t cpsr = 0;
    std::set<uint32_t> files;
    std::map<uint32_t, FileRanges> reads;
};

// Boots `rom` on ntdlib's DS with `backup` in its card until the ARM9 reaches `hook`, for at most 30 s.
HookBoot BootTo(const ntdlib::ntr::Rom& rom, uint32_t hook, const ntdlib::ntr::BackupMemory& backup = {});

// The state of the first of `boots`, with the files and the ranges of every boot. A set keeps the files that any boot
// reads before the hook.
HookBoot MergedBoot(std::span<const HookBoot> boots);

// The files that a library keeps: every file that any boot of the game reads before the hook, `game`, every file that
// any boot of the full image reads, `full`, and the songs' files `songs`. `game` and `full` are merged states
// (MergedBoot). The full image is the set's image with every file kept whole.
std::set<uint32_t> LibraryFiles(const HookBoot& game, const HookBoot& full, const std::set<uint32_t>& songs);

// Whether a boot reaches the hook in a thread with interrupts on, where a stub can run: in user or system mode with the
// CPSR's I bit clear.
bool LetsStubRun(const HookBoot& boot);

// The ranges of the card that a set's image keeps: the file system's tables and each file with bytes, or, for a file of
// `partial`, the ranges of it that `partial` gives. A mini's run fails when the game reads anything else after the
// hook.
std::vector<std::pair<uint32_t, uint32_t>> KeptRanges(const ntdlib::ntr::Rom& rom,
                                                      const std::map<uint32_t, FileRanges>& partial = {});

// Whether a card read of `bytes` bytes at `address` reaches one of `ranges`.
bool Reaches(std::span<const std::pair<uint32_t, uint32_t>> ranges, uint32_t address, uint32_t bytes);

// The error of a mini's run whose card read at `address`, after the hook, reaches no range that the set keeps.
std::string ReadOutsideSetError(uint32_t address);

// A boot of a set: its backup memory, the clock of the hook on the library's boot, and the time from the boot within
// which a mini's sound must start.
struct SetBoot
{
    ntdlib::ntr::BackupMemory backup;
    uint64_t hook_clock = 0;
    double seconds = 0;
};

// The backup memories of `boots`, in their order.
std::vector<ntdlib::ntr::BackupMemory> BackupsOf(std::span<const SetBoot> boots);

// Follows the sound unit's output from a sound's start: whether it ever leaves its first value.
class OutputWatch
{
public:
    void Add(std::span<const ntdlib::ntr::SoundOutput> samples);
    bool Sounded() const;

private:
    std::optional<ntdlib::ntr::SoundOutput> first_;
    bool sounded_ = false;
};

// What a mini's run on one boot gives, for the check over every boot.
struct BootRun
{
    std::optional<std::string> error; // why the run fails
    bool sounded = false;             // whether the output changed after the sound's start (OutputWatch)
};

// Why a mini fails the check over the boots with `backups`, from `runs`, its run on each boot in the same order, with
// no backup memory first. A mini fails when a run fails, and when a run with a backup memory stays silent where the run
// without one sounds. The message names the boot. Nothing when the mini passes.
std::optional<std::string> BootsError(std::span<const ntdlib::ntr::BackupMemory> backups,
                                      std::span<const BootRun> runs);

} // namespace twosf::rip
