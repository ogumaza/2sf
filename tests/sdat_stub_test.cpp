// SPDX-License-Identifier: MIT

// Tests the stubs of a set for a game on the SDK's sound archive player (rip/sdat_set.h) on ntdlib's ARM interpreter:
// the load, with and without the loader and with a restart, the start, and each frame's wait and update; a game's
// starts of a BGM and of an ME and its wait; the start of a stream, the SDK's or a game's stream manager's; the calls
// of a state that plays on, an effect's block and the size of the start blocks; and the instructions that the hook may
// move.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/arm/cpu.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sdat/layout.h"
#include "ntdlib/testing/sdat_files.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "rip/sdat_set.h"
#include "sdat_test_code.h"
#include "stub_memory.h"

namespace
{

namespace arm = ntdlib::arm;
namespace sdat = ntdlib::sdat;

using twosf::rip::SdatCode;
using twosf::testing::ArchiveModel;
using twosf::testing::GameStubCode;
using twosf::testing::kBase;
using twosf::testing::kBgmStart;
using twosf::testing::kHeapStart;
using twosf::testing::kLevelWord;
using twosf::testing::kMeStart;
using twosf::testing::kOne;
using twosf::testing::kPop;
using twosf::testing::kPush;
using twosf::testing::kReturn;
using twosf::testing::kStack;
using twosf::testing::kStreamPlay;
using twosf::testing::kStreamSetUp;
using twosf::testing::kTempoSetter;
using twosf::testing::kThumbStart;
using twosf::testing::kTrackSetter;
using twosf::testing::Memory;
using twosf::testing::TestCode;
using xsflib::testing::Check;

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
    uint32_t restart_arg = 0xFFFFFFFF; // the argument of the restart's routine at the load, or none
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
    if (code.restart)
    {
        // The restart's routine stores its argument 12 bytes after its start: str r0, [pc, #4], then bx lr.
        xsflib::StoreLe32(memory.At(code.restart->routine), 0xE58F0004);
        xsflib::StoreLe32(memory.At(code.restart->routine + 4), kReturn);
        xsflib::StoreLe32(memory.At(code.restart->routine + 12), 0xFFFFFFFF);
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
        if (code.restart)
        {
            calls.restart_arg = xsflib::LoadLe32(memory.At(code.restart->routine + 12));
        }
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
          "the handle is in the stub");
    Check(loaded.r4_after_update == loaded.start_handle - 8,
          "the update's first instruction pushes the stub's registers");

    SdatCode with_restart = TestCode(true);
    with_restart.restart = twosf::rip::SoundRestart{.sequence = 86, .routine = kBase + 0x160, .argument = 1};
    const Calls restarted = RunStub(with_restart, kHeap, 86);
    Check(Order(restarted) == "loader start wait update wait" && restarted.restart_arg == 1,
          "the restart's sequence: the game's start-up with its argument, before the load");
    Check(restarted.load_args == std::vector<uint32_t>{86, kHeap} && restarted.start_sound == 86,
          "after the restart, the load and the start of the sequence");
    const Calls not_restarted = RunStub(with_restart, kHeap, 85);
    Check(Order(not_restarted) == "loader start wait update wait" && not_restarted.restart_arg == 0xFFFFFFFF,
          "another sequence: no restart");

    const Calls plain = RunStub(TestCode(false), kHeap, 7);
    Check(Order(plain) == "load start wait update wait", "without a loader, the load");
    Check(plain.load_args == std::vector<uint32_t>{7, 0xFF, kHeap, 1} && plain.load_stacked == 0,
          "the load takes the sequence, every kind, the heap, 1 and a null pointer on the stack");
}

// A call that a stub makes, as the routine sees it: its address and r0 to r3.
struct SeenCall
{
    friend bool operator==(const SeenCall&, const SeenCall&) = default;

    uint32_t routine = 0;
    std::array<uint32_t, 4> args = {};
};

// What the stub of a game in the table of games calls, in order, up to its first wait, and whether the frames follow.
struct GameCalls
{
    std::vector<SeenCall> calls;
    std::vector<uint32_t> wait_args; // r0 at each of the first two waits
    bool updated = false;
};

// Puts the routines `routines`, the wait and the update of `code` in `memory`. Each routine returns 1 at once, in ARM
// code or, with bit 0 set, in Thumb code.
void PlaceRoutines(Memory& memory, const SdatCode& code, std::span<const uint32_t> routines)
{
    constexpr uint16_t kThumbOne = 0x2001;    // movs r0, #1
    constexpr uint16_t kThumbReturn = 0x4770; // bx lr
    for (const uint32_t routine : routines)
    {
        if ((routine & 1) != 0)
        {
            xsflib::StoreLe16(memory.At(routine & ~1u), kThumbOne);
            xsflib::StoreLe16(memory.At((routine & ~1u) + 2), kThumbReturn);
        }
        else
        {
            xsflib::StoreLe32(memory.At(routine), kOne);
            xsflib::StoreLe32(memory.At(routine + 4), kReturn);
        }
    }

    xsflib::StoreLe32(memory.At(code.wait), kOne);
    xsflib::StoreLe32(memory.At(code.wait + 4), kReturn);
    xsflib::StoreLe32(memory.At(code.update), 0xEA000000); // the hook's branch, never run here
    xsflib::StoreLe32(memory.At(code.update + 4), kPop);
}

// Runs the stub of `code` in `memory` from its start, instruction by instruction, up to its second wait. The CPU starts
// in ARM state. Each routine of `routines` records its call.
GameCalls WatchStub(Memory& memory, const SdatCode& code, std::span<const uint32_t> routines)
{
    arm::Cpu cpu(memory, arm::Architecture::kArmV5TE);
    cpu.Jump(code.stub, arm::Mode::kSystem);
    cpu.SetReg(13, kStack);
    GameCalls seen;
    for (int step = 0; step < 2000 && seen.wait_args.size() < 2; step++)
    {
        const uint32_t pc = cpu.Pc();
        const bool thumb = (cpu.Cpsr() & 0x20) != 0;
        const uint32_t at = pc | (thumb ? 1u : 0u);
        if (std::ranges::find(routines, at) != routines.end())
        {
            seen.calls.push_back({at, {cpu.Reg(0), cpu.Reg(1), cpu.Reg(2), cpu.Reg(3)}});
        }
        if (pc == code.wait)
        {
            seen.wait_args.push_back(cpu.Reg(0));
        }
        if (pc == code.update + 4)
        {
            seen.updated = true;
        }
        cpu.Run(1);
    }

    return seen;
}

// Runs the stub of `code` with the game's starts, with the start block of `sound` in `state`, up to its second wait
// (WatchStub).
GameCalls RunGameStub(const SdatCode& code, uint32_t sound, std::optional<uint32_t> player,
                      const twosf::rip::GameState* state)
{
    const std::vector<uint32_t> routines = {kBgmStart, kMeStart, kThumbStart, kTrackSetter, kTempoSetter};
    Memory memory;
    PlaceRoutines(memory, code, routines);
    xsflib::StoreLe32(memory.At(kLevelWord), 105);

    const twosf::rip::SdatStub stub =
        twosf::rip::BuildSdatStub(code, 0, sound, twosf::rip::SoundKind::kSequence, player);
    const std::vector<uint8_t> block = twosf::rip::StartBlock(code, sound, player, state);
    Check(stub.bytes.size() <= code.stub_end - code.stub, "the stub fits in the decompressor's body");
    Check(block.size() == stub.sound_bytes, "every start block has the largest block's size");
    std::memcpy(memory.At(code.stub), stub.bytes.data(), stub.bytes.size());
    std::memcpy(memory.At(code.stub), block.data(), block.size());

    return WatchStub(memory, code, routines);
}

// A game's stub calls the routines of the sound's list with their arguments: the first list whose numbers contain the
// sound, or whose player is the record's, or else the default. A state's calls follow. Then each frame's wait, with its
// argument, and the update.
void TestGameStub()
{
    using twosf::rip::GameState;
    const SdatCode code = GameStubCode();
    const std::vector<const GameState*> states = twosf::rip::StatesOf(code, "SEQ_X");

    for (const uint32_t sound : {1u, 128u, 199u, 220u})
    {
        const GameCalls bgm = RunGameStub(code, sound, 0, nullptr);
        Check(bgm.calls == std::vector<SeenCall>{{kBgmStart, {sound, 0x100, 0, 0}}},
              "a sequence outside the ME numbers starts with the game's start of a BGM and its level, alone");
        Check(bgm.wait_args == std::vector<uint32_t>{0, 0} && bgm.updated,
              "then each frame's wait, with a null pointer, and the update");
    }

    for (const uint32_t sound : {200u, 209u, 219u})
    {
        const GameCalls me = RunGameStub(code, sound, 6, nullptr);
        Check(me.calls.size() == 1 && me.calls[0].routine == kMeStart && me.calls[0].args[0] == sound,
              "an ME number starts with the game's start of an ME, before the list of its player");
    }

    const GameCalls thumb = RunGameStub(code, 7, 6, nullptr);
    Check(thumb.calls.size() == 1 && thumb.calls[0].routine == kThumbStart && thumb.calls[0].args[0] == 7,
          "the record's player picks a list, and a Thumb routine runs in Thumb state");
    Check(thumb.wait_args.size() == 2 && thumb.updated, "back in ARM state for the frames");

    if (!Check(states.size() == 2 && states[0]->suffix.empty() && states[1]->suffix == "_S",
               "the states of a label: the one without a suffix first"))
    {
        return;
    }
    Check(twosf::rip::StatesOf(code, "SEQ_Y").empty() && twosf::rip::StatesOf(TestCode(true), "SEQ_X").empty(),
          "no states for another label, nor for a game outside the table");

    const GameCalls base = RunGameStub(code, 5, 0, states[0]);
    Check(base.calls == std::vector<SeenCall>{{kBgmStart, {5, 0x100, 0, 0}}, {kTrackSetter, {0xFFFF, 127, 0, 0}}},
          "the base's state: the start, then the state's calls");
    const GameCalls variant = RunGameStub(code, 5, 0, states[1]);
    Check(variant.calls == std::vector<SeenCall>{{kBgmStart, {5, 0x100, 0, 0}},
                                                 {kTrackSetter, {0x0300, 0, 0, 0}},
                                                 {kTempoSetter, {0x02F0ABCD, 105, 0, 0}}},
          "a state with a suffix: the start, then its calls in order, a word read from memory as the stub runs");
    Check(variant.wait_args == std::vector<uint32_t>{0, 0} && variant.updated, "then the frames");
}

// What a stream's stub passes to the start of a stream, and whether each frame's wait and update follow.
struct StreamCalls
{
    bool started = false;
    uint32_t handle = 0;
    uint32_t handle_word = 0xFFFFFFFF;
    uint32_t stream = 0;
    uint32_t offset = 0xFFFFFFFF;
    std::vector<uint32_t> wait_args; // r0 at each of the first two waits
    bool updated = false;
};

// Runs the stream's stub of `code` to the start of the stream, then up to its second wait.
StreamCalls RunStreamStub(const SdatCode& code, uint32_t stream)
{
    Memory memory;
    for (const uint32_t routine : {*code.stream_start, code.wait})
    {
        xsflib::StoreLe32(memory.At(routine), kOne);
        xsflib::StoreLe32(memory.At(routine + 4), kReturn);
    }
    xsflib::StoreLe32(memory.At(code.update), 0xEA000000); // the hook's branch, never run here
    xsflib::StoreLe32(memory.At(code.update + 4), kPop);

    const twosf::rip::SdatStub stub = twosf::rip::BuildSdatStub(code, 0, stream, twosf::rip::SoundKind::kStream);
    Check(stub.bytes.size() <= code.stub_end - code.stub, "the stream's stub fits in the decompressor's body");
    std::memcpy(memory.At(code.stub), stub.bytes.data(), stub.bytes.size());

    arm::Cpu cpu(memory, arm::Architecture::kArmV5TE);
    cpu.Jump(code.stub, arm::Mode::kSystem);
    cpu.SetReg(13, kStack);
    StreamCalls calls;
    cpu.SetStopAddress(*code.stream_start);
    calls.started = cpu.Run(100000) == arm::StopReason::kStopAddress;
    calls.handle = cpu.Reg(0);
    calls.handle_word = xsflib::LoadLe32(memory.At(cpu.Reg(0)));
    calls.stream = cpu.Reg(1);
    calls.offset = cpu.Reg(2);

    for (int i = 0; i < 2 && calls.started; i++)
    {
        cpu.SetStopAddress(code.wait);
        if (cpu.Run(100000) == arm::StopReason::kStopAddress)
        {
            calls.wait_args.push_back(cpu.Reg(0));
        }

        if (i == 0)
        {
            cpu.SetStopAddress(code.update + 4);
            calls.updated = cpu.Run(100000) == arm::StopReason::kStopAddress;
        }
    }

    return calls;
}

// The stub of a stream calls the start of a stream with an empty handle, the stream and 0, then each frame's wait and
// update. With a game's starts, the wait takes a null pointer. Without the start of a stream, the stub can't be built.
void TestStreamStub()
{
    SdatCode code = TestCode(true);
    code.stream_start = kBase + 0x190;
    SdatCode game_code = code;
    game_code.game_start = GameStubCode().game_start;
    game_code.wait_arguments = {0};
    const SdatCode none = TestCode(true);

    const StreamCalls plain = RunStreamStub(code, 22);
    const StreamCalls game = RunStreamStub(game_code, 3);
    bool threw = false;
    try
    {
        twosf::rip::BuildSdatStub(none, 0, 1, twosf::rip::SoundKind::kStream);
    }
    catch (const std::logic_error&)
    {
        threw = true;
    }

    Check(plain.started && plain.stream == 22 && plain.offset == 0 && plain.handle_word == 0,
          "the start of a stream takes an empty handle, the stream and 0");
    Check(plain.handle >= code.stub && plain.handle < code.stub_end, "the handle is in the stub");
    Check(plain.wait_args.size() == 2 && plain.updated, "then each frame's wait and the update");
    Check(game.started && game.stream == 3 && game.wait_args == std::vector<uint32_t>{0, 0} && game.updated,
          "with a game's starts, the stream starts the same, and the wait takes a null pointer");
    Check(threw, "no stream's stub without the start of a stream");
}

// A stub with a frame routine (BuildCallStub): the play routine once with the argument, then each frame the wait and
// the game's frame routine, whose call of the update the hook brings back to the stub. The stub then runs the update
// and returns to the frame routine. The test's frame routine pushes r4 and lr, calls the update and pops them.
void TestCallStubWithFrameRoutine()
{
    constexpr uint32_t kPlay = kBase + 0x200;
    constexpr uint32_t kFrame = kBase + 0x210;
    constexpr uint32_t kAfterUpdate = kFrame + 8;
    const SdatCode code = TestCode(true);
    Memory memory;
    PlaceRoutines(memory, code, std::vector<uint32_t>{kPlay});
    const auto branch = [](uint32_t from, uint32_t to)
    {
        return 0xEA000000 | (((to - from - 8) >> 2) & 0x00FFFFFF);
    };
    xsflib::StoreLe32(memory.At(code.update), branch(code.update, code.stub)); // the hook
    xsflib::StoreLe32(memory.At(kFrame), kPush);
    xsflib::StoreLe32(memory.At(kFrame + 4), 0xEB000000 | (((code.update - kFrame - 4 - 8) >> 2) & 0x00FFFFFF));
    xsflib::StoreLe32(memory.At(kAfterUpdate), kPop);

    const twosf::rip::SdatStub stub = twosf::rip::BuildCallStub(code, kPlay, 496, kFrame);
    Check(stub.bytes.size() <= code.stub_end - code.stub, "the stub with a frame routine fits");
    std::memcpy(memory.At(code.stub), stub.bytes.data(), stub.bytes.size());

    // The game enters the hook with a return address of its own, as a call of the update.
    arm::Cpu cpu(memory, arm::Architecture::kArmV5TE);
    cpu.Jump(code.update, arm::Mode::kSystem);
    cpu.SetReg(13, kStack);
    std::vector<std::string> order;
    std::vector<uint32_t> play_args;
    for (int step = 0; step < 4000 && order.size() < 12; step++)
    {
        const uint32_t pc = cpu.Pc();
        if (pc == kPlay)
        {
            order.emplace_back("play");
            play_args.push_back(cpu.Reg(0));
        }
        else if (pc == code.wait)
        {
            order.emplace_back("wait");
        }
        else if (pc == kFrame)
        {
            order.emplace_back("frame");
        }
        else if (pc == code.update + 4)
        {
            order.emplace_back("update");
        }
        else if (pc == kAfterUpdate)
        {
            order.emplace_back("back");
        }
        cpu.Run(1);
    }

    const std::vector<std::string> kExpected = {"play",  "wait",   "frame", "update", "back",  "wait",
                                                "frame", "update", "back",  "wait",   "frame", "update"};
    Check(order == kExpected, "the play routine once, then the wait, the frame routine and the update each frame");
    Check(play_args == std::vector<uint32_t>{496}, "the play routine takes the argument");
}

// Runs the stream's stub of `code`, a game whose stream manager starts its streams, for `stream`, up to its second
// wait (WatchStub). `sdk_start` records a call too.
GameCalls RunManagerStreamStub(const SdatCode& code, uint32_t stream, uint32_t sdk_start)
{
    const std::vector<uint32_t> routines = {kHeapStart, kStreamSetUp, kStreamPlay, sdk_start};
    Memory memory;
    PlaceRoutines(memory, code, routines);

    const twosf::rip::SdatStub stub = twosf::rip::BuildSdatStub(code, 0, stream, twosf::rip::SoundKind::kStream);
    Check(stub.bytes.size() <= code.stub_end - code.stub, "the manager's stream stub fits in the decompressor's body");
    std::memcpy(memory.At(code.stub), stub.bytes.data(), stub.bytes.size());

    return WatchStub(memory, code, routines);
}

// A game whose stream manager starts its streams: the stream's stub makes the manager's calls in order, with their
// constants and the stream's number, in place of the SDK's start. Then the frames follow. A stream's number that takes
// a literal fits too. Such a game's set contains streams without the SDK's start. A game in the table of games without
// the manager's calls and without the SDK's start contains none.
void TestManagerStreamStub()
{
    using twosf::rip::StubArgument;
    using Kind = StubArgument::Kind;
    constexpr uint32_t kSdkStart = kBase + 0x190;
    constexpr uint32_t kLargeStream = 0x00012345;
    const auto constant = [](uint32_t value)
    {
        return StubArgument{.kind = Kind::kConstant, .value = value};
    };
    SdatCode code = GameStubCode();
    const SdatCode no_streams = code;
    code.game_start->stream_calls = {
        {.routine = kHeapStart, .arguments = {constant(1), constant(0x16), constant(0x120000)}, .count = 3},
        {.routine = kStreamSetUp, .arguments = {constant(0x16)}, .count = 1},
        {.routine = kStreamPlay, .arguments = {StubArgument{.kind = Kind::kSound}}, .count = 1}};
    SdatCode both = code;
    both.stream_start = kSdkStart;

    const GameCalls small = RunManagerStreamStub(code, 0, kSdkStart);
    const GameCalls large = RunManagerStreamStub(code, kLargeStream, kSdkStart);
    const GameCalls with_sdk = RunManagerStreamStub(both, 7, kSdkStart);
    bool threw = false;
    try
    {
        twosf::rip::BuildSdatStub(no_streams, 0, 0, twosf::rip::SoundKind::kStream);
    }
    catch (const std::logic_error&)
    {
        threw = true;
    }

    Check(small.calls.size() == 3 && small.calls[0] == SeenCall{kHeapStart, {1, 0x16, 0x120000, 0}},
          "the manager's first call takes its three constants");
    Check(small.calls.size() == 3 && small.calls[1].routine == kStreamSetUp && small.calls[1].args[0] == 0x16,
          "then the manager's start-up, a Thumb routine, with its constant");
    Check(small.calls.size() == 3 && small.calls[2].routine == kStreamPlay && small.calls[2].args[0] == 0,
          "then the manager's start of the stream, with the stream's number");
    Check(small.wait_args == std::vector<uint32_t>{0, 0} && small.updated, "then each frame's wait and the update");
    Check(large.calls.size() == 3 && large.calls[2].routine == kStreamPlay && large.calls[2].args[0] == kLargeStream &&
              large.wait_args.size() == 2 && large.updated,
          "a stream's number that takes a literal reaches the manager's start, and the frames follow");
    Check(with_sdk.calls.size() == 3 && with_sdk.calls[2].routine == kStreamPlay && with_sdk.calls[2].args[0] == 7,
          "the manager's calls replace the SDK's start where the code has both");
    Check(twosf::rip::StartsStreams(code) && twosf::rip::StartsStreams(both),
          "a set of a game with the manager's calls contains streams, with or without the SDK's start");
    Check(threw && !twosf::rip::StartsStreams(no_streams),
          "a game in the table of games without either contains no streams, and its stream's stub can't be built");
}

// A state's call can take the result of the call before as its first argument, from an ARM or a Thumb routine, as a
// download-play program's piece opens a handle and copies a member through it. No other argument can.
void TestResultArgument()
{
    using twosf::rip::StubArgument;
    using Kind = StubArgument::Kind;
    constexpr uint16_t kThumbReturnValue = 0x2033;   // movs r0, #0x33
    constexpr uint16_t kThumbReturn = 0x4770;        // bx lr
    constexpr uint32_t kArmReturnValue = 0xE3A0002A; // mov r0, #0x2A
    const auto constant = [](uint32_t value)
    {
        return StubArgument{.kind = Kind::kConstant, .value = value};
    };
    const StubArgument result = {.kind = Kind::kResult};

    SdatCode code = GameStubCode();
    code.game_start->states.push_back(
        {.label = "SEQ_R",
         .suffix = "",
         .calls = {{.routine = kHeapStart, .arguments = {constant(1), constant(0x16)}, .count = 2},
                   {.routine = kStreamSetUp, .arguments = {result, constant(9)}, .count = 2},
                   {.routine = kBgmStart, .arguments = {result}, .count = 1}}});
    const std::vector<uint32_t> routines = {kBgmStart, kHeapStart, kStreamSetUp};
    Memory memory;
    PlaceRoutines(memory, code, routines);
    xsflib::StoreLe32(memory.At(kHeapStart), kArmReturnValue);
    xsflib::StoreLe16(memory.At(kStreamSetUp & ~1u), kThumbReturnValue);
    xsflib::StoreLe16(memory.At((kStreamSetUp & ~1u) + 2), kThumbReturn);
    const std::vector<const twosf::rip::GameState*> states = twosf::rip::StatesOf(code, "SEQ_R");
    const twosf::rip::SdatStub stub = twosf::rip::BuildSdatStub(code, 0, 5);
    const std::vector<uint8_t> block = twosf::rip::StartBlock(code, 5, 0, states.front());
    std::memcpy(memory.At(code.stub), stub.bytes.data(), stub.bytes.size());
    std::memcpy(memory.At(code.stub), block.data(), block.size());
    const GameCalls seen = WatchStub(memory, code, routines);
    // A call with one argument leaves r1 as the call before set it.
    Check(seen.calls == std::vector<SeenCall>{{kBgmStart, {5, 0x100, 0, 0}},
                                              {kHeapStart, {1, 0x16, 0, 0}},
                                              {kStreamSetUp, {0x2A, 9, 0, 0}},
                                              {kBgmStart, {0x33, 9, 0, 0}}},
          "each result reaches the next call's first argument, from an ARM routine and from a Thumb one");
    Check(seen.wait_args == std::vector<uint32_t>{0, 0} && seen.updated, "then the frames");

    const auto refused = [&code]
    {
        try
        {
            twosf::rip::StartBlock(code, 5, 0, twosf::rip::StatesOf(code, "SEQ_R").front());
        }
        catch (const std::logic_error&)
        {
            return true;
        }

        return false;
    };

    std::vector<twosf::rip::StubCall>& calls = code.game_start->states.back().calls;
    calls[1].arguments = {constant(9), result};
    Check(refused(), "no start block with a result as a second argument");

    // The state's list follows the game's list, but its first call has no call before it in the list.
    calls[1].arguments = {result, constant(9)};
    calls[0].arguments = {result, constant(0x16)};
    Check(refused(), "no start block with a result in a list's first call");
}

// The ARM forms of the update's first two Thumb instructions, and some that the hook can't move.
void TestRelocatableThumb()
{
    using twosf::rip::RelocatableThumb;
    using Pair = std::optional<std::array<uint32_t, 2>>;
    Check(RelocatableThumb(0x2400B510) == Pair({0xE92D4010, 0xE3B04000}), "push {r4, lr}, movs r4, #0");
    Check(RelocatableThumb(0xB5381C05) == Pair({0xE2905000, 0xE92D4038}), "adds r5, r0, #0, push {r3, r4, r5, lr}");
    Check(!RelocatableThumb(0xB081B518), "sub sp, #4");
    Check(!RelocatableThumb(0x2400B410), "push {r4} without lr");
    Check(!RelocatableThumb(0x4770B510), "bx lr");
}

// A Thumb update with the hook: a Thumb caller calls it, the hook's blx enters the stub in ARM state, the stub loads
// and starts the sequence, then each frame runs the update's first two instructions in ARM state and the rest in Thumb
// state. The rest returns to the stub in ARM state.
void TestThumbHook()
{
    constexpr uint32_t kCaller = kBase + 0x300; // Thumb: bl update, then b .
    SdatCode code = TestCode(true);
    code.update = kBase + 0x101;
    code.update_first = 0x2400B510; // push {r4, lr}, movs r4, #0
    Memory memory;
    for (const uint32_t routine : {code.start, *code.loader, code.wait})
    {
        xsflib::StoreLe32(memory.At(routine), kOne);
        xsflib::StoreLe32(memory.At(routine + 4), kReturn);
    }
    const uint32_t update = code.update & ~1u;
    xsflib::StoreLe32(memory.At(update), twosf::rip::HookWord(code));
    xsflib::StoreLe16(memory.At(update + 4), 0x205A); // movs r0, #0x5A
    xsflib::StoreLe16(memory.At(update + 6), 0xBD10); // pop {r4, pc}
    const uint32_t offset = update - (kCaller + 4);
    xsflib::StoreLe16(memory.At(kCaller), static_cast<uint16_t>(0xF000 | ((offset >> 12) & 0x7FF)));
    xsflib::StoreLe16(memory.At(kCaller + 2), static_cast<uint16_t>(0xF800 | ((offset >> 1) & 0x7FF)));
    xsflib::StoreLe16(memory.At(kCaller + 4), 0xE7FE);
    const twosf::rip::SdatStub stub = twosf::rip::BuildSdatStub(code, 0x0230A800, 3);
    std::memcpy(memory.At(code.stub), stub.bytes.data(), stub.bytes.size());

    arm::Cpu cpu(memory, arm::Architecture::kArmV5TE);
    cpu.Jump(kCaller | 1, arm::Mode::kSystem);
    cpu.SetReg(13, kStack);
    bool entered_arm = false;
    std::vector<uint32_t> after_update; // r0 at each wait after the first, and whether the CPU is in ARM state
    int waits = 0;
    for (int step = 0; step < 4000 && waits < 3; step++)
    {
        const bool thumb = (cpu.Cpsr() & 0x20) != 0;
        if (cpu.Pc() == code.stub && !thumb)
        {
            entered_arm = true;
        }
        if (cpu.Pc() == code.wait)
        {
            if (waits > 0)
            {
                after_update.push_back(thumb ? 0xFFFFFFFF : cpu.Reg(0));
            }
            waits++;
        }
        cpu.Run(1);
    }

    Check(entered_arm, "the hook's blx enters the stub in ARM state");
    Check(after_update == std::vector<uint32_t>{0x5A, 0x5A},
          "the update's rest runs in Thumb and returns in ARM state");
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

// What the stub of a state that plays on does, in order, as letters: s for the game's start, u for the update, w for a
// wait, f for the call of each frame and t for the timed call. The stub runs up to its `waits`-th wait.
std::string GameFrameEvents(const SdatCode& code, const twosf::rip::GameState& state, int waits)
{
    const std::vector<uint32_t> routines = {kBgmStart, kTrackSetter, kTempoSetter};
    Memory memory;
    PlaceRoutines(memory, code, routines);
    const twosf::rip::SdatStub stub = twosf::rip::BuildSdatStub(code, 0, 5);
    const std::vector<uint8_t> block = twosf::rip::StartBlock(code, 5, 0, &state);
    std::memcpy(memory.At(code.stub), stub.bytes.data(), stub.bytes.size());
    std::memcpy(memory.At(code.stub), block.data(), block.size());

    arm::Cpu cpu(memory, arm::Architecture::kArmV5TE);
    cpu.Jump(code.stub, arm::Mode::kSystem);
    cpu.SetReg(13, kStack);
    const std::map<uint32_t, char> letters = {
        {kBgmStart, 's'}, {kTrackSetter, 'f'}, {kTempoSetter, 't'}, {code.wait, 'w'}, {code.update + 4, 'u'}};
    std::string events;
    for (int step = 0; step < 20000 && std::ranges::count(events, 'w') < waits; step++)
    {
        const auto letter = letters.find(cpu.Pc());
        if (letter != letters.end())
        {
            events += letter->second;
        }
        cpu.Run(1);
    }

    return events;
}

// A state that plays on: the start, then the update that ends the start's game frame, then each game frame of two
// frames of the screen: two waits, the call of each frame, the timed call in its frame only, and the update.
void TestGameFrames()
{
    using twosf::rip::StubArgument;
    const StubArgument one = {.kind = StubArgument::Kind::kConstant, .value = 1};
    const twosf::rip::StubCall each = {.routine = kTrackSetter, .arguments = {one}, .count = 1};
    const twosf::rip::StubCall timed = {.routine = kTempoSetter, .arguments = {one}, .count = 1};
    twosf::rip::GameState state = {
        .label = "SEQ_P", .suffix = "_ON", .frame_calls = {each}, .timed_calls = {{2, timed}}, .video_frames = 2};
    SdatCode code = GameStubCode();
    code.game_start->states.push_back(state);

    Check(twosf::rip::PlaysOn(state) && !twosf::rip::PlaysOn(code.game_start->states.front()),
          "a state with calls in its frames plays on");
    Check(GameFrameEvents(code, state, 8) == "suwwfuwwftuwwfuww",
          "the start, the start's update, then each game frame: two waits, the frame's call, the timed call in game "
          "frame 2 alone, and the update");

    state.video_frames = 1;
    state.timed_calls = {};
    code.game_start->states.back() = state;
    Check(GameFrameEvents(code, state, 3) == "suwfuwfuw", "a game frame of one frame of the screen");
}

// An archive with sequence archive 1, the file 4: entry 0 at offset 0 with bank 0, entry 1 at 0x10 with bank 7.
sdat::Archive EffectArchive(std::vector<uint8_t>& bytes)
{
    namespace files = ntdlib::testing::sdat_files;
    sdat::ArchiveModel model = ArchiveModel();
    model.sequence_archives = {std::nullopt, sdat::SequenceArchiveInfo{.file_id = 4}};
    model.files.push_back(
        files::SequenceArchive({{.offset = 0, .bank = 0}, {.offset = 0x10, .bank = 7}}, files::Bytes(0x20, 0xFF)));
    bytes = sdat::BuildArchive(model);

    return sdat::Archive::Parse(bytes);
}

// An effect's block calls the game's effects' calls with the archive's number, the entry, the entry's bank, the heap
// and the packed id, padded to the size of every start block. A music mini's block keeps its size: the effects' block
// counts in it. A sequence outside the sequence archives has no effect's block.
void TestEffectBlock()
{
    using twosf::rip::StubArgument;
    using Kind = StubArgument::Kind;
    SdatCode code = GameStubCode();
    const twosf::rip::SdatStub before = twosf::rip::BuildSdatStub(code, 0, 5);
    code.game_start->effect_calls = {
        {.routine = kTrackSetter,
         .arguments = {StubArgument{.kind = Kind::kArchive}, StubArgument{.kind = Kind::kEntry},
                       StubArgument{.kind = Kind::kBank}, StubArgument{.kind = Kind::kHeap}},
         .count = 4},
        {.routine = kTempoSetter, .arguments = {StubArgument{.kind = Kind::kPackedEntry, .value = 10}}, .count = 1}};
    // Calls with words, larger than any list of the game.
    const StubArgument word = {.kind = Kind::kWord, .value = kLevelWord};
    for (int i = 0; i < 3; i++)
    {
        code.game_start->effect_calls.push_back(
            {.routine = kBgmStart, .arguments = {word, word, word, word}, .count = 4});
    }

    std::vector<uint8_t> bytes;
    const sdat::Archive archive = EffectArchive(bytes);
    const std::vector<uint32_t> routines = {kTrackSetter, kTempoSetter};
    Memory memory;
    PlaceRoutines(memory, code, std::vector<uint32_t>{kTrackSetter, kTempoSetter, kBgmStart});
    const twosf::rip::SdatStub stub = twosf::rip::BuildSdatStub(code, 0, 5);
    const std::vector<uint8_t> block = twosf::rip::EffectBlock(code, archive, {.index = 1, .archived = 1}, 0x02345678);
    Check(block.size() == stub.sound_bytes && stub.sound_bytes > before.sound_bytes,
          "the effect's block has the size of every start block, and every start block grows to hold it");
    std::memcpy(memory.At(code.stub), stub.bytes.data(), stub.bytes.size());
    std::memcpy(memory.At(code.stub), block.data(), block.size());
    const GameCalls seen = WatchStub(memory, code, routines);
    Check(seen.calls ==
              std::vector<SeenCall>{{kTrackSetter, {1, 1, 7, 0x02345678}}, {kTempoSetter, {0x401, 1, 7, 0x02345678}}},
          "the archive, the entry, its bank and the heap, then the archive shifted by 10 bits with the entry");
    Check(seen.wait_args == std::vector<uint32_t>{0, 0} && seen.updated, "then the frames");

    bool refused = false;
    try
    {
        twosf::rip::EffectBlock(code, archive, {.index = 0}, 0);
    }
    catch (const std::logic_error&)
    {
        refused = true;
    }
    Check(refused, "no effect's block for a sequence outside the sequence archives");
}

// The size of a set's start blocks: the largest that any list makes with any state, where it fits beside the frames.
// Otherwise the largest that the archive's sequences make with their states: a state of a label that the archive
// doesn't have doesn't count.
void TestStartBlockBytes()
{
    using twosf::rip::StubArgument;
    SdatCode code = GameStubCode();
    std::vector<uint8_t> bytes;
    const sdat::Archive archive = EffectArchive(bytes);
    const uint32_t any = twosf::rip::BuildSdatStub(code, 0, 5).sound_bytes;
    Check(twosf::rip::StartBlockBytes(code, archive) == any, "the largest block of any list and state, where it fits");

    // A long state of a label outside the archive. The block no longer fits beside the frames.
    const StubArgument word = {.kind = StubArgument::Kind::kWord, .value = 0x02FFFF00};
    twosf::rip::GameState far = {.label = "SEQ_FAR", .suffix = "_LONG"};
    for (uint32_t i = 0; i < 6; i++)
    {
        far.calls.push_back({.routine = kTrackSetter, .arguments = {word, word, word, word}, .count = 4});
    }
    code.game_start->states.push_back(far);
    const uint32_t exact = twosf::rip::StartBlockBytes(code, archive);
    Check(exact < any, "the archive's sequences alone, without the state of a label outside the archive");

    code.start_block = exact;
    const twosf::rip::SdatStub stub = twosf::rip::BuildSdatStub(code, 0, 5);
    Check(stub.sound_bytes == exact && stub.bytes.size() <= code.stub_end - code.stub,
          "a stub with the archive's size fits");
}

// The frames after a game's start blocks call the wait through r12 where that fits, and directly where only the direct
// call fits. Where neither fits, the stub's check refuses the game.
void TestDirectWaitAfterBlocks()
{
    SdatCode code = GameStubCode();
    const twosf::rip::SdatStub through_r12 = twosf::rip::BuildSdatStub(code, 0, 5);
    const auto room = static_cast<uint32_t>(through_r12.bytes.size());

    code.stub_end = code.stub + room;
    Check(twosf::rip::BuildSdatStub(code, 0, 5).bytes == through_r12.bytes,
          "a stub that fills the room exactly keeps the call through r12");

    code.stub_end = code.stub + room - 4;
    const twosf::rip::SdatStub direct = twosf::rip::BuildSdatStub(code, 0, 5);
    Check(direct.sound_bytes == through_r12.sound_bytes && direct.bytes.size() == room - 12,
          "the same blocks, and frames 12 bytes shorter: no literal and no mov lr, pc");
    Check(!twosf::rip::StubSizeProblem(code), "the stub fits");
    const std::vector<const twosf::rip::GameState*> states = twosf::rip::StatesOf(code, "SEQ_X");
    const GameCalls variant = RunGameStub(code, 5, 0, states.back());
    Check(variant.calls.size() == 3 && variant.wait_args == std::vector<uint32_t>{0, 0} && variant.updated,
          "the start and the state's calls, then each frame's wait and the update");

    code.stub_end = code.stub + room - 16;
    Check(twosf::rip::StubSizeProblem(code).has_value(), "a stub that doesn't fit either way is refused");
}

} // namespace

int main()
{
    TestStub();
    TestGameStub();
    TestStreamStub();
    TestCallStubWithFrameRoutine();
    TestManagerStreamStub();
    TestResultArgument();
    TestRelocatable();
    TestRelocatableThumb();
    TestThumbHook();
    TestGameFrames();
    TestEffectBlock();
    TestStartBlockBytes();
    TestDirectWaitAfterBlocks();

    return xsflib::testing::Summary("sdat_stub");
}
