// SPDX-License-Identifier: MIT

// Tests the boots of a set for a game on the SDK's sound archive player (rip/sdat_set.h) on hand-built images: the
// boots with and without a backup memory, the library check against the full image, a boot's state at the hook against
// the game's heap, and the runs of a mini.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/arm/assembler.h"
#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/ntr/machine.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sdat/layout.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "backup_image.h"
#include "rip/boots.h"
#include "rip/sdat_set.h"
#include "sdat_test_code.h"

namespace
{

namespace arm = ntdlib::arm;
namespace sdat = ntdlib::sdat;

using twosf::rip::SdatCode;
using twosf::testing::ArchiveModel;
using twosf::testing::kPush;
using twosf::testing::PagesModel;
using xsflib::testing::Check;

// The boots of `image` to the hook at `update`: with no backup memory, then with the tests' backup memory.
std::vector<twosf::rip::HookState> BootsToHook(const std::vector<uint8_t>& image, uint32_t update)
{
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);
    std::vector<twosf::rip::HookState> boots;
    for (const ntdlib::ntr::BackupMemory& backup : {ntdlib::ntr::BackupMemory{}, twosf::testing::TestBackup()})
    {
        boots.push_back(twosf::rip::BootToHook(rom, update, backup));
    }

    return boots;
}

// An image whose boot calls the update at `update` only with a backup memory.
std::vector<uint8_t> UpdateWithBackupImage(uint32_t& update)
{
    arm::ArmAssembler a(twosf::testing::kArm9Base);
    const int halt = a.NewLabel();
    const int update_label = a.NewLabel();
    twosf::testing::EmitBackupCheck(a, halt);
    a.Bl(update_label);
    twosf::testing::EmitHaltForever(a, halt);
    a.Bind(update_label);
    a.Bx(14);
    update = a.Address(update_label);

    return twosf::testing::ImageWithArm9(a.Finish());
}

// BootToHook boots with the backup memory that it's given: the image reaches the update only with one.
void TestBootToHookWithBackup()
{
    uint32_t update = 0;
    const std::vector<uint8_t> image = UpdateWithBackupImage(update);
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);

    const twosf::rip::HookState with = twosf::rip::BootToHook(rom, update, twosf::testing::TestBackup());
    const twosf::rip::HookState without = twosf::rip::BootToHook(rom, update, {});

    Check(with.reached, "the boot with a backup memory reaches the hook");
    Check(!without.reached, "the boot without one doesn't");
}

// Emits a wait of `loops` loops of the BIOS's WaitByLoop, whose time ntdlib's DS passes. Nothing for 0. Uses r0.
void EmitWait(arm::ArmAssembler& a, uint32_t loops)
{
    constexpr uint32_t kWaitByLoop = 0x03;

    if (loops == 0)
    {
        return;
    }

    a.LoadConstant(0, loops);
    a.Swi(kWaitByLoop);
}

// An image whose boot waits `before` loops, then a further `with_backup` loops with a backup memory alone, and then
// calls the update, whose address goes to `update`. A test's full image and library differ only in their delays. Each
// boot of the library therefore reaches the hook at another clock.
std::vector<uint8_t> DelayedUpdateImage(uint32_t before, uint32_t with_backup, uint32_t& update)
{
    arm::ArmAssembler a(twosf::testing::kArm9Base);
    const int no_backup = a.NewLabel();
    const int halt = a.NewLabel();
    const int update_label = a.NewLabel();
    EmitWait(a, before);
    twosf::testing::EmitBackupCheck(a, no_backup);
    EmitWait(a, with_backup);
    a.Bind(no_backup);
    a.Bl(update_label);
    twosf::testing::EmitHaltForever(a, halt);
    a.Bind(update_label);
    a.Bx(14);
    update = a.Address(update_label);

    return twosf::testing::ImageWithArm9(a.Finish());
}

// The clock of the library check over the boots (rip::UnlikeHookBoot) on hand-built images. A library whose boots all
// reach the hook later than the full image's by the same clock fails on the first boot. One whose boot with a backup
// memory alone comes later fails on that boot.
void TestUnlikeHookBootWithBackup()
{
    constexpr uint32_t kFullBefore = 10000;
    constexpr uint32_t kFullWithBackup = 20000;
    constexpr uint32_t kLater = 10000;

    uint32_t update = 0;
    const auto boots = [&update](uint32_t before, uint32_t with_backup)
    {
        const std::vector<uint8_t> image = DelayedUpdateImage(before, with_backup, update);
        return BootsToHook(image, update);
    };

    const std::vector<twosf::rip::HookState> full = boots(kFullBefore, kFullWithBackup);
    const std::vector<twosf::rip::HookState> later = boots(kFullBefore + kLater, kFullWithBackup);
    const std::vector<twosf::rip::HookState> unlike = boots(kFullBefore, kFullWithBackup + kLater);

    Check(full[0].reached && full[1].reached && full[1].clock > full[0].clock,
          "the full image's boot with a backup memory reaches the hook later");
    Check(later[0].clock > full[0].clock && twosf::rip::UnlikeHookBoot(full, later) == std::optional<std::size_t>(0),
          "a library later by the same clock on every boot fails on the first boot");
    Check(unlike[0].clock == full[0].clock && unlike[1].clock > full[1].clock,
          "the library's boot with a backup memory alone comes later");
    Check(twosf::rip::UnlikeHookBoot(full, unlike) == std::optional<std::size_t>(1),
          "the check refuses the library on the boot with a backup memory");
    Check(twosf::rip::UnlikeHookBoot(full, full) == std::nullopt, "the full image passes against itself");
}

// The SDAT ripper's check over the boots (rip::UnlikeHookBoot): rip::DifferentBoot, then the same count of card reads
// and the same sound heap on every boot.
void TestUnlikeHookBoot()
{
    const auto boot = [](uint64_t clock, std::set<uint32_t> files, uint32_t heap, std::size_t reads = 3)
    {
        twosf::rip::HookState state;
        state.reached = true;
        state.clock = clock;
        state.files = std::move(files);
        state.reads.assign(reads, {0x8000, 0x8200});
        state.heap = twosf::rip::SoundHeap{.address = heap, .free = 0x1000};
        return state;
    };

    const std::vector<twosf::rip::HookState> full{boot(100, {1}, 0x02100000), boot(150, {1, 2}, 0x02100000)};
    const std::vector<twosf::rip::HookState> alike{boot(110, {1}, 0x02100000), boot(160, {1, 2}, 0x02100000)};
    const std::vector<twosf::rip::HookState> fewer{boot(100, {1}, 0x02100000), boot(150, {1}, 0x02100000)};
    const std::vector<twosf::rip::HookState> heap{boot(100, {1}, 0x02100000), boot(150, {1, 2}, 0x02100040)};
    const std::vector<twosf::rip::HookState> read_more{boot(100, {1}, 0x02100000), boot(150, {1, 2}, 0x02100000, 4)};
    std::vector<twosf::rip::HookState> no_heap = full;
    no_heap[0].heap.reset();

    Check(!twosf::rip::UnlikeHookBoot(full, full), "the full image against itself");
    Check(twosf::rip::UnlikeHookBoot(full, alike) == std::optional<std::size_t>(0),
          "later by the same clock on every boot, with the same heap");
    Check(twosf::rip::UnlikeHookBoot(full, fewer) == std::optional<std::size_t>(1), "fewer files with a backup memory");
    Check(twosf::rip::UnlikeHookBoot(full, heap) == std::optional<std::size_t>(1),
          "the heap elsewhere with a backup memory alone");
    Check(twosf::rip::UnlikeHookBoot(full, read_more) == std::optional<std::size_t>(1),
          "a card read more at the same clock with a backup memory");
    Check(twosf::rip::UnlikeHookBoot(full, no_heap) == std::optional<std::size_t>(0), "no heap on the first boot");
}

// The library keeps what the game's boots or the full image's boots read, of the image and of the sound archive. Each
// side alone misses a file of the other.
void TestLibraryRead()
{
    const twosf::rip::BootFiles game{.files = {5, 6}, .archive_files = {3}};
    const twosf::rip::BootFiles full{.files = {6, 9}, .archive_files = {3, 4}};

    const twosf::rip::BootFiles read = twosf::rip::LibraryRead(game, full);

    Check(read.files == (std::set<uint32_t>{5, 6, 9}), "the files of the image that either reads");
    Check(read.archive_files == (std::set<uint32_t>{3, 4}), "the files of the sound archive that either reads");
    Check(twosf::rip::LibraryRead(game, game) == game, "the game's files alone");
}

// A boot must let the stub run with the sound heap at the address of the game's boot with no backup memory
// (rip::HookProblem with a heap). A first boot with no heap gives HookProblem's message. A boot with a heap fails
// against a first boot with none.
void TestHookProblemWithHeap()
{
    twosf::rip::HookState hook;
    hook.reached = true;
    hook.cpsr = 0x1F;
    hook.heap = twosf::rip::SoundHeap{.address = 0x02100000, .free = 0x1000};
    twosf::rip::HookState missed = hook;
    missed.reached = false;
    twosf::rip::HookState no_heap = hook;
    no_heap.heap.reset();
    const twosf::rip::SoundHeap elsewhere{.address = 0x02100040, .free = 0x1000};

    Check(!twosf::rip::HookProblem(hook, hook.heap), "the heap at the game's address");
    Check(twosf::rip::HookProblem(hook, elsewhere) == "the sound heap is elsewhere at the hook",
          "the heap at another address");
    Check(twosf::rip::HookProblem(missed, hook.heap) == twosf::rip::HookProblem(missed), "a boot without a hook");
    Check(twosf::rip::HookProblem(no_heap, hook.heap) == twosf::rip::HookProblem(no_heap), "no heap");
    Check(twosf::rip::HookProblem(no_heap, no_heap.heap) ==
              "no sound heap is in memory at the first call of the sound library's update",
          "a first boot with no heap");

    // `no_heap.heap` was reset from the game's heap. A call that read its address would find the game's address.
    Check(twosf::rip::HookProblem(hook, no_heap.heap) == "the sound heap is elsewhere at the hook",
          "a heap at the hook and none on the first boot");
}

// Emits a read of 0x200 bytes of the card, with the plain read command, from the card's address in r1 into main memory
// at the address in r2. It polls ROMCTRL for each word, and stores the address's bytes at `scratch` on the way. Uses
// r0 to r3 and r12.
void EmitCardRead(arm::ArmAssembler& a, uint32_t scratch)
{
    constexpr uint32_t kIo = 0x04000000;
    constexpr int kSpiControlHigh = 0x1A1; // AUXSPICNT's high byte
    constexpr int kRomControl = 0x1A4;
    constexpr int kCommand = 0x1A8; // the command's eight bytes, the first at the lowest address
    constexpr uint32_t kCardData = 0x04100010;
    constexpr uint32_t kSlotInRomMode = 0x80;
    constexpr uint32_t kReadCommand = 0xB7;
    constexpr uint32_t kStartBlock = 0xA1000000; // start, with a block of 0x200 bytes
    constexpr uint32_t kWordReady = 0x800000;
    constexpr uint32_t kBusy = 0x80000000;

    // The command: B7, then the address with its most significant byte first, then zeros.
    a.LoadConstant(3, kIo);
    a.Mov(0, kSlotInRomMode);
    a.Strb(0, 3, kSpiControlHigh);
    a.Mov(0, kReadCommand);
    a.Strb(0, 3, kCommand);
    a.LoadConstant(12, scratch);
    a.Str(1, 12);
    for (int i = 0; i < 4; i++)
    {
        a.Ldrb(0, 12, 3 - i);
        a.Strb(0, 3, kCommand + 1 + i);
    }

    a.Mov(0, 0);
    for (int i = 5; i < 8; i++)
    {
        a.Strb(0, 3, kCommand + i);
    }

    // The block, a word at a time.
    const int poll = a.NewLabel();
    const int not_ready = a.NewLabel();
    a.LoadConstant(0, kStartBlock);
    a.Str(0, 3, kRomControl);
    a.LoadConstant(12, kCardData);
    a.Bind(poll);
    a.Ldr(0, 3, kRomControl);
    a.Tst(0, kWordReady);
    a.B(not_ready, arm::Condition::kEq);
    a.Ldr(0, 12);
    a.StrPost(0, 2, 4);
    a.Bind(not_ready);
    a.Ldr(0, 3, kRomControl);
    a.Tst(0, kBusy);
    a.B(poll, arm::Condition::kNe);
}

// The memory of the images whose boot reads the card: the header's FAT offset, in its copy in main memory, where the
// boot reads the FAT, a file's block and the card address of a read. The ARM9 binary takes 0x8000 bytes: a card read
// below 0x8000 doesn't read the image's bytes there. The decompressor's body, where the stub goes, takes 0x100 bytes.
constexpr uint32_t kFatOffset = 0x027FFE48;
constexpr uint32_t kFat = 0x02100000;
constexpr uint32_t kBlock = 0x02100200;
constexpr uint32_t kScratch = 0x02100400;
constexpr int kFatEntryBytes = 8;
constexpr uint32_t kArm9Bytes = 0x8000;
constexpr uint32_t kStubBytes = 0x100;

// Emits a read of the FAT's first block to kFat. Uses r0 to r3 and r12.
void EmitFatRead(arm::ArmAssembler& a)
{
    a.LoadConstant(1, kFatOffset);
    a.Ldr(1, 1);
    a.LoadConstant(2, kFat);
    EmitCardRead(a, kScratch);
}

// Ends the code of an image whose boot reads the card: at `call_update`, the call of an update that pushes r4 and lr
// and pops them, then a halt for good. The decompressor's body follows at `stub`, and zeros up to kArm9Bytes. `code`
// gets the routines for MakeSdatSet: the update, and the decompressor's body for the stub. The others are never
// called.
void EndReadsImage(arm::ArmAssembler& a, int call_update, int stub, SdatCode& code)
{
    const int halt = a.NewLabel();
    const int update = a.NewLabel();
    a.Bind(call_update);
    a.Bl(update);
    twosf::testing::EmitHaltForever(a, halt);
    a.Bind(update);
    a.Push(1 << 4 | 1 << 14);
    a.Pop(1 << 4 | 1 << 15);
    a.Pool();
    a.Bind(stub);
    twosf::testing::PadTo(a, twosf::testing::kArm9Base + kArm9Bytes);

    code.update = a.Address(update);
    code.update_first = kPush;
    code.start = code.update;
    code.load = code.update;
    code.wait = code.update;
    code.stub = a.Address(stub);
    code.stub_end = code.stub + kStubBytes;
}

// The paths of the files of FileReadsImage.
constexpr const char* kEarlyFile = "data/early.bin";
constexpr const char* kLateFile = "data/late.bin";

// An image with kEarlyFile and kLateFile, whose boot finds files through the FAT and reads their first block, then
// calls the update. The ARM9 binary takes 0x8000 bytes. The FAT and the files are then where a card read reaches them.
// The boot reads kEarlyFile. Where the decompressor's body doesn't start with 0, as in a set's image, it also reads
// kLateFile first, unless the FAT gives the file no bytes. `early` and `late` are the files' IDs. `code` gets the
// routines for MakeSdatSet: the update, and the decompressor's body for the stub. The others are never called.
std::vector<uint8_t> FileReadsImage(uint32_t early, uint32_t late, SdatCode& code)
{
    arm::ArmAssembler a(twosf::testing::kArm9Base);
    const int call_update = a.NewLabel();
    const int stub = a.NewLabel();

    // The FAT's first block, then the early file's.
    EmitFatRead(a);
    a.LoadConstant(2, kFat);
    a.Ldr(1, 2, static_cast<int>(early) * kFatEntryBytes);
    a.LoadConstant(2, kBlock);
    EmitCardRead(a, kScratch);

    // The late file's, in a set's image, when it has bytes.
    a.LoadAddress(0, stub);
    a.Ldr(0, 0);
    a.Cmp(0, 0);
    a.B(call_update, arm::Condition::kEq);
    a.LoadConstant(2, kFat);
    a.Ldr(1, 2, static_cast<int>(late) * kFatEntryBytes);
    a.Ldr(0, 2, static_cast<int>(late) * kFatEntryBytes + 4);
    a.CmpRegister(0, 1);
    a.B(call_update, arm::Condition::kEq);
    a.LoadConstant(2, kBlock);
    EmitCardRead(a, kScratch);

    EndReadsImage(a, call_update, stub, code);

    const std::vector<uint8_t> block(0x200, 0x5A);
    return twosf::testing::ImageWithArm9(a.Finish(), {{kEarlyFile, block}, {kLateFile, block}});
}

// The library check against the full image (rip::UnlikeHookBoot) on hand-built images. The game's boot reads one file
// before the hook. A set's boot also reads a second file before it. A library with the files of the game's boots leaves
// the second file out, and its boot reads no card for it. The full image's boot reads the file, and the check refuses
// that library on the first boot. The library with the files of every boot of the game and of the full image passes.
void TestLibraryReadsDroppedFile()
{
    // The files' IDs, from an image with the same files.
    const std::vector<uint8_t> files_image = twosf::testing::ImageWithArm9(
        twosf::testing::Halts(), {{kEarlyFile, std::vector<uint8_t>(4)}, {kLateFile, std::vector<uint8_t>(4)}});
    const ntdlib::ntr::Rom files_rom = ntdlib::ntr::Rom::Parse(files_image);
    const uint32_t early = files_rom.Find(kEarlyFile)->id;
    const uint32_t late = files_rom.Find(kLateFile)->id;
    SdatCode code;
    const std::vector<uint8_t> image = FileReadsImage(early, late, code);
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);
    const twosf::rip::SdatStub stub = twosf::rip::BuildSdatStub(code, 0, 0);

    const auto boots = [&code](const std::vector<uint8_t>& boot_image)
    {
        return BootsToHook(boot_image, code.update);
    };
    const auto set_image = [&](const std::set<uint32_t>& ids)
    {
        return twosf::rip::MakeSdatSet(rom, code, stub, twosf::rip::WholeFiles(rom, ids)).image;
    };

    const std::vector<twosf::rip::HookState> game = boots(image);
    const std::vector<twosf::rip::HookState> full = boots(set_image(twosf::rip::EveryFile(rom)));
    const std::set<uint32_t> game_files = twosf::rip::MergedHook(game).files;
    const twosf::rip::BootFiles full_read{.files = twosf::rip::MergedHook(full).files};
    const std::set<uint32_t> every_read = twosf::rip::LibraryRead({.files = game_files}, full_read).files;

    const std::vector<twosf::rip::HookState> dropped = boots(set_image(game_files));
    const std::vector<twosf::rip::HookState> kept = boots(set_image(every_read));

    Check(game[0].reached && game_files == std::set<uint32_t>{early}, "the game's boot reads one file before the hook");
    Check(full[0].reached && full[0].files == (std::set<uint32_t>{early, late}), "the full image's boot reads both");
    Check(dropped[0].reached && dropped[0].files == std::set<uint32_t>{early},
          "the library with the game's files reads no card for the other file");
    Check(twosf::rip::UnlikeHookBoot(full, dropped) == std::optional<std::size_t>(0),
          "the check refuses the library that leaves out a file that its boot reads");
    Check(!twosf::rip::UnlikeHookBoot(full, kept), "the check passes the library with the full image's files");
}

// The path of the archive of PageReadModel().
constexpr const char* kPagedArchive = "sound/paged.sdat";

// The archive of the paged read: PagesModel()'s first three files, which sequence 0 plays, then a file that the cut
// drops, then the boot's file (4) of 0x40 bytes. File 4 starts 0x1E0 bytes into a card page. The tools' cut puts it in
// the dropped file's place, inside one page.
sdat::ArchiveModel PageReadModel()
{
    sdat::ArchiveModel model = PagesModel();
    model.files.resize(5);
    model.files[3].clear();
    const uint32_t dropped = sdat::Archive::Parse(sdat::BuildArchive(model)).files[3].offset;
    model.files[3].assign((0x1E0 + 2 * twosf::rip::kCardPage - dropped % twosf::rip::kCardPage) % twosf::rip::kCardPage,
                          4);
    if (model.files[3].empty())
    {
        model.files[3].assign(twosf::rip::kCardPage, 4);
    }

    return model;
}

// An image with kPagedArchive, whose boot finds the archive through the image's FAT, reads the archive's first page,
// finds file 4 through the archive's FAT entry at `entry` from the archive's start, and reads each card page of the
// file, then calls the update. `archive_id` is the archive's ID in the image. `code` gets the routines for MakeSdatSet,
// as FileReadsImage gives them.
std::vector<uint8_t> PagedReadImage(uint32_t archive_id, uint32_t entry, const std::vector<uint8_t>& archive,
                                    SdatCode& code)
{
    constexpr uint32_t kData = 0x02100600;
    constexpr uint32_t kPageMask = ~(twosf::rip::kCardPage - 1);
    constexpr int kArchive = 4;
    constexpr int kFileStart = 7;
    constexpr int kFirstPage = 8;
    constexpr int kLastPage = 9;

    arm::ArmAssembler a(twosf::testing::kArm9Base);
    const int call_update = a.NewLabel();
    const int stub = a.NewLabel();

    // The image's FAT, the archive's start, and the archive's first page.
    EmitFatRead(a);
    a.LoadConstant(2, kFat);
    a.Ldr(kArchive, 2, static_cast<int>(archive_id) * kFatEntryBytes);
    a.MovRegister(1, kArchive);
    a.LoadConstant(2, kBlock);
    EmitCardRead(a, kScratch);

    // File 4's card address (the archive's start plus the file's offset) and the pages of its first and last bytes.
    a.LoadConstant(2, kBlock);
    a.Ldr(5, 2, static_cast<int>(entry));
    a.Ldr(6, 2, static_cast<int>(entry) + 4);
    a.Mov(0, 0);
    a.SubRegister(0, 0, 5);
    a.SubRegister(kFileStart, kArchive, 0);
    a.LoadConstant(3, kPageMask);
    a.AndRegister(kFirstPage, kFileStart, 3);
    a.Mov(0, 0);
    a.SubRegister(0, 0, 6);
    a.SubRegister(kLastPage, kFileStart, 0);
    a.Sub(kLastPage, kLastPage, 1);
    a.AndRegister(kLastPage, kLastPage, 3);

    // Each page of the file.
    a.MovRegister(1, kFirstPage);
    a.LoadConstant(2, kData);
    EmitCardRead(a, kScratch);
    a.CmpRegister(kLastPage, kFirstPage);
    a.B(call_update, arm::Condition::kEq);
    a.Add(1, kFirstPage, twosf::rip::kCardPage);
    a.LoadConstant(2, kData);
    EmitCardRead(a, kScratch);

    EndReadsImage(a, call_update, stub, code);

    return twosf::testing::ImageWithArm9(a.Finish(), {{kPagedArchive, archive}});
}

// The library check with the page layout, on a hand-built image whose boot reads a file of the sound archive page by
// page. The file starts 0x1E0 bytes into a page and takes two pages. The cut keeps it at its place in its page, and
// the library's boot makes the full image's card reads at the full image's clock on every boot. The tools' cut would
// put the file in one page, and the library's boot would make one read fewer.
void TestLibraryReadsPages()
{
    const std::vector<uint8_t> archive_bytes = sdat::BuildArchive(PageReadModel());
    const sdat::Archive archive = sdat::Archive::Parse(archive_bytes);
    const uint32_t entry = xsflib::LoadLe32(&archive_bytes[0x20]) + 12 + 16 * 4;
    const std::vector<uint8_t> ids_image =
        twosf::testing::ImageWithArm9(twosf::testing::Halts(), {{kPagedArchive, archive_bytes}});
    const uint32_t archive_id = ntdlib::ntr::Rom::Parse(ids_image).Find(kPagedArchive)->id;
    if (!Check(archive.files[4].offset % twosf::rip::kCardPage == 0x1E0 && entry + 8 <= twosf::rip::kCardPage &&
                   archive.files[3].offset % twosf::rip::kCardPage + 0x40 <= twosf::rip::kCardPage,
               "the archive's file 4 starts 0x1E0 bytes into a page, and its FAT entry is in the first page"))
    {
        return;
    }

    SdatCode code;
    const std::vector<uint8_t> image = PagedReadImage(archive_id, entry, archive_bytes, code);
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);
    const twosf::rip::SdatStub stub = twosf::rip::BuildSdatStub(code, 0, 0);
    const auto boots = [&code](const std::vector<uint8_t>& boot_image)
    {
        return BootsToHook(boot_image, code.update);
    };

    const std::vector<ntdlib::ntr::SequenceRef> sequences = {{0}};
    const std::optional<std::vector<uint8_t>> cut = twosf::rip::CutArchive(archive, sequences, {4});
    if (!Check(cut.has_value(), "the archive cuts"))
    {
        return;
    }
    std::map<uint32_t, std::vector<uint8_t>> library_files = twosf::rip::WholeFiles(rom, {archive_id});
    library_files[archive_id] = *cut;
    const std::vector<twosf::rip::HookState> full =
        boots(twosf::rip::MakeSdatSet(rom, code, stub, twosf::rip::WholeFiles(rom, twosf::rip::EveryFile(rom))).image);
    const std::vector<twosf::rip::HookState> library =
        boots(twosf::rip::MakeSdatSet(rom, code, stub, library_files).image);

    Check(full[0].reached && full[0].files.contains(archive_id), "the full image's boot reads the archive");
    Check(!twosf::rip::UnlikeHookBoot(full, library), "the check passes the library");
    for (std::size_t i = 0; i < full.size(); i++)
    {
        Check(
            library[i].reached && library[i].clock == full[i].clock && library[i].reads.size() == full[i].reads.size(),
            "boot " + std::to_string(i) + " of the library has the full image's clock and card reads");
    }
}

// An image with the archive of ArchiveModel() as a file, whose boot sends the driver a start of sequence 0 only with a
// backup memory. The start names the sequence's bytes and the bank's. The ARM9 binary contains both, and the bank's
// link to its wave archive points into main memory. `arm7` is the ARM7 binary. With `read_header`, the boot first reads
// the card's first block: the header, outside every file.
std::vector<uint8_t> StartWithBackupImage(std::vector<uint8_t> arm7 = twosf::testing::Halts(), bool read_header = false)
{
    constexpr uint32_t kData = twosf::testing::kArm9Base + 0x400;
    constexpr uint32_t kList = kData;
    constexpr uint32_t kSequence = kData + 0x40;
    constexpr uint32_t kBank = kData + 0x80;
    constexpr uint32_t kHeaderCopy = 0x02100000;
    const sdat::ArchiveModel model = ArchiveModel();

    arm::ArmAssembler a(twosf::testing::kArm9Base);
    const int halt = a.NewLabel();

    if (read_header)
    {
        a.Mov(1, 0);
        a.LoadConstant(2, kHeaderCopy);
        EmitCardRead(a, kScratch);
    }

    twosf::testing::EmitBackupCheck(a, halt);
    twosf::testing::EmitSendLists(a, {kList});
    twosf::testing::EmitHaltForever(a, halt);
    twosf::testing::EndCodeAt(a, kList);

    // The list: the start, with its four arguments: the sequence's data and the bank.
    const uint32_t data_offset = xsflib::LoadLe32(&model.files[0][0x18]);
    twosf::testing::EmitList(
        a, kList,
        {{static_cast<uint32_t>(ntdlib::ntr::snd::CommandId::kStartSequence), 0u, kSequence + data_offset, 0u, kBank}});

    twosf::testing::PadTo(a, kSequence);
    for (std::size_t i = 0; i < model.files[0].size(); i += 4)
    {
        a.Word(xsflib::LoadLe32(&model.files[0][i]));
    }

    std::vector<uint8_t> bank = model.files[1];
    xsflib::StoreLe32(&bank[0x18], twosf::testing::kArm9Base);
    for (std::size_t i = 0; i < bank.size(); i += 4)
    {
        a.Word(xsflib::LoadLe32(&bank[i]));
    }

    return twosf::testing::ImageWithArm9(a.Finish(), {{"sound/sound_data.sdat", sdat::BuildArchive(model)}},
                                         std::move(arm7));
}

// RunMini boots with the backup memory that it's given: the sequence starts only with one.
void TestRunMiniWithBackup()
{
    const std::vector<uint8_t> image = StartWithBackupImage();
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);
    const ntdlib::ntr::RomFile* archive = rom.Find("sound/sound_data.sdat");
    if (!Check(archive != nullptr, "the archive's file"))
    {
        return;
    }

    const twosf::rip::MiniRun with =
        twosf::rip::RunMini(image, 0, archive->id, {0}, {}, 1.0, twosf::testing::TestBackup());
    const twosf::rip::MiniRun without = twosf::rip::RunMini(image, 0, archive->id, {0}, {}, 1.0, {});

    Check(!with.error, "the run with a backup memory starts the sequence: " + with.error.value_or(""));
    Check(without.error == "the sequence doesn't start within 1 s of the boot", "the run without one doesn't");
    Check(!with.not_started && without.not_started, "only the run without one fails for the start alone");
}

// A run that fails before its sequence would start doesn't fail for the start alone (MiniRun::not_started): its boot
// reads the card's header, outside the set's files, and then never starts the sequence.
void TestRunMiniFailsBeforeStart()
{
    const std::vector<uint8_t> image = StartWithBackupImage(twosf::testing::Halts(), true);
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);
    const uint32_t archive = rom.Find("sound/sound_data.sdat")->id;

    const twosf::rip::MiniRun run = twosf::rip::RunMini(image, 0, archive, {0}, {}, 1.0, {});

    Check(run.error.value_or("").starts_with("the game reads "), "the read fails: " + run.error.value_or(""));
    Check(!run.not_started, "the run doesn't fail for the start alone");
}

// RunMini follows the sound unit's output from the sequence's start (MiniRun::sounded). An ARM7 that plays a tone makes
// it change. An ARM7 that halts leaves it at its first value.
void TestRunMiniSounded()
{
    const std::vector<uint8_t> tone = StartWithBackupImage(twosf::testing::ToneArm7());
    const std::vector<uint8_t> quiet = StartWithBackupImage();
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(tone);
    const uint32_t archive = rom.Find("sound/sound_data.sdat")->id;

    const twosf::rip::MiniRun sounds =
        twosf::rip::RunMini(tone, 0, archive, {0}, {}, 1.0, twosf::testing::TestBackup());
    const twosf::rip::MiniRun silent =
        twosf::rip::RunMini(quiet, 0, archive, {0}, {}, 1.0, twosf::testing::TestBackup());

    Check(!sounds.error && sounds.sounded, "the run with a tone sounds: " + sounds.error.value_or(""));
    Check(!silent.error && !silent.sounded, "the run with a halted ARM7 stays silent: " + silent.error.value_or(""));
}

} // namespace

int main()
{
    TestBootToHookWithBackup();
    TestUnlikeHookBootWithBackup();
    TestUnlikeHookBoot();
    TestLibraryRead();
    TestHookProblemWithHeap();
    TestLibraryReadsDroppedFile();
    TestLibraryReadsPages();
    TestRunMiniWithBackup();
    TestRunMiniFailsBeforeStart();
    TestRunMiniSounded();

    return xsflib::testing::Summary("sdat_boots");
}
