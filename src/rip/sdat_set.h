// SPDX-License-Identifier: MIT

// The parts of a set for a game on the SDK's sound archive player (NITRO-System's, with a sound archive, SDAT): the
// game's routines, the boot to the hook, the stub, the library's image and the run of a mini on ntdlib's DS.
//
// A set runs the game's code. Its ARM9 binary and the overlays that it keeps are stored decompressed. The start-up
// decompressor then returns at once whenever the game calls it: the entry code calls it for the ARM9 binary, and the
// SDK's overlay loader for each compressed overlay. Its body never runs, and the stub goes there. The first instruction
// of the sound library's update (NNS_SndMain) becomes a branch to the stub. The game calls the update for the first
// time once it has set up its sound heap and opened its archive. The stub never returns. It loads the sound's data into
// the game's sound heap and starts the sound with the game's routines. Then, once a frame, it waits for the vertical
// blank and runs the update: the instruction that the branch replaced, then the rest of the update.
//
// The library is a compact image (ntdlib/ntr/compact_image.h): the files that the game's boot reads before the hook,
// whole, and the sound archive cut to the data of the set's sounds. A mini stores its sound's number in the stub's
// word for it.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/ntr/code.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/sdat/formats.h"

namespace twosf::rip
{

// The game's routines that a set calls. Each is ARM code.
struct SdatCode
{
    uint32_t update = 0;            // NNS_SndMain
    uint32_t update_first = 0;      // the update's first instruction, which the hook replaces
    uint32_t start = 0;             // NNS_SndArcPlayerStartSeq(handle, sequence)
    std::optional<uint32_t> loader; // NNS_SndArcLoadSeq(sequence, heap), which returns nonzero once loaded
    uint32_t load = 0;              // without a loader: (sequence, 0xFF, heap, 1, null), which returns 0 once loaded
    uint32_t wait = 0;              // OS_WaitVBlankIntr

    // The decompressor's body, from `stub` up to `stub_end`, which a set never runs.
    uint32_t stub = 0;
    uint32_t stub_end = 0;
};

// The game's sound archive: the file of the image that holds it at its start, with sequences.
struct SoundArchive
{
    uint32_t file_id = 0;
    ntdlib::sdat::Archive archive;
};

// Finds the routines in `rom`'s ARM9 binary (ntdlib/ntr/sound_routines.h). Returns nothing, with `error` set, when the
// search doesn't give exactly one update, start and load and one wait, when it finds no start-up decompressor, when
// the update or the decompressor lies outside the binary's static part, when the update's first instruction can't run
// elsewhere (Relocatable), or when the stub doesn't fit in the decompressor's body.
std::optional<SdatCode> FindSdatCode(const ntdlib::ntr::Rom& rom, std::string& error);

// Whether the ARM instruction `word` runs the same at another address: a push of registers other than the pc onto the
// stack (stmfd sp!, or str rd, [sp, #-4]!), or an instruction of data processing whose registers aren't the pc. The
// hook moves the update's first instruction into the stub.
bool Relocatable(uint32_t word);

// The first of `files`, file IDs of `rom`, that contains a sound archive at its start with at least one sequence.
// Nothing when none does.
std::optional<SoundArchive> FindSoundArchive(const ntdlib::ntr::Rom& rom, const std::set<uint32_t>& files);

// The sequences outside the sequence archives that count as music, by index: the music of ntdlib::ntr::rip::IsMusic
// in an archive with names. When none counts, or the archive has no names, every sequence outside the sequence
// archives does. The SDK's tools put most sound effects in sequence archives.
std::set<uint32_t> MusicSequences(const ntdlib::sdat::Archive& archive);

// The game's sound heap (NNSSndHeapHandle): the heap whose frame heap, with the signature FRMH 0x10 bytes after the
// heap's start, contains the others. A frame heap counts only when its start, its head, its tail and its end lie in
// main memory in that order. `free` gives the frame heap's free bytes.
struct SoundHeap
{
    friend bool operator==(const SoundHeap&, const SoundHeap&) = default;

    uint32_t address = 0;
    uint32_t free = 0;
};

// The sound heap in `memory`, the DS's main memory from 0x02000000. Nothing when it holds no frame heap.
std::optional<SoundHeap> FindSoundHeap(std::span<const uint8_t> memory);

// What a boot of an image gives at its first call of the update: the time, the ARM9's CPSR, the files that the card's
// reads reached, and the sound heap.
struct HookState
{
    bool reached = false;
    uint64_t clock = 0; // the machine's clock, in cycles of the system clock
    double seconds = 0;
    uint32_t cpsr = 0;
    std::set<uint32_t> files;
    std::vector<std::pair<uint32_t, uint32_t>> reads; // each read's start and end in the image
    std::optional<SoundHeap> heap;
};

// Boots `rom` on ntdlib's DS until the ARM9 reaches `update`, for at most 30 s.
HookState BootToHook(const ntdlib::ntr::Rom& rom, uint32_t update);

// Why the game's state at the hook doesn't let a stub run: no hook, no heap, an interrupt's mode or interrupts off.
// Nothing when it does.
std::optional<std::string> HookProblem(const HookState& hook);

// The IDs of the files of `archive`, which starts at `start` in the image, that the reads of `hook` reach.
std::set<uint32_t> ArchiveFilesRead(const HookState& hook, uint32_t start, const ntdlib::sdat::Archive& archive);

// `archive` cut to the data that `sequences` play (ntdlib::ntr::rip::KeepSequenceData), with the files `extra` kept
// too, in the archive's layout. Nothing when the cut fails: then a set holds the whole archive.
std::optional<std::vector<uint8_t>> CutArchive(const ntdlib::sdat::Archive& archive,
                                               std::span<const ntdlib::ntr::SequenceRef> sequences,
                                               const std::set<uint32_t>& extra);

// The stub at `code.stub`, and where its word for the sequence's number, `sound`, lies from its start.
struct SdatStub
{
    std::vector<uint8_t> bytes;
    uint32_t sound_offset = 0;
};

SdatStub BuildSdatStub(const SdatCode& code, uint32_t heap, uint32_t sound);

// The library's image of `rom`: the ARM9 binary decompressed with `stub` and the hook, and the files `files`. The SDK's
// overlay loader decompresses an overlay with the start-up decompressor. A kept overlay that the image compresses
// therefore goes in decompressed, and its entry in the overlay table loses its compression flags.
// `sound_word` gives the offset in the image of the stub's word for the sound's number, and `stub` the stub's offset.
struct SdatSet
{
    std::vector<uint8_t> image;
    uint32_t stub = 0;
    uint32_t sound_word = 0;
};

SdatSet MakeSdatSet(const ntdlib::ntr::Rom& rom, const SdatCode& code, const SdatStub& stub,
                    const std::map<uint32_t, std::vector<uint8_t>>& files);

// What a mini's run on ntdlib's DS gives: its boot, then the sound's start. `start_ms` is the time from the boot to the
// list of the driver's commands that starts the sound. `variables` contains the global variables that the commands up
// to then set. `error` says why the run doesn't count.
struct MiniRun
{
    std::optional<std::string> error;
    double start_ms = 0;
    ntdlib::ntr::Variables variables;
};

// The time that a mini's run plays on after its sequence starts.
constexpr double kMiniPlaySeconds = 5.0;

// Boots `image`, a set's image with a mini in place, until `sequence` of the set's sound archive, the file
// `archive_file`, starts, for at most `seconds`. Then it plays on for kMiniPlaySeconds. The run fails:
// - when the sequence doesn't start in time;
// - when the command that prepares the sequence doesn't name the sequence's bytes and its bank's in main memory, or
//   when a slot of the bank that names a wave archive has no link to main memory once the load has written the links;
// - when a later list of the driver's commands prepares, starts or stops a sequence, sets a variable, or sets a
//   volume, a mute or a parameter of a player or a track: the lengths assume that the game leaves the sequence alone;
// - when a card read after `hook_clock`, the clock of the hook (HookState), reaches none of the image's files and none
//   of the archive's.
MiniRun RunMini(std::span<const uint8_t> image, uint64_t hook_clock, uint32_t archive_file,
                const ntdlib::ntr::SequenceRef& sequence, double seconds);

} // namespace twosf::rip
