// SPDX-License-Identifier: MIT

// The form of a table entry for a game or a program whose code starts its music with routines of its own
// (rip/sdat_set.h): the calls of each start, the states, the patches and the instruction words that the entry expects.
// The table of games in sdat_set.cpp and the table of download-play programs in program_set.cpp use it.

#pragma once

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "ntdlib/ntr/rom.h"

#include "rip/arm_code.h"
#include "rip/sdat_set.h"

namespace twosf::rip
{

// The arguments of a table's calls.
constexpr StubArgument Sound()
{
    return {.kind = StubArgument::Kind::kSound};
}

constexpr StubArgument Constant(uint32_t value)
{
    return {.kind = StubArgument::Kind::kConstant, .value = value};
}

constexpr StubArgument WordOf(uint32_t address)
{
    return {.kind = StubArgument::Kind::kWord, .value = address};
}

constexpr StubArgument Result()
{
    return {.kind = StubArgument::Kind::kResult};
}

constexpr StubArgument ArchiveNumber()
{
    return {.kind = StubArgument::Kind::kArchive};
}

constexpr StubArgument Entry()
{
    return {.kind = StubArgument::Kind::kEntry};
}

constexpr StubArgument BankNumber()
{
    return {.kind = StubArgument::Kind::kBank};
}

constexpr StubArgument Heap()
{
    return {.kind = StubArgument::Kind::kHeap};
}

constexpr StubArgument PackedEntry(uint32_t shift)
{
    return {.kind = StubArgument::Kind::kPackedEntry, .value = shift};
}

constexpr StubCall MakeCall(uint32_t routine, std::initializer_list<StubArgument> arguments)
{
    StubCall call{.routine = routine, .count = arguments.size()};
    std::ranges::copy(arguments, call.arguments.begin());

    return call;
}

// The numbers that the game starts with other calls (NumberCalls), in a table entry.
struct NumberCallsEntry
{
    std::span<const std::pair<uint32_t, uint32_t>> ranges;
    std::optional<uint32_t> player;
    std::span<const StubCall> calls;
};

// A state of a sequence (GameState) in a table entry: the calls of the game's routines that set it, and those that the
// game makes in its frames after the start.
struct GameStateEntry
{
    std::string_view label;
    std::string_view suffix;
    std::span<const StubCall> calls;
    std::span<const StubCall> frame_calls = {};
    std::span<const FrameCall> timed_calls = {};
    uint32_t video_frames = 1;
};

// A state of a sequence's tracks that a game's field unit sets in Pokémon Black and Black 2, in the form of the unit's
// table rows: the tracks `on` at volume 127 and `off` at volume 0, through the game's setter of the BGM's track
// volumes, and a tempo's ratio other than 256 with the pitch `pitch` of the tracks `pitch_tracks`, through the game's
// tuning routine (TrackRoutines).
struct TrackStateEntry
{
    std::string_view label;
    std::string_view suffix;
    uint16_t on = 0;
    uint16_t off = 0;
    uint16_t tempo_ratio = 256;
    uint16_t pitch_tracks = 0;
    int16_t pitch = 0;
};

// A game's routines that set a state of its BGM's tracks (TrackStateEntry), Thumb: the setter of the tracks' volumes
// (tracks, volume) and the tuning routine (tracks, tempo's ratio, pitch, pan). The tuning routine leaves each value of
// -1 as it is.
struct TrackRoutines
{
    uint32_t volume = 0;
    uint32_t tuning = 0;
};

// A game in the table of games: its music manager's calls (GameStart), its stream manager's calls where it has one, the
// wait where the search's isn't the game's, the patches of its code, the word of its sound heap, and the instruction
// words that FindSdatCode checks in the ARM9 binary before it uses the entry: the first word of each routine that a
// call names, and the words that the comments give as the evidence of each constant.
struct GameStartEntry
{
    std::string_view game_code;
    std::span<const StubCall> calls;
    std::span<const NumberCallsEntry> numbers;
    std::span<const StubCall> stream_calls = {};
    std::span<const StubCall> effect_calls = {};
    uint32_t park = 0;
    std::optional<StubCall> wait;
    std::span<const GamePatch> patches;
    std::optional<uint32_t> heap_word;
    std::span<const GameWord> words;
    std::span<const GameStateEntry> states = {};
    std::span<const TrackStateEntry> track_states = {};
    TrackRoutines track_routines = {};

    // Whether numbers with one record share a mini where their runs start them alike (GameStart::merge_same_starts).
    bool merge_same_starts = false;
};

// FindSdatCode for `rom` with the starts of `entry`, whatever the game code of `rom`: the routines of a download-play
// program (rip/program_set.h). The header of such a program doesn't name the game that sends it.
std::optional<SdatCode> FindTableCode(const ntdlib::ntr::Rom& rom, const GameStartEntry& entry, std::string& error);

} // namespace twosf::rip
