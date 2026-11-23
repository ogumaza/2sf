// SPDX-License-Identifier: MIT

// Tests the parts of a set for a game on Nintendo SPD's driver (rip/spd_set.h): the stub on ntdlib's ARM interpreter
// (the load of set B until it's done, the start, then the frames), the words that a mini writes, the table, and the
// run of a mini: the check of the sound that the stub starts, and the sound unit's output.

#include "rip/spd_set.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/arm/assembler.h"
#include "ntdlib/arm/cpu.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "backup_image.h"
#include "stub_memory.h"

namespace
{

namespace arm = ntdlib::arm;

using twosf::rip::SpdCode;
using twosf::rip::SpdStart;
using twosf::testing::kBase;
using twosf::testing::kStack;
using twosf::testing::Memory;
using xsflib::testing::Check;

// The game's routines at the addresses of `code`. The load of set B returns 0 twice, then 1. The start, the frame's
// messages and the setter of the branch flag return at once.
void WriteGameRoutines(Memory& memory, const SpdCode& code)
{
    constexpr uint32_t kCounter = kBase + 0x800;

    xsflib::StoreLe32(memory.At(kCounter), 2);
    arm::ArmAssembler load(code.load_set_b);
    const int loaded = load.NewLabel();
    load.LoadConstant(2, kCounter);
    load.Ldr(3, 2);
    load.Mov(0, 1);
    load.Cmp(3, 0);
    load.B(loaded, arm::Condition::kEq);
    load.Sub(3, 3, 1);
    load.Str(3, 2);
    load.Mov(0, 0);
    load.Bind(loaded);
    load.Bx(14);
    load.Pool();
    const std::vector<uint8_t> load_bytes = load.Finish();
    std::memcpy(memory.At(code.load_set_b), load_bytes.data(), load_bytes.size());
    constexpr uint32_t kReturn = 0xE12FFF1E; // bx lr
    xsflib::StoreLe32(memory.At(code.play), kReturn);
    xsflib::StoreLe32(memory.At(code.sound_frame), kReturn);
    xsflib::StoreLe32(memory.At(code.branch_flag_1), kReturn);
}

// The routines that the stub calls in their order, with their arguments, and the SWIs between them, over the routines
// of WriteGameRoutines.
std::string RunStub(const SpdStart& start)
{
    SpdCode code;
    code.hook = kBase + 0x400;
    code.hook_end = kBase + 0x480;
    code.load_set_b = kBase + 0x100;
    code.play = kBase + 0x200;
    code.sound_frame = kBase + 0x300;
    code.branch_flag_1 = kBase + 0x380;

    Memory memory;
    WriteGameRoutines(memory, code);

    const twosf::rip::SpdStub stub = twosf::rip::BuildSpdStub(code, start);
    Check(stub.bytes.size() <= code.hook_end - code.hook, "the stub fits after the hook");
    std::memcpy(memory.At(code.hook), stub.bytes.data(), stub.bytes.size());
    Check(xsflib::LoadLe32(memory.At(code.hook + stub.start_offset)) == start.sound &&
              xsflib::LoadLe32(memory.At(code.hook + stub.start_offset + 4)) == start.slot &&
              xsflib::LoadLe32(memory.At(code.hook + stub.start_offset + 8)) == start.branch_flag,
          "the stub's words contain the sound, the slot and the branch flag");

    arm::Cpu cpu(memory, arm::Architecture::kArmV5TE);
    cpu.Jump(code.hook, arm::Mode::kSystem);
    cpu.SetReg(13, kStack);
    std::string calls;
    const auto note = [&calls](const std::string& call)
    {
        calls += (calls.empty() ? "" : " ") + call;
    };
    cpu.SetSwiHandler(
        [&](arm::Cpu&, uint32_t comment)
        {
            note("swi(" + std::to_string(comment >> 16) + ")");
            return true;
        });

    const std::pair<uint32_t, const char*> routines[] = {
        {code.load_set_b, "load"}, {code.play, "play"}, {code.sound_frame, "frame"}, {code.branch_flag_1, "flag"}};
    for (int step = 0; step < 7; step++)
    {
        cpu.SetStopAddress(0xFFFFFFFF);
        bool reached = false;
        for (int i = 0; i < 100 && !reached; i++)
        {
            cpu.Run(1);
            for (const auto& [address, name] : routines)
            {
                if (cpu.Reg(15) == address)
                {
                    const std::string arguments = std::to_string(cpu.Reg(0)) + ", " + std::to_string(cpu.Reg(1)) +
                                                  (address == code.play ? ", " + std::to_string(cpu.Reg(2)) : "");
                    const bool none = address == code.sound_frame || address == code.branch_flag_1;
                    note(std::string(name) + "(" + (none ? "" : arguments) + ")");
                    reached = true;
                }
            }
        }
    }

    return calls;
}

void TestStub()
{
    const std::string calls = RunStub({.sound = 7, .slot = 3});
    Check(calls == "load(7, 3) load(7, 3) load(7, 3) play(7, 3, 0) swi(5) frame() swi(5) frame() swi(5) frame()",
          "the stub loads set B until it's loaded, starts the sound with no fade, then waits for each vertical blank "
          "and sends the frame's messages: " +
              calls);
    const std::string flag = RunStub({.sound = 7, .slot = 3, .branch_flag = 1});
    Check(flag == "load(7, 3) load(7, 3) load(7, 3) play(7, 3, 0) flag() swi(5) frame() swi(5) frame()",
          "with the branch flag at 1, the stub sends it right after the start: " + flag);
}

void TestWords()
{
    Check(twosf::rip::SpdStartWords({.sound = 0x1234, .slot = 3, .branch_flag = 1}) ==
              std::vector<uint8_t>{0x34, 0x12, 0, 0, 3, 0, 0, 0, 1, 0, 0, 0},
          "a mini writes the sound, the slot and the branch flag");
}

void TestTable()
{
    Check(twosf::rip::ListsSpdGame("ASPE"), "the table lists Super Princess Peach");
    Check(!twosf::rip::ListsSpdGame("C2SE"), "the table lists no other game");
}

// An image whose hook starts sound 7 of slot 3 with the routine `code.play`, then halts. `arm7` is the ARM7 binary.
std::vector<uint8_t> StartImage(SpdCode& code, std::vector<uint8_t> arm7 = twosf::testing::Halts())
{
    arm::ArmAssembler a(twosf::testing::kArm9Base);
    const int halt = a.NewLabel();
    const int hook = a.NewLabel();
    const int play = a.NewLabel();
    a.Bind(hook);
    a.Mov(0, 7);
    a.Mov(1, 3);
    a.Bl(play);
    twosf::testing::EmitHaltForever(a, halt);
    a.Bind(play);
    a.Bx(14);
    a.Pool();

    code.hook = a.Address(hook);
    code.play = a.Address(play);

    return twosf::testing::ImageWithArm9(a.Finish(), {}, std::move(arm7));
}

// RunSpdMini checks the sound and the slot that the stub starts, and follows the sound unit's output from the start.
void TestMini()
{
    SpdCode code;
    const std::vector<uint8_t> quiet = StartImage(code);
    const std::vector<uint8_t> tone = StartImage(code, twosf::testing::ToneArm7());

    const twosf::rip::SpdMiniRun right = twosf::rip::RunSpdMini(quiet, code, 0, {.sound = 7, .slot = 3}, 1.0);
    const twosf::rip::SpdMiniRun wrong = twosf::rip::RunSpdMini(quiet, code, 0, {.sound = 8, .slot = 3}, 1.0);
    const twosf::rip::SpdMiniRun sounds = twosf::rip::RunSpdMini(tone, code, 0, {.sound = 7, .slot = 3}, 1.0);

    Check(!right.error && !right.sounded,
          "the run starts the mini's sound, and a halted ARM7 stays silent: " + right.error.value_or(""));
    Check(wrong.error == "the stub starts sound 7 of slot 3, not sound 8 of slot 3",
          "a run that starts another sound fails: " + wrong.error.value_or(""));
    Check(!sounds.error && sounds.sounded, "the run with a tone sounds: " + sounds.error.value_or(""));
}

} // namespace

int main()
{
    TestStub();
    TestWords();
    TestTable();
    TestMini();

    return xsflib::testing::Summary("spd_set");
}
