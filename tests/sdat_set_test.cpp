// SPDX-License-Identifier: MIT

// Tests the parts of a set for a game on the SDK's sound archive player (rip/sdat_set.h) that the stubs, the archive,
// the table of games and the boots leave: the search of main memory for the sound heap, with and without a heap that
// contains the others, the rule for music, the settings of the length analysis, the first number of each record, and
// the checks of a mini's runs against the profile.

#include "rip/sdat_set.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/ntr/game_profiles.h"
#include "ntdlib/ntr/length_model.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sdat/layout.h"
#include "ntdlib/testing/sdat_files.h"
#include "xsflib/bytes.h"
#include "xsflib/rip/length.h"
#include "xsflib/testing/check.h"

#include "sdat_test_code.h"
#include "stub_memory.h"

namespace
{

namespace sdat = ntdlib::sdat;

using twosf::rip::SdatCode;
using twosf::testing::ArchiveModel;
using twosf::testing::GameStubCode;
using twosf::testing::kBase;
using twosf::testing::TestCode;
using xsflib::testing::Check;

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

// A sound archive's information block at `at` in `memory`, `size` bytes, with each of its eight tables at `table`.
void PutInfoBlock(std::vector<uint8_t>& memory, uint32_t at, uint32_t size, uint32_t table)
{
    xsflib::StoreLe32(&memory[at], 0x4F464E49); // "INFO"
    xsflib::StoreLe32(&memory[at + 4], size);
    for (uint32_t index = 0; index < 8; index++)
    {
        xsflib::StoreLe32(&memory[at + 8 + 4 * index], table);
    }
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

// The sound heap that a word of main memory names, as Mario Kart DS's word at 0x0217D110 does, where no heap contains
// the others. A word that names no frame heap, or is outside main memory, gives none.
void TestHeapAt()
{
    using twosf::rip::SoundHeapAt;
    std::vector<uint8_t> memory(0x4000, 0);
    PutHeap(memory, 0x1000, kBase + 0x1040, kBase + 0x1800, kBase + 0x1100, kBase + 0x1700);
    PutHeap(memory, 0x2000, kBase + 0x2040, kBase + 0x2800, kBase + 0x2100, kBase + 0x2400);
    xsflib::StoreLe32(&memory[0x3000], kBase + 0x2000);
    xsflib::StoreLe32(&memory[0x3004], kBase + 0x2004);

    const std::optional<twosf::rip::SoundHeap> named = SoundHeapAt(memory, kBase + 0x3000);

    Check(named && named->address == kBase + 0x2000 && named->free == 0x300, "the heap that the word names");
    Check(!SoundHeapAt(memory, kBase + 0x3004), "a word that names no heap");
    Check(!SoundHeapAt(memory, kBase + 0x8000), "a word outside main memory");
}

// Where two heaps are side by side, the sound heap is the one whose used part contains the archive's information block.
void TestHeapWithInfo()
{
    using twosf::rip::FindSoundHeap;
    std::vector<uint8_t> apart(0x4000, 0);
    PutHeap(apart, 0x1000, kBase + 0x1040, kBase + 0x1800, kBase + 0x1100, kBase + 0x1700);
    PutHeap(apart, 0x2000, kBase + 0x2040, kBase + 0x2800, kBase + 0x2200, kBase + 0x2700);

    std::vector<uint8_t> second = apart;
    PutInfoBlock(second, 0x2080, 0x100, 0x40);
    const std::optional<twosf::rip::SoundHeap> found = FindSoundHeap(second);
    Check(found && found->address == kBase + 0x2000 && found->free == 0x500, "the heap with the information block");

    std::vector<uint8_t> both = second;
    PutInfoBlock(both, 0x1040, 0x80, 0x40);
    Check(!FindSoundHeap(both), "an information block in each heap");

    std::vector<uint8_t> free_part = apart;
    PutInfoBlock(free_part, 0x2300, 0x100, 0x40);
    Check(!FindSoundHeap(free_part), "an information block past the head");

    std::vector<uint8_t> past_head = apart;
    PutInfoBlock(past_head, 0x2180, 0x100, 0x40);
    Check(!FindSoundHeap(past_head), "an information block that runs past the head");

    std::vector<uint8_t> table_outside = apart;
    PutInfoBlock(table_outside, 0x2080, 0x100, 0x100);
    Check(!FindSoundHeap(table_outside), "a table past the information block's end");

    std::vector<uint8_t> table_in_header = apart;
    PutInfoBlock(table_in_header, 0x2080, 0x100, 0x20);
    Check(!FindSoundHeap(table_in_header), "a table inside the information block's header");

    std::vector<uint8_t> nested = second;
    PutHeap(nested, 0x1000, kBase + 0x1040, kBase + 0x3000, kBase + 0x2900, kBase + 0x2E00);
    const std::optional<twosf::rip::SoundHeap> outer = FindSoundHeap(nested);
    Check(outer && outer->address == kBase + 0x1000, "the heap that contains the other, wherever the block is");
}

// The music of an archive with names, and every sequence outside the sequence archives where none counts or the
// archive has no names.
void TestMusic()
{
    sdat::ArchiveModel model = ArchiveModel();
    model.sequences.push_back(sdat::SequenceInfo{0, 0, 100, 64, 32, 0, 0, ""});
    model.sequences.push_back(std::nullopt);
    const std::vector<uint8_t> unnamed = sdat::BuildArchive(model);
    Check(twosf::rip::MusicSequences(sdat::Archive::Parse(unnamed), "AZEE") == std::set<uint32_t>{0, 1},
          "an archive without names");

    sdat::Symbols symbols;
    symbols.names[0] = {"SEQ_BGM_FIELD", "SEQ_SE_DOOR", std::nullopt};
    model.symbols = symbols;
    const std::vector<uint8_t> named = sdat::BuildArchive(model);
    Check(twosf::rip::MusicSequences(sdat::Archive::Parse(named), "AZEE") == std::set<uint32_t>{0},
          "the music by name");

    symbols.names[0] = {"SEQ_FIELD", "SEQ_DOOR", std::nullopt};
    model.symbols = symbols;
    const std::vector<uint8_t> no_music = sdat::BuildArchive(model);
    Check(twosf::rip::MusicSequences(sdat::Archive::Parse(no_music), "AZEE") == std::set<uint32_t>{0, 1},
          "names that give no music");

    symbols.names[0] = {"SEQ_BGM_FIELD", "FF_LEVELUP1", std::nullopt};
    model.symbols = symbols;
    const sdat::Archive fanfare = sdat::Archive::Parse(sdat::BuildArchive(model));
    Check(twosf::rip::MusicSequences(fanfare, "YFEE") == std::set<uint32_t>{0, 1} &&
              twosf::rip::MusicSequences(fanfare, "VI2J") == std::set<uint32_t>{0, 1},
          "the fanfares of both Fire Emblem games");
    Check(twosf::rip::MusicSequences(fanfare, "AZEE") == std::set<uint32_t>{0}, "a fanfare in another game");

    const std::set<uint32_t> music = {0, 2};
    Check(twosf::rip::AnalysisLimit(music, 2) == xsflib::rip::Limit::kLong, "music runs to the long limit");
    Check(twosf::rip::AnalysisLimit(music, 1) == xsflib::rip::Limit::kDefault, "the rest keeps the default limit");
}

// The settings of the length analysis: the run's variables, the channels that its start gave the record's player where
// they differ from the archive's, and the profile's reverb and output effect only while the run has the capture units
// running. The profile's variables and channels stay out.
void TestLengthSettings()
{
    ntdlib::ntr::GameSettings profile;
    profile.variables.global[3] = 7;
    profile.channels[0] = 0x8000;
    profile.reverb = ntdlib::ntr::Reverb{.size = 0x900, .sample_rate = 16000, .volume = 45};

    twosf::rip::MiniRun run;
    run.variables.global[1] = 5;
    run.channels = 0x7FF;
    const ntdlib::ntr::GameSettings idle = twosf::rip::LengthSettings(run, profile, 7, 0x7FF);
    Check(idle.variables.global[1] == 5 && !idle.variables.global[3], "the run's variables, not the profile's");
    Check(idle.channels.empty() && idle.track_channels.empty(), "no channels where the start gives the archive's");
    Check(!idle.reverb && !idle.effect, "no echo while the capture units are idle");
    Check(!twosf::rip::EchoLeftOut(run, idle), "no warning while the capture units are idle");

    run.captures_run = true;
    run.channels = 0x7FFF;
    const ntdlib::ntr::GameSettings running = twosf::rip::LengthSettings(run, profile, 7, 0x7FF);
    Check(running.reverb && running.reverb->size == 0x900 && !running.effect, "the profile's reverb while they run");
    Check(running.channels == std::map<uint32_t, uint16_t>{{7, 0x7FFF}},
          "the channels that the game's start gave the record's player");
    Check(!twosf::rip::EchoLeftOut(run, running), "no warning with the profile's reverb");

    const ntdlib::ntr::GameSettings unprofiled = twosf::rip::LengthSettings(run, {}, 7, 0x7FF);
    Check(!unprofiled.reverb && !unprofiled.effect, "no echo without one in the profile");
    Check(twosf::rip::EchoLeftOut(run, unprofiled), "a warning when the profile has none");
}

// The mini's sequence is the one that a start command starts on a player with a prepared sequence. A stop drops a
// player's prepare, as when the game's start stops the title music that the boot queued. What the lists set on that
// player from its prepare to the end of the start's list counts, moved to player 0; the commands on other players
// don't.
void TestStartFollower()
{
    using ntdlib::ntr::snd::Command;
    using ntdlib::ntr::snd::CommandId;
    constexpr uint32_t kTwoBytes = 2u << 24;
    const auto prepare = [](uint32_t player, uint32_t sequence)
    {
        return Command{.id = CommandId::kPrepareSequence, .args = {player, sequence, 0, 0x02300000}};
    };
    const auto start = [](uint32_t player)
    {
        return Command{.id = CommandId::kStartPreparedSequence, .args = {player}};
    };
    const auto stop = [](uint32_t player)
    {
        return Command{.id = CommandId::kStopSequence, .args = {player, 0}};
    };
    const Command channels = {.id = CommandId::kAllocatableChannels, .args = {7, 0xFFFF, 0x7FFF}};
    const Command volume = {.id = CommandId::kPlayerParameter, .args = {7, 0x06, 0xFFE8, 2}};
    const Command priority = {.id = CommandId::kPlayerParameter, .args = {7, 0x04, 30, 1}};
    const Command track = {.id = CommandId::kTrackParameter, .args = {kTwoBytes | 7, 0x0004, 0x0A, 0xFFFF8000}};
    const Command elsewhere = {.id = CommandId::kTrackParameter, .args = {kTwoBytes | 1, 0x0004, 0x0A, 0}};
    const Command variable = {.id = CommandId::kGlobalVariable, .args = {3, 5}};

    twosf::rip::MiniRun run;
    twosf::rip::StartFollower follower(run);
    Check(!follower.Take({prepare(7, 0x02280000), variable, start(1)}),
          "the boot's queued prepare, and a start on another player, start nothing");
    Check(!follower.Take({stop(7), prepare(7, 0x02290000), channels, volume, priority, elsewhere}),
          "the game's start stops the queued sequence and prepares the mini's");
    const std::optional<Command> started = follower.Take({track, start(7), volume, start(1)});
    Check(started && started->args[1] == 0x02290000 && follower.Player() == 7,
          "the start of the prepared sequence, the one after the stop");
    Check(run.variables.global[3] == 5, "a global variable before the start");
    Check(run.channels == 0x7FFF && run.volume == -24 && run.priority == 30,
          "the channels and the player's volume and priority before the start");
    Check(run.commands.size() == 2 && run.commands[0].args[0] == kTwoBytes && run.commands[1].args[1] == 0x06,
          "the track's volume before the start and the player's volume after it, on player 0");

    twosf::rip::MiniRun dropped;
    twosf::rip::StartFollower without_prepare(dropped);
    Check(!without_prepare.Take({prepare(7, 0x02280000), stop(7), start(7)}), "a start after a stop starts nothing");
}

// The check of a start through the game's code against the profile compares what each side leaves set on the
// sequence's player. A value at the start's value counts as unset on either side, and so does a track's pan of 0. The
// capture units must run where the profile has a reverb, and only there.
void TestProfileDifferences()
{
    using ntdlib::ntr::snd::Command;
    using ntdlib::ntr::snd::CommandId;
    constexpr uint32_t kTwoBytes = 2u << 24;
    const Command on = {.id = CommandId::kTrackParameter, .args = {kTwoBytes, 0x027F, 0x0A, 0}};
    const Command off = {.id = CommandId::kTrackParameter, .args = {kTwoBytes, 0xFD80, 0x0A, 0xFFFF8000}};
    const Command tempo = {.id = CommandId::kPlayerParameter, .args = {0, 0x1A, 235, 2}};
    const Command pan = {.id = CommandId::kTrackParameter, .args = {1u << 24, 0xFFFF, 0x09, 0}};

    ntdlib::ntr::GameProfile profile;
    profile.commands = {on, off};
    const ntdlib::ntr::ProfileArrangement lower{.suffix = "_LOWER1", .commands = {tempo}};

    twosf::rip::MiniRun run;
    run.channels = 0x7FF;
    run.volume = -24;
    run.priority = 30;
    const Command start_priority = {.id = CommandId::kPlayerParameter, .args = {0, 0x04, 30, 1}};
    run.commands = {off, on, start_priority, off};
    Check(twosf::rip::ProfileDifferences(run, profile, nullptr, 0x7FF).empty(),
          "the same values set, in another order and more than once, and the start's priority");

    run.commands = {on, off, tempo, pan};
    Check(twosf::rip::ProfileDifferences(run, profile, &lower, 0x7FF).empty(),
          "an arrangement's commands follow the base's, and a pan of 0 counts as unset");
    Check(twosf::rip::ProfileDifferences(run, profile, nullptr, 0x7FF).size() == 1,
          "a tempo that the profile doesn't set");

    run.commands = {on};
    Check(twosf::rip::ProfileDifferences(run, profile, nullptr, 0x7FF).size() == 8,
          "each of the eight tracks that the profile silences and the game doesn't");

    run.commands = {on, off};
    run.channels = 0x7FFF;
    ntdlib::ntr::GameProfile title;
    title.settings.channels[7] = 0x7FFF;
    title.settings.reverb = ntdlib::ntr::Reverb{.size = 0x1000, .sample_rate = 16000, .volume = 30};
    title.commands = {on, off};
    Check(twosf::rip::ProfileDifferences(run, title, nullptr, 0x7FF).size() == 1,
          "the profile's reverb, and the game's capture units idle");
    run.captures_run = true;
    Check(twosf::rip::ProfileDifferences(run, title, nullptr, 0x7FF).empty(), "the scene's channels and reverb");
    Check(twosf::rip::ProfileDifferences(run, profile, nullptr, 0x7FF).size() == 2,
          "channels and capture units that the profile doesn't give");
}

// The first number of each record. Every field of the record counts, and the name doesn't.
void TestFirstOfRecord()
{
    sdat::ArchiveModel model = ArchiveModel();
    const sdat::SequenceInfo first = *model.sequences[0];
    std::vector<sdat::SequenceInfo> others(7, first);
    others[0].file_id = 1;
    others[1].bank = 1;
    others[2].volume = 90;
    others[3].channel_priority = 63;
    others[4].player_priority = 31;
    others[5].player = 1;
    others[6].reserved = 1;

    model.sequences = {first};
    model.sequences.insert(model.sequences.end(), others.begin(), others.end());
    model.sequences.insert(model.sequences.end(), {std::nullopt, first, others[2], std::nullopt});

    sdat::Symbols symbols;
    for (std::size_t i = 0; i < model.sequences.size(); i++)
    {
        symbols.names[0].push_back("SEQ_" + std::to_string(i));
    }
    model.symbols = symbols;

    const sdat::Archive archive = sdat::Archive::Parse(sdat::BuildArchive(model));
    const std::vector<uint32_t> all = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    const std::vector<uint32_t> reordered = {9, 0, 12, 12};

    const SdatCode sdk_start = TestCode(true);
    SdatCode game_starts = TestCode(true);
    game_starts.game_start = GameStubCode().game_start;

    const std::vector<uint32_t> firsts = twosf::rip::FirstOfRecord(sdk_start, archive, all);
    const std::vector<uint32_t> reordered_firsts = twosf::rip::FirstOfRecord(sdk_start, archive, reordered);
    const std::vector<uint32_t> game_firsts = twosf::rip::FirstOfRecord(game_starts, archive, all);

    if (!Check(firsts.size() == all.size(), "a first number for each number"))
    {
        return;
    }

    Check(firsts[0] == 0, "the first number");
    Check(firsts[9] == 0, "a later number of a record, with another name");
    Check(firsts[10] == 3, "a later number of a record that isn't the first number's");
    Check(firsts[1] == 1, "another sequence file");
    Check(firsts[2] == 2, "another bank");
    Check(firsts[3] == 3, "another volume");
    Check(firsts[4] == 4, "another channel priority");
    Check(firsts[5] == 5, "another player priority");
    Check(firsts[6] == 6, "another player");
    Check(firsts[7] == 7, "other reserved bytes");
    Check(firsts[8] == 8 && firsts[11] == 11, "numbers without a record");
    Check(reordered_firsts == std::vector<uint32_t>{9, 9, 12, 12}, "the order given, and numbers past the table");
    Check(game_firsts == all, "a game with its starts: every number keeps its mini");
}

// A mini's runs pass with the same global variables on every boot. A run with a backup memory fails when the driver's
// commands set other ones than the first, even when they match the run before it. A failed run keeps its error. Each
// check keeps the run's sound.
void TestMiniChecks()
{
    twosf::rip::MiniRun first;
    first.variables.global[0] = 5;
    first.sounded = true;
    twosf::rip::MiniRun same = first;
    same.sounded = false;
    twosf::rip::MiniRun other = first;
    other.variables.global[0] = 6;
    twosf::rip::MiniRun failed = other;
    failed.error = "the sequence doesn't start within 2 s";

    const std::vector<twosf::rip::BootRun> checks =
        twosf::rip::MiniChecks(std::vector{first, same, other, failed, other});

    Check(checks.size() == 5, "a check for each run");
    Check(!checks[0].error && checks[0].sounded && !checks[1].error && !checks[1].sounded,
          "the same global variables, with each run's sound");
    Check(checks[2].error == "the driver's commands set other variables than with no backup memory",
          "other global variables with a backup memory");
    Check(checks[3].error == failed.error, "a failed run keeps its error");
    Check(checks[4].error == checks[2].error,
          "a run like the one before it, with other global variables than the first");

    twosf::rip::MiniRun local = first;
    local.variables.local[0] = 0;
    Check(twosf::rip::MiniChecks(std::vector{first, local})[1].error == checks[2].error,
          "another local variable of the sequence's player with a backup memory");
}

// rip::LeftOutUnstarted leaves out a failing mini only for a game in the table of games, only when every run fails
// because the sequence doesn't start, and only when the sequence plays no notes on the model.
void TestLeftOutUnstarted()
{
    SdatCode table_game;
    table_game.game_start.emplace();
    const SdatCode other_game;
    twosf::rip::MiniRun not_started;
    not_started.error = "the sequence doesn't start within 11 s of the boot";
    not_started.not_started = true;
    twosf::rip::MiniRun other_error;
    other_error.error = "a list of the driver's commands after the start sets a variable";
    xsflib::rip::Timing silent;
    silent.silent = true;
    const xsflib::rip::Timing sounds;
    const std::vector both{not_started, not_started};
    const std::vector one{not_started, other_error};

    const bool left_out = twosf::rip::LeftOutUnstarted(table_game, both, silent);
    const bool sounding = twosf::rip::LeftOutUnstarted(table_game, both, sounds);
    const bool unmeasured = twosf::rip::LeftOutUnstarted(table_game, both, std::nullopt);
    const bool outside_table = twosf::rip::LeftOutUnstarted(other_game, both, silent);
    const bool another_failure = twosf::rip::LeftOutUnstarted(table_game, one, silent);
    const bool no_runs = twosf::rip::LeftOutUnstarted(table_game, {}, silent);

    Check(left_out, "a silent sequence of a game in the table that no run starts is left out");
    Check(!sounding, "a sequence that plays notes isn't");
    Check(!unmeasured, "a sequence that the model doesn't measure isn't");
    Check(!outside_table, "a game outside the table isn't");
    Check(!another_failure, "a mini with a run that fails for another reason isn't");
    Check(!no_runs, "a mini with no runs isn't");
}

// A command on player 0 that mutes the tracks `tracks`, and one that sets the tempo's ratio.
ntdlib::ntr::snd::Command Mute(uint32_t tracks)
{
    return {.id = ntdlib::ntr::snd::CommandId::kMuteTrack, .args = {0, tracks, 1, 0}};
}

ntdlib::ntr::snd::Command Tempo(uint32_t ratio)
{
    return {.id = ntdlib::ntr::snd::CommandId::kPlayerParameter, .args = {0, 0x1A, ratio, 2}};
}

// A run's later lists in game frames of two frames of the screen, and an arrangement that sends mutes from the start
// and a fade at tick 100.
struct LaterCase
{
    twosf::rip::MiniRun run;
    ntdlib::ntr::GameProfile profile;
    ntdlib::ntr::ProfileArrangement intro;
};

LaterCase MatchingLater()
{
    constexpr uint64_t kGameFrame = 2 * ntdlib::ntr::kFrameCycles;
    LaterCase c;
    c.intro.suffix = "_INTRO";
    c.intro.commands = {Mute(0x1)};
    c.intro.tick_commands = {
        {.tick = 100, .video_frames = 2, .frames = {{Tempo(200)}, {Tempo(200)}, {Tempo(150)}}},
        {.tick = 0, .video_frames = 2, .frames = {{}, {Mute(0x2)}}},
    };
    c.run.video_frames = 2;
    c.run.later = {{.clocks = kGameFrame - 1000, .commands = {Mute(0x1)}},
                   {.clocks = 2 * kGameFrame + 1000, .commands = {Mute(0x1), Mute(0x2)}},
                   {.clocks = 40 * kGameFrame, .commands = {Tempo(200)}},
                   {.clocks = 41 * kGameFrame, .commands = {Tempo(200)}},
                   {.clocks = 42 * kGameFrame, .commands = {Tempo(150)}},
                   {.clocks = 43 * kGameFrame, .commands = {Tempo(150)}}};
    c.run.played_clocks = 30 * ntdlib::ntr::kSystemClockHz;

    return c;
}

// The later lists of a run count where they give the profile's changes in the frames of the profile's lists. The start
// counts from the first game frames, and a list that repeats a value changes nothing.
void TestLaterDifferences()
{
    using twosf::rip::LaterDifferences;
    LaterCase c = MatchingLater();
    Check(LaterDifferences(c.run, c.profile, &c.intro, 0).empty(), "the profile's changes in their frames");
    Check(twosf::rip::ProfileDifferences(c.run, c.profile, &c.intro, 0).empty(),
          "the start's values come in the first game frame");
    twosf::rip::MiniRun alone;
    Check(LaterDifferences(alone, c.profile, nullptr, 0).empty(), "neither side has later lists");

    c.run.later[1].clocks += 2 * ntdlib::ntr::kFrameCycles;
    Check(LaterDifferences(c.run, c.profile, &c.intro, 0).size() == 1, "a list at tick 0 a game frame late");

    c = MatchingLater();
    c.run.later[4].clocks += 2 * ntdlib::ntr::kFrameCycles;
    Check(LaterDifferences(c.run, c.profile, &c.intro, 0).size() == 1, "a step of the fade a game frame late");

    c = MatchingLater();
    c.run.later[4].commands = {Tempo(160)};
    c.run.later[5].commands = {Tempo(160)};
    Check(LaterDifferences(c.run, c.profile, &c.intro, 0).size() == 1, "another value");

    c = MatchingLater();
    c.run.later.pop_back();
    c.run.later.pop_back();
    Check(LaterDifferences(c.run, c.profile, &c.intro, 0).size() == 1, "a change that the game doesn't make");
    Check(!LaterDifferences(c.run, c.profile, nullptr, 0).empty(), "later lists that the profile doesn't have");

    c = MatchingLater();
    c.run.played_clocks = 44 * 2 * ntdlib::ntr::kFrameCycles;
    Check(LaterDifferences(c.run, c.profile, &c.intro, 0).size() == 1, "a change too near the run's end");
}

ntdlib::ntr::snd::Command Lock(uint32_t channels, bool unlock = false)
{
    using ntdlib::ntr::snd::CommandId;
    return {.id = unlock ? CommandId::kUnlockChannels : CommandId::kLockChannels, .args = {channels, 0, 0, 0}};
}

// Channels that the game locks count as channels of none of the player's tracks. A lock at the start equals the
// profile's channels of every track without them, and its lock in frame 0. A later lock comes in the game frame nearest
// the profile's frame of the driver: frame 526 of 174,592 clocks is game frame 82 of 1,120,380. Another frame, other
// channels, an unlock and a missing lock each make a difference.
void TestChannelLocks()
{
    using twosf::rip::LaterDifferences;
    using twosf::rip::ProfileDifferences;
    constexpr uint64_t kGameFrame = 2 * ntdlib::ntr::kFrameCycles;
    twosf::rip::MiniRun locked;
    locked.locked = 0xC0;
    ntdlib::ntr::GameProfile tracks;
    tracks.settings.track_channels = {{.tracks = 0xFFFF, .channels = 0xFF3F}};
    ntdlib::ntr::GameProfile at_start;
    at_start.channel_locks = {{.frame = 0, .channels = 0xC0}};
    Check(ProfileDifferences(locked, tracks, nullptr, 0).empty(), "a lock at the start as the tracks' channels");
    Check(ProfileDifferences(locked, at_start, nullptr, 0).empty(), "a lock at the start as the profile's lock");
    Check(ProfileDifferences(locked, {}, nullptr, 0).empty(), "a lock of channels that no lock of the profile names");
    Check(!ProfileDifferences(twosf::rip::MiniRun{}, tracks, nullptr, 0).empty(),
          "the tracks' channels without the lock");
    Check(ProfileDifferences(twosf::rip::MiniRun{}, at_start, nullptr, 0).size() == 1,
          "a lock that the game doesn't make");
    ntdlib::ntr::GameProfile other_start;
    other_start.channel_locks = {{.frame = 0, .channels = 0x30}};
    Check(ProfileDifferences(locked, other_start, nullptr, 0).size() == 1, "a lock of other channels at the start");

    ntdlib::ntr::GameProfile later;
    later.channel_locks = {{.frame = 526, .channels = 0xC0}};
    twosf::rip::MiniRun run;
    run.video_frames = 2;
    run.played_clocks = 30 * ntdlib::ntr::kSystemClockHz;
    run.later = {{.clocks = 82 * kGameFrame, .commands = {Lock(0xC0)}}};
    Check(LaterDifferences(run, later, nullptr, 0).empty(), "a later lock in the nearest game frame");
    Check(ProfileDifferences(run, later, nullptr, 0).empty(), "a later lock leaves the start alone");
    Check(LaterDifferences(run, {}, nullptr, 0).size() == 1, "a later lock that the profile doesn't have");

    twosf::rip::MiniRun late = run;
    late.later[0].clocks += kGameFrame;
    Check(LaterDifferences(late, later, nullptr, 0).size() == 1, "a later lock a game frame late");
    twosf::rip::MiniRun other = run;
    other.later[0].commands = {Lock(0x30)};
    Check(LaterDifferences(other, later, nullptr, 0).size() == 1, "a later lock of other channels");
    twosf::rip::MiniRun unlock = run;
    unlock.later[0].commands = {Lock(0xC0, true)};
    Check(LaterDifferences(unlock, later, nullptr, 0).size() == 1, "an unlock in place of a lock");
    Check(LaterDifferences(twosf::rip::MiniRun{}, later, nullptr, 0).size() == 1,
          "a later lock that the game doesn't make");

    const std::vector<ntdlib::ntr::TickCommands> lists = twosf::rip::LaterTickCommands(run);
    Check(lists.size() == 1 && lists[0].frames.size() == 82 &&
              lists[0].frames[81] == ntdlib::ntr::snd::CommandList{Lock(0xC0)},
          "the length analysis takes a later lock in its game frame");
}

// The later lists for the length analysis: each in its game frame after the start, up to the last change.
void TestLaterTickCommands()
{
    const LaterCase c = MatchingLater();
    const std::vector<ntdlib::ntr::TickCommands> lists = twosf::rip::LaterTickCommands(c.run);
    if (!Check(lists.size() == 1 && lists[0].tick == 0 && lists[0].video_frames == 2, "one list from tick 0"))
    {
        return;
    }

    Check(lists[0].frames.size() == 42, "up to the last change, in game frame 42");
    Check(lists[0].frames[0] == ntdlib::ntr::snd::CommandList{Mute(0x1)} &&
              lists[0].frames[1] == ntdlib::ntr::snd::CommandList{Mute(0x1), Mute(0x2)} && lists[0].frames[2].empty() &&
              lists[0].frames[41] == ntdlib::ntr::snd::CommandList{Tempo(150)},
          "each game frame's commands at its place, and none where the game sends none");
    Check(twosf::rip::LaterTickCommands(twosf::rip::MiniRun{}).empty(), "nothing without later lists");
}

// Numbers 0 to 4 share a record and number 5 has another. The runs of 0 and 1 start alike, as do those of 2 and 3,
// which start otherwise. Number 4 keeps its mini (no run), and 5 starts like 0 but has another record.
void TestFirstOfSameStart()
{
    sdat::ArchiveModel model = ArchiveModel();
    const sdat::SequenceInfo first = *model.sequences[0];
    sdat::SequenceInfo other = first;
    other.volume = 90;
    model.sequences = {first, first, first, first, first, other};
    const sdat::Archive archive = sdat::Archive::Parse(sdat::BuildArchive(model));

    twosf::rip::MiniRun alike;
    alike.commands = {Tempo(300)};
    twosf::rip::MiniRun louder = alike;
    louder.volume = 100;
    const std::vector<uint32_t> numbers = {0, 1, 2, 3, 4, 5};
    const std::vector<const twosf::rip::MiniRun*> runs = {&alike, &alike, &louder, &louder, nullptr, &alike};

    Check(twosf::rip::FirstOfSameStart(archive, numbers, runs) == std::vector<uint32_t>{0, 0, 2, 2, 4, 5},
          "the first number of each record whose run starts alike");
}

// A mini's later lists reach the length analysis through LaterTickCommands. The game's frames last two frames of the
// screen, and a list that doubles the tempo comes in game frame 1. The length model sends it before the driver's frame
// 7. The driver takes it after that frame's tick. The driver runs ticks 0 to 3 in frames 0, 2, 4 and 6, and a tick in
// each frame from frame 8: tick t in frame t + 4. The jump back at ticks 96 and 192 then loops in frames 100 and 196.
// The fade starts two frames after the second loop, and a pass is 96 frames, 500 ms. Without the list, the loops come
// in frames 192 and 384.
void TestLaterListsInLength()
{
    namespace files = ntdlib::testing::sdat_files;
    files::Archive spec;
    spec.items[0] = {files::SequenceItem(0, 0, "SEQ", 1)};
    spec.items[2] = {files::BankItem(1, {0, 0xFFFF, 0xFFFF, 0xFFFF}, "BANK")};
    spec.items[3] = {files::FileItem(2, "WAVE")};
    spec.items[4] = {files::PlayerItem(1, 0, 0, "PLAYER_0"), files::PlayerItem(1, 0x0FF0, 0, "PLAYER_1")};
    const files::Wave wave = {
        .loop = 1,
        .loop_length = 4,
        .data = {0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0}};
    spec.files = {files::Sequence({0x3C, 0x7F, 0x30, 0x80, 0x30, 0x94, 0x00, 0x00, 0x00}),
                  files::Bank({{.type = 1, .region = {}}}), files::WaveArchive({wave})};
    const std::vector<uint8_t> file = files::SoundArchive(spec);
    const sdat::Archive archive = sdat::Archive::Parse(file);

    twosf::rip::MiniRun run;
    run.video_frames = 2;
    run.later = {{.clocks = 2 * ntdlib::ntr::kFrameCycles, .commands = {Tempo(512)}}};
    const auto measure = [&archive](const twosf::rip::MiniRun& mini)
    {
        return ntdlib::ntr::MeasureSequence(archive, {0}, {}, {}, {}, xsflib::rip::Limit::kDefault, {},
                                            twosf::rip::LaterTickCommands(mini));
    };
    const auto loop_from = [](uint64_t frame)
    {
        return xsflib::rip::LoopAt(frame, ntdlib::ntr::kFrameMs, 7000.0);
    };

    const std::optional<xsflib::rip::Timing> faster = measure(run);
    const std::optional<xsflib::rip::Timing> plain = measure({});

    Check(faster && faster->length_ms == loop_from(198).length_ms && faster->loop && faster->loop->length_ms == 500,
          "the list's tempo from driver frame 8");
    Check(plain && plain->length_ms == loop_from(386).length_ms, "no list without later lists");
}

} // namespace

int main()
{
    TestHeap();
    TestHeapWithInfo();
    TestHeapAt();
    TestMusic();
    TestLengthSettings();
    TestProfileDifferences();
    TestFirstOfRecord();
    TestFirstOfSameStart();
    TestMiniChecks();
    TestLeftOutUnstarted();
    TestStartFollower();
    TestLaterDifferences();
    TestLaterTickCommands();
    TestChannelLocks();
    TestLaterListsInLength();

    return xsflib::testing::Summary("sdat_set");
}
