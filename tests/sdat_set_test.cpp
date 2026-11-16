// SPDX-License-Identifier: MIT

// Tests the parts of a set for a game on the SDK's sound archive player (rip/sdat_set.h): the stub on ntdlib's ARM
// interpreter (the load, with and without the loader, the start, and each frame's wait and update), the instructions
// that the hook may move, the search of main memory for the sound heap, the rule for music, and the archive's files
// that a boot reads and that a cut keeps.

#include "rip/sdat_set.h"

#include <cstdint>
#include <cstring>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/arm/cpu.h"
#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sdat/layout.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

namespace
{

namespace arm = ntdlib::arm;
namespace sdat = ntdlib::sdat;

using twosf::rip::SdatCode;
using xsflib::testing::Check;

constexpr uint32_t kBase = 0x02000000;
constexpr uint32_t kSize = 0x10000;
constexpr uint32_t kStack = kBase + 0xF000;

// bx lr, mov r0, #1, stmfd sp!, {r4, lr} and ldmfd sp!, {r4, pc}.
constexpr uint32_t kReturn = 0xE12FFF1E;
constexpr uint32_t kOne = 0xE3A00001;
constexpr uint32_t kPush = 0xE92D4010;
constexpr uint32_t kPop = 0xE8BD8010;

// 64 KiB of memory at kBase.
class Memory : public arm::Bus
{
public:
    Memory() : bytes_(kSize, 0)
    {
    }

    uint8_t* Page(uint32_t page) override
    {
        return page >= kBase && page - kBase < kSize ? bytes_.data() + (page - kBase) : nullptr;
    }

    uint32_t Read(uint32_t, int) override
    {
        return 0;
    }

    void Write(uint32_t, int, uint32_t) override
    {
    }

    uint8_t* At(uint32_t address)
    {
        return bytes_.data() + (address - kBase);
    }

private:
    std::vector<uint8_t> bytes_;
};

// Routines in the test's memory. Each returns 1 at once. The update pushes r4 and lr, then pops them into r4 and pc.
SdatCode TestCode(bool loader)
{
    SdatCode code;
    code.update = kBase + 0x100;
    code.update_first = kPush;
    code.start = kBase + 0x120;
    if (loader)
    {
        code.loader = kBase + 0x130;
    }
    code.load = kBase + 0x140;
    code.wait = kBase + 0x150;
    code.stub = kBase + 0x958;
    code.stub_end = kBase + 0x9F0;

    return code;
}

// What the stub passes to the routines that it calls, in their order.
struct Calls
{
    std::vector<std::string> order;
    std::vector<uint32_t> load_args;
    uint32_t load_stacked = 0xFFFFFFFF; // the fifth argument of the load without a loader
    uint32_t start_handle = 0;
    uint32_t handle_word = 0xFFFFFFFF; // the handle's word as the start sees it
    uint32_t start_sound = 0;
    uint32_t r4_after_update = 0;
};

// Runs the stub of `code`, entered from the update as the hook does, up to its second wait.
Calls RunStub(const SdatCode& code, uint32_t heap, uint32_t sound)
{
    Memory memory;
    for (const uint32_t routine : {code.start, code.load, code.wait})
    {
        xsflib::StoreLe32(memory.At(routine), kOne);
        xsflib::StoreLe32(memory.At(routine + 4), kReturn);
    }
    if (code.loader)
    {
        xsflib::StoreLe32(memory.At(*code.loader), kOne);
        xsflib::StoreLe32(memory.At(*code.loader + 4), kReturn);
    }
    xsflib::StoreLe32(memory.At(code.update), 0xEA000000); // the hook's branch, never run here
    xsflib::StoreLe32(memory.At(code.update + 4), kPop);

    const twosf::rip::SdatStub stub = twosf::rip::BuildSdatStub(code, heap, sound);
    Check(stub.bytes.size() <= code.stub_end - code.stub, "the stub fits in the decompressor's body");
    std::memcpy(memory.At(code.stub), stub.bytes.data(), stub.bytes.size());
    Check(xsflib::LoadLe32(memory.At(code.stub + stub.sound_offset)) == sound, "the stub's word holds the sound");

    arm::Cpu cpu(memory, arm::Architecture::kArmV5TE);
    cpu.Jump(code.stub, arm::Mode::kSystem);
    cpu.SetReg(13, kStack);
    Calls calls;
    const auto run_to = [&](uint32_t address, const char* name)
    {
        cpu.SetStopAddress(address);
        const bool reached = cpu.Run(100000) == arm::StopReason::kStopAddress;
        calls.order.push_back(reached ? name : "?");
        return reached;
    };

    if (code.loader)
    {
        run_to(*code.loader, "loader");
        calls.load_args = {cpu.Reg(0), cpu.Reg(1)};
    }
    else
    {
        run_to(code.load, "load");
        calls.load_args = {cpu.Reg(0), cpu.Reg(1), cpu.Reg(2), cpu.Reg(3)};
        calls.load_stacked = xsflib::LoadLe32(memory.At(cpu.Reg(13)));
    }

    run_to(code.start, "start");
    calls.start_handle = cpu.Reg(0);
    calls.handle_word = xsflib::LoadLe32(memory.At(cpu.Reg(0)));
    calls.start_sound = cpu.Reg(1);

    run_to(code.wait, "wait");
    run_to(code.update + 4, "update");
    calls.r4_after_update = xsflib::LoadLe32(memory.At(cpu.Reg(13)));
    run_to(code.wait, "wait");

    return calls;
}

std::string Order(const Calls& calls)
{
    std::string text;
    for (const std::string& call : calls.order)
    {
        text += (text.empty() ? "" : " ") + call;
    }

    return text;
}

void TestStub()
{
    constexpr uint32_t kHeap = 0x0230A800;

    const SdatCode with_loader = TestCode(true);
    const Calls loaded = RunStub(with_loader, kHeap, 42);
    Check(Order(loaded) == "loader start wait update wait", "the loader, the start, then each frame's wait and update");
    Check(loaded.load_args == std::vector<uint32_t>{42, kHeap}, "the loader takes the sequence and the heap");
    Check(loaded.start_sound == 42 && loaded.handle_word == 0, "the start takes the sequence and an empty handle");
    Check(loaded.start_handle >= with_loader.stub && loaded.start_handle < with_loader.stub_end,
          "the handle lies in the stub");
    Check(loaded.r4_after_update == loaded.start_handle - 8,
          "the update's first instruction pushes the stub's registers");

    const Calls plain = RunStub(TestCode(false), kHeap, 7);
    Check(Order(plain) == "load start wait update wait", "without a loader, the load");
    Check(plain.load_args == std::vector<uint32_t>{7, 0xFF, kHeap, 1} && plain.load_stacked == 0,
          "the load takes the sequence, every kind, the heap, 1 and a null pointer on the stack");
}

// The update's first instructions that the hook moves into the stub, and some that it can't move.
void TestRelocatable()
{
    using twosf::rip::Relocatable;
    Check(Relocatable(0xE92D4010), "stmfd sp!, {r4, lr}");
    Check(Relocatable(0xE52DE004), "str lr, [sp, #-4]!");
    Check(Relocatable(0xE24DD008), "sub sp, sp, #8");
    Check(Relocatable(0xE1A04000), "mov r4, r0");
    Check(!Relocatable(0xE92D8010), "stmfd sp!, {r4, pc}");
    Check(!Relocatable(0xE52DF004), "str pc, [sp, #-4]!");
    Check(!Relocatable(0xE59F0010), "ldr r0, [pc, #16]");
    Check(!Relocatable(0xE08F0000), "add r0, pc, r0");
    Check(!Relocatable(0xEB000010), "bl");
    Check(!Relocatable(0xE12FFF1E), "bx lr");
    Check(!Relocatable(0x03A00001), "moveq r0, #1");
    Check(!Relocatable(0xE5900000), "ldr r0, [r0]");
}

// A frame heap at `at` in `memory`, a word with its signature's address before it: its start, end, head and tail.
void PutHeap(std::vector<uint8_t>& memory, uint32_t at, uint32_t start, uint32_t end, uint32_t head, uint32_t tail)
{
    xsflib::StoreLe32(&memory[at], kBase + at + 0x10); // the signature's address
    xsflib::StoreLe32(&memory[at + 0x10], 0x46524D48);
    xsflib::StoreLe32(&memory[at + 0x10 + 0x18], start);
    xsflib::StoreLe32(&memory[at + 0x10 + 0x1C], end);
    xsflib::StoreLe32(&memory[at + 0x10 + 0x24], head);
    xsflib::StoreLe32(&memory[at + 0x10 + 0x28], tail);
}

// The sound heap at 0x02001000 contains the heap at 0x02002000. Heaps that contain no other, and fields out of order,
// give no sound heap.
void TestHeap()
{
    using twosf::rip::FindSoundHeap;
    std::vector<uint8_t> memory(0x4000, 0);
    PutHeap(memory, 0x2000, kBase + 0x2040, kBase + 0x2800, kBase + 0x2100, kBase + 0x2700);
    PutHeap(memory, 0x1000, kBase + 0x1040, kBase + 0x3000, kBase + 0x2900, kBase + 0x2E00);
    const std::optional<twosf::rip::SoundHeap> found = FindSoundHeap(memory);
    Check(found && found->address == kBase + 0x1000 && found->free == 0x500, "the heap that contains the other");

    std::vector<uint8_t> apart(0x4000, 0);
    PutHeap(apart, 0x1000, kBase + 0x1040, kBase + 0x1800, kBase + 0x1100, kBase + 0x1700);
    PutHeap(apart, 0x2000, kBase + 0x2040, kBase + 0x2800, kBase + 0x2100, kBase + 0x2700);
    Check(!FindSoundHeap(apart), "two heaps, neither of which contains the other");

    std::vector<uint8_t> one(0x4000, 0);
    PutHeap(one, 0x1000, kBase + 0x1040, kBase + 0x1800, kBase + 0x1100, kBase + 0x1700);
    Check(FindSoundHeap(one).has_value(), "one heap");
    std::vector<uint8_t> disordered = one;
    xsflib::StoreLe32(&disordered[0x1000 + 0x10 + 0x24], kBase + 0x1780); // the head past the tail
    Check(!FindSoundHeap(disordered), "a head past the tail");
    std::vector<uint8_t> outside = one;
    xsflib::StoreLe32(&outside[0x1000 + 0x10 + 0x1C], kBase + 0x8000); // the end past main memory
    Check(!FindSoundHeap(outside), "an end past main memory");

    Check(!FindSoundHeap(std::vector<uint8_t>(0x1000, 0)), "no heap");
    std::vector<uint8_t> stray(0x1000, 0);
    xsflib::StoreLe32(&stray[0x110], 0x46524D48);
    Check(!FindSoundHeap(stray), "a signature without the heap's address before it");
}

// An archive of one sequence (file 0), its bank (file 1), its wave archive (file 2) and a file that only the boot reads
// (file 3).
sdat::ArchiveModel ArchiveModel()
{
    sdat::ArchiveModel model;
    model.sequences = {sdat::SequenceInfo{0, 0, 100, 64, 32, 0, 0, ""}};
    model.banks = {sdat::BankInfo{1, {0, 0xFFFF, 0xFFFF, 0xFFFF}, ""}};
    model.wave_archives = {sdat::WaveArchiveInfo{2, ""}, sdat::WaveArchiveInfo{3, ""}};
    model.files = {std::vector<uint8_t>(64, 1), std::vector<uint8_t>(64, 2), std::vector<uint8_t>(64, 3),
                   std::vector<uint8_t>(64, 4)};
    model.file_alignment = 4;

    return model;
}

// The music of an archive with names, and every sequence outside the sequence archives where none counts or the
// archive has no names.
void TestMusic()
{
    sdat::ArchiveModel model = ArchiveModel();
    model.sequences.push_back(sdat::SequenceInfo{0, 0, 100, 64, 32, 0, 0, ""});
    model.sequences.push_back(std::nullopt);
    const std::vector<uint8_t> unnamed = sdat::BuildArchive(model);
    Check(twosf::rip::MusicSequences(sdat::Archive::Parse(unnamed)) == std::set<uint32_t>{0, 1},
          "an archive without names");

    sdat::Symbols symbols;
    symbols.names[0] = {"SEQ_BGM_FIELD", "SEQ_SE_DOOR", std::nullopt};
    model.symbols = symbols;
    const std::vector<uint8_t> named = sdat::BuildArchive(model);
    Check(twosf::rip::MusicSequences(sdat::Archive::Parse(named)) == std::set<uint32_t>{0}, "the music by name");

    symbols.names[0] = {"SEQ_FIELD", "SEQ_DOOR", std::nullopt};
    model.symbols = symbols;
    const std::vector<uint8_t> no_music = sdat::BuildArchive(model);
    Check(twosf::rip::MusicSequences(sdat::Archive::Parse(no_music)) == std::set<uint32_t>{0, 1},
          "names that give no music");
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
// The archive keeps its layout on 4 bytes.
void TestCut()
{
    const std::vector<uint8_t> bytes = sdat::BuildArchive(ArchiveModel());
    const sdat::Archive archive = sdat::Archive::Parse(bytes);
    const std::vector<ntdlib::ntr::SequenceRef> sequences = {{0}};

    const std::optional<std::vector<uint8_t>> plain = twosf::rip::CutArchive(archive, sequences, {});
    const std::optional<std::vector<uint8_t>> with_boot = twosf::rip::CutArchive(archive, sequences, {3});
    Check(plain && with_boot, "the archive cuts");
    if (!plain || !with_boot)
    {
        return;
    }

    sdat::ArchiveModel expected = ArchiveModel();
    expected.files[3].clear();
    Check(*plain == sdat::BuildArchive(expected), "the cut keeps the sequence's files");
    Check(*with_boot == bytes, "the cut keeps the boot's file too");
}

} // namespace

int main()
{
    TestStub();
    TestRelocatable();
    TestHeap();
    TestMusic();
    TestArchiveFiles();
    TestCut();

    return xsflib::testing::Summary("sdat_set");
}
