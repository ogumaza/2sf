// SPDX-License-Identifier: MIT

// The streams that a game plays from files of its own, outside the SDK's sound archive, through a play routine in the
// game's code. The table contains The Legend of Zelda: Four Swords Anniversary Edition (USA and Europe, KQ9E and KQ9V).
// Its play routine takes a number: a sequence from 400, a stream from 490. A table of stream records in a file of the
// game gives each stream's file, its format and its loop.
//
// A game stream's mini is a stub that calls the play routine with the stream's number, then calls the game's frame
// routine once a frame, in place of the library's stub. The frame routine updates the game's stream objects and calls
// the update of the SDK's sound library. The stream's file goes in a library of its own at its offset in the image,
// where the set's library has zeros. A file that a boot reads before the hook stays in the set's library, and its
// stream requires no second library.

#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/ntr/rom.h"

#include "rip/sdat_set.h"
#include "rip/stream_set.h"

namespace twosf::rip
{

// A stream of the game's table, as its record gives it.
struct GameStream
{
    std::string label;       // the file's name without its extension
    uint32_t number = 0;     // the play routine's argument
    uint32_t file_id = 0;    // the image's file
    uint32_t rate = 0;       // the samples a second
    uint32_t samples = 0;    // the samples of the file
    uint32_t loop_start = 0; // the loop's first sample
    bool loop = false;
};

// The game streams of an image: the play routine, the frame routine and the streams in the order of their numbers.
struct GameStreams
{
    uint32_t play = 0;
    uint32_t frame = 0;
    std::vector<GameStream> streams;
};

// The game streams of `rom`. Nothing for a game outside the table. For a game in the table whose code or records differ
// from the table's, nothing, with the reason in `error`.
std::optional<GameStreams> FindGameStreams(const ntdlib::ntr::Rom& rom, std::string& error);

// The stub of `stream`'s mini at the stub's address of `code`: the play routine's call with the stream's number, then
// the frames, each with a call of the frame routine (BuildCallStub).
SdatStub BuildGameStreamStub(const SdatCode& code, const GameStreams& streams, const GameStream& stream);

// The target of `stream`'s mini run (RunStreamMini), in `rom`, the set's image with the stream's file in place: one
// channel set up at the record's rate, samples and loop, with data that start with the stream file's first 0x200 bytes.
// Only the lists of the driver's commands after the hook count. The files of `reserved` (CutArchive) are left out.
StreamTarget GameStreamTarget(const ntdlib::ntr::Rom& rom, const GameStream& stream,
                              const std::set<uint32_t>& reserved);

} // namespace twosf::rip
