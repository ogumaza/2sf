// SPDX-License-Identifier: MIT

#include "rip/sdat_set.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/arm/assembler.h"
#include "ntdlib/ntr/compact_image.h"
#include "ntdlib/ntr/machine.h"
#include "ntdlib/ntr/rip.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/ntr/sound_routines.h"
#include "ntdlib/sdat/layout.h"
#include "xsflib/bytes.h"
#include "xsflib/text.h"

namespace twosf::rip
{

namespace
{

namespace arm = ntdlib::arm;
namespace ntr = ntdlib::ntr;
namespace sdat = ntdlib::sdat;
using ntr::snd::CommandId;

constexpr uint32_t kMainMemory = 0x02000000;

// The boot's limit, and the step of a mini's run between its checks.
constexpr uint64_t kBootClock = 30ull * ntr::kSystemClockHz;
constexpr uint64_t kRunStep = ntr::kSystemClockHz / 60;

// A frame heap's signature, and the fields of the sound heap around it.
constexpr uint32_t kFrameHeap = 0x46524D48; // "FRMH"
constexpr uint32_t kHeapToSignature = 0x10;
constexpr uint32_t kHeapStartField = 0x18;
constexpr uint32_t kHeapEndField = 0x1C;
constexpr uint32_t kHeapHeadField = 0x24;
constexpr uint32_t kHeapTailField = 0x28;

// The IPC's tag of the sound commands. A FIFO word with it gives a list's address in its bits 6 to 31.
constexpr uint32_t kSoundTag = 7;
constexpr uint32_t kTagBits = 0x1F;
constexpr int kListShift = 6;

// A command in main memory: the link to the next, the id, then four arguments.
constexpr uint32_t kCommandId = 4;
constexpr uint32_t kCommandArgs = 8;

// A sequence file's offset of its data, and a bank's links to its wave archives: for each of its four slots, the wave
// archive's address, then a link to the next bank that uses it.
constexpr std::size_t kSequenceDataField = 0x18;
constexpr std::size_t kBankLinksStart = 0x18;
constexpr std::size_t kBankLinksEnd = 0x38;
constexpr uint32_t kBankLinkBytes = 8;

// The CPSR's mode bits, its I bit, and the modes in which a stub may run: user and system.
constexpr uint32_t kModeBits = 0x1F;
constexpr uint32_t kIrqDisabled = 0x80;
constexpr uint32_t kUserMode = 0x10;
constexpr uint32_t kSystemMode = 0x1F;

// The load's arguments without a loader: every kind of data (0xFF), the heap, 1 and a null pointer.
constexpr uint32_t kEveryKind = 0xFF;

// b <target> at `address`, in ARM code.
uint32_t Branch(uint32_t address, uint32_t target)
{
    constexpr uint32_t kB = 0xEA000000;
    constexpr uint32_t kOffsetBits = 0x00FFFFFF;
    return kB | (((target - (address + 8)) >> 2) & kOffsetBits);
}

uint32_t Word(std::span<const uint8_t> memory, uint32_t address)
{
    return xsflib::LoadLe32(&memory[(address - kMainMemory) & static_cast<uint32_t>(memory.size() - 1)]);
}

bool InMemory(std::span<const uint8_t> memory, uint32_t address)
{
    return address >= kMainMemory && address - kMainMemory < memory.size();
}

// Whether main memory holds `file` at `address`. A bank's links are left out.
bool Holds(std::span<const uint8_t> memory, uint32_t address, std::span<const uint8_t> file, bool bank)
{
    if (!InMemory(memory, address) || memory.size() - (address - kMainMemory) < file.size())
    {
        return false;
    }

    const std::span<const uint8_t> held = memory.subspan(address - kMainMemory, file.size());
    for (std::size_t i = 0; i < file.size(); i++)
    {
        if (held[i] != file[i] && !(bank && i >= kBankLinksStart && i < kBankLinksEnd))
        {
            return false;
        }
    }

    return true;
}

// A call of the routine at `address`, in ARM or in Thumb code.
void Call(arm::ArmAssembler& a, uint32_t address)
{
    constexpr int kScratch = 12;
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

} // namespace

bool Relocatable(uint32_t word)
{
    constexpr uint32_t kPushMask = 0xFFFF0000;
    constexpr uint32_t kPush = 0xE92D0000; // stmfd sp!, {...}
    constexpr uint32_t kPc = 15;
    if ((word & kPushMask) == kPush && (word & (1u << kPc)) == 0)
    {
        return true;
    }

    constexpr uint32_t kPushOneMask = 0xFFFF0FFF;
    constexpr uint32_t kPushOne = 0xE52D0004; // str rd, [sp, #-4]!
    if ((word & kPushOneMask) == kPushOne && ((word >> 12) & 0xF) != kPc)
    {
        return true;
    }

    // Data processing, always: bits 26 and 27 clear, not a multiply, a swap or a halfword's transfer.
    constexpr uint32_t kAlways = 0xE0000000;
    const bool always = (word & 0xF0000000) == kAlways;
    const bool data = (word & 0x0C000000) == 0;
    const bool immediate = (word & 0x02000000) != 0;
    const bool extra = !immediate && (word & 0x90) == 0x90;
    const bool bx = (word & 0x0FFFFFF0) == 0x012FFF10;
    const uint32_t rn = (word >> 16) & 0xF;
    const uint32_t rd = (word >> 12) & 0xF;
    const uint32_t rm = word & 0xF;
    const uint32_t rs = (word >> 8) & 0xF;
    const bool shift_by_register = !immediate && (word & 0x10) != 0;
    return always && data && !extra && !bx && rn != kPc && rd != kPc &&
           (immediate || (rm != kPc && (!shift_by_register || rs != kPc)));
}

std::optional<SdatCode> FindSdatCode(const ntr::Rom& rom, std::string& error)
{
    const ntr::LoadedCode arm9 = ntr::Arm9Code(rom);
    const ntr::SoundRoutines routines = ntr::FindSoundRoutines(arm9, rom.Header().arm9.entry);
    const auto count = [](std::size_t n, const char* what) -> std::optional<std::string>
    {
        if (n == 1)
        {
            return std::nullopt;
        }

        return n == 0 ? std::string("the search of the game's code doesn't find ") + what
                      : "the search of the game's code finds " + std::to_string(n) + " candidates for " + what;
    };
    for (const std::optional<std::string>& problem :
         {count(routines.updates.size(), "the sound library's update (NNS_SndMain)"),
          count(routines.starts.size(), "the start of a sequence (NNS_SndArcPlayerStartSeq)"),
          count(routines.loads.size(), "the load of a sequence (NNS_SndArcLoadSeq)"),
          count(routines.waits.size(), "the wait for the vertical blank (OS_WaitVBlankIntr)")})
    {
        if (problem)
        {
            error = *problem;
            return std::nullopt;
        }
    }

    if (!routines.decompressor)
    {
        error = "the search of the game's code finds no start-up decompressor, where the stub would go";
        return std::nullopt;
    }

    SdatCode code;
    code.update = routines.updates[0].address;
    code.start = routines.starts[0].address;
    code.loader = routines.loads[0].wrapper;
    code.load = routines.loads[0].load;
    code.wait = routines.waits[0];
    code.stub = routines.decompressor->body;
    code.stub_end = routines.decompressor->exit;

    // The update's first instruction moves into the stub. It must lie in the binary's static part.
    const std::optional<ntr::ModuleParams> params = ntr::ReadModuleParams(arm9.bytes);
    const uint32_t static_end = params ? params->autoload_start : arm9.address + uint32_t(arm9.bytes.size());
    if (code.update < arm9.address || code.update + 4 > static_end || code.stub_end > static_end)
    {
        error = "the update or the decompressor lies outside the ARM9 binary's static part";
        return std::nullopt;
    }

    code.update_first = xsflib::LoadLe32(&arm9.bytes[code.update - arm9.address]);
    if (!Relocatable(code.update_first))
    {
        error =
            "the update's first instruction (" + xsflib::Hex(code.update_first, 8, true) + ") can't move into the stub";
        return std::nullopt;
    }

    const std::size_t stub_bytes = BuildSdatStub(code, 0, 0).bytes.size();
    if (stub_bytes > code.stub_end - code.stub)
    {
        error = "the stub takes " + std::to_string(stub_bytes) + " bytes, and the decompressor's body has " +
                std::to_string(code.stub_end - code.stub);
        return std::nullopt;
    }

    return code;
}

std::optional<SoundArchive> FindSoundArchive(const ntr::Rom& rom, const std::set<uint32_t>& files)
{
    for (const uint32_t id : files)
    {
        if (id >= rom.Files().size())
        {
            continue;
        }

        const std::span<const uint8_t> data = rom.FileData(id);
        if (data.size() < 4 || std::string_view(reinterpret_cast<const char*>(data.data()), 4) != "SDAT")
        {
            continue;
        }

        try
        {
            sdat::Archive archive = sdat::Archive::Parse(data);
            if (std::ranges::any_of(archive.sequences, [](const auto& sequence) { return sequence.has_value(); }))
            {
                return SoundArchive{id, std::move(archive)};
            }
        }
        catch (const std::exception&)
        {
            continue;
        }
    }

    return std::nullopt;
}

std::optional<SoundHeap> FindSoundHeap(std::span<const uint8_t> memory)
{
    // Each frame heap's address and its start, head, tail and end.
    struct FrameHeap
    {
        uint32_t heap = 0;
        uint32_t start = 0;
        uint32_t head = 0;
        uint32_t tail = 0;
        uint32_t end = 0;
    };
    const uint64_t memory_end = kMainMemory + uint64_t{memory.size()};
    std::vector<FrameHeap> heaps;
    for (uint32_t at = kHeapToSignature; at + kHeapTailField + 4 <= memory.size(); at += 4)
    {
        if (xsflib::LoadLe32(&memory[at]) != kFrameHeap ||
            xsflib::LoadLe32(&memory[at - kHeapToSignature]) != kMainMemory + at)
        {
            continue;
        }

        const FrameHeap heap{.heap = kMainMemory + at - kHeapToSignature,
                             .start = xsflib::LoadLe32(&memory[at + kHeapStartField]),
                             .head = xsflib::LoadLe32(&memory[at + kHeapHeadField]),
                             .tail = xsflib::LoadLe32(&memory[at + kHeapTailField]),
                             .end = xsflib::LoadLe32(&memory[at + kHeapEndField])};
        if (heap.start >= kMainMemory && heap.start <= heap.head && heap.head <= heap.tail && heap.tail <= heap.end &&
            heap.end <= memory_end)
        {
            heaps.push_back(heap);
        }
    }

    for (const FrameHeap& heap : heaps)
    {
        const auto inside = [&heap](const FrameHeap& other)
        {
            return other.heap == heap.heap || (other.heap >= heap.start && other.heap < heap.end);
        };
        if (std::ranges::all_of(heaps, inside))
        {
            return SoundHeap{.address = heap.heap, .free = heap.tail - heap.head};
        }
    }

    return std::nullopt;
}

std::set<uint32_t> MusicSequences(const sdat::Archive& archive)
{
    std::set<uint32_t> all;
    std::set<uint32_t> music;
    for (uint32_t index = 0; index < archive.sequences.size(); index++)
    {
        if (archive.sequences[index])
        {
            all.insert(index);
            if (archive.has_symbols && ntr::rip::IsMusic(archive, ntr::SequenceRef{.index = index}))
            {
                music.insert(index);
            }
        }
    }

    return music.empty() ? all : music;
}

HookState BootToHook(const ntr::Rom& rom, uint32_t update)
{
    HookState hook;
    const auto machine = std::make_unique<ntr::Machine>(rom);
    machine->on_card_read = [&](uint32_t address, uint32_t bytes)
    {
        hook.reads.emplace_back(address, address + bytes);
        for (const uint32_t id : rom.FilesIn(address, address + bytes))
        {
            hook.files.insert(id);
        }
    };
    machine->Arm9().SetStopAddress(update);
    std::vector<ntr::SoundOutput> out;
    hook.reached = machine->Run(kBootClock, out);
    hook.clock = machine->Clock();
    hook.seconds = static_cast<double>(machine->Clock()) / ntr::kSystemClockHz;
    hook.cpsr = machine->Arm9().Cpsr();
    hook.heap = FindSoundHeap(machine->MainMemory());

    return hook;
}

std::optional<std::string> HookProblem(const HookState& hook)
{
    if (!hook.reached)
    {
        return "the game doesn't call the sound library's update within 30 s of its boot";
    }

    const uint32_t mode = hook.cpsr & kModeBits;
    if (mode != kSystemMode && mode != kUserMode)
    {
        return "the game calls the sound library's update in an interrupt's mode (CPSR " +
               xsflib::Hex(hook.cpsr, 8, true) + ")";
    }

    if ((hook.cpsr & kIrqDisabled) != 0)
    {
        return "the game calls the sound library's update with interrupts off";
    }

    if (!hook.heap)
    {
        return "no sound heap is in memory at the first call of the sound library's update";
    }

    return std::nullopt;
}

std::set<uint32_t> ArchiveFilesRead(const HookState& hook, uint32_t start, const sdat::Archive& archive)
{
    std::set<uint32_t> read;
    for (uint32_t id = 0; id < archive.files.size(); id++)
    {
        const uint64_t file_start = uint64_t{start} + archive.files[id].offset;
        const uint64_t file_end = file_start + archive.files[id].size;
        const auto overlaps = [&](const std::pair<uint32_t, uint32_t>& range)
        {
            return range.first < file_end && file_start < range.second;
        };
        if (archive.files[id].size > 0 && std::ranges::any_of(hook.reads, overlaps))
        {
            read.insert(id);
        }
    }

    return read;
}

std::optional<std::vector<uint8_t>> CutArchive(const sdat::Archive& archive,
                                               std::span<const ntr::SequenceRef> sequences,
                                               const std::set<uint32_t>& extra)
{
    const std::optional<std::vector<uint8_t>> cut = ntr::rip::KeepSequenceData(archive, sequences);
    if (!cut || extra.empty())
    {
        return cut;
    }

    // The files that the cut keeps, and the extra ones, in the archive's layout.
    const sdat::Archive kept = sdat::Archive::Parse(*cut);
    sdat::ArchiveModel model = sdat::ReadArchiveModel(archive.bytes);
    for (uint32_t id = 0; id < model.files.size(); id++)
    {
        if (kept.File(id).empty() && !extra.contains(id))
        {
            model.files[id].clear();
        }
    }

    return sdat::BuildArchive(model);
}

SdatStub BuildSdatStub(const SdatCode& code, uint32_t heap, uint32_t sound)
{
    constexpr int kData = 4; // the data's address, kept across the calls
    constexpr uint32_t kSoundField = 0;
    constexpr uint32_t kHeapField = 4;
    constexpr uint32_t kHandleField = 8;

    arm::ArmAssembler a(code.stub);
    const int data = a.NewLabel();
    const int frame = a.NewLabel();
    const int update = a.NewLabel();

    a.LoadAddress(kData, data);

    // The load: the loader's (sequence, heap), or (sequence, 0xFF, heap, 1, null) with the null on the stack.
    if (code.loader)
    {
        a.Ldr(0, kData, kSoundField);
        a.Ldr(1, kData, kHeapField);
        Call(a, *code.loader);
    }
    else
    {
        a.Sub(arm::kSp, arm::kSp, 8);
        a.Mov(0, 0);
        a.Str(0, arm::kSp);
        a.Ldr(0, kData, kSoundField);
        a.Mov(1, kEveryKind);
        a.Ldr(2, kData, kHeapField);
        a.Mov(3, 1);
        Call(a, code.load);
    }

    // NNS_SndArcPlayerStartSeq(handle, sequence).
    a.Add(0, kData, kHandleField);
    a.Ldr(1, kData, kSoundField);
    Call(a, code.start);

    // Each frame: the wait, then the update.
    a.Bind(frame);
    Call(a, code.wait);
    a.Bl(update);
    a.B(frame);

    // The update: its first instruction, then a jump to its second.
    a.Bind(update);
    a.Word(code.update_first);
    a.Ldr(arm::kPc, arm::kPc, -4);
    a.Word(code.update + 4);

    a.Pool();
    a.Bind(data);
    a.Word(sound);
    a.Word(heap);
    a.Word(0); // the handle

    SdatStub stub;
    stub.sound_offset = a.Address(data) + kSoundField - code.stub;
    stub.bytes = a.Finish();

    return stub;
}

SdatSet MakeSdatSet(const ntr::Rom& rom, const SdatCode& code, const SdatStub& stub,
                    const std::map<uint32_t, std::vector<uint8_t>>& files)
{
    if (stub.bytes.size() > code.stub_end - code.stub)
    {
        throw std::runtime_error("the stub takes " + std::to_string(stub.bytes.size()) +
                                 " bytes, and the decompressor's body has " +
                                 std::to_string(code.stub_end - code.stub));
    }

    ntr::CompactParts parts{.arm9 = ntr::UncompressedArm9(rom), .files = files};
    const uint32_t base = rom.Header().arm9.ram_address;
    std::ranges::copy(stub.bytes, parts.arm9.begin() + (code.stub - base));
    xsflib::StoreLe32(&parts.arm9[code.update - base], Branch(code.update, code.stub));

    // The SDK's overlay loader decompresses an overlay with the start-up decompressor, whose body contains the stub.
    // The kept overlays go in decompressed.
    std::vector<std::size_t> decompressed;
    const std::vector<ntr::Overlay>& overlays = rom.Arm9Overlays();
    for (std::size_t i = 0; i < overlays.size(); i++)
    {
        const auto kept = parts.files.find(overlays[i].file_id);
        if (ntr::OverlayCompressed(overlays[i]) && kept != parts.files.end())
        {
            kept->second = ntr::OverlayBytes(rom.FileData(overlays[i].file_id), overlays[i]);
            decompressed.push_back(i);
        }
    }

    SdatSet set;
    set.image = ntr::WriteCompactImage(rom, parts);
    for (const std::size_t i : decompressed)
    {
        ntr::ClearArm9OverlayFlags(set.image, i);
    }
    set.stub = rom.Header().arm9.rom_offset + (code.stub - base);
    set.sound_word = set.stub + stub.sound_offset;

    return set;
}

namespace
{

// The ranges of `image`'s bytes that a card read after the stub starts may reach: every file with bytes other than
// the sound archive's, the archive's header and tables, and each of the archive's files with bytes.
std::vector<std::pair<uint32_t, uint32_t>> KeptRanges(const ntr::Rom& rom, uint32_t archive_file,
                                                      const sdat::Archive& archive)
{
    std::vector<std::pair<uint32_t, uint32_t>> kept;
    for (const ntr::RomFile& file : rom.Files())
    {
        if (file.id != archive_file && file.end > file.start)
        {
            kept.emplace_back(file.start, file.end);
        }
    }

    const uint32_t start = rom.Files().at(archive_file).start;
    uint32_t first_file = static_cast<uint32_t>(archive.bytes.size());
    for (const sdat::FileEntry& entry : archive.files)
    {
        if (entry.size > 0)
        {
            kept.emplace_back(start + entry.offset, start + entry.offset + entry.size);
            first_file = std::min(first_file, entry.offset);
        }
    }
    kept.emplace_back(start, start + first_file);

    return kept;
}

// Whether a list of the driver's commands that follows the start's changes what the sequence plays, and how.
std::optional<std::string> Interference(CommandId id)
{
    switch (id)
    {
    case CommandId::kStartSequence:
    case CommandId::kPrepareSequence:
    case CommandId::kStartPreparedSequence:
        return "starts or prepares a sequence";
    case CommandId::kStopSequence:
        return "stops a sequence";
    case CommandId::kPlayerLocalVariable:
    case CommandId::kGlobalVariable:
        return "sets a variable";
    case CommandId::kPlayerParameter:
    case CommandId::kTrackParameter:
    case CommandId::kMuteTrack:
        return "sets a player's or a track's volume, mute or parameter";
    case CommandId::kMasterVolume:
    case CommandId::kChannelVolume:
        return "sets a volume";
    default:
        return std::nullopt;
    }
}

} // namespace

MiniRun RunMini(std::span<const uint8_t> image, uint64_t hook_clock, uint32_t archive_file,
                const ntr::SequenceRef& sequence, double seconds)
{
    MiniRun run;
    const ntr::Rom rom = ntr::Rom::Parse(image);
    const sdat::Archive archive = sdat::Archive::Parse(rom.FileData(archive_file));
    const auto machine = std::make_unique<ntr::Machine>(rom);
    const std::span<uint8_t> memory = machine->MainMemory();
    const auto fail = [&run](std::string error)
    {
        if (!run.error)
        {
            run.error = std::move(error);
        }
    };

    // The expected files of the sequence: its file and its bank's.
    const sdat::SequenceInfo& info = *archive.sequences.at(sequence.index);
    const sdat::BankInfo& bank = *archive.banks.at(info.bank);
    const std::span<const uint8_t> sequence_file = archive.File(info.file_id);
    const std::span<const uint8_t> bank_file = archive.File(bank.file_id);

    // After the hook, each card read must reach a range that the set keeps.
    const std::vector<std::pair<uint32_t, uint32_t>> kept = KeptRanges(rom, archive_file, archive);
    machine->on_card_read = [&](uint32_t address, uint32_t bytes)
    {
        const auto reaches = [&](const std::pair<uint32_t, uint32_t>& range)
        {
            return range.first < uint64_t{address} + bytes && address < range.second;
        };
        if (machine->Clock() >= hook_clock && !std::ranges::any_of(kept, reaches))
        {
            fail("the game reads " + xsflib::Hex(address, 8, true) + ", outside the files that the set keeps");
        }
    };

    bool prepared = false;
    bool started = false;
    uint32_t bank_address = 0;
    machine->on_fifo_word = [&](bool to_arm7, uint32_t word)
    {
        if (!to_arm7 || (word & kTagBits) != kSoundTag)
        {
            return;
        }

        const bool after_start = started;
        for (uint32_t command = word >> kListShift; InMemory(memory, command); command = Word(memory, command))
        {
            const auto id = static_cast<CommandId>(Word(memory, command + kCommandId));
            const auto arg = [&](int i)
            {
                return Word(memory, command + kCommandArgs + 4 * uint32_t(i));
            };
            if (after_start)
            {
                if (const std::optional<std::string> change = Interference(id))
                {
                    fail("a list of the driver's commands after the start " + *change);
                }
                continue;
            }

            if (id == CommandId::kGlobalVariable && arg(0) < run.variables.global.size())
            {
                run.variables.global[arg(0)] = static_cast<int16_t>(arg(1));
            }
            else if ((id == CommandId::kPrepareSequence || id == CommandId::kStartSequence) && !prepared)
            {
                prepared = true;
                bank_address = arg(3);
                const uint32_t data_offset = xsflib::LoadLe32(&sequence_file[kSequenceDataField]);
                if (!Holds(memory, arg(1) - data_offset, sequence_file, false))
                {
                    fail("the driver's prepare command names " + xsflib::Hex(arg(1), 8, true) +
                         ", not the sequence's bytes");
                }
                else if (!Holds(memory, bank_address, bank_file, true))
                {
                    fail("the driver's prepare command names " + xsflib::Hex(bank_address, 8, true) +
                         ", not the bank's bytes");
                }
            }

            const bool sequence_starts = id == CommandId::kStartSequence || id == CommandId::kStartPreparedSequence;
            if (prepared && sequence_starts && !started)
            {
                started = true;
                run.start_ms = static_cast<double>(machine->Clock()) * 1000.0 / ntr::kSystemClockHz;
            }
        }
    };

    std::vector<ntr::SoundOutput> out;
    const auto limit = static_cast<uint64_t>(seconds * ntr::kSystemClockHz);
    uint64_t end = limit;
    while (!run.error && machine->Clock() < end)
    {
        machine->Run(std::min(end, machine->Clock() + kRunStep), out);
        out.clear();
        if (started && end == limit)
        {
            end = machine->Clock() + static_cast<uint64_t>(kMiniPlaySeconds * ntr::kSystemClockHz);
        }
    }

    if (!started)
    {
        fail("the sequence doesn't start within " + std::to_string(static_cast<int>(seconds)) + " s of the boot");
    }

    // Each slot of the bank that names a wave archive links to one in main memory.
    for (uint32_t slot = 0; slot < bank.wave_archives.size() && prepared; slot++)
    {
        const uint32_t link = bank_address + static_cast<uint32_t>(kBankLinksStart) + kBankLinkBytes * slot;
        if (bank.wave_archives[slot] != sdat::kNoWaveArchive && InMemory(memory, link) &&
            !InMemory(memory, Word(memory, link)))
        {
            fail("the bank's slot " + std::to_string(slot) + " has no wave archive in memory");
        }
    }

    return run;
}

} // namespace twosf::rip
