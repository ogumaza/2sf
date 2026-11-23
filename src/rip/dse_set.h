// SPDX-License-Identifier: MIT

// The parts of a set for a game on Procyon Studio's sound driver, Digital Sound Elements (DSE): the game's routines,
// the stub, the library's image and the run of a mini on ntdlib's DS.
//
// DSE's sequencer runs on the ARM9, in a thread of its own that an alarm of the ARM7's sound driver wakes once a tick.
// A set runs the game's code, and the game's code plays the song. The game's main function calls the game's start-up
// function once. The start-up function starts the sound, the thread of the game's frames, their alarm and the handler
// of the vertical blank. Then it starts the thread of the game's tasks and runs its main loop. The hook is the first
// instruction after the handler's set-up. The stub goes there, over code that a set never runs, and never returns. It
// starts the song with the game's routine for music (BGM) or for a short piece of music (ME), by the song's number,
// then sleeps. The game's sound thread loads the song and its bank through the game's file system, and starts it. Where
// the table gives the game's start of music at a level, the stub starts music with it, with no fade, at the level of
// the song's state (ntdlib/dse/game_starts.h).
//
// The set changes two more places in the game's code: the loop of the game's frames (MakeDseSet), and the compare that
// refuses music 0 in the sound thread's start of music. Each game has a song 0 that none of its routines starts. With
// the change, the game's code starts it like any other song.
//
// The library is a compact image (ntdlib/ntr/compact_image.h): the files that the game's boot reads before the hook,
// whole, and the files of the set's songs. A mini stores its song's number in the stub's first word, 1 in the second
// word for an ME, and the level in the third where the song's level isn't the library's.
//
// Other games start a song by its name in the index of the game's sound pack, and run the game's sound work once a
// frame from their main loop. The hook is then the main loop's first instruction. The stub starts the song with the
// game's routine for music, by name, then waits for each frame and runs the game's sound work for it. Their songs and
// banks are in the pack. The library keeps the pack in place, with zeros in place of every byte that neither the boot
// reads nor a song of the set uses. A mini stores the song's name in the stub's data.
//
// The Professor Layton games run their engine and their sound in overlays. The hook follows the engine's start of its
// parts, in an overlay that a set stores decompressed. The stub makes the game's calls that start a piece of music by
// its number on the game's player of music, then sleeps or runs the game's sound work for each frame. The player loads
// the song and its bank in the stub's thread. A song inside a pack or an archive that the game decompresses keeps the
// ranges of the pack that a run of its mini on the full image reads. A mini stores the song's number.
//
// The Shiren games start a song by number with the game's request of music, in the thread that makes it. Shiren DS 2
// takes the level of the song's state too, and the stub passes it. Shiren 4 and 5 start a short piece with another
// request. The sound's work for a frame plays that request: the stub then runs the frames. Their game code is Thumb
// code, and so is the hook. The code after the hook has too little room, and the stub goes over a routine that only the
// game's shutdown runs. The hook switches to ARM state and jumps there. Some of Shiren 5's events lower a song right
// after its start, and the level after the change is the song's state. The stub of such a state makes the game's change
// of the music's volume after the request, and the sound's work for each frame then applies it.
//
// Professor Layton and the Last Specter contains a second program, London Life. London Life starts DSE again with a
// tick of its own and plays songs of its own. The game's main function runs London Life in place of the main game after
// a reset with a parameter. A set of the second program patches the main function's read of the parameter. The set's
// boot then starts London Life, and its hook follows London Life's start of its parts. Its songs and banks are members
// of a DARC archive.
//
// The Luminous Arc games and Soma Bringer start a song by number with the game's request of music, then sleep or run
// the game's sound work for each frame. Luminous Arc 2 and 3 keep their songs as loose files, and the game's table
// gives each song's name in 8 bytes. Luminous Arc keeps its songs as members of a pack, and the game's table gives each
// song's member. The library keeps the pack's table of members. The game reads it at its first load. Soma Bringer keeps
// its songs in a cartridge image inside the game's file system, a file for each song, named by a letter and a number.
// The library keeps each whole file that a run of a song reads. In Luminous Arc 2 and 3 and in Soma Bringer, the stub
// goes over a routine that a set never runs, away from the hook, and the hook jumps there.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/dse/game_starts.h"
#include "ntdlib/dse/sets.h"
#include "ntdlib/ntr/machine.h"
#include "ntdlib/ntr/rom.h"

#include "rip/arm_code.h"
#include "rip/boots.h"
#include "rip/sadl.h"

namespace twosf::rip
{

// How a set's stub starts a song.
enum class DseStartKind
{
    kNumber, // the game's start of music or of a short piece takes the song's number, and the stub then sleeps
    kName,   // the game's start of music takes the song's name in the pack's index, and the stub runs the frames
    kCalls,  // the stub makes the calls of DseCode::calls with the song's number, then sleeps or runs the frames
};

// How the index of a game's sound pack finds a file.
enum class DsePackIndex
{
    kNames,    // an entry of 32 bytes for each file: the name in 24 bytes, the file's offset in the pack and its size
    kCrcTree,  // a tree that the bits of a key's CRC-32 walk, from the lowest, to an entry: the offset and the size
    kCrcTable, // entries of 16 bytes in the order of their keys' CRC-32: the CRC-32, the offset and the size
    kOffsets,  // a word for each entry, its offset in the pack, then the pack's size: entry n is song n
    kMembers,  // in the pack: at 0x14 the number of words of the table at 0x20, an offset from 0x10 and a size for each
               // member. The game's table of songs gives each song's member.
    kRom,      // the pack is a cartridge image, and its folder `pack_index` contains a file for each song, named by the
               // song's number (DseCode::name_numbers)
    kDarc      // the pack is a DARC archive (DseDarcMembers), and song n is member `DseCode::first_member` + n
};

// A prefix of the names of a game's files of songs, and the number of the song whose name is the prefix and a number of
// 0 in decimal.
struct DseNameNumber
{
    std::string_view prefix;
    uint32_t base = 0;
};

// The bytes of a song's name in a stub's data, the terminating zero included.
constexpr uint32_t kDseNameBytes = 24;

// The arguments of a DseCall that stand for the song's number, the level of the song's state, and the volume and the
// frames of the change that follows the start (DseStart::change).
constexpr uint32_t kDseSongNumber = 0xFFFFFFFF;
constexpr uint32_t kDseSongLevel = 0xFFFFFFFE;
constexpr uint32_t kDseChangeVolume = 0xFFFFFFFD;
constexpr uint32_t kDseChangeFrames = 0xFFFFFFFC;

// A call of the game's code that a stub of kCalls makes: the routine, r0 to r3 and the words on the stack.
struct DseCall
{
    uint32_t routine = 0;
    std::array<uint32_t, 4> arguments = {};

    // The registers from r0 that the call sets: the routine reads no argument after them.
    uint32_t registers = 4;

    // The arguments that the call reads from memory: bit n set gives r(n) the word at the address that the argument
    // gives, such as a pointer to the game's sound object.
    uint32_t loads = 0;

    // What the call adds to the song's number in an argument of kDseSongNumber.
    uint32_t number_base = 0;

    // The arguments that the call passes as the table gives them where they equal kDseSongNumber or kDseSongLevel: bit
    // n set passes r(n) as the word, such as -1.
    uint32_t literals = 0;

    // The words of 0 that the call passes on the stack, after r3.
    uint32_t stack_zeros = 0;

    // The words that the call passes on the stack after r3, in their order, where one of them isn't 0. The call then
    // passes no `stack_zeros`.
    std::span<const uint32_t> stack = {};

    // For a routine that pushes no registers, such as one that only sets an object's fields: its first instruction.
    // FindDseCode checks it in place of a push with the link register. 0 for a routine that starts with that push.
    uint32_t first_instruction = 0;
};

// A kind of the game's streams of music (rip/sadl.h), and how a stub starts one. The game's code plays the streams
// beside DSE. For kCalls, `format` gives the streams' paths with %03u or %02u for the stream's number, from `first` to
// `last`, and the stub makes `calls`, with kDseSongNumber for the number, then runs the frames or sleeps as it does
// after a song's start. For kName, `format` gives the folder and the start of the streams' names, and the stub passes
// the file's name, with its extension, to the game's start of music. That routine plays a stream for a name that ends
// in .SAD.
struct DseStreamKind
{
    const char* format = nullptr;
    uint32_t first = 0;
    uint32_t last = 999;
    std::array<DseCall, 4> calls = {};
};

// The routines of a game on DSE that a set calls or watches, and where its code limits the songs' numbers. Each address
// is of code in the ARM9 binary's static part or in one of `overlays`: ARM code, or Thumb code where bit 0 is set.
struct DseCode
{
    std::string_view game_code;
    uint8_t revision = 0; // the header's byte 0x1E

    // The ARM9 overlays that the game has loaded at the hook and that contain the hook or a routine. A set stores the
    // overlay that contains the hook decompressed, with the stub in place.
    std::span<const uint32_t> overlays = {};

    // The hook, and the end of the code after it that a set never runs: the start of the function's literal pool, or of
    // a later function's where the functions between never run in a set. A hook in Thumb code starts a routine at a
    // multiple of 4.
    uint32_t hook = 0;
    uint32_t hook_end = 0;

    // Where the stub goes when the code after the hook has too little room for it: from `stub` up to `stub_end`, over
    // code that a set never runs, in the code that contains the hook. The hook then contains a jump to the stub in ARM
    // state. 0 for a stub at the hook.
    uint32_t stub = 0;
    uint32_t stub_end = 0;

    // In the loop of the thread of the game's frames: the instruction after the frame's count, and the instruction that
    // starts the frame's signal to the threads that wait for it. The set branches from the one to the other. 0 for a
    // game whose set runs no such thread.
    uint32_t frame_cut = 0;
    uint32_t frame_signal = 0;

    DseStartKind start = DseStartKind::kNumber;

    // The game's start of a piece of music: by number (PlayBgmById), or for kName, (player, 0, name, 0) and a fifth
    // argument of 0 on the stack, where the second argument picks the first of the player's slots for music, the fourth
    // gives the fade-out of the piece that plays and the fifth the fade-in.
    uint32_t play_bgm = 0;

    // The game's start of a piece of music by number at a level: (number, fade in frames, level), where 256 is full
    // level (PlayBgmByIdVolume). 0 where the table doesn't give it: the stub then starts music with `play_bgm`.
    uint32_t play_bgm_level = 0;

    uint32_t play_me = 0; // the same for a short piece (PlayMeById)
    uint32_t sleep = 0;   // OS_Sleep(milliseconds)

    // DSE's routines that the game's sound thread calls: the load of a wave bank, of a song, the song's start and its
    // stop.
    uint32_t load_bank = 0;
    uint32_t load_song = 0;
    uint32_t start_song = 0;
    uint32_t stop_song = 0;

    // Where `start_song` takes the song's volume: in the 16 bits at 0xC of the parameters that r1 points to (true), or
    // in r1 (false).
    bool volume_in_parameters = false;

    // DSE's change of a song's parameters. It takes the volume as `start_song` does. The check of a start with a change
    // watches it. 0 for a game without `change_calls`.
    uint32_t set_song = 0;

    // The instructions `cmp rN, #first` and `cmp rN, #last` that give the lowest and the highest number of music that
    // the sound thread's start of music takes, and `cmp rN, #last` for the start of short pieces. Music starts from 1,
    // and the short pieces from 0. The set changes the compare at `bgm_first` to `cmp rN, #0`. 0 for kName and kCalls.
    uint32_t bgm_first = 0;
    uint32_t bgm_limit = 0;
    uint32_t me_limit = 0;

    // The songs' paths, with the number for %04u, or for kCalls %03u. Null for kName.
    const char* bgm_format = nullptr;
    const char* me_format = nullptr;

    // For kName: the first argument of `play_bgm` and of `frame_update`, the game's wait for the next frame
    // (OS_WaitVBlankIntr) and the game's sound work for a frame. For kCalls: the wait, where it isn't 0.
    uint32_t player = 0;
    uint32_t frame_wait = 0;
    uint32_t frame_update = 0;

    // For kName: the pack that contains the songs and their banks, and its index. Null for a game whose songs are files
    // of their own: a song's name is then its file's name without the extension. An index by CRC-32 finds a song and
    // its bank by the key NAME.SMW, lowercase: an entry that starts with the size of its header, then the song. For
    // kCalls, an index of kOffsets gives the song's number, and `me_pack` and `me_pack_index` those of the short
    // pieces.
    const char* pack = nullptr;
    const char* pack_index = nullptr;
    DsePackIndex index_kind = DsePackIndex::kNames;
    const char* me_pack = nullptr;
    const char* me_pack_index = nullptr;

    // For kCalls with a pack of kRom: the number of each song by its file's name, without the extension. The name is
    // one of these prefixes and a number in decimal. The song's number is the prefix's base plus that number.
    std::span<const DseNameNumber> name_numbers = {};

    // For kCalls with a pack of kDarc: the member that contains song 0, and the member that contains its bank. The game
    // loads song n with the bank in member `first_bank_member` + n.
    uint32_t first_member = 0;
    uint32_t first_bank_member = 0;

    // The main bank that the game loads with each song, where its path differs from the main bank that the song's bank
    // names in the game's files (ntdlib::dse::FindSequenceSet), with the same bytes. Null for that main bank.
    const char* main_bank = nullptr;

    // The changes of the game's code that let the boot reach the hook (GamePatch). The ripper boots the game with them
    // and makes the set from that image (PatchedImage). The set's image therefore contains them.
    std::span<const GamePatch> patches = {};

    // For kCalls: the game's table of songs, where it gives a song's number in place of `bgm_format`. Each of its
    // `song_count` entries has 12 bytes: the address of the song's name, of its bank's name, and a byte that is 1 for a
    // short piece. The song's path is `bgm_format` or `me_format` with the name, or `marked_format` with the rest of a
    // name that starts with '='. Where `song_name_bytes` isn't 0, each entry is the song's name instead, in that many
    // bytes, and the song's path is `bgm_format` with the name. For a pack of kMembers, the low 16 bits of an entry's
    // first word give the number of the member that contains song n, and an entry whose first word is 0 gives no song.
    uint32_t song_table = 0;
    uint32_t song_count = 0;
    const char* marked_format = nullptr;
    uint32_t song_name_bytes = 0;

    // For kName: what follows the name in the game's start of music, such as ".SMW", and whether the game loads the
    // song before its bank.
    const char* name_suffix = "";
    bool song_before_bank = false;

    // For kCalls: the calls that start a piece of music, in order, and the game's work for each frame. The stub runs
    // that work after each frame's wait. A call whose routine is 0 is left out. The stub sleeps where `frame_wait` is
    // 0. `bgm_format` gives the songs' paths, or the path of the pack or archive member that contains a song. A game
    // that starts a short piece with other calls gives them in `me_calls`.
    std::array<DseCall, 4> calls = {};
    std::array<DseCall, 4> frame_calls = {};
    std::array<DseCall, 4> me_calls = {};

    // For kCalls: the game's change of the music's volume. The stub of a start with a change (DseStart::change) calls
    // it after `calls`, with kDseChangeVolume and kDseChangeFrames for the change's values.
    std::array<DseCall, 1> change_calls = {};

    // The kinds of the game's streams of music, in the order of the set's minis after the songs': the music, then the
    // jingles and other short pieces. `stream_build` gives the build of the game's stream player.
    std::span<const DseStreamKind> streams = {};
    SadlBuild stream_build = SadlBuild::kEarlier;
};

// Whether FindDseCode's table lists the game whose header gives `game_code`.
bool ListsDseGame(std::string_view game_code);

// A second program of a game on DSE that starts DSE again with settings and songs of its own: its routines, and the
// name that its set's library takes after the game's library.
struct DseProgram
{
    std::string_view name;
    DseCode code;
};

// Whether the stub of `code` starts music at the level of the song's state: by the game's start at a level, or by a
// call with an argument of kDseSongLevel.
bool DseTakesLevel(const DseCode& code);

// The routines of the game in `rom`, by its game code and revision: the three Explorers games of Pokémon Mystery
// Dungeon, the four Inazuma Eleven games, the three Professor Layton games, the three Shiren games, the three Luminous
// Arc games and Soma Bringer that dse_set.cpp's table lists. Returns nothing, with `error` set, for another game, or
// when the code at an address isn't what the table expects: a push with the link register at each routine's start, or
// the call's `first_instruction`, a call before the hook or a push with the link register at it, a call four
// instructions before the frame's cut (the thread's sleep), a call after the frame's signal starts and a branch back to
// the loop's start two instructions later, a compare with an immediate at each limit, and a compare with 1 at
// `bgm_first`, or with 0 in a set's library. A hook in Thumb code must be at a multiple of 4 and start a routine or
// follow a call.
std::optional<DseCode> FindDseCode(const ntdlib::ntr::Rom& rom, std::string& error);

// The second programs of the game in `rom` (Layton 4's London Life), each checked as FindDseCode checks a game, and
// each patch's word too (GamePatch::expected, or in a set's library the patch's value). Returns nothing, with `error`
// set, when a check of a program fails, and none for a game without a second program.
std::optional<std::vector<DseProgram>> FindDsePrograms(const ntdlib::ntr::Rom& rom, std::string& error);

// A stream of the game that a kind of `DseCode::streams` reaches: its path, its file, its kind, its start (the number,
// or the name for kName) and its label, the file's name without the extension.
struct DseStream
{
    std::string path;
    uint32_t file_id = 0;
    std::size_t kind = 0;
    uint32_t number = 0;
    std::string name;
    std::string label;
};

// The streams of `rom` that the kinds of `code` reach, kind by kind, each kind's in the order of their paths. A file
// whose header the stream player can't read (ReadSadlHeader) is left out.
std::vector<DseStream> DseStreams(const ntdlib::ntr::Rom& rom, const DseCode& code);

// The level of music that the game starts at full level.
constexpr uint32_t kDseFullLevel = 256;

// How a set starts a song: by its number, as music or as a short piece, and music at its level, or for kName by its
// name in the pack's index, without the extension. `change` is the change of the music's volume that the game's code
// makes right after the start, for a state that the change reaches (DseCode::change_calls).
struct DseStart
{
    friend bool operator==(const DseStart&, const DseStart&) = default;

    uint32_t number = 0;
    bool me = false;
    uint32_t level = kDseFullLevel;
    std::string name;
    std::optional<ntdlib::dse::StartChange> change = {};
};

// The numbers that the game's starts of music and of short pieces take, from the code at the limits. Music below
// `first_bgm` is music that the game never starts.
struct DseLimits
{
    uint32_t first_bgm = 0;
    uint32_t last_bgm = 0;
    uint32_t last_me = 0;
};

DseLimits ReadDseLimits(const ntdlib::ntr::Rom& rom, const DseCode& code);

// The start of the song at `path`, a path in the game's file system. Music takes 0 to `limits.last_bgm`: the set's
// change to the game's code lets music 0 start too. Nothing for a path that neither format gives with such a number.
// For kCalls, a path inside the file that the format gives counts too: the format's path, a '/' and the rest.
std::optional<DseStart> DseStartOf(const DseCode& code, const DseLimits& limits, const std::string& path);

// For kName: the starts of the songs in the pack's index `index`, by each file's offset in the pack. A name of more
// than kDseNameBytes - 1 bytes gets no start.
std::map<uint32_t, DseStart> DsePackStarts(std::span<const uint8_t> index);

// For an index by CRC-32: the offset in the pack of the entry for `key`, with the case of its letters ignored. Nothing
// where the index ends first or has no entry with the key's CRC-32. A tree gives an entry for any key: the caller
// checks what it finds.
std::optional<uint32_t> DsePackEntry(DsePackIndex kind, std::span<const uint8_t> index, std::string_view key);

// For an index of kOffsets: the number of the entry that contains the pack's byte at `offset`. Nothing where no entry
// contains it.
std::optional<uint32_t> DseOffsetEntry(std::span<const uint8_t> index, uint32_t offset);

// For a pack of kMembers: the number of the member that starts at `offset` in `pack`. Nothing where none does.
std::optional<uint32_t> DseMemberAt(std::span<const uint8_t> pack, uint32_t offset);

// For a pack of kMembers: the end of its table of members. The game reads the table from the pack.
uint32_t DseMembersEnd(std::span<const uint8_t> pack);

// For a pack of kDarc: each member's range in the pack, its header of 16 bytes included, in the members' order. The
// archive gives at 4 the number of members, then from 8 a word for each member: member k starts that many bytes after
// 12 + 4k. A member's header gives its size at 4, and its data follow the header. None where `pack` isn't such an
// archive or a member is outside it.
std::vector<std::pair<uint32_t, uint32_t>> DseDarcMembers(std::span<const uint8_t> pack);

// For a pack of kDarc: the member whose data start at `offset` of `pack`. Nothing where none does.
std::optional<uint32_t> DseDarcMemberAt(std::span<const uint8_t> pack, uint32_t offset);

// For a pack of kRom: a file of the folder of the cartridge image, its name without the extension and its range in the
// pack.
struct DseRomEntry
{
    std::string name;
    uint32_t first = 0;
    uint32_t end = 0;
};

// The files of the folder `folder` of the cartridge image `pack`. None where `pack` isn't a cartridge image.
std::vector<DseRomEntry> DseRomEntries(std::span<const uint8_t> pack, std::string_view folder);

// For a pack of kRom: the number of the song whose file has the name `name` (DseCode::name_numbers). Nothing for a name
// that no prefix starts with, or that has no number of 1 to 4 digits after it.
std::optional<uint32_t> DseNameNumberOf(const DseCode& code, std::string_view name);

// For a pack of kMembers: the number of each song by the number of its member, from the game's table of songs
// (`code.song_table`).
std::map<uint32_t, uint32_t> DseMemberNumbers(const ntdlib::ntr::Rom& rom, const DseCode& code);

// For kCalls with `code.song_table`: the start of each song that the game's table names, by the song's path. A song
// that the table names twice keeps its first number.
std::map<std::string, DseStart> DseTableStarts(const ntdlib::ntr::Rom& rom, const DseCode& code);

// The stub at `code.stub`, or else at `code.hook`, and the offset of its words from its start: the song's number, then
// 1 for a short piece, then the level of music where the stub starts music at a level (DseTakesLevel). For kName: the
// song's name in kDseNameBytes. For kCalls: the song's number, and the word of a short piece where the game gives
// `me_calls`, and the level where a call takes it. Where the game gives `change_calls`, the level follows in any case,
// then the change's volume, 0 for a start without a change, and its frames.
struct DseStub
{
    std::vector<uint8_t> bytes;
    uint32_t start_offset = 0;
};

DseStub BuildDseStub(const DseCode& code, const DseStart& start);

// The stub that starts `stream`: for kCalls, the calls of its kind in place of the song's (BuildDseStub), for kName the
// stub of the start by the stream's name.
DseStub BuildDseStreamStub(const DseCode& code, const DseStream& stream);

// The library's image of `rom`: the ARM9 binary with `stub` at the hook, or at `code.stub` with a jump to it at the
// hook, the frame's cut and the start of music from 0, and the files `files`. For a code with patches, `rom` is the
// image with them (PatchedImage). `start_word` gives the offset in the image of the stub's first word
// (DseStub::start_offset), and `stub` the offset of the stub. Where the hook is in an overlay, the overlay's file in
// the image contains the overlay decompressed with the stub. The overlay table then marks the overlay uncompressed, and
// the game's loader loads it as it is. `files` must contain the overlay's file.
//
// The thread of the game's frames wakes once a frame. It counts the frame, runs the game's work for it, signals the
// frame to the threads that wait for it, and runs the sound's work for the frame. In a set it counts the frame and goes
// straight to the signal. The game's work for a frame reads the buttons, the touch screen and the hinge, and steps the
// game's random numbers. Where compared, the audio is the same without it. Without the cut, the player spends over 90 %
// of its time there. The game's start of a song waits for the signal.
struct DseSet
{
    std::vector<uint8_t> image;
    uint32_t start_word = 0;
    uint32_t stub = 0; // the offset in the image of the stub's first byte
};

// Throws std::runtime_error when `stub` doesn't fit between the hook and the end of its code, or between `code.stub`
// and `code.stub_end`.
void CheckDseStubPlace(const DseCode& code, const DseStub& stub);

// Throws std::runtime_error when the stub doesn't fit its place (CheckDseStubPlace), or when `files` doesn't contain
// the file of the hook's overlay.
DseSet MakeDseSet(const ntdlib::ntr::Rom& rom, const DseCode& code, const DseStub& stub,
                  const std::map<uint32_t, std::vector<uint8_t>>& files);

// The bytes of a mini's block for `start` in a library whose stub starts `library`: the number, then the word 1 for a
// short piece or 0 for music where the library's song is a short piece or the level or the change differs, then the
// level of music where it or the change differs from the library's, then the change's volume and frames, or two words
// of 0 for music without a change, where the change differs. A start by name gives the name, padded with zeros to
// kDseNameBytes.
std::vector<uint8_t> DseStartWords(const DseStart& start, const DseStart& library);

// Whether the files of `a` and `b`, indices into `files`, have the same bytes: the songs, the banks in their order and
// the main banks.
bool SameDseFiles(std::span<const ntdlib::dse::NamedFile> files, const ntdlib::dse::SequenceSet& a,
                  const ntdlib::dse::SequenceSet& b);

// Whether a mini that starts the song of `b` plays as one that starts the song of `a`: the files have the same bytes
// (SameDseFiles), and DSE starts both songs at the same volume on every boot. `a_volumes` and `b_volumes` give the
// volume of DSE's start of each song on each boot, in the boots' order.
bool SameDseStart(std::span<const ntdlib::dse::NamedFile> files, const ntdlib::dse::SequenceSet& a,
                  std::span<const int> a_volumes, const ntdlib::dse::SequenceSet& b, std::span<const int> b_volumes);

// What a mini's run on ntdlib's DS gives: `start_ms` is the time from the boot to the call of DSE's start of the song.
// `sounded` says whether the sound unit's output changed after the start. `error` says why the run doesn't count.
// `reads` gives the ranges of each file that the card's reads reached from the hook to DSE's start of the song.
// `stop_ms` gives the time from DSE's start of the song to the game's stop of it, where the game stops the song within
// kDseMiniPlaySeconds. `volume` is the volume that the game's code passes to DSE's start of the song, or for a state
// with a change the volume that it passes to the last change.
struct DseMiniRun
{
    std::optional<std::string> error;
    double start_ms = 0;
    int volume = 0;
    bool sounded = false;
    std::optional<double> stop_ms;
    std::map<uint32_t, FileRanges> reads;
};

// The time that a mini's run plays on after its song starts.
constexpr double kDseMiniPlaySeconds = 5.0;

// The time within which the game's code makes a change of the music's volume after the song's start.
constexpr double kDseChangeSeconds = 1.0;

// Boots `image`, a set's image with a mini in place, with `backup` in its card, until DSE starts the song, for at most
// `seconds`. Then it plays on for kDseMiniPlaySeconds, or until the game stops the song. The run fails:
// - when the boot doesn't reach the hook, or DSE doesn't then load a bank, load a song and start it, in that order (the
//   song first where `code.song_before_bank` is set), in time;
// - when the bank that DSE loads first after the hook isn't `bank`, or the song isn't `song`;
// - when DSE's start of the song takes a volume other than the one of `state`, the mini's state
//   (ntdlib::dse::StartOf), or for a state with a change the one of the level before the change;
// - for a state with a change, when DSE's change of the song's parameters doesn't come as many times as the change has
//   frames within kDseChangeSeconds of the start, or the last of them takes a volume other than the one of the state;
// - when a card read after `hook_clock`, the clock of the hook on the boot with `backup`, reaches neither the file
//   system's tables nor any of the image's files, or reaches a file of `packs` outside the ranges that it gives.
DseMiniRun RunDseMini(std::span<const uint8_t> image, const DseCode& code, uint64_t hook_clock,
                      std::span<const uint8_t> bank, std::span<const uint8_t> song, const ntdlib::dse::SongState& state,
                      double seconds, const ntdlib::ntr::BackupMemory& backup = {},
                      const std::map<uint32_t, FileRanges>& packs = {});

} // namespace twosf::rip
