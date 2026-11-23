// SPDX-License-Identifier: MIT

// Tests the stubs of a set for a game on DSE (rip/dse_set.h) on ntdlib's ARM interpreter: the start of music or of a
// short piece by number, music at a level, then the sleeps; the start by name, then the frames; the start by the game's
// calls, then the frames or the sleeps, with the level of the song's state, and in Thumb code with the stub at the hook
// or away from it; and the stub in an overlay or away from a hook in ARM code.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/arm/cpu.h"
#include "ntdlib/ntr/code.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/testing/nds_files.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "backup_image.h"
#include "dse_test_code.h"
#include "rip/boots.h"
#include "rip/dse_set.h"
#include "stub_memory.h"

namespace
{

namespace arm = ntdlib::arm;

using twosf::rip::DseCode;
using twosf::rip::DseStart;
using twosf::testing::CallsTestCode;
using twosf::testing::kBase;
using twosf::testing::kStack;
using twosf::testing::Memory;
using twosf::testing::TestCode;
using xsflib::testing::Check;

// bx lr.
constexpr uint32_t kReturn = 0xE12FFF1E;

// Routines in the test's memory for a game that starts a song by name and runs the frames.
DseCode NamedTestCode()
{
    DseCode code = TestCode();
    code.start = twosf::rip::DseStartKind::kName;
    code.player = kBase + 0x8000;
    code.frame_wait = kBase + 0x140;
    code.frame_update = kBase + 0x150;

    return code;
}

// TestCode with the game's start of music at a level.
DseCode TestCodeWithLevels()
{
    DseCode code = TestCode();
    code.play_bgm_level = kBase + 0x130;

    return code;
}

// The routines that the stub calls in their order, each with its first argument: the start, then two sleeps. The start
// at a level shows its three arguments. A start of another kind loops for ever: a call of it reaches nothing more.
std::string RunStub(const DseStart& start, const DseCode& code = TestCode())
{
    constexpr uint32_t kLoop = 0xEAFFFFFE; // b .

    const bool level = !start.me && code.play_bgm_level != 0;
    const uint32_t routine = start.me ? code.play_me : level ? code.play_bgm_level : code.play_bgm;
    Memory memory;
    for (const uint32_t address : {code.play_bgm, code.play_me, code.play_bgm_level})
    {
        if (address != 0)
        {
            xsflib::StoreLe32(memory.At(address), address == routine ? kReturn : kLoop);
        }
    }
    xsflib::StoreLe32(memory.At(code.sleep), kReturn);

    const twosf::rip::DseStub stub = twosf::rip::BuildDseStub(code, start);
    Check(stub.bytes.size() <= code.hook_end - code.hook, "the stub fits after the hook");
    std::memcpy(memory.At(code.hook), stub.bytes.data(), stub.bytes.size());
    Check(xsflib::LoadLe32(memory.At(code.hook + stub.start_offset)) == start.number,
          "the stub's first word contains the number");

    arm::Cpu cpu(memory, arm::Architecture::kArmV5TE);
    cpu.Jump(code.hook, arm::Mode::kSystem);
    cpu.SetReg(13, kStack);
    std::string calls;
    const auto run_to = [&](uint32_t address, const char* name, int arguments)
    {
        cpu.SetStopAddress(address);
        const bool reached = cpu.Run(100000) == arm::StopReason::kStopAddress;
        std::string values = std::to_string(cpu.Reg(0));
        for (int r = 1; r < arguments; r++)
        {
            values += ", " + std::to_string(cpu.Reg(r));
        }
        calls += std::string(calls.empty() ? "" : " ") + (reached ? name : "?") + "(" + values + ")";
        cpu.SetStopAddress(0xFFFFFFFF);
        cpu.Run(1);
    };

    run_to(routine, start.me ? "me" : level ? "level" : "bgm", level ? 3 : 1);
    run_to(code.sleep, "sleep", 1);
    run_to(code.sleep, "sleep", 1);

    return calls;
}

// The routines that the stub of a start by name calls in their order. The start shows its four arguments, the name at
// the third and the fifth argument on the stack. The player's work shows its argument. The stack starts with words of
// all ones: a fifth argument of 0 is the stub's.
std::string RunNamedStub(const DseStart& start)
{
    const DseCode code = NamedTestCode();
    Memory memory;
    for (const uint32_t address : {code.play_bgm, code.frame_wait, code.frame_update})
    {
        xsflib::StoreLe32(memory.At(address), kReturn);
    }
    std::memset(memory.At(kStack - 0x100), 0xFF, 0x100);

    const twosf::rip::DseStub stub = twosf::rip::BuildDseStub(code, start);
    Check(stub.bytes.size() <= code.hook_end - code.hook, "the stub by name fits after the hook");
    std::memcpy(memory.At(code.hook), stub.bytes.data(), stub.bytes.size());

    arm::Cpu cpu(memory, arm::Architecture::kArmV5TE);
    cpu.Jump(code.hook, arm::Mode::kSystem);
    cpu.SetReg(13, kStack);
    const auto player = [&](uint32_t value)
    {
        return value == code.player ? std::string("player") : std::to_string(value);
    };
    std::string calls;
    const auto run_to = [&](uint32_t address, const char* name)
    {
        cpu.SetStopAddress(address);
        const bool reached = cpu.Run(100000) == arm::StopReason::kStopAddress;
        std::string values;
        if (address == code.play_bgm)
        {
            const uint32_t name_at = cpu.Reg(2);
            std::string text = "?";
            if (name_at == code.hook + stub.start_offset)
            {
                const auto* bytes = reinterpret_cast<const char*>(memory.At(name_at));
                text = '"' + std::string(bytes, strnlen(bytes, twosf::rip::kDseNameBytes)) + '"';
            }
            values = player(cpu.Reg(0)) + ", " + std::to_string(cpu.Reg(1)) + ", " + text + ", " +
                     std::to_string(cpu.Reg(3)) + ", " + std::to_string(xsflib::LoadLe32(memory.At(cpu.Reg(13))));
        }
        else if (address == code.frame_update)
        {
            values = player(cpu.Reg(0));
        }
        calls += std::string(calls.empty() ? "" : " ") + (reached ? name : "?") + "(" + values + ")";
        cpu.SetStopAddress(0xFFFFFFFF);
        cpu.Run(1);
    };

    run_to(code.play_bgm, "play");
    for (int frame = 0; frame < 2; frame++)
    {
        run_to(code.frame_wait, "wait");
        run_to(code.frame_update, "update");
    }

    return calls;
}

// The routines that the stub of a start by calls calls in their order: each call with its four arguments, the wait for
// a frame with none, and the sleep with its time in milliseconds. Each routine returns at once.
std::string RunCallsStub(const DseStart& start, const DseCode& code)
{
    std::vector<std::pair<uint32_t, const char*>> routines = {
        {code.calls[0].routine, "first"}, {code.calls[1].routine, "second"}, {code.sleep, "sleep"}};
    if (code.frame_wait != 0)
    {
        routines = {
            {code.calls[0].routine, "first"},      {code.calls[1].routine, "second"},     {code.frame_wait, "wait"},
            {code.frame_calls[0].routine, "work"}, {code.frame_calls[1].routine, "more"}, {code.frame_wait, "wait"}};
    }

    Memory memory;
    for (const auto& [address, name] : routines)
    {
        xsflib::StoreLe32(memory.At(address), kReturn);
    }

    const twosf::rip::DseStub stub = twosf::rip::BuildDseStub(code, start);
    Check(stub.bytes.size() <= code.hook_end - code.hook, "the stub by calls fits after the hook");
    std::memcpy(memory.At(code.hook), stub.bytes.data(), stub.bytes.size());
    Check(xsflib::LoadLe32(memory.At(code.hook + stub.start_offset)) == start.number,
          "the stub by calls keeps the number at its offset");

    arm::Cpu cpu(memory, arm::Architecture::kArmV5TE);
    cpu.Jump(code.hook, arm::Mode::kSystem);
    cpu.SetReg(13, kStack);
    std::string calls;
    for (const auto& [address, name] : routines)
    {
        cpu.SetStopAddress(address);
        const bool reached = cpu.Run(100000) == arm::StopReason::kStopAddress;
        const std::string routine = name;
        std::string arguments = xsflib::Hex(cpu.Reg(0));
        if (routine == "wait")
        {
            arguments.clear();
        }
        else if (routine != "sleep")
        {
            arguments +=
                ", " + xsflib::Hex(cpu.Reg(1)) + ", " + xsflib::Hex(cpu.Reg(2)) + ", " + xsflib::Hex(cpu.Reg(3));
        }
        calls += std::string(calls.empty() ? "" : " ") + (reached ? routine : "?") + "(" + arguments + ")";
        cpu.SetStopAddress(0xFFFFFFFF);
        cpu.Run(1);
    }

    return calls;
}

// The word of the game's sound object, and where the game keeps it.
constexpr uint32_t kObjectPointer = kBase + 0x8000;
constexpr uint32_t kObject = 0x02ABCDE0;

// A game in Thumb code that starts a song by calls, as Shiren 4 does. Its hook and routines are Thumb code (bit 0
// set): the start of music takes the object, the number, 127 and 1; the start of a short piece takes the object,
// 0x401 + the number, 127, 0x40 and two words of 0 on the stack; and the work for a frame takes the object alone. Each
// call reads the object from its pointer. With `away`, the stub goes over a routine away from the hook.
DseCode ThumbTestCode(bool away)
{
    DseCode code;
    code.start = twosf::rip::DseStartKind::kCalls;
    code.hook = kBase + 0x401;
    code.hook_end = away ? kBase + 0x40C : kBase + 0x600;
    code.stub = away ? kBase + 0x700 : 0;
    code.stub_end = away ? kBase + 0x800 : 0;
    code.sleep = kBase + 0x121;
    code.frame_wait = kBase + 0x141;
    code.calls[0] = {
        .routine = kBase + 0x161, .arguments = {kObjectPointer, twosf::rip::kDseSongNumber, 127, 1}, .loads = 1};
    code.me_calls[0] = {.routine = kBase + 0x171,
                        .arguments = {kObjectPointer, twosf::rip::kDseSongNumber, 127, 0x40},
                        .loads = 1,
                        .number_base = 0x401,
                        .stack_zeros = 2};
    code.frame_calls[0] = {
        .routine = kBase + 0x181, .arguments = {kObjectPointer, 5, 6, 7}, .registers = 1, .loads = 1};

    return code;
}

// The routines that a set of ThumbTestCode calls in their order, from the hook in Thumb state: the start with its four
// arguments, and for a short piece or a stream (`stream`, with BuildDseStreamStub) the two words on the stack, then for
// music with a change the change with r0 to r3, then the wait for a frame, the work with r0 to r3 and the wait again.
// The set is MakeDseSet's, with an ARM9 binary of 4 KiB at kBase. Each routine is Thumb's bx lr. A wait shows the stack
// pointer's change since the hook. The stack starts with words of all ones: a word of 0 is the stub's.
std::string RunThumbStub(const DseStart& start, const DseCode& code, const twosf::rip::DseStream* stream = nullptr)
{
    constexpr uint16_t kThumbReturn = 0x4770; // bx lr
    std::vector<uint8_t> arm9(0x1000, 0);
    for (const uint32_t routine :
         {code.sleep, code.frame_wait, code.calls[0].routine, code.me_calls[0].routine, code.frame_calls[0].routine})
    {
        xsflib::StoreLe16(&arm9[(routine & ~1U) - kBase], kThumbReturn);
    }
    for (const twosf::rip::DseStreamKind& kind : code.streams)
    {
        xsflib::StoreLe16(&arm9[(kind.calls[0].routine & ~1U) - kBase], kThumbReturn);
    }
    if (code.change_calls[0].routine != 0)
    {
        xsflib::StoreLe16(&arm9[(code.change_calls[0].routine & ~1U) - kBase], kThumbReturn);
    }

    const std::vector<uint8_t> image = twosf::testing::ImageWithArm9(arm9);
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);
    const twosf::rip::DseStub stub =
        stream != nullptr ? twosf::rip::BuildDseStreamStub(code, *stream) : twosf::rip::BuildDseStub(code, start);
    const twosf::rip::DseSet set =
        twosf::rip::MakeDseSet(rom, code, stub, twosf::rip::WholeFiles(rom, twosf::rip::EveryFile(rom)));
    Check(xsflib::LoadLe32(&set.image[set.start_word]) == start.number,
          "the start word of the Thumb set is the number");

    Memory memory;
    const ntdlib::ntr::LoadedCode code_of_set = ntdlib::ntr::Arm9Code(ntdlib::ntr::Rom::Parse(set.image));
    std::memcpy(memory.At(code_of_set.address), code_of_set.bytes.data(), code_of_set.bytes.size());
    xsflib::StoreLe32(memory.At(kObjectPointer), kObject);
    std::memset(memory.At(kStack - 0x100), 0xFF, 0x100);

    const uint32_t start_routine = stream != nullptr ? code.streams[stream->kind].calls[0].routine
                                   : start.me        ? code.me_calls[0].routine
                                                     : code.calls[0].routine;
    const char* const start_name = stream != nullptr ? "stream" : start.me ? "me" : "music";
    std::vector<std::pair<uint32_t, const char*>> routines = {{start_routine, start_name},
                                                              {code.frame_wait, "wait"},
                                                              {code.frame_calls[0].routine, "work"},
                                                              {code.frame_wait, "wait"}};
    if (start.change && !start.me && stream == nullptr)
    {
        routines.insert(routines.begin() + 1, {code.change_calls[0].routine, "change"});
    }
    arm::Cpu cpu(memory, arm::Architecture::kArmV5TE);
    cpu.Jump(code.hook, arm::Mode::kSystem);
    cpu.SetReg(13, kStack);
    std::string calls;
    for (const auto& [address, name] : routines)
    {
        cpu.SetStopAddress(address & ~1U);
        const bool reached = cpu.Run(100000) == arm::StopReason::kStopAddress;
        const std::string routine = name;
        std::string values;
        if (routine == "wait")
        {
            values = std::to_string(static_cast<int>(cpu.Reg(13) - kStack));
        }
        else
        {
            values = xsflib::Hex(cpu.Reg(0)) + ", " + xsflib::Hex(cpu.Reg(1)) + ", " + xsflib::Hex(cpu.Reg(2)) + ", " +
                     xsflib::Hex(cpu.Reg(3));
        }
        if (routine == "me" || routine == "stream")
        {
            values += "; " + xsflib::Hex(xsflib::LoadLe32(memory.At(cpu.Reg(13)))) + ", " +
                      xsflib::Hex(xsflib::LoadLe32(memory.At(cpu.Reg(13) + 4)));
        }
        calls += std::string(calls.empty() ? "" : " ") + (reached ? routine : "?") + "(" + values + ")";
        cpu.SetStopAddress(0xFFFFFFFF);
        cpu.Run(1);
    }

    return calls;
}

void TestStub()
{
    Check(RunStub({.number = 12, .me = false}) == "bgm(12) sleep(1000) sleep(1000)",
          "music starts with the game's start of music, then the stub sleeps");
    Check(RunStub({.number = 3, .me = true}) == "me(3) sleep(1000) sleep(1000)",
          "a short piece starts with the game's start of short pieces");

    const DseCode levels = TestCodeWithLevels();
    Check(RunStub({.number = 86, .me = false, .level = 180}, levels) == "level(86, 0, 180) sleep(1000) sleep(1000)",
          "where the game starts music at a level, music starts with that routine, with no fade");
    Check(RunStub({.number = 1, .me = false}, levels) == "level(1, 0, 256) sleep(1000) sleep(1000)",
          "and music at full level too");
    Check(RunStub({.number = 3, .me = true}, levels) == "me(3) sleep(1000) sleep(1000)",
          "a short piece still starts with the game's start of short pieces");

    Check(
        RunNamedStub({.name = "P04"}) == "play(player, 0, \"P04\", 0, 0) wait() update(player) wait() update(player)",
        "a start by name gives the player, the first slot, the name, no fade-out and no fade-in, then runs the frames");
    Check(RunNamedStub({.name = "SR_001_A_NAME_OF_24_BYTE"}) ==
              "play(player, 0, \"SR_001_A_NAME_OF_24_BYT\", 0, 0) wait() update(player) wait() update(player)",
          "a name keeps 23 bytes and the terminating zero");

    Check(RunCallsStub({.number = 0x2A}, CallsTestCode(true)) ==
              "first(0x7, 0x0, 0x0, 0x0) second(0x2008000, 0x2a, 0x3f800000, 0x0) wait() "
              "work(0x2008100, 0x0, 0x0, 0x0) more(0x0, 0x0, 0x0, 0x0) wait()",
          "a start by calls makes each call with its arguments and the song's number, then runs the frames: " +
              RunCallsStub({.number = 0x2A}, CallsTestCode(true)));
    Check(RunCallsStub({.number = 4}, CallsTestCode(false)) ==
              "first(0x7, 0x0, 0x0, 0x0) second(0x2008000, 0x4, 0x3f800000, 0x0) sleep(0x3e8)",
          "with no wait for the frames, the stub sleeps: " + RunCallsStub({.number = 4}, CallsTestCode(false)));

    // A start by calls with the level of the song's state, as Shiren DS 2 makes it.
    DseCode level = CallsTestCode(false);
    level.calls[0] = {.routine = kBase + 0x160,
                      .arguments = {twosf::rip::kDseSongNumber, twosf::rip::kDseSongLevel, 0, 0}};
    level.calls[1] = {.routine = kBase + 0x170};
    Check(RunCallsStub({.number = 7, .level = 240}, level) ==
              "first(0x7, 0xf0, 0x0, 0x0) second(0x0, 0x0, 0x0, 0x0) sleep(0x3e8)",
          "a call takes the level of the song's state: " + RunCallsStub({.number = 7, .level = 240}, level));

    // Thumb code: the stub switches to ARM state at the hook, or jumps from the hook to its place, and calls the
    // routines in Thumb state.
    for (const bool away : {false, true})
    {
        const DseCode code = ThumbTestCode(away);
        const std::string where = away ? " (the stub away from the hook)" : " (the stub at the hook)";
        Check(RunThumbStub({.number = 0x2A}, code) ==
                  "music(0x2abcde0, 0x2a, 0x7f, 0x1) wait(0) work(0x2abcde0, 0x2a, 0x7f, 0x1) wait(0)",
              "music starts with the object from its pointer, then the frame's work takes the object alone" + where +
                  ": " + RunThumbStub({.number = 0x2A}, code));
        Check(RunThumbStub({.number = 3, .me = true}, code) ==
                  "me(0x2abcde0, 0x404, 0x7f, 0x40; 0x0, 0x0) wait(0) work(0x2abcde0, 0x404, 0x7f, 0x40) wait(0)",
              "a short piece starts with its calls, 0x401 + its number and two words of 0 on the stack, and the stub "
              "then drops the words" +
                  where + ": " + RunThumbStub({.number = 3, .me = true}, code));

        // A game with a change of the music's volume after its start, as Shiren 5 makes it: the object, the volume and
        // the frames.
        DseCode change = code;
        change.change_calls[0] = {
            .routine = kBase + 0x191,
            .arguments = {kObjectPointer, twosf::rip::kDseChangeVolume, twosf::rip::kDseChangeFrames, 0},
            .registers = 3,
            .loads = 1};
        const DseStart changed = {.number = 0x2A, .change = ntdlib::dse::StartChange{156, 102, 3}};
        Check(RunThumbStub(changed, change) ==
                  "music(0x2abcde0, 0x2a, 0x7f, 0x1) change(0x2abcde0, 0x66, 0x3, 0x1) wait(0) "
                  "work(0x2abcde0, 0x66, 0x3, 0x1) wait(0)",
              "music with a change starts, then the stub calls the change with its volume and frames" + where + ": " +
                  RunThumbStub(changed, change));
        Check(RunThumbStub({.number = 0x2A}, change) ==
                  "music(0x2abcde0, 0x2a, 0x7f, 0x1) wait(0) work(0x2abcde0, 0x2a, 0x7f, 0x1) wait(0)",
              "music without a change makes no call of the change" + where + ": " +
                  RunThumbStub({.number = 0x2A}, change));
        Check(RunThumbStub({.number = 3, .me = true, .change = ntdlib::dse::StartChange{156, 102, 3}}, change) ==
                  "me(0x2abcde0, 0x404, 0x7f, 0x40; 0x0, 0x0) wait(0) work(0x2abcde0, 0x404, 0x7f, 0x40) wait(0)",
              "a short piece makes no call of the change" + where);
    }
}

// A stream's stub makes its kind's calls in place of the song's, with the stream's number, a literal -1 and the kind's
// words on the stack. It then drops the words and runs the frames as a song's stub does.
void TestStreamStub()
{
    static constexpr uint32_t kWords[] = {0x3F800000, 0x11};
    for (const bool away : {false, true})
    {
        DseCode code = ThumbTestCode(away);
        const twosf::rip::DseStreamKind kinds[] = {
            {.format = "music/%03u.SAD",
             .calls = {{{.routine = kBase + 0x191,
                         .arguments = {kObjectPointer, 1, twosf::rip::kDseSongNumber, 0xFFFFFFFF},
                         .loads = 1,
                         .literals = 0b1000,
                         .stack = kWords}}}}};
        code.streams = kinds;
        const twosf::rip::DseStream stream = {.path = "music/007.SAD", .number = 7, .label = "007"};
        const std::string where = away ? " (the stub away from the hook)" : " (the stub at the hook)";
        Check(RunThumbStub({.number = 7}, code, &stream) ==
                  "stream(0x2abcde0, 0x1, 0x7, 0xffffffff; 0x3f800000, 0x11) wait(0) "
                  "work(0x2abcde0, 0x1, 0x7, 0xffffffff) wait(0)",
              "a stream starts with its kind's calls, its number and the words on the stack" + where + ": " +
                  RunThumbStub({.number = 7}, code, &stream));
    }
}

// An ARM hook with the stub away from it: the hook jumps to the stub. The stub makes the game's call with two words of
// 0 on the stack, then sleeps.
void TestArmStubAway()
{
    DseCode code = CallsTestCode(false);
    code.hook = kBase + 0x400;
    code.hook_end = kBase + 0x408;
    code.stub = kBase + 0x700;
    code.stub_end = kBase + 0x800;
    code.calls[0] = {.routine = kBase + 0x160,
                     .arguments = {kBase + 0x8000, twosf::rip::kDseSongNumber, 127, 127},
                     .stack_zeros = 2};
    code.calls[1] = {};

    std::vector<uint8_t> arm9(0x1000, 0);
    for (const uint32_t routine : {code.sleep, code.calls[0].routine})
    {
        xsflib::StoreLe32(&arm9[routine - kBase], kReturn);
    }
    const std::vector<uint8_t> image = twosf::testing::ImageWithArm9(arm9);
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);
    const DseStart start = {.number = 0x2A};
    const twosf::rip::DseStub stub = twosf::rip::BuildDseStub(code, start);
    const twosf::rip::DseSet set =
        twosf::rip::MakeDseSet(rom, code, stub, twosf::rip::WholeFiles(rom, twosf::rip::EveryFile(rom)));

    Memory memory;
    const ntdlib::ntr::LoadedCode code_of_set = ntdlib::ntr::Arm9Code(ntdlib::ntr::Rom::Parse(set.image));
    std::memcpy(memory.At(code_of_set.address), code_of_set.bytes.data(), code_of_set.bytes.size());
    std::memset(memory.At(kStack - 0x100), 0xFF, 0x100);

    arm::Cpu cpu(memory, arm::Architecture::kArmV5TE);
    cpu.Jump(code.hook, arm::Mode::kSystem);
    cpu.SetReg(13, kStack);
    cpu.SetStopAddress(code.calls[0].routine);
    const bool called = cpu.Run(100000) == arm::StopReason::kStopAddress;
    Check(called && cpu.Reg(0) == kBase + 0x8000 && cpu.Reg(1) == 0x2A && cpu.Reg(2) == 127 && cpu.Reg(3) == 127 &&
              xsflib::LoadLe32(memory.At(cpu.Reg(13))) == 0 && xsflib::LoadLe32(memory.At(cpu.Reg(13) + 4)) == 0,
          "the hook jumps to the stub, and the stub calls the routine with its arguments and two words of 0 on the "
          "stack");

    cpu.SetStopAddress(0xFFFFFFFF);
    cpu.Run(1);
    cpu.SetStopAddress(code.sleep);
    Check(cpu.Run(100000) == arm::StopReason::kStopAddress && cpu.Reg(13) == kStack,
          "then sleeps, with the stack back");

    const auto make_set = [&rom, &code, &stub]
    {
        static_cast<void>(
            twosf::rip::MakeDseSet(rom, code, stub, twosf::rip::WholeFiles(rom, twosf::rip::EveryFile(rom))));
    };

    code.hook_end = kBase + 0x404;
    Check(xsflib::testing::ErrorOf<std::runtime_error>(make_set) ==
              "the stub takes 8 bytes, and the code after the hook has 4",
          "a hook with no room for the jump");

    code.hook_end = kBase + 0x408;
    const auto stub_bytes = static_cast<uint32_t>(stub.bytes.size());
    code.stub_end = code.stub + stub_bytes - 4;
    const std::string too_big =
        "the stub takes " + std::to_string(stub_bytes) + " bytes, and its place has " + std::to_string(stub_bytes - 4);
    Check(xsflib::testing::ErrorOf<std::runtime_error>(make_set) == too_big, "a stub that doesn't fit in its place");
    code.stub_end = code.stub + stub_bytes;
    Check(xsflib::testing::ErrorOf<std::runtime_error>(make_set).empty(), "a stub that fills its place");
}

// A hook in an overlay: the set stores the overlay decompressed with the stub, and the overlay table loses the
// compression flags. The stub's number is in the overlay's file of the set. The test image's overlay 0 is at
// 0x02100000. Its file ends with the footer of a compressed part that contains no data: decompressed, it is the same
// bytes. A set without the overlay's file can't contain the stub.
void TestOverlayHook()
{
    constexpr uint32_t kOverlay = 0x02100000;
    constexpr std::size_t kOverlayBytes = 0x200;
    constexpr uint32_t kCompressed = 0x01000000;
    constexpr uint32_t kFooter = 8;
    constexpr std::array<uint32_t, 1> kOverlays = {0};

    ntdlib::testing::nds_files::Image spec;
    spec.arm9 = twosf::testing::Halts();
    spec.arm9_overlay.assign(kOverlayBytes, 0x5A);
    xsflib::StoreLe32(&spec.arm9_overlay[kOverlayBytes - 8], (kFooter << 24) | kFooter);
    xsflib::StoreLe32(&spec.arm9_overlay[kOverlayBytes - 4], 0);
    spec.files = {{"data/song.bin", std::vector<uint8_t>(0x80, 0x11)}};
    std::vector<uint8_t> image = ntdlib::testing::nds_files::BuildImage(spec).first;
    const uint32_t table = xsflib::LoadLe32(&image[0x50]);
    xsflib::StoreLe32(&image[table + 28], kCompressed | static_cast<uint32_t>(kOverlayBytes));
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);

    DseCode code = CallsTestCode(false);
    code.overlays = kOverlays;
    code.hook = kOverlay + 0x40;
    code.hook_end = kOverlay + 0x180;
    const DseStart start = {.number = 0x123};
    const twosf::rip::DseStub stub = twosf::rip::BuildDseStub(code, start);

    const twosf::rip::DseSet set =
        twosf::rip::MakeDseSet(rom, code, stub, twosf::rip::WholeFiles(rom, twosf::rip::EveryFile(rom)));
    const ntdlib::ntr::Rom set_rom = ntdlib::ntr::Rom::Parse(set.image);
    const std::span<const uint8_t> overlay = set_rom.FileData(set_rom.Arm9Overlays().front().file_id);
    const auto without = [&]
    {
        twosf::rip::MakeDseSet(rom, code, stub, twosf::rip::WholeFiles(rom, {1, 2}));
    };

    Check(
        rom.Arm9Overlays().front().flags == (kCompressed | kOverlayBytes) && set_rom.Arm9Overlays().front().flags == 0,
        "the set's overlay table marks the overlay uncompressed");
    Check(overlay.size() == kOverlayBytes && std::equal(stub.bytes.begin(), stub.bytes.end(), overlay.begin() + 0x40),
          "the overlay's file contains the stub at the hook");
    Check(overlay[0] == 0x5A && overlay[0x3F] == 0x5A && overlay[0x1F0] == 0x5A,
          "the rest of the overlay stays as it was");
    Check(xsflib::LoadLe32(&set.image[set.start_word]) == 0x123, "the start word is the stub's number");
    Check(xsflib::testing::ErrorOf<std::runtime_error>(without) == "the library doesn't keep the overlay of the hook",
          "a library without the overlay's file");

    // The stub away from the hook, in the same overlay: the hook jumps to it.
    code.hook_end = kOverlay + 0x48;
    code.stub = kOverlay + 0x100;
    code.stub_end = kOverlay + 0x180;
    const twosf::rip::DseSet away =
        twosf::rip::MakeDseSet(rom, code, stub, twosf::rip::WholeFiles(rom, twosf::rip::EveryFile(rom)));
    const ntdlib::ntr::Rom away_rom = ntdlib::ntr::Rom::Parse(away.image);
    const std::span<const uint8_t> away_overlay = away_rom.FileData(away_rom.Arm9Overlays().front().file_id);
    Check(xsflib::LoadLe32(&away_overlay[0x44]) == kOverlay + 0x100 &&
              std::equal(stub.bytes.begin(), stub.bytes.end(), away_overlay.begin() + 0x100),
          "the hook jumps to the stub, away from it in the overlay");
    Check(xsflib::LoadLe32(&away.image[away.start_word]) == 0x123, "the start word is that of the stub away");
    Check(away_overlay[0x48] == 0x5A && away_overlay[0xFF] == 0x5A, "the overlay between them stays as it was");

    code.stub = kBase + 0x100;
    code.stub_end = kBase + 0x180;
    const auto apart = [&]
    {
        twosf::rip::MakeDseSet(rom, code, stub, twosf::rip::WholeFiles(rom, twosf::rip::EveryFile(rom)));
    };
    Check(xsflib::testing::ErrorOf<std::runtime_error>(apart) ==
              "a stub away from the hook requires both in the ARM9 binary or both in one overlay",
          "a stub in the ARM9 binary for a hook in an overlay");
}

} // namespace

int main()
{
    TestStub();
    TestStreamStub();
    TestArmStubAway();
    TestOverlayHook();

    return xsflib::testing::Summary("dse_stub");
}
