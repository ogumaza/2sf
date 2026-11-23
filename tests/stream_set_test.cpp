// SPDX-License-Identifier: MIT

// Tests the streams' parts of a set (rip/stream_set.h): the search for the start of a stream on a binary that the ARM
// assembler writes, the labels, the files and the rule for sound of an archive's streams, the first stream of each
// record (rip/sdat_set.h), the start of a stream's channels with and without a rate and with buffers that the player
// fills after the start, the rule for the lists after a stream's start, the run of a stream's mini with a backup memory
// and with a list after the start, the checks of a stream mini's runs, and a stream's timing.

#include "rip/stream_set.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/arm/assembler.h"
#include "ntdlib/ntr/code.h"
#include "ntdlib/ntr/length_model.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/sdat/files.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sdat/layout.h"
#include "xsflib/rip/length.h"
#include "xsflib/testing/check.h"

#include "backup_image.h"
#include "rip/boots.h"
#include "rip/sdat_set.h"

namespace
{

namespace arm = ntdlib::arm;
namespace sdat = ntdlib::sdat;

using xsflib::testing::Check;

constexpr uint32_t kBase = 0x02000000;

// Registers as the bits of a push or a pop.
constexpr uint16_t kR4 = 1u << 4;
constexpr uint16_t kR5 = 1u << 5;
constexpr uint16_t kR6 = 1u << 6;
constexpr uint16_t kLr = 1u << 14;
constexpr uint16_t kPc = 1u << 15;

// A binary of the shapes of the SDK's routines, and of routines that are almost the same. `starts` gets the starts of
// a stream in it.
ntdlib::ntr::LoadedCode StreamBinary(std::vector<uint32_t>& starts)
{
    arm::ArmAssembler a(kBase);
    const int get_info = a.NewLabel();
    const int player_prepare = a.NewLabel();
    const int start_prepared = a.NewLabel();
    const int prepare = a.NewLabel();
    const int wrong_prepare = a.NewLabel();

    // The callees: each returns at once.
    for (const int label : {get_info, player_prepare, start_prepared})
    {
        a.Bind(label);
        a.Push(kR4 | kLr);
        a.Pop(kR4 | kPc);
    }

    // The prepare: the record of the stream, its priority and player, then the player's prepare with the handle.
    // `field` is the offset of the priority.
    const auto write_prepare = [&](int label, int field)
    {
        a.Bind(label);
        a.Push(kR4 | kR5 | kR6 | kLr);
        a.MovRegister(5, 1);
        a.MovRegister(6, 0);
        a.MovRegister(0, 5);
        a.MovRegister(4, 2);
        a.Bl(get_info);
        a.MovRegister(1, 0);
        a.Ldrb(2, 1, 6);
        a.Ldrb(3, 1, field);
        a.MovRegister(0, 6);
        a.Bl(player_prepare);
        a.Pop(kR4 | kR5 | kR6 | kPc);
    };
    write_prepare(prepare, 5);
    write_prepare(wrong_prepare, 4);

    // A start: the prepare with the three arguments, then the start of the prepared stream with the handle.
    const auto write_start = [&](int callee, bool handle_again)
    {
        starts.push_back(a.Here());
        a.Push(kR4 | kLr);
        a.MovRegister(4, 0);
        a.Bl(callee);
        if (handle_again)
        {
            a.MovRegister(0, 4);
        }
        a.Bl(start_prepared);
        a.Mov(0, 1);
        a.Pop(kR4 | kPc);
    };
    write_start(prepare, true);
    write_start(wrong_prepare, true);
    write_start(get_info, true);
    write_start(prepare, false);

    return {.address = kBase, .bytes = a.Finish()};
}

// The start whose prepare reads the record's priority and player, and passes the handle on. Not a start whose prepare
// reads another field, whose first call isn't a prepare, or whose second call doesn't take the handle.
void TestStreamStarts()
{
    std::vector<uint32_t> starts;
    const ntdlib::ntr::LoadedCode binary = StreamBinary(starts);

    const std::vector<uint32_t> found = twosf::rip::FindStreamStarts(binary);

    Check(found == std::vector<uint32_t>{starts[0]}, "the one start of a stream");
}

// The same shapes in Thumb code, in the form of Pokémon Black 2's SDK: the callees, the prepare that reads the priority
// (a byte at 5) and the player (a byte at 6), and the start. `start` gets the start's address, with bit 0 set.
ntdlib::ntr::LoadedCode ThumbStreamBinary(uint32_t& start)
{
    constexpr uint32_t kGetInfo = 0x00;
    constexpr uint32_t kPlayerPrepare = 0x04;
    constexpr uint32_t kStartPrepared = 0x08;
    constexpr uint32_t kPrepare = 0x0C;
    constexpr uint32_t kStart = 0x30;
    std::vector<uint16_t> code(0x48 / 2, 0);
    const auto put = [&code](uint32_t at, std::initializer_list<uint16_t> halfwords)
    {
        for (const uint16_t halfword : halfwords)
        {
            code[at / 2] = halfword;
            at += 2;
        }
    };
    // bl from `at` to `to`: the high half of the offset, then the low.
    const auto bl = [&put](uint32_t at, uint32_t to)
    {
        const uint32_t offset = to - (at + 4);
        put(at, {static_cast<uint16_t>(0xF000 | ((offset >> 12) & 0x7FF)),
                 static_cast<uint16_t>(0xF800 | ((offset >> 1) & 0x7FF))});
    };
    for (const uint32_t callee : {kGetInfo, kPlayerPrepare, kStartPrepared})
    {
        put(callee, {0xB510, 0xBD10}); // push {r4, lr}, pop {r4, pc}
    }

    // push {r4, r5, r6, lr}, adds r5, r1, #0, adds r6, r0, #0, adds r0, r5, #0, adds r4, r2, #0
    put(kPrepare, {0xB570, 0x1C0D, 0x1C06, 0x1C28, 0x1C14});
    bl(kPrepare + 0x0A, kGetInfo);
    // adds r1, r0, #0, ldrb r2, [r1, #6], ldrb r3, [r1, #5], adds r0, r6, #0
    put(kPrepare + 0x0E, {0x1C01, 0x798A, 0x794B, 0x1C30});
    bl(kPrepare + 0x16, kPlayerPrepare);
    put(kPrepare + 0x1A, {0xBD70}); // pop {r4, r5, r6, pc}

    put(kStart, {0xB510, 0x1C04}); // push {r4, lr}, adds r4, r0, #0
    bl(kStart + 0x04, kPrepare);
    put(kStart + 0x08, {0x1C20}); // adds r0, r4, #0
    bl(kStart + 0x0A, kStartPrepared);
    put(kStart + 0x0E, {0x2001, 0xBD10}); // movs r0, #1, pop {r4, pc}

    start = (kBase + kStart) | 1;
    std::vector<uint8_t> bytes;
    for (const uint16_t halfword : code)
    {
        bytes.push_back(static_cast<uint8_t>(halfword));
        bytes.push_back(static_cast<uint8_t>(halfword >> 8));
    }

    return {.address = kBase, .bytes = std::move(bytes)};
}

// The search reads Thumb code too.
void TestThumbStreamStart()
{
    uint32_t start = 0;
    const ntdlib::ntr::LoadedCode binary = ThumbStreamBinary(start);

    Check(twosf::rip::FindStreamStarts(binary) == std::vector<uint32_t>{start}, "the start of a stream in Thumb code");
}

// A stream file of one channel at 32,728 Hz: a sine, or silence.
std::vector<uint8_t> StreamOf(bool audible)
{
    std::vector<int16_t> samples(4096, 0);
    for (std::size_t i = 0; i < samples.size() && audible; i++)
    {
        samples[i] = static_cast<int16_t>(8000.0 * std::sin(0.05 * static_cast<double>(i)));
    }

    return sdat::EncodeStream(std::vector<std::vector<int16_t>>{samples}, 32728, sdat::WaveEncoding::kImaAdpcm,
                              std::nullopt);
}

// Streams 0 and 1 play a sine, stream 2 silence, stream 3 the sine at volume 0, stream 4 a file past the FAT, and
// stream 5 has no record. Stream 1 has a name.
sdat::Archive StreamArchive(std::vector<uint8_t>& bytes)
{
    sdat::ArchiveModel model;
    model.file_alignment = 4;
    model.files = {StreamOf(true), StreamOf(false)};
    model.streams = {sdat::StreamInfo{.file_id = 0, .volume = 100}, sdat::StreamInfo{.file_id = 0, .volume = 100},
                     sdat::StreamInfo{.file_id = 1, .volume = 100}, sdat::StreamInfo{.file_id = 0, .volume = 0},
                     sdat::StreamInfo{.file_id = 7, .volume = 100}, std::nullopt};
    sdat::Symbols symbols;
    symbols.names[7] = {std::nullopt, "STRM_BGM_TITLE", std::nullopt, std::nullopt, std::nullopt, std::nullopt};
    model.symbols = symbols;
    bytes = sdat::BuildArchive(model);

    return sdat::Archive::Parse(bytes);
}

void TestStreams()
{
    std::vector<uint8_t> bytes;
    const sdat::Archive archive = StreamArchive(bytes);

    Check(twosf::rip::StreamLabel(archive, 0) == "STRM_0" && twosf::rip::StreamLabel(archive, 1) == "STRM_BGM_TITLE",
          "a stream's name, or its number");
    Check(twosf::rip::StreamFile(archive, 1) == 0u && twosf::rip::StreamFile(archive, 2) == 1u, "the stream's file");
    Check(!twosf::rip::StreamFile(archive, 4) && !twosf::rip::StreamFile(archive, 5) &&
              !twosf::rip::StreamFile(archive, 6),
          "no file past the FAT, without a record, or past the table");
    Check(twosf::rip::StreamAudible(archive, 0), "a sine makes a sound");
    Check(!twosf::rip::StreamAudible(archive, 2), "silence makes none");
    Check(!twosf::rip::StreamAudible(archive, 3), "nor does a volume of 0");
    Check(!twosf::rip::StreamAudible(archive, 4), "nor does a stream without a file");
}

// Stream 1 has stream 0's record under another name, and stream 8 has stream 2's. Streams 2 to 6 each differ from
// stream 0 in one field: the volume, the player priority, the player, the flags and the reserved bytes. Stream 7 has no
// record, and stream 9 another file. A game in the table of games starts its streams with the SDK's start too. Its
// streams share records the same way. Where the table gives the calls of the game's stream manager, the game's routines
// start each stream, and every stream maps to itself.
void TestStreamRecords()
{
    const sdat::StreamInfo base{.file_id = 0, .volume = 90, .player_priority = 64, .player = 0, .flags = 0};
    sdat::ArchiveModel model;
    model.file_alignment = 4;
    model.files = {StreamOf(true), StreamOf(true)};
    model.streams = std::vector<std::optional<sdat::StreamInfo>>(10, base);
    model.streams[2]->volume = 127;
    model.streams[3]->player_priority = 32;
    model.streams[4]->player = 1;
    model.streams[5]->flags = 1;
    model.streams[6]->reserved = {1, 2, 3, 4};
    model.streams[7].reset();
    model.streams[8]->volume = 127;
    model.streams[9]->file_id = 1;
    sdat::Symbols symbols;
    symbols.names[7].resize(model.streams.size());
    symbols.names[7][1] = "STRM_A";
    model.symbols = symbols;
    const std::vector<uint8_t> bytes = sdat::BuildArchive(model);
    const sdat::Archive archive = sdat::Archive::Parse(bytes);
    const std::vector<uint32_t> all = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    const std::vector<uint32_t> reordered = {8, 1, 2, 0};
    const twosf::rip::SdatCode sdk_start;
    twosf::rip::SdatCode game_starts;
    game_starts.game_start = twosf::rip::GameStart{};
    twosf::rip::SdatCode manager_starts;
    manager_starts.game_start = twosf::rip::GameStart{.stream_calls = {{.routine = 0x02007055, .count = 1}}};

    const std::vector<uint32_t> firsts =
        twosf::rip::FirstOfRecord(sdk_start, archive, all, twosf::rip::SoundKind::kStream);
    const std::vector<uint32_t> reordered_firsts =
        twosf::rip::FirstOfRecord(sdk_start, archive, reordered, twosf::rip::SoundKind::kStream);
    const std::vector<uint32_t> game_firsts =
        twosf::rip::FirstOfRecord(game_starts, archive, all, twosf::rip::SoundKind::kStream);
    const std::vector<uint32_t> manager_firsts =
        twosf::rip::FirstOfRecord(manager_starts, archive, all, twosf::rip::SoundKind::kStream);

    Check(firsts == std::vector<uint32_t>{0, 0, 2, 3, 4, 5, 6, 7, 2, 9},
          "a stream with another name plays as the first of its record, and one with other reserved bytes doesn't");
    Check(reordered_firsts == std::vector<uint32_t>{8, 1, 8, 1}, "the first in the set's order");
    Check(game_firsts == firsts, "a game in the table of games shares the records of its streams");
    Check(manager_firsts == all, "each stream of a game whose stream manager starts them maps to itself");
}

// The stream player's refills: a read after the start counts when it reaches bytes of the file that the reads before
// the start didn't. Reads before the start that cover the whole file count too.
void TestStreamFileReads()
{
    constexpr uint32_t kStart = 0x1000;
    constexpr uint32_t kEnd = 0x5000;
    twosf::rip::StreamFileReads refill(kStart, kEnd);
    twosf::rip::StreamFileReads reread(kStart, kEnd);
    twosf::rip::StreamFileReads outside(kStart, kEnd);
    twosf::rip::StreamFileReads whole(kStart, kEnd);
    for (twosf::rip::StreamFileReads* reads : {&refill, &reread, &outside})
    {
        reads->Add(kStart, 0x800, false);
        reads->Add(kStart + 0x2000, 0x800, false);
    }
    whole.Add(kStart - 0x100, 0x2100, false);
    whole.Add(kStart + 0x2000, 0x2000, false);

    refill.Add(kStart + 0x800, 0x800, true);
    reread.Add(kStart + 0x200, 0x400, true);
    reread.Add(kStart + 0x2000, 0x800, true);
    outside.Add(kEnd, 0x800, true);
    outside.Add(kStart - 0x800, 0x800, true);
    whole.Add(kStart, 0x10, true);

    Check(refill.Refilled(), "a read of the block's next bytes after the start");
    Check(!reread.Refilled(), "reads of bytes that the first buffers contain");
    Check(!outside.Refilled(), "reads outside the file");
    Check(whole.Refilled(), "reads before the start that cover the file");
}

// A list that sets up hardware channels 6 and 7 to play `bytes` bytes at 0x02001000 and 0x02002000, with timers of
// 0x200 and `timer7`, then starts both.
ntdlib::ntr::snd::CommandList StartList(uint32_t bytes, uint32_t timer7)
{
    using ntdlib::ntr::snd::CommandId;
    const uint32_t words = bytes / 4;

    return {{.id = CommandId::kSetUpPcmChannel, .args = {6 | 0x200u << 16, 0x02001000, words, 0}},
            {.id = CommandId::kSetUpPcmChannel, .args = {7 | timer7 << 16, 0x02002000, words, 0}},
            {.id = CommandId::kStartTimer, .args = {0xC0, 0, 1, 0}}};
}

// The start of a stream's channels from the driver's commands. The stream is PCM8 with one block for each channel,
// longer than the buffer of 0x800 bytes that each hardware channel plays: the buffers that contain each block's start
// count. A wrong buffer doesn't, a silent start matches either channel, and timers that differ fail.
void TestReadCommands()
{
    constexpr uint32_t kBlock = 0x3000;
    constexpr uint32_t kBuffer = 0x800;
    std::vector<std::vector<uint8_t>> heads;
    for (uint8_t channel = 0; channel < 2; channel++)
    {
        std::vector<uint8_t> block(kBlock);
        for (uint32_t i = 0; i < kBlock; i++)
        {
            block[i] = static_cast<uint8_t>(i * 7 + channel * 50 + 1);
        }
        heads.push_back(block);
        heads.emplace_back(2 * kBlock, uint8_t{0xEE});
    }

    const std::vector<std::vector<uint8_t>> silent_heads = {std::vector<uint8_t>(kBlock), std::vector<uint8_t>(kBlock),
                                                            std::vector<uint8_t>(kBlock), std::vector<uint8_t>(kBlock)};

    std::vector<uint8_t> memory(0x4000);
    std::copy_n(heads[0].begin(), kBuffer, memory.begin() + 0x1000);
    std::copy_n(heads[2].begin(), kBuffer, memory.begin() + 0x2000);
    std::vector<uint8_t> wrong = memory;
    wrong[0x2000 + 0x100] ^= 0xFF;
    const std::vector<uint8_t> silent(0x4000);

    std::vector<std::string> errors;
    const auto fail = [&errors](std::string error)
    {
        errors.push_back(std::move(error));
    };
    twosf::rip::ChannelStart buffers;
    twosf::rip::ChannelStart wrong_buffer;
    twosf::rip::ChannelStart silent_start;
    twosf::rip::ChannelStart other_timer;
    ntdlib::ntr::snd::CommandList with_level = StartList(kBuffer, 0x200);
    with_level.push_back({.id = ntdlib::ntr::snd::CommandId::kChannelVolume, .args = {0xC0, 100, 0, 0}});

    const std::size_t read = twosf::rip::ReadCommands(with_level, memory, heads, 2, buffers, fail);
    const std::size_t read_wrong =
        twosf::rip::ReadCommands(StartList(kBuffer, 0x200), wrong, heads, 2, wrong_buffer, fail);
    twosf::rip::ReadCommands(StartList(kBuffer, 0x200), silent, silent_heads, 2, silent_start, fail);
    const std::size_t errors_before = errors.size();
    twosf::rip::ReadCommands(StartList(kBuffer, 0x100), memory, heads, 2, other_timer, fail);

    Check(buffers.started && buffers.hardware == std::set<uint32_t>{6, 7} && buffers.timer == 0x200 &&
              std::abs(buffers.rate - ntdlib::ntr::kSystemClockHz / 2.0 / 0x200) < 1e-6,
          "buffers that contain the start of each channel's block");
    Check(read == 3, "the read stops after the command that starts the channels");
    Check(!wrong_buffer.started && wrong_buffer.hardware.empty() && read_wrong == 3,
          "a buffer that differs from its block's start, with the whole list read");
    Check(silent_start.started && silent_start.hardware == std::set<uint32_t>{6, 7},
          "a silent start matches either channel");
    Check(errors_before == 0 && errors.size() == 1, "channels with different timers fail");
}

// The start of a stream whose player fills the buffers after the start (ReadCommands with `heads_after_start`): each
// buffer that the start command's channels play counts as one of the stream's channels, and the heads aren't read. A
// stream of one channel that plays its buffer on channels 6 and 7 starts both. Two buffers make two channels, and one
// buffer can't be two.
void TestReadCommandsHeadsAfterStart()
{
    using ntdlib::ntr::snd::CommandId;
    const ntdlib::ntr::snd::CommandList one_buffer = {
        {.id = CommandId::kSetUpPcmChannel, .args = {6 | 0x200u << 16, 0x02001000, 0x100, 0}},
        {.id = CommandId::kSetUpPcmChannel, .args = {7 | 0x200u << 16, 0x02001000, 0x100, 0}},
        {.id = CommandId::kStartTimer, .args = {0xC0, 0, 1, 0}}};
    const std::vector<std::vector<uint8_t>> heads = {std::vector<uint8_t>(0x200, 0x11),
                                                     std::vector<uint8_t>(0x200, 0x11)};
    const std::vector<uint8_t> memory(0x4000);
    const auto fail = [](const std::string&) {};
    const double rate = ntdlib::ntr::kSystemClockHz / 2.0 / 0x200;

    twosf::rip::ChannelStart mono;
    twosf::rip::ChannelStart stereo;
    twosf::rip::ChannelStart too_few;
    twosf::rip::ChannelStart read_heads;
    twosf::rip::ReadCommands(one_buffer, memory, heads, 1, mono, fail, rate, true);
    twosf::rip::ReadCommands(StartList(0x400, 0x200), memory, heads, 2, stereo, fail, rate, true);
    twosf::rip::ReadCommands(one_buffer, memory, heads, 2, too_few, fail, rate, true);
    twosf::rip::ReadCommands(one_buffer, memory, heads, 1, read_heads, fail, rate);
    Check(mono.started && mono.hardware == std::set<uint32_t>{6, 7}, "one buffer on two channels");
    Check(stereo.started && stereo.hardware == std::set<uint32_t>{6, 7}, "two buffers");
    Check(!too_few.started, "one buffer for a stream of two channels");
    Check(!read_heads.started, "without heads_after_start, the empty buffers don't match the heads");
}

// The start of a game stream's channel with a rate (ReadCommands): a setup counts only when its timer gives the rate
// within 1 %. Channel 6 plays the head with a timer of 0x200, 32,728.5 Hz. A rate 0.5 % off finds it. Rates 2 % off
// either way don't, and the whole list is read. A later setup at another rate drops the first.
void TestReadCommandsAtRate()
{
    using ntdlib::ntr::snd::CommandId;
    constexpr double kRate = ntdlib::ntr::kSystemClockHz / 2.0 / 0x200;
    const std::vector<uint8_t> head(0x200, 0x5A);
    const std::vector<std::vector<uint8_t>> heads = {head, head};
    std::vector<uint8_t> memory(0x4000);
    std::copy(head.begin(), head.end(), memory.begin() + 0x1000);
    const ntdlib::ntr::snd::CommandList list = {
        {.id = CommandId::kSetUpPcmChannel, .args = {6 | 0x200u << 16, 0x02001000, 0x80, 0}},
        {.id = CommandId::kStartTimer, .args = {0x40, 0, 1, 0}}};
    const ntdlib::ntr::snd::CommandList reset = {
        {.id = CommandId::kSetUpPcmChannel, .args = {6 | 0x200u << 16, 0x02001000, 0x80, 0}},
        {.id = CommandId::kSetUpPcmChannel, .args = {6 | 0x400u << 16, 0x02001000, 0x80, 0}},
        {.id = CommandId::kStartTimer, .args = {0x40, 0, 1, 0}}};
    std::vector<std::string> errors;
    const auto fail = [&errors](std::string error)
    {
        errors.push_back(std::move(error));
    };
    twosf::rip::ChannelStart near;
    twosf::rip::ChannelStart low;
    twosf::rip::ChannelStart high;
    twosf::rip::ChannelStart later;

    twosf::rip::ReadCommands(list, memory, heads, 1, near, fail, kRate * 1.005);
    const std::size_t read_low = twosf::rip::ReadCommands(list, memory, heads, 1, low, fail, kRate * 0.98);
    twosf::rip::ReadCommands(list, memory, heads, 1, high, fail, kRate * 1.02);
    twosf::rip::ReadCommands(reset, memory, heads, 1, later, fail, kRate);

    Check(near.started && near.hardware == std::set<uint32_t>{6} && std::abs(near.rate - kRate) < 1e-6,
          "a setup within 1 % of the rate");
    Check(!low.started && read_low == 2, "a setup 2 % under the rate, with the whole list read");
    Check(!high.started, "a setup 2 % over the rate");
    Check(!later.started && later.setups.empty(), "a later setup at another rate drops the channel");
    Check(errors.empty(), "no error");
}

// The timing of a stream from its samples and its rate (StreamTiming). A stream with a loop plays its intro and
// kLengthLoops passes of its loop, then fades. A stream without one plays to its end, and so does one whose loop starts
// at its end.
void TestStreamTiming()
{
    const xsflib::rip::Timing looped = twosf::rip::StreamTiming(48000, 16000, true, 16000);
    const xsflib::rip::Timing ends = twosf::rip::StreamTiming(48000, 16000, false, 16000);
    const xsflib::rip::Timing loop_at_end = twosf::rip::StreamTiming(48000, 48000, true, 16000);
    const xsflib::rip::Timing rounded = twosf::rip::StreamTiming(48001, 0, false, 16000);

    Check(looped.loop.has_value() && looped.loop->start_ms == 1000 && looped.loop->length_ms == 2000,
          "the loop's pass");
    Check(looped.length_ms == 1000 + xsflib::rip::kLengthLoops * 2000, "the length of a loop");
    Check(looped.fade_ms == static_cast<long long>(ntdlib::ntr::LengthRules().fade_ms), "the fade of a loop");
    Check(!ends.loop && ends.length_ms == 3000 && ends.fade_ms == 0, "a stream that ends");
    Check(!loop_at_end.loop && loop_at_end.length_ms == 3000, "a loop that starts at the end");
    Check(rounded.length_ms == 3000, "the length rounds to the millisecond");
}

// The setup of a channel that plays a whole stream, as ReadCommands reads it, against the stream's record
// (SetupLoopError): the loop's start and the data's end in words, at 4 samples a word in PCM8, 2 in PCM16 and 8 in
// IMA-ADPCM, and whether the channel repeats its loop.
void TestSetupLoop()
{
    using ntdlib::ntr::snd::CommandId;
    using twosf::rip::SetupLoopError;
    // The setup of channel 6 in `format`: a loop from word 0x10, 0x30 words long, repeated when `repeat` is 1.
    const auto setup = [](uint32_t format, uint32_t repeat)
    {
        const ntdlib::ntr::snd::CommandList list = {
            {.id = CommandId::kSetUpPcmChannel,
             .args = {6 | 0x200u << 16, 0x02001000, 0x30, 0x10 | format << 24 | repeat << 26}}};
        twosf::rip::ChannelStart start;
        twosf::rip::ReadCommands(list, {}, {}, 1, start, [](const std::string&) {});
        return start.setups.at(6);
    };
    const twosf::rip::ChannelSetup pcm8 = setup(0, 1);
    const twosf::rip::ChannelSetup once = setup(0, 2);

    Check(pcm8.format == 0 && pcm8.loops && pcm8.loop_start == 0x10 && pcm8.bytes == 0x100, "the setup's loop");
    Check(!SetupLoopError(pcm8, 0x100, 0x40, true), "PCM8: 4 samples a word");
    Check(!SetupLoopError(setup(1, 1), 0x80, 0x20, true), "PCM16: 2 samples a word");
    Check(!SetupLoopError(setup(2, 1), 0x200, 0x80, true), "IMA-ADPCM: 8 samples a word, the header's word included");
    Check(
        SetupLoopError(pcm8, 0x100, 0x44, true) ==
            "the stream's channel plays 256 samples with a loop from 64, and its record gives 256 with a loop from 68",
        "another loop start");
    Check(SetupLoopError(pcm8, 0x104, 0x40, true).has_value(), "other samples");
    Check(SetupLoopError(pcm8, 0x100, 0x40, false) == "the stream's channel repeats a loop, and its record has none",
          "a record without a loop");
    Check(
        SetupLoopError(once, 0x100, 0x40, true) == "the stream's record has a loop, and its channel doesn't repeat one",
        "a channel that plays once");
    Check(!SetupLoopError(once, 0x100, 0x44, false), "without a loop, only the samples count");
    Check(SetupLoopError(once, 0xFC, 0, false).has_value(), "without a loop, other samples");
    Check(SetupLoopError(setup(3, 1), 0x100, 0x40, true) == "the stream's channel plays neither PCM nor IMA-ADPCM",
          "a PSG setup");
}

// The error of ReadCommandsAfterStart on `lists`, one after another, after the start of hardware channels 6 and 7 at a
// timer of 0x200. Nothing when every list passes.
std::optional<std::string> AfterStartError(const std::vector<ntdlib::ntr::snd::CommandList>& lists,
                                           bool ends_in_run = false)
{
    twosf::rip::ChannelStart start;
    start.started = true;
    start.hardware = {6, 7};
    start.timer = 0x200;
    std::optional<std::string> error;
    const auto fail = [&error](std::string message)
    {
        error = error.value_or(std::move(message));
    };

    for (const ntdlib::ntr::snd::CommandList& list : lists)
    {
        twosf::rip::ReadCommandsAfterStart(list, start, ends_in_run, fail);
    }

    return error;
}

// A command of `id` with `args`.
ntdlib::ntr::snd::Command Command(ntdlib::ntr::snd::CommandId id, std::array<uint32_t, 4> args = {})
{
    return {.id = id, .args = args};
}

// The rule of a stream's run after the start, point by point: the starts and setups of the stream's channels (1), their
// stops, unless the stream ends in the run (2), another timer (3), a volume of 0 or a second level (4), the commands of
// a sequence, a player or a track, and the master volume (5). The pans, the alarms, the timers of other channels and
// every command for other channels pass.
void TestReadCommandsAfterStart()
{
    using ntdlib::ntr::snd::CommandId;
    const std::string prefix = "a driver command after the start ";
    const std::string sets_up = prefix + "sets up one of the stream's channels";
    const std::string starts = prefix + "starts one of the stream's channels";
    const std::string stops = prefix + "stops one of the stream's channels";
    const std::string timer = prefix + "sets another timer for one of the stream's channels";
    const std::string silences = prefix + "silences one of the stream's channels";
    const std::string second_level = prefix + "sets a second level for the stream's channels";

    // 1: a start or a setup of one of the stream's channels.
    Check(AfterStartError({{Command(CommandId::kStartTimer, {0x80, 0, 0, 0})}}) == starts, "a start of channel 7");
    Check(
        AfterStartError({{Command(CommandId::kSetUpPcmChannel, {6 | 0x200u << 16, 0x02001000, 0x200, 0})}}) == sets_up,
        "a PCM setup of channel 6");
    Check(AfterStartError({{Command(CommandId::kSetUpPsgChannel, {7, 0, 0, 0})}}) == sets_up,
          "a PSG setup of channel 7");
    Check(AfterStartError({{Command(CommandId::kSetUpNoiseChannel, {6, 0, 0, 0})}}) == sets_up,
          "a noise setup of channel 6");
    Check(!AfterStartError({{Command(CommandId::kStartTimer, {0x01, 0, 0x01, 0}),
                             Command(CommandId::kSetUpPcmChannel, {5 | 0x200u << 16, 0x02001000, 0x200, 0}),
                             Command(CommandId::kSetUpPsgChannel, {8, 0, 0, 0})}}),
          "the alarms' timers and setups of other channels pass");

    // 2: a stop of one of the stream's channels, unless the stream ends in the run.
    Check(AfterStartError({{Command(CommandId::kStopTimer, {0x40, 0, 0, 0})}}) == stops, "a stop of channel 6");
    Check(!AfterStartError({{Command(CommandId::kStopTimer, {0xC0, 0, 0, 0})}}, true),
          "a stop of a stream that ends in the run");
    Check(!AfterStartError({{Command(CommandId::kStopTimer, {0x01, 0, 0x01, 0})}}), "a stop of another channel");

    // 3: another timer for one of the stream's channels.
    Check(AfterStartError({{Command(CommandId::kChannelTimer, {0x80, 0x180, 0, 0})}}) == timer, "another timer");
    Check(!AfterStartError({{Command(CommandId::kChannelTimer, {0xC0, 0x200, 0, 0}),
                             Command(CommandId::kChannelTimer, {0x01, 0x180, 0, 0})}}),
          "the start's timer, and a timer of another channel");

    // 4: a volume of 0, or a second level after the start. The first level and its repeats pass, in later lists too.
    Check(AfterStartError({{Command(CommandId::kChannelVolume, {0x40, 0, 0, 0})}}) == silences, "a volume of 0");
    Check(AfterStartError({{Command(CommandId::kChannelVolume, {0x40, 0x80, 0, 0})}}) == silences,
          "a volume whose 7 bits that the hardware reads are 0");
    Check(!AfterStartError({{Command(CommandId::kChannelVolume, {0xC0, 100, 1, 0})},
                            {Command(CommandId::kChannelVolume, {0x40, 100, 1, 0}),
                             Command(CommandId::kChannelVolume, {0x01, 0, 0, 0})}}),
          "the first level, its repeat, and a volume of 0 on another channel");
    Check(AfterStartError({{Command(CommandId::kChannelVolume, {0xC0, 100, 0, 0})},
                           {Command(CommandId::kChannelVolume, {0xC0, 90, 0, 0})}}) == second_level,
          "a second volume in a later list");
    Check(AfterStartError({{Command(CommandId::kChannelVolume, {0xC0, 100, 0, 0}),
                            Command(CommandId::kChannelVolume, {0x80, 100, 2, 0})}}) == second_level,
          "a second divider");

    // 5: the commands of a sequence, a player or a track, and the master volume.
    for (const CommandId id :
         {CommandId::kStartSequence, CommandId::kStopSequence, CommandId::kPrepareSequence,
          CommandId::kStartPreparedSequence, CommandId::kPlayerParameter, CommandId::kTrackParameter,
          CommandId::kMuteTrack, CommandId::kPlayerLocalVariable, CommandId::kGlobalVariable, CommandId::kMasterVolume})
    {
        const std::optional<std::string> error = AfterStartError({{Command(id)}});
        Check(error && error->starts_with(prefix), "command " + std::to_string(static_cast<uint32_t>(id)) + " fails");
    }

    // The rest pass.
    Check(
        !AfterStartError(
            {{Command(CommandId::kChannelPan, {0xC0, 64, 0, 0}), Command(CommandId::kSetUpAlarm, {1, 0x100, 0x10, 0}),
              Command(CommandId::kLockChannels, {0xC0, 0, 0, 0}), Command(CommandId::kUnlockChannels, {0xC0, 0, 0, 0}),
              Command(CommandId::kStopUnlockedChannels, {0xFFFF, 0, 0, 0}),
              Command(CommandId::kAllocatableChannels, {0, 0xFFFF, 0, 0}),
              Command(CommandId::kSetSharedWork, {0x02100000, 0, 0, 0}),
              Command(CommandId::kReadDriverInfo, {0x02100000, 0, 0, 0}), Command(CommandId::kInvalidateSequence),
              Command(CommandId::kInvalidateBank), Command(CommandId::kInvalidateWave),
              Command(CommandId::kSurroundDecay), Command(CommandId::kMasterPan, {64, 0, 0, 0}),
              Command(CommandId::kOutputSelector), Command(CommandId::kSetUpCapture),
              Command(CommandId::kPauseSequence), Command(CommandId::kSkipSequence)}}),
        "the pans, the alarms, the locks, the shared work, the master pan and the captures pass");
}

// The level that the start list sets after the start command is the first level. A different level in a later list
// fails, and the same level passes. A volume of 0 there fails. A start list without a level leaves the first later
// level free.
void TestStartListLevel()
{
    using ntdlib::ntr::snd::CommandId;
    const std::vector<std::vector<uint8_t>> heads(4, std::vector<uint8_t>(0x800));
    const std::vector<uint8_t> memory(0x4000);
    const auto error_of = [&heads, &memory](std::optional<uint32_t> start_volume, uint32_t later_volume)
    {
        ntdlib::ntr::snd::CommandList list = StartList(0x800, 0x200);
        if (start_volume)
        {
            list.push_back(Command(CommandId::kChannelVolume, {0xC0, *start_volume, 0, 0}));
        }
        const ntdlib::ntr::snd::CommandList later = {Command(CommandId::kChannelVolume, {0xC0, later_volume, 0, 0})};
        twosf::rip::ChannelStart start;
        std::optional<std::string> error;
        const auto fail = [&error](std::string message)
        {
            error = error.value_or(std::move(message));
        };

        const std::size_t read = twosf::rip::ReadCommands(list, memory, heads, 2, start, fail);
        twosf::rip::ReadCommandsAfterStart(std::span(list).subspan(read), start, false, fail);
        twosf::rip::ReadCommandsAfterStart(later, start, false, fail);

        return error;
    };

    const std::string prefix = "a driver command after the start ";
    Check(error_of(100, 90) == prefix + "sets a second level for the stream's channels",
          "another level after the level of the start list");
    Check(!error_of(100, 100), "the level of the start list again");
    Check(error_of(0, 100) == prefix + "silences one of the stream's channels", "a volume of 0 in the start list");
    Check(!error_of(std::nullopt, 90), "a first level in a later list");
}

// An ARM7 binary that turns the sound unit on, starts channel 6 on a looped buffer of 8-bit samples and halts for good.
// With `tone`, the buffer is a square wave and the output changes from the start. Without it, the buffer contains zeros
// and the output stays the same.
std::vector<uint8_t> Channel6Arm7(bool tone)
{
    constexpr uint32_t kLoopedPcm8 = 0x8840007F; // started, looped, 8-bit samples, at the centre, at full volume

    std::vector<uint32_t> buffer;
    for (uint32_t i = 0; i < 4; i++)
    {
        buffer.push_back(tone ? (i % 2 == 0 ? 0x7F7F7F7Fu : 0x80808080u) : 0);
    }

    return twosf::testing::Channel6Arm7(buffer, kLoopedPcm8);
}

// An image with an archive of one stream as a file, `stream` (a silent one by default), whose boot sends the driver the
// start of two channels only with a backup memory. Both channels play the stream's start, the zeros of main memory:
// channel 6 at a timer of 0x200, channel 7 at `timer_7`. The list of the start ends with `start_rest`. When `later` has
// commands, the boot sends them as a second list after the start. The image's ARM7 is `arm7`.
std::vector<uint8_t> StreamStartWithBackupImage(uint32_t timer_7 = 0x100,
                                                std::vector<uint8_t> arm7 = twosf::testing::Halts(),
                                                const std::vector<std::vector<uint32_t>>& later = {},
                                                std::vector<uint8_t> stream = StreamOf(false),
                                                const std::vector<std::vector<uint32_t>>& start_rest = {})
{
    using ntdlib::ntr::snd::CommandId;
    constexpr uint32_t kList = twosf::testing::kArm9Base + 0x400;
    constexpr uint32_t kLaterList = kList + 0x100;
    constexpr uint32_t kData = twosf::testing::kArm9Base + 0x1000;
    constexpr uint32_t kBufferWords = 0x200;
    constexpr int kTimerShift = 16;

    arm::ArmAssembler a(twosf::testing::kArm9Base);
    const int halt = a.NewLabel();
    twosf::testing::EmitBackupCheck(a, halt);
    std::vector<uint32_t> lists = {kList};
    if (!later.empty())
    {
        lists.push_back(kLaterList);
    }
    twosf::testing::EmitSendLists(a, lists);
    twosf::testing::EmitHaltForever(a, halt);
    twosf::testing::EndCodeAt(a, kList);

    // Channels 6 and 7 play the data at their timers, and the timer starts both.
    std::vector<std::vector<uint32_t>> start = {
        {static_cast<uint32_t>(CommandId::kSetUpPcmChannel), 6 | 0x200u << kTimerShift, kData, kBufferWords, 0},
        {static_cast<uint32_t>(CommandId::kSetUpPcmChannel), 7 | timer_7 << kTimerShift, kData, kBufferWords, 0},
        {static_cast<uint32_t>(CommandId::kStartTimer), 0xC0, 0, 1, 0}};
    start.insert(start.end(), start_rest.begin(), start_rest.end());
    twosf::testing::EmitList(a, kList, start);
    twosf::testing::PadTo(a, kLaterList);
    twosf::testing::EmitList(a, kLaterList, later);
    twosf::testing::PadTo(a, kData + 4 * kBufferWords);

    sdat::ArchiveModel model;
    model.file_alignment = 4;
    model.files = {std::move(stream)};
    model.streams = {sdat::StreamInfo{.file_id = 0, .volume = 100}};

    return twosf::testing::ImageWithArm9(a.Finish(), {{"sound/sound_data.sdat", sdat::BuildArchive(model)}},
                                         std::move(arm7));
}

// RunStreamMini boots with the backup memory that it's given: the channels start only with one, and their rates then
// differ.
void TestRunStreamMiniWithBackup()
{
    const std::vector<uint8_t> image = StreamStartWithBackupImage();
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);
    const ntdlib::ntr::RomFile* archive = rom.Find("sound/sound_data.sdat");
    if (!Check(archive != nullptr, "the archive's file"))
    {
        return;
    }

    const twosf::rip::StreamRun with =
        twosf::rip::RunStreamMini(image, 0, archive->id, 0, {}, 1.0, twosf::testing::TestBackup());
    const twosf::rip::StreamRun without = twosf::rip::RunStreamMini(image, 0, archive->id, 0, {}, 1.0, {});

    Check(with.error == "the stream's channels play at different rates",
          "the run with a backup memory starts the channels: " + with.error.value_or(""));
    Check(without.error == "the stream's channels don't sound within 1 s of the boot",
          "the run without one doesn't: " + without.error.value_or(""));
}

// RunStreamMini reports whether the sound unit's output changes after the channels start (rip::BootsError). The ARM7
// starts channel 6 on a square wave or on zeros. Neither run has a stream player that reads the file again.
void TestRunStreamMiniSounded()
{
    constexpr uint32_t kTimer = 0x200;
    const std::vector<uint8_t> tone = StreamStartWithBackupImage(kTimer, Channel6Arm7(true));
    const std::vector<uint8_t> quiet = StreamStartWithBackupImage(kTimer, Channel6Arm7(false));
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(tone);
    const uint32_t archive = rom.Find("sound/sound_data.sdat")->id;

    const twosf::rip::StreamRun sounds =
        twosf::rip::RunStreamMini(tone, 0, archive, 0, {}, 1.0, twosf::testing::TestBackup());
    const twosf::rip::StreamRun silent =
        twosf::rip::RunStreamMini(quiet, 0, archive, 0, {}, 1.0, twosf::testing::TestBackup());

    const std::string no_refill = "the stream player reads nothing more of the stream's file after the start";
    Check(sounds.error == no_refill && sounds.sounded,
          "the run with a square wave sounds: " + sounds.error.value_or(""));
    Check(silent.error == no_refill && !silent.sounded,
          "the run with zeros stays silent: " + silent.error.value_or(""));
}

// RunStreamMini with a target that sets `heads_after_start`: the channels' buffer, the zeros of main memory, must start
// with a head of the stream's channel at some step after the start. Heads of zeros pass that check, and the run then
// fails only for the missing refills. Heads that the buffer never starts with fail it. The ARM7 starts channel 6 on a
// square wave.
void TestRunStreamMiniHeadsAfterStart()
{
    constexpr uint32_t kTimer = 0x200;
    const std::vector<uint8_t> image = StreamStartWithBackupImage(kTimer, Channel6Arm7(true));
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);
    const uint32_t archive = rom.Find("sound/sound_data.sdat")->id;
    const auto run = [&](uint8_t head_byte)
    {
        twosf::rip::StreamTarget target = twosf::rip::ArchiveStreamTarget(rom, archive, 0, {});
        target.heads_after_start = true;
        target.heads.assign(2, std::vector<uint8_t>(0x200, head_byte));
        return twosf::rip::RunStreamMini(image, 0, archive, target, 1.0, twosf::testing::TestBackup()).error;
    };

    const std::optional<std::string> filled = run(0);
    const std::optional<std::string> never = run(0x5A);

    Check(filled == "the stream player reads nothing more of the stream's file after the start",
          "a buffer that starts with the heads: " + filled.value_or(""));
    Check(never == "the stream's channels don't play the stream: their buffers never start with the stream's samples",
          "a buffer that never starts with the heads: " + never.value_or(""));
}

// A stream of `samples` silent samples at 32,728 Hz, with a loop from its start when `loops`.
std::vector<uint8_t> SilentStream(std::size_t samples, bool loops)
{
    const std::vector<int16_t> channel(samples, 0);
    const std::optional<uint32_t> loop_start = loops ? std::optional<uint32_t>(0) : std::nullopt;

    return sdat::EncodeStream(std::vector<std::vector<int16_t>>{channel}, 32728, sdat::WaveEncoding::kImaAdpcm,
                              loop_start);
}

// RunStreamMini reads the commands after the start by ReadCommandsAfterStart, from the start list on. A stop of the
// stream's channels fails a stream with a loop and one that lasts past the run, and passes one that ends in the run. A
// later level other than the one after the start command in the start list fails, and the same level passes. The
// channels play at 32,728.5 Hz: 5 s are 163,642 samples.
void TestRunStreamMiniAfterStart()
{
    using ntdlib::ntr::snd::CommandId;
    constexpr uint32_t kTimer = 0x200;
    const std::vector<std::vector<uint32_t>> silence = {
        {static_cast<uint32_t>(CommandId::kChannelVolume), 0x40, 0, 0, 0}};
    const std::vector<std::vector<uint32_t>> stop = {{static_cast<uint32_t>(CommandId::kStopTimer), 0x40, 0, 0, 0}};
    const std::vector<std::vector<uint32_t>> level = {
        {static_cast<uint32_t>(CommandId::kChannelVolume), 0xC0, 100, 0, 0}};
    const std::vector<std::vector<uint32_t>> lower = {
        {static_cast<uint32_t>(CommandId::kChannelVolume), 0xC0, 90, 0, 0}};
    const auto run = [](const std::vector<uint8_t>& image)
    {
        const uint32_t archive = ntdlib::ntr::Rom::Parse(image).Find("sound/sound_data.sdat")->id;
        return twosf::rip::RunStreamMini(image, 0, archive, 0, {}, 1.0, twosf::testing::TestBackup()).error;
    };
    const std::vector<uint8_t> silenced = StreamStartWithBackupImage(kTimer, twosf::testing::Halts(), silence);
    const std::vector<uint8_t> looped =
        StreamStartWithBackupImage(kTimer, twosf::testing::Halts(), stop, SilentStream(4096, true));
    const std::vector<uint8_t> past_run =
        StreamStartWithBackupImage(kTimer, twosf::testing::Halts(), stop, SilentStream(164000, false));
    const std::vector<uint8_t> in_run =
        StreamStartWithBackupImage(kTimer, twosf::testing::Halts(), stop, SilentStream(163000, false));
    const std::vector<uint8_t> lowered =
        StreamStartWithBackupImage(kTimer, twosf::testing::Halts(), lower, StreamOf(false), level);
    const std::vector<uint8_t> kept =
        StreamStartWithBackupImage(kTimer, twosf::testing::Halts(), level, StreamOf(false), level);

    const std::optional<std::string> silenced_error = run(silenced);
    const std::optional<std::string> looped_error = run(looped);
    const std::optional<std::string> past_run_error = run(past_run);
    const std::optional<std::string> in_run_error = run(in_run);
    const std::optional<std::string> lowered_error = run(lowered);
    const std::optional<std::string> kept_error = run(kept);

    const std::string prefix = "a driver command after the start ";
    Check(silenced_error == prefix + "silences one of the stream's channels",
          "a volume of 0 after the start: " + silenced_error.value_or(""));
    Check(looped_error == prefix + "stops one of the stream's channels",
          "a stop of a stream with a loop: " + looped_error.value_or(""));
    Check(past_run_error == prefix + "stops one of the stream's channels",
          "a stop of a stream that lasts past the run: " + past_run_error.value_or(""));
    Check(in_run_error == "the stream's channels don't sound within 1 s of the boot",
          "a stop of a stream that ends in the run passes: " + in_run_error.value_or(""));
    Check(lowered_error == prefix + "sets a second level for the stream's channels",
          "a later level other than the start list's: " + lowered_error.value_or(""));
    Check(kept_error == "the stream's channels don't sound within 1 s of the boot",
          "the level of the start list again passes: " + kept_error.value_or(""));
}

// A stream mini's runs pass with the same rate on every boot. A run with a backup memory fails when the channels play
// at another rate than the first, even when it matches the run before it. A failed run keeps its error. Each check
// keeps the run's sound.
void TestStreamChecks()
{
    twosf::rip::StreamRun first;
    first.rate = 32728.5;
    first.sounded = true;
    twosf::rip::StreamRun same = first;
    same.sounded = false;
    twosf::rip::StreamRun other = first;
    other.rate = 16364.25;
    twosf::rip::StreamRun failed = other;
    failed.error = "the stream's channels don't sound within 2 s of the boot";

    const std::vector<twosf::rip::BootRun> checks =
        twosf::rip::StreamChecks(std::vector{first, same, other, failed, other});

    Check(checks.size() == 5, "a check for each run");
    Check(!checks[0].error && checks[0].sounded && !checks[1].error && !checks[1].sounded,
          "the same rate, with each run's sound");
    Check(checks[2].error == "the stream's channels play at another rate than with no backup memory",
          "another rate with a backup memory");
    Check(checks[3].error == failed.error, "a failed run keeps its error");
    Check(checks[4].error == checks[2].error, "a run like the one before it, with other rate than the first");
}

} // namespace

int main()
{
    TestStreamStarts();
    TestThumbStreamStart();
    TestStreams();
    TestStreamRecords();
    TestStreamFileReads();
    TestReadCommands();
    TestReadCommandsAtRate();
    TestReadCommandsHeadsAfterStart();
    TestReadCommandsAfterStart();
    TestStartListLevel();
    TestRunStreamMiniWithBackup();
    TestRunStreamMiniSounded();
    TestRunStreamMiniAfterStart();
    TestRunStreamMiniHeadsAfterStart();
    TestStreamChecks();
    TestStreamTiming();
    TestSetupLoop();

    return xsflib::testing::Summary("stream_set");
}
