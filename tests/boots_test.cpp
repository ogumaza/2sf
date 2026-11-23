// SPDX-License-Identifier: MIT

// Tests the boots on which a ripper reads a game and checks its minis (rip/boots.h): the backup memories of the boots,
// their names in messages, the watch of the sound unit's output, the check of a mini over every boot, the exact check
// of a library over every boot, the ranges of a file, and the merge of the boots' files and ranges.

#include "rip/boots.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/ntr/code.h"
#include "ntdlib/ntr/machine.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/ntr/sound.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "hand_built_set.h"

namespace
{

namespace ntr = ntdlib::ntr;

using twosf::rip::BootRun;
using xsflib::testing::Check;

// The boots: none first, then one blank backup memory for each size of address, each a size that ntdlib's DS accepts.
// The infrared chip comes with a game code that starts with I.
void TestBackups()
{
    const std::vector<ntr::BackupMemory> backups = twosf::rip::BootBackups("AREJ");
    const std::vector<ntr::BackupMemory> infrared = twosf::rip::BootBackups("IPKE");

    Check(backups.size() == 4 && infrared.size() == 4, "four boots");
    if (backups.size() != 4 || infrared.size() != 4)
    {
        return;
    }

    Check(backups[0].address_bytes == 0 && !infrared[0].infrared, "the first boot has no backup memory");
    for (int i = 1; i < 4; i++)
    {
        Check(backups[i].address_bytes == i && backups[i].blank == 0xFF && !backups[i].infrared,
              "a blank backup memory with " + std::to_string(i) + " address bytes");
        Check(infrared[i].address_bytes == i && infrared[i].infrared, "the infrared chip in front of the memory");
    }

    Check(backups[1].size == 512 && backups[2].size == 0x10000 && backups[3].size == 0x80000, "the sizes");
    const std::vector<uint8_t> image = twosf::testing::HaltingImage();
    const ntr::Rom rom = ntr::Rom::Parse(image);
    for (const ntr::BackupMemory& backup : backups)
    {
        bool accepted = true;
        try
        {
            const ntr::Machine machine(rom, backup);
        }
        catch (const std::invalid_argument&)
        {
            accepted = false;
        }
        Check(accepted, "ntdlib's DS accepts a backup memory " + twosf::rip::BootName(backup));
    }
}

void TestNames()
{
    const std::vector<ntr::BackupMemory> backups = twosf::rip::BootBackups("AREJ");

    Check(twosf::rip::BootName(backups[0]) == "with no backup memory", "no backup memory");
    Check(twosf::rip::BootName(backups[1]) == "with a blank backup memory of 512 bytes", "512 bytes");
    Check(twosf::rip::BootName(backups[2]) == "with a blank backup memory of 64 KiB", "64 KiB");
    Check(twosf::rip::BootName(backups[3]) == "with a blank backup memory of 512 KiB", "512 KiB");
}

// RunOnBoot returns what the run returns. A run that throws on a boot with a backup memory throws again, with the boot
// named at the message's end. The boot with none adds nothing. A std::bad_alloc passes unchanged.
void TestRunOnBoot()
{
    const std::vector<ntr::BackupMemory> backups = twosf::rip::BootBackups("AREJ");
    const auto fault = []() -> int
    {
        throw std::runtime_error("the ARM9 stops on a fault at 0x02058D64");
    };
    const auto message_on = [&](const ntr::BackupMemory& backup)
    {
        try
        {
            twosf::rip::RunOnBoot(backup, fault);
        }
        catch (const std::runtime_error& e)
        {
            return std::string(e.what());
        }
        return std::string("no exception");
    };

    const int value = twosf::rip::RunOnBoot(backups[1], [] { return 7; });
    const std::string with = message_on(backups[1]);
    const std::string without = message_on(backups[0]);

    Check(value == 7, "the run's value");
    Check(with == "the ARM9 stops on a fault at 0x02058D64 (the boot with a blank backup memory of 512 bytes)",
          "a throw on the boot with a backup memory names it: " + with);
    Check(without == "the ARM9 stops on a fault at 0x02058D64", "a throw on the boot with none: " + without);

    bool out_of_memory = false;
    try
    {
        twosf::rip::RunOnBoot(backups[1], []() -> int { throw std::bad_alloc(); });
    }
    catch (const std::bad_alloc&)
    {
        out_of_memory = true;
    }
    catch (const std::exception&)
    {
    }
    Check(out_of_memory, "a std::bad_alloc on the boot with a backup memory stays a std::bad_alloc");
}

// The watch counts a change from the first sample, on either side, across calls. A constant output stays silent, at
// any level.
void TestWatch()
{
    const ntr::SoundOutput bias{0x200, 0x200};
    const std::vector<ntr::SoundOutput> quiet(64, bias);
    const std::vector<ntr::SoundOutput> right_moves = {bias, {0x200, 0x201}};
    const std::vector<ntr::SoundOutput> low(64, ntr::SoundOutput{0, 0});

    twosf::rip::OutputWatch empty;
    twosf::rip::OutputWatch silent;
    silent.Add(quiet);
    silent.Add(quiet);
    twosf::rip::OutputWatch sounds;
    sounds.Add(quiet);
    sounds.Add(right_moves);
    twosf::rip::OutputWatch level;
    level.Add(quiet);
    level.Add(low);

    Check(!empty.Sounded(), "no output");
    Check(!silent.Sounded(), "the bias alone");
    Check(sounds.Sounded(), "one side moves, in a later call");
    Check(level.Sounded(), "a jump from the bias");
}

// A mini passes when every run passes and each run with a backup memory sounds where the run without one does. The
// first failure names its boot.
void TestBootsError()
{
    const std::vector<ntr::BackupMemory> backups = twosf::rip::BootBackups("AREJ");
    const BootRun sounds{std::nullopt, true};
    const BootRun silent{std::nullopt, false};
    const BootRun fails{std::string("the sequence doesn't start within 10 s of the boot"), false};

    const std::optional<std::string> all_sound =
        twosf::rip::BootsError(backups, std::vector<BootRun>{sounds, sounds, sounds, sounds});
    const std::optional<std::string> all_silent =
        twosf::rip::BootsError(backups, std::vector<BootRun>{silent, silent, silent, silent});
    const std::optional<std::string> only_without =
        twosf::rip::BootsError(backups, std::vector<BootRun>{silent, sounds, silent, sounds});
    const std::optional<std::string> silent_with =
        twosf::rip::BootsError(backups, std::vector<BootRun>{sounds, sounds, silent, sounds});
    const std::optional<std::string> fails_with =
        twosf::rip::BootsError(backups, std::vector<BootRun>{sounds, sounds, sounds, fails});
    const std::optional<std::string> fails_without =
        twosf::rip::BootsError(backups, std::vector<BootRun>{fails, sounds, silent, sounds});

    Check(!all_sound, "every boot sounds");
    Check(!all_silent, "silent on every boot: the check of the sequence's notes is elsewhere");
    Check(!only_without, "silent without a backup memory");
    Check(silent_with ==
              "the mini stays silent with a blank backup memory of 64 KiB, where it sounds with no backup "
              "memory",
          "silent with a backup memory alone");
    Check(fails_with ==
              "the sequence doesn't start within 10 s of the boot (the boot with a blank backup memory of "
              "512 KiB)",
          "a run with a backup memory fails");
    Check(fails_without == "the sequence doesn't start within 10 s of the boot (the boot with no backup memory)",
          "the first failure counts");
}

// A boot as the rippers record it, for DifferentBoot.
struct Boot
{
    bool reached = true;
    uint64_t clock = 0;
    std::set<uint32_t> files;
};

// The library check over the boots, against the full image. On each boot, the library must read the same files as the
// full image and reach the stop at the full image's clock. A boot that misses the stop fails.
void TestDifferentBootAgainstFullImage()
{
    const std::vector<Boot> full{{true, 100, {1, 2}}, {true, 150, {1, 2, 3}}, {true, 100, {1, 2}}};

    // The library comes 44,810 later on every boot. Another layout of a cut archive can do that.
    const std::vector<Boot> later{{true, 44910, {1, 2}}, {true, 44960, {1, 2, 3}}, {true, 44910, {1, 2}}};
    // The library leaves file 2 out, and its boots read no card for it. Every boot differs alike.
    const std::vector<Boot> dropped{{true, 99, {1}}, {true, 149, {1, 3}}, {true, 99, {1}}};
    // The boot with a backup memory skips file 3, as on an error screen.
    const std::vector<Boot> fewer{{true, 100, {1, 2}}, {true, 150, {1, 2}}, {true, 100, {1, 2}}};
    // The boot with a backup memory comes 30 later.
    const std::vector<Boot> another_clock{{true, 100, {1, 2}}, {true, 180, {1, 2, 3}}, {true, 100, {1, 2}}};
    // The last boot misses the stop.
    const std::vector<Boot> missed{{true, 100, {1, 2}}, {true, 150, {1, 2, 3}}, {false, 100, {1, 2}}};
    // The boot with a backup memory also reads file 4. The full image doesn't read it before the stop.
    const std::vector<Boot> more{{true, 100, {1, 2}}, {true, 150, {1, 2, 3, 4}}, {true, 100, {1, 2}}};

    const twosf::rip::BootDifference difference = twosf::rip::DifferenceOf(full[1], dropped[1]);
    Check(
        difference.clock == -1 && difference.only_reference == std::set<uint32_t>{2} && difference.only_library.empty(),
        "the difference of one boot");
    Check(twosf::rip::DifferenceOf(full[1], more[1]).only_library == std::set<uint32_t>{4},
          "the files that only the library reads");
    Check(!twosf::rip::DifferentBoot(full, full), "the full image against itself");
    Check(twosf::rip::DifferentBoot(full, later) == std::optional<std::size_t>(0),
          "later by the same clock on every boot");
    Check(twosf::rip::DifferentBoot(full, dropped) == std::optional<std::size_t>(0), "a file left out on every boot");
    Check(twosf::rip::DifferentBoot(full, fewer) == std::optional<std::size_t>(1), "fewer files with a backup memory");
    Check(twosf::rip::DifferentBoot(full, another_clock) == std::optional<std::size_t>(1),
          "another clock with a backup memory");
    Check(twosf::rip::DifferentBoot(full, missed) == std::optional<std::size_t>(2), "a boot that misses the stop");
    Check(twosf::rip::DifferentBoot(full, more) == std::optional<std::size_t>(1), "more files with a backup memory");
}

// The exact check. A library that reaches the main loop earlier on every boot, after the same files, fails on the first
// boot. A library that blanks a file that the game reads before its main loop can shorten the boot without changing
// the files that it reads.
void TestDifferentBoot()
{
    const std::vector<Boot> reference(4, Boot{true, 3770100, {171}});
    const std::vector<Boot> early(4, Boot{true, 3541261, {171}});
    std::vector<Boot> missed = reference;
    missed[3].reached = false;
    std::vector<Boot> more = reference;
    more[2].files.insert(172);

    Check(!twosf::rip::DifferentBoot(reference, reference), "the same boots");
    Check(twosf::rip::DifferentBoot(reference, early) == std::optional<std::size_t>(0), "early on every boot");
    Check(twosf::rip::DifferentBoot(reference, missed) == std::optional<std::size_t>(3), "a boot that misses the stop");
    Check(twosf::rip::DifferentBoot(reference, more) == std::optional<std::size_t>(2), "a file more on one boot");
}

// Ranges merge with the ranges that they overlap or touch, and stay in order.
void TestRanges()
{
    twosf::rip::FileRanges ranges;
    twosf::rip::AddRange(ranges, 0x400, 0x600);
    twosf::rip::AddRange(ranges, 0x100, 0x200);
    twosf::rip::AddRange(ranges, 0x800, 0x900);
    twosf::rip::AddRange(ranges, 0x300, 0x300);
    Check(ranges == twosf::rip::FileRanges{{0x100, 0x200}, {0x400, 0x600}, {0x800, 0x900}},
          "apart ranges in order, with no empty range");

    twosf::rip::AddRange(ranges, 0x200, 0x280);
    Check(ranges == twosf::rip::FileRanges{{0x100, 0x280}, {0x400, 0x600}, {0x800, 0x900}},
          "a range that touches the end of another");
    twosf::rip::AddRange(ranges, 0x500, 0x800);
    Check(ranges == twosf::rip::FileRanges{{0x100, 0x280}, {0x400, 0x900}}, "a range that joins two");
    twosf::rip::AddRange(ranges, 0, 0x1000);
    Check(ranges == twosf::rip::FileRanges{{0, 0x1000}}, "a range over all");
}

// The card pages that a file's bytes reach are the image's pages, cut to the file. The file starts 0x80 bytes into the
// image's page 2 and ends 0x40 bytes into page 7.
void TestCardPages()
{
    const ntdlib::ntr::RomFile file = {.id = 3, .start = 0x480, .end = 0xE40};
    twosf::rip::FileRanges ranges;

    twosf::rip::AddCardPages(ranges, file, 0x200, 0x210);
    Check(ranges == twosf::rip::FileRanges{{0x180, 0x380}}, "the image's page 3, not the file's page 1");
    twosf::rip::AddCardPages(ranges, file, 0x10, 0x20);
    Check(ranges == twosf::rip::FileRanges{{0, 0x380}}, "page 2 from the file's start, merged with page 3");
    twosf::rip::AddCardPages(ranges, file, 0x980, 0x9B0);
    Check(ranges == twosf::rip::FileRanges{{0, 0x380}, {0x980, 0x9C0}}, "page 7 up to the file's end");
}

// The merge keeps the first boot's state and the files of every boot.
void TestMergedBoot()
{
    twosf::rip::HookBoot without;
    without.reached = true;
    without.clock = 1000;
    without.seconds = 0.25;
    without.cpsr = 0x1F;
    without.files = {1, 2, 30, 31};
    without.reads[30] = {{0, 0x200}};
    twosf::rip::HookBoot with = without;
    with.clock = 2000;
    with.files = {1, 2, 38, 39};
    with.reads = {{30, {{0x200, 0x400}}}, {38, {{0x800, 0xA00}}}};

    const twosf::rip::HookBoot merged = twosf::rip::MergedBoot(std::vector{without, with});
    const twosf::rip::HookBoot none = twosf::rip::MergedBoot({});

    Check(merged.files == (std::set<uint32_t>{1, 2, 30, 31, 38, 39}), "the files of every boot");
    Check(merged.reads == (std::map<uint32_t, twosf::rip::FileRanges>{{30, {{0, 0x400}}}, {38, {{0x800, 0xA00}}}}),
          "the ranges that every boot reads, merged");
    Check(merged.reached && merged.clock == 1000 && merged.seconds == 0.25 && merged.cpsr == 0x1F,
          "the first boot's clock and mode");
    Check(!none.reached && none.files.empty(), "no boot");
}

// A library keeps the files that the game's boots or the full image's boots read, and the songs' files. Each alone
// misses a file of the others.
void TestLibraryFiles()
{
    twosf::rip::HookBoot game;
    game.files = {1, 2, 30};
    twosf::rip::HookBoot full;
    full.files = {1, 2, 31};

    Check(twosf::rip::LibraryFiles(game, full, {40, 41}) == (std::set<uint32_t>{1, 2, 30, 31, 40, 41}),
          "the files of either image's boots and the songs' files");
}

// The image that a ripper boots: the image itself without patches, and with a patch the ARM9 binary patched and every
// file the same. The patch's check reads the expected word.
void TestPatchedImage()
{
    const std::vector<uint8_t> image = twosf::testing::HaltingImage();
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);
    const uint32_t base = rom.Header().arm9.ram_address;
    const twosf::rip::GamePatch patch = {.address = base, .expected = 0xEF060000, .value = 0xE1A00000};
    const ntdlib::ntr::LoadedCode arm9 = ntdlib::ntr::Arm9Code(rom);

    Check(twosf::rip::PatchedImage(rom, {}) == image, "no patches: the image itself");
    Check(twosf::rip::PatchMatches(arm9, patch) &&
              !twosf::rip::PatchMatches(arm9, {.address = base, .expected = 0xE1A00000, .value = 0}) &&
              !twosf::rip::PatchMatches(arm9, {.address = base + 2, .expected = 0xEF060000, .value = 0}),
          "a patch matches its expected word, at a multiple of 4");

    const std::vector<uint8_t> patched_image = twosf::rip::PatchedImage(rom, std::span(&patch, 1));
    const ntdlib::ntr::Rom patched = ntdlib::ntr::Rom::Parse(patched_image);
    const ntdlib::ntr::LoadedCode patched_arm9 = ntdlib::ntr::Arm9Code(patched);
    Check(xsflib::LoadLe32(patched_arm9.bytes.data()) == 0xE1A00000 &&
              xsflib::LoadLe32(patched_arm9.bytes.data() + 4) == 0xEAFFFFFD,
          "the patched image's ARM9 binary contains the patch, and the rest as it was");
    const auto file = [](const ntdlib::ntr::Rom& of)
    {
        const auto found = std::ranges::find(of.Files(), "sound/sound_data.sdat", &ntdlib::ntr::RomFile::path);
        const std::span<const uint8_t> bytes = of.FileData(found->id);
        return std::vector<uint8_t>(bytes.begin(), bytes.end());
    };
    Check(file(patched) == file(rom), "the patched image keeps every file whole");
}

} // namespace

int main()
{
    TestBackups();
    TestNames();
    TestRunOnBoot();
    TestWatch();
    TestBootsError();
    TestDifferentBootAgainstFullImage();
    TestDifferentBoot();
    TestRanges();
    TestCardPages();
    TestMergedBoot();
    TestLibraryFiles();
    TestPatchedImage();

    return xsflib::testing::Summary("boots");
}
