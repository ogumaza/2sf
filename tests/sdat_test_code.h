// SPDX-License-Identifier: MIT

// The code and the archives that the tests of a set for a game on the SDK's sound archive player share: the
// instructions of the test's routines, a game's code in the test's memory, the game of the table of games that the
// stubs' tests run, and two small archives.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ntdlib/sdat/formats.h"
#include "ntdlib/sdat/layout.h"

#include "rip/sdat_set.h"
#include "stub_memory.h"

namespace twosf::testing
{

// bx lr, mov r0, #1, stmfd sp!, {r4, lr} and ldmfd sp!, {r4, pc}.
constexpr uint32_t kReturn = 0xE12FFF1E;
constexpr uint32_t kOne = 0xE3A00001;
constexpr uint32_t kPush = 0xE92D4010;
constexpr uint32_t kPop = 0xE8BD8010;

// Routines in the test's memory. Each returns 1 at once. The update pushes r4 and lr, then pops them into r4 and pc.
inline rip::SdatCode TestCode(bool loader)
{
    rip::SdatCode code;
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

// The routines of TestGameStub's game: ARM routines that return at once, and a Thumb one (bit 0 set).
constexpr uint32_t kBgmStart = kBase + 0x170;
constexpr uint32_t kMeStart = kBase + 0x180;
constexpr uint32_t kThumbStart = kBase + 0x191;
constexpr uint32_t kTrackSetter = kBase + 0x1A0;
constexpr uint32_t kTempoSetter = kBase + 0x1B0;
constexpr uint32_t kLevelWord = kBase + 0x1C0;

// The routines of TestManagerStreamStub's game: the start of a heap in ARM code, and the stream manager's start-up and
// its start of a stream in Thumb code (bit 0 set).
constexpr uint32_t kHeapStart = kBase + 0x1D0;
constexpr uint32_t kStreamSetUp = kBase + 0x1E1;
constexpr uint32_t kStreamPlay = kBase + 0x1F1;

// The game of TestGameStub: a BGM start with the sound and 0x100, an ME start for 200 to 219, a Thumb start for the
// sounds of player 6, and two states of SEQ_X, the one with a suffix listed first.
inline rip::SdatCode GameStubCode()
{
    using twosf::rip::StubArgument;
    using Kind = StubArgument::Kind;
    const StubArgument sound = {.kind = Kind::kSound};
    const auto constant = [](uint32_t value)
    {
        return StubArgument{.kind = Kind::kConstant, .value = value};
    };

    rip::SdatCode code = TestCode(false);
    code.wait = kBase + 0x160;
    code.wait_arguments = {0};
    code.game_start = twosf::rip::GameStart{
        .calls = {{.routine = kBgmStart, .arguments = {sound, constant(0x100)}, .count = 2}},
        .numbers = {{.ranges = {{200, 219}}, .calls = {{.routine = kMeStart, .arguments = {sound}, .count = 1}}},
                    {.player = 6, .calls = {{.routine = kThumbStart, .arguments = {sound}, .count = 1}}}},
        .states = {
            {.label = "SEQ_X",
             .suffix = "_S",
             .calls = {{.routine = kTrackSetter, .arguments = {constant(0x0300), constant(0)}, .count = 2},
                       {.routine = kTempoSetter,
                        .arguments = {constant(0x02F0ABCD), StubArgument{.kind = Kind::kWord, .value = kLevelWord}},
                        .count = 2}}},
            {.label = "SEQ_X",
             .suffix = "",
             .calls = {{.routine = kTrackSetter, .arguments = {constant(0xFFFF), constant(127)}, .count = 2}}}}};

    return code;
}

// An archive of one sequence (file 0), its bank (file 1), its wave archive (file 2) and a file that only the boot reads
// (file 3).
inline ntdlib::sdat::ArchiveModel ArchiveModel()
{
    ntdlib::sdat::ArchiveModel model;
    model.sequences = {ntdlib::sdat::SequenceInfo{0, 0, 100, 64, 32, 0, 0, ""}};
    model.banks = {ntdlib::sdat::BankInfo{1, {0, 0xFFFF, 0xFFFF, 0xFFFF}, ""}};
    model.wave_archives = {ntdlib::sdat::WaveArchiveInfo{2, ""}, ntdlib::sdat::WaveArchiveInfo{3, ""}};
    model.files = {std::vector<uint8_t>(64, 1), std::vector<uint8_t>(64, 2), std::vector<uint8_t>(64, 3),
                   std::vector<uint8_t>(64, 4)};
    model.file_alignment = 4;

    return model;
}

// An archive on 32 bytes whose sequence plays files 0 to 2, with a file of 0x1E0 bytes (3) in front of two files of
// 0x40 bytes (4 and 5), then a file of 0x400 bytes (6) in front of a file of 0x40 bytes (7).
inline ntdlib::sdat::ArchiveModel PagesModel()
{
    ntdlib::sdat::ArchiveModel model;
    model.sequences = {ntdlib::sdat::SequenceInfo{0, 0, 100, 64, 32, 0, 0, ""}};
    model.banks = {ntdlib::sdat::BankInfo{1, {0, 0xFFFF, 0xFFFF, 0xFFFF}, ""}};
    model.wave_archives = {ntdlib::sdat::WaveArchiveInfo{2, ""}};
    const std::vector<std::size_t> sizes = {64, 64, 64, 0x1E0, 0x40, 0x40, 0x400, 0x40};
    for (std::size_t id = 0; id < sizes.size(); id++)
    {
        model.files.emplace_back(sizes[id], static_cast<uint8_t>(id + 1));
    }

    return model;
}

} // namespace twosf::testing
