// SPDX-License-Identifier: MIT

// Tests the sound archive of a set for a game on the SDK's sound archive player (rip/sdat_set.h): the files and reads
// of several boots, the archive's files that a boot reads and that a cut keeps, the cut's page layout and the set's
// archive, and the guard of the card reads.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/ntr/compact_image.h"
#include "ntdlib/ntr/machine.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sdat/layout.h"
#include "ntdlib/testing/nds_files.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "rip/sdat_set.h"
#include "sdat_test_code.h"

namespace
{

namespace sdat = ntdlib::sdat;

using twosf::testing::ArchiveModel;
using twosf::testing::PagesModel;
using xsflib::testing::Check;

// The merge keeps the first boot's state, and the files and the reads of every boot. The archive's files that the
// merged reads reach include those that only a later boot reads.
void TestMergedHook()
{
    const std::vector<uint8_t> bytes = sdat::BuildArchive(ArchiveModel());
    const sdat::Archive archive = sdat::Archive::Parse(bytes);
    constexpr uint32_t kStart = 0x10000;
    const uint32_t file_3 = kStart + archive.files[3].offset;
    twosf::rip::HookState without;
    without.reached = true;
    without.clock = 1000;
    without.seconds = 0.25;
    without.cpsr = 0x1F;
    without.files = {5, 6, 40};
    without.reads = {{0x200, 0x400}};
    without.heap = twosf::rip::SoundHeap{0x02100000, 0x8000};
    twosf::rip::HookState with = without;
    with.clock = 2000;
    with.seconds = 0.5;
    with.files = {29, 30, 40};
    with.reads = {{0x200, 0x400}, {file_3, file_3 + 4}};
    with.heap = twosf::rip::SoundHeap{0x02100000, 0x7000};
    twosf::rip::HookState same_files = without;
    same_files.reads = {{0x800, 0x900}};

    const twosf::rip::HookState merged = twosf::rip::MergedHook(std::vector{without, with, same_files});
    const twosf::rip::HookState alone = twosf::rip::MergedHook(std::vector{without});
    const twosf::rip::HookState none = twosf::rip::MergedHook({});

    Check(merged.files == (std::set<uint32_t>{5, 6, 29, 30, 40}), "the files of every boot");
    Check(merged.reads.size() == 4 && merged.reads[0] == without.reads[0] && merged.reads[2].first == file_3 &&
              merged.reads[3].first == 0x800,
          "the reads of every boot, in order");
    Check(merged.reached && merged.clock == 1000 && merged.seconds == 0.25 && merged.cpsr == 0x1F &&
              merged.heap == without.heap,
          "the first boot's clock, mode and heap");
    Check(twosf::rip::ArchiveFilesRead(merged, kStart, archive) == std::set<uint32_t>{3},
          "an archive's file that a later boot alone reads");
    Check(twosf::rip::ArchiveFilesRead(without, kStart, archive).empty(), "the first boot alone reads none");
    Check(alone.files == without.files && alone.reads == without.reads, "one boot");
    Check(!none.reached && none.files.empty(), "no boot");
}

void TestArchiveFiles()
{
    const std::vector<uint8_t> bytes = sdat::BuildArchive(ArchiveModel());
    const sdat::Archive archive = sdat::Archive::Parse(bytes);
    constexpr uint32_t kStart = 0x10000;

    twosf::rip::HookState hook;
    hook.reads = {{0, 0x200}, {kStart + archive.files[3].offset + 8, kStart + archive.files[3].offset + 16}};
    Check(twosf::rip::ArchiveFilesRead(hook, kStart, archive) == std::set<uint32_t>{3}, "a read inside a file");
    hook.reads.emplace_back(kStart + archive.files[0].offset - 4, kStart + archive.files[0].offset + 1);
    Check(twosf::rip::ArchiveFilesRead(hook, kStart, archive) == (std::set<uint32_t>{0, 3}),
          "a read that reaches a file's first byte");
}

// The cut keeps what the sequence plays (the sequence's file, its bank's and its wave archive's) and the boot's files.
// A reserved file keeps its size and contains zeros, unless the boot reads it. The archive keeps its layout on 4
// bytes.
void TestCut()
{
    const std::vector<uint8_t> bytes = sdat::BuildArchive(ArchiveModel());
    const sdat::Archive archive = sdat::Archive::Parse(bytes);
    const std::vector<ntdlib::ntr::SequenceRef> sequences = {{0}};
    sdat::ArchiveModel without_file = ArchiveModel();
    without_file.files[3].clear();
    sdat::ArchiveModel zeroed_file = ArchiveModel();
    zeroed_file.files[3].assign(64, 0);

    const std::optional<std::vector<uint8_t>> plain = twosf::rip::CutArchive(archive, sequences, {});
    const std::optional<std::vector<uint8_t>> with_boot = twosf::rip::CutArchive(archive, sequences, {3});
    const std::optional<std::vector<uint8_t>> reserved = twosf::rip::CutArchive(archive, sequences, {}, {3});
    const std::optional<std::vector<uint8_t>> reserved_boot = twosf::rip::CutArchive(archive, sequences, {3}, {3});

    if (!Check(plain && with_boot && reserved && reserved_boot, "the archive cuts"))
    {
        return;
    }

    Check(*plain == sdat::BuildArchive(without_file), "the cut keeps the sequence's files");
    Check(*with_boot == bytes, "the cut keeps the boot's file too");
    Check(*reserved == sdat::BuildArchive(zeroed_file), "a reserved file keeps its size and contains zeros");
    Check(*reserved_boot == bytes, "a reserved file that the boot reads keeps its data");
}

// The page layout: a kept file keeps its offset in its card page, behind a dropped file of 0x1E0 bytes. Two kept files
// of one page keep their distance. Two kept files of different pages don't share a page. The tables' pages stay.
void TestPageOffsets()
{
    const std::vector<uint8_t> bytes = sdat::BuildArchive(PagesModel());
    const sdat::Archive archive = sdat::Archive::Parse(bytes);
    const std::vector<uint32_t> offsets = twosf::rip::PageOffsets(archive, {0, 1, 2, 4, 5, 7});
    const auto page = [](uint32_t offset)
    {
        return offset / twosf::rip::kCardPage;
    };

    Check(offsets.size() == archive.files.size(), "an offset for each file");
    Check(offsets[0] == archive.files[0].offset && offsets[2] == archive.files[2].offset, "the first page stays");
    Check(offsets[4] % twosf::rip::kCardPage == archive.files[4].offset % twosf::rip::kCardPage &&
              offsets[4] != archive.files[3].offset,
          "a kept file behind a dropped one keeps its place in its page");
    Check(offsets[5] - offsets[4] == 0x40, "two kept files of one page keep their distance");
    Check(page(offsets[7]) > page(offsets[5] + 0x40 - 1) &&
              offsets[7] % twosf::rip::kCardPage == archive.files[7].offset % twosf::rip::kCardPage,
          "a kept file of a later page gets a page of its own");
    Check(offsets[7] < archive.files[7].offset, "the dropped pages go");
}

// The cut lays its kept files out by pages (rip::PageOffsets) and reads back. The tools' layout packs file 4 0x1E0
// bytes earlier, in the place of file 3.
void TestCutPages()
{
    const std::vector<uint8_t> bytes = sdat::BuildArchive(PagesModel());
    const sdat::Archive archive = sdat::Archive::Parse(bytes);
    const std::vector<ntdlib::ntr::SequenceRef> sequences = {{0}};
    const std::optional<std::vector<uint8_t>> cut = twosf::rip::CutArchive(archive, sequences, {4, 5, 7});
    if (!Check(cut.has_value(), "the archive cuts"))
    {
        return;
    }

    const sdat::Archive laid_out = sdat::Archive::Parse(*cut);
    const std::vector<uint32_t> offsets = twosf::rip::PageOffsets(archive, {0, 1, 2, 4, 5, 7});
    const sdat::ArchiveModel model = PagesModel();
    for (const uint32_t id : {0U, 1U, 2U, 4U, 5U, 7U})
    {
        Check(laid_out.files[id].offset == offsets[id] && std::ranges::equal(laid_out.File(id), model.files[id]),
              "kept file " + std::to_string(id) + " at its page offset, with its bytes");
    }
    Check(laid_out.File(3).empty() && laid_out.File(6).empty(), "the dropped files are empty");
    Check(laid_out.files[4].offset % twosf::rip::kCardPage == archive.files[4].offset % twosf::rip::kCardPage,
          "file 4 keeps its place in its page");
}

// The set's archive: the cut and its reserved files, or, where the cut fails, the whole archive with every stream file
// in it and no reserved file. An archive whose files aren't where the SDK's tools put them doesn't cut.
void TestSetArchive()
{
    const std::vector<ntdlib::ntr::SequenceRef> sequences = {{0}};
    const std::vector<uint8_t> tools = sdat::BuildArchive(ArchiveModel());
    const twosf::rip::SetArchive cut = twosf::rip::MakeSetArchive(sdat::Archive::Parse(tools), sequences, {}, {3});

    sdat::ArchiveModel gap_model = ArchiveModel();
    const std::vector<uint8_t> laid_out = sdat::BuildArchive(gap_model);
    std::vector<uint32_t> offsets;
    for (const sdat::FileEntry& file : sdat::Archive::Parse(laid_out).files)
    {
        offsets.push_back(file.offset);
    }
    offsets[3] += 0x40;
    const std::vector<uint8_t> gap = sdat::BuildArchive(gap_model, offsets);
    const twosf::rip::SetArchive whole = twosf::rip::MakeSetArchive(sdat::Archive::Parse(gap), sequences, {}, {3});

    Check(!cut.whole && cut.reserved == std::set<uint32_t>{3}, "a cut keeps its reserved files");
    Check(whole.whole && whole.bytes == gap && whole.reserved.empty(),
          "an archive that doesn't cut stays whole and reserves nothing");
}

// After the hook, the guard of the card reads passes a read of a file that the set keeps and fails one of a file that
// it leaves out: a reserved file contains zeros in the image.
void TestGuard()
{
    ntdlib::testing::nds_files::Image spec;
    spec.files = {{"sound_data.sdat", sdat::BuildArchive(ArchiveModel())}};
    const auto [image, ids] = ntdlib::testing::nds_files::BuildImage(spec);
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);
    const sdat::Archive archive = sdat::Archive::Parse(rom.FileData(ids[0]));
    const uint32_t start = rom.Files().at(ids[0]).start;
    ntdlib::ntr::Machine machine(rom);
    std::vector<std::string> errors;
    const auto fail = [&errors](std::string error)
    {
        errors.push_back(std::move(error));
    };
    twosf::rip::GuardCardReads(machine, rom, ids[0], archive, {3}, 0, fail);

    machine.on_card_read(start + archive.files[0].offset, 4);
    const std::size_t after_kept = errors.size();
    machine.on_card_read(start + archive.files[3].offset, 4);

    Check(after_kept == 0, "a read of a file that the set keeps passes");
    Check(errors.size() == 1, "a read of a file that the set leaves out fails");
}

// In an image that runs only on a DSi, the guard counts whole sectors of the digest. The cut-down image keeps a, e and
// the sound archive, and leaves out c (ntdlib's WriteCompactImage). With sectors of 0x400 bytes from a's start, c
// shares its first sector with a and its last with e, and has one sector of its own. c's bytes in the shared sectors
// stay, and the middle sector contains zeros. Reads of a's sector, the padding after a in it, and c's bytes in both
// shared sectors pass. A read of the middle sector fails.
void TestGuardInDsiImage()
{
    ntdlib::testing::nds_files::Image spec;
    spec.unit_code = 3;
    spec.files = {{"a.bin", std::vector<uint8_t>(0x10, 2)},
                  {"c.bin", std::vector<uint8_t>(0x800, 4)},
                  {"e.bin", std::vector<uint8_t>(0x10, 5)},
                  {"sound_data.sdat", sdat::BuildArchive(ArchiveModel())}};
    auto [whole, ids] = ntdlib::testing::nds_files::BuildImage(spec);
    const uint32_t a = ids[0];
    const uint32_t c = ids[1];
    const uint32_t e = ids[2];
    const uint32_t archive_file = ids[3];
    const uint32_t a_start = ntdlib::ntr::Rom::Parse(whole).Files().at(a).start;
    xsflib::StoreLe32(&whole[0x1E0], a_start);
    xsflib::StoreLe32(&whole[0x200], 0x400);
    const ntdlib::ntr::Rom whole_rom = ntdlib::ntr::Rom::Parse(whole);
    ntdlib::ntr::CompactParts parts;
    parts.arm9.assign(whole_rom.Arm9().begin(), whole_rom.Arm9().end());
    for (const uint32_t kept : {a, e, archive_file})
    {
        parts.files[kept].assign(whole_rom.FileData(kept).begin(), whole_rom.FileData(kept).end());
    }
    const std::vector<uint8_t> image = ntdlib::ntr::WriteCompactImage(whole_rom, parts);
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);
    const ntdlib::ntr::RomFile& file_a = rom.Files().at(a);
    const ntdlib::ntr::RomFile& file_c = rom.Files().at(c);
    const ntdlib::ntr::RomFile& file_e = rom.Files().at(e);
    Check(file_c.start - a_start == 0x200 && file_c.end - a_start == 0xA00 && file_e.start - a_start == 0xA00,
          "the layout: c from 0x200 to 0xA00 after a's start, and e after c");

    const sdat::Archive archive = sdat::Archive::Parse(rom.FileData(archive_file));
    ntdlib::ntr::Machine machine(rom);
    std::vector<std::string> errors;
    twosf::rip::GuardCardReads(machine, rom, archive_file, archive, {3}, 0,
                               [&errors](std::string error) { errors.push_back(std::move(error)); });
    machine.on_card_read(a_start, 0x400);
    machine.on_card_read(file_a.end, 4);
    machine.on_card_read(file_c.start, 4);
    machine.on_card_read(file_c.end - 4, 4);
    Check(errors.empty(), "reads of a's sector, the padding after a, and c's bytes in the shared sectors pass");
    machine.on_card_read(a_start + 0x400, 4);
    Check(errors.size() == 1, "a read of c's middle sector fails");
}

} // namespace

int main()
{
    TestMergedHook();
    TestArchiveFiles();
    TestCut();
    TestPageOffsets();
    TestCutPages();
    TestSetArchive();
    TestGuard();
    TestGuardInDsiImage();

    return xsflib::testing::Summary("sdat_archive");
}
