// SPDX-License-Identifier: MIT

// The streams (STRM) of a set for a game on the SDK's sound archive player. The game's code plays them, as it plays the
// sequences.
//
// The library's sound archive keeps the record and the FAT entry of each stream of the set, and the stream's file
// contains zeros there. Each stream file goes in a library of its own, at the file's offset in the library's image. A
// stream's mini names the set's library as `_lib` and the stream's library as `_lib2`. Its block is a stub that calls
// the SDK's NNS_SndArcStrmStart, or the routines of the game's stream manager where the table of games gives them
// (rip/sdat_set.h, SoundKind::kStream). The block is written over the library's stub. The library and the sequences'
// minis stay as they are without streams.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/ntr/code.h"
#include "ntdlib/ntr/machine.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/sdat/formats.h"
#include "xsflib/rip/length.h"

#include "rip/boots.h"

namespace twosf::rip
{

// The routines of `arm9`, the ARM9 binary decompressed at its address, that have the shape of the SDK's
// NNS_SndArcStrmStart(handle, stream, offset). That routine makes two calls and returns. The first passes its three
// arguments to the stream's prepare, and the second passes the handle to the start of the prepared stream. The prepare
// calls the stream record's getter (NNS_SndArcGetStrmInfo) with the stream, reads the record's priority (a byte at 5)
// and player (a byte at 6), and calls the stream player's prepare with the handle. It loads nothing else. The rule
// reads ARM and Thumb code: Pokémon Black 2's sound code is Thumb. A Thumb start has bit 0 set.
std::vector<uint32_t> FindStreamStarts(const ntdlib::ntr::LoadedCode& arm9);

// A stream's label: its name in the archive, or "STRM_" and its number.
std::string StreamLabel(const ntdlib::sdat::Archive& archive, uint32_t stream);

// The archive's file of `stream`. Nothing when the stream has no record, or when its file isn't in the archive.
std::optional<uint32_t> StreamFile(const ntdlib::sdat::Archive& archive, uint32_t stream);

// Whether `stream` makes a sound: a volume above 0, and a sample of a channel that xsflib::rip::Audible counts. False
// when the stream's file doesn't parse.
bool StreamAudible(const ntdlib::sdat::Archive& archive, uint32_t stream);

// Where the library's image `image` places the file `file_id` of the sound archive, the image's file `archive_file`:
// the offset in the image. Nothing when the image's archive doesn't parse or has no such file.
std::optional<uint32_t> ArchiveFileOffset(std::span<const uint8_t> image, uint32_t archive_file, uint32_t file_id);

// What a stream mini's run on ntdlib's DS gives: its boot, then the stream's start. `start_ms` is the time from the
// boot to the first output sample of the sound unit in which the stream's channels play. `rate` is the channels' rate
// in Hz, from the timer that the game gives them. `sounded` says whether the sound unit's output changed after the
// start. `error` says why the run doesn't count.
struct StreamRun
{
    std::optional<std::string> error;
    double start_ms = 0;
    double rate = 0;
    bool sounded = false;
};

// A hardware channel as a list of the driver's commands sets it up: the address of its data, the bytes that it plays
// (the start and the length of its loop), the ticks of its timer a sample, its format (0 for PCM8, 1 for PCM16, 2 for
// IMA-ADPCM), whether it repeats its loop, and the loop's start in words of 4 bytes.
struct ChannelSetup
{
    uint32_t data = 0;
    uint32_t bytes = 0;
    uint32_t timer = 0;
    uint32_t format = 0;
    bool loops = false;
    uint32_t loop_start = 0;
};

// The level of a hardware channel as the driver's command sets it: the volume and the divider.
struct ChannelLevel
{
    uint32_t volume = 0;
    uint32_t divider = 0;

    bool operator==(const ChannelLevel&) const = default;
};

// The start of a stream's channels, from the lists of the driver's commands up to it. `setups` gives each hardware
// channel's setup, as the last list that set it up gave it. `hardware` gives the channels that play the stream once
// `started`, and `timer` the ticks of their timer a sample. `level` is the first level that a command after the start
// gives one of them, in the start list or a later one.
struct ChannelStart
{
    std::map<uint32_t, ChannelSetup> setups;
    bool started = false;
    std::set<uint32_t> hardware;
    uint32_t timer = 0;
    double rate = 0;
    std::optional<ChannelLevel> level;
};

// Reads `list`, a list of the driver's commands, into `start`, up to the command that starts the timers of a hardware
// channel for each of the stream's `channels`. Each such hardware channel's data in `memory`, the DS's main memory,
// starts with a head of `heads`: the first block of each of the stream's channels as the file stores it, then decoded
// to 16-bit PCM, in the channels' order. Where a channel plays fewer bytes than its block has, as the stream player's
// buffer of a stream of PCM that stores each channel as one block, the data equals the block's start. A silent start
// matches several channels. Where `rate` is given, a setup counts only when its timer gives that rate within 1 %. With
// `heads_after_start`, for a stream player that fills the buffers only after the start, each buffer of the channels
// that the command starts counts as one of the stream's channels, in their order, and the heads aren't read. Two
// hardware channels can play one buffer, such as a stream of one channel in the middle. Calls `fail` when the channels'
// timers differ. Returns the number of commands read: up to and including the command that starts the channels, or the
// whole list.
std::size_t ReadCommands(const ntdlib::ntr::snd::CommandList& list, std::span<const uint8_t> memory,
                         const std::vector<std::vector<uint8_t>>& heads, uint32_t channels, ChannelStart& start,
                         const std::function<void(std::string)>& fail, std::optional<double> rate = std::nullopt,
                         bool heads_after_start = false);

// Reads `list`, commands of the driver after the command that starts the stream's channels of `start`: the rest of the
// start list, or a later list. The length tag assumes that the game leaves the stream alone in the run. Calls `fail`
// when a command changes what the stream plays:
// 1. a command that starts the timer of one of the stream's channels, or sets one up (PCM, PSG or noise): another sound
//    takes the channel, or the stream restarts;
// 2. a command that stops the timer of one of them, unless `ends_in_run`: the stream has no loop and its samples end
//    within the run, where the stream player stops its channels;
// 3. a command that sets another timer for one of them than the start's: the length tag uses the start's rate; 4. a
// command that sets the volume of one of them to 0, or sets a level that differs from the first level after the
//    start command: a fade sends several levels. The stream player sends the first level in the start list, after
//    the start command, or sends none;
// 5. a command of Interference (rip/sdat_set.h) other than a channel's volume. The stream player sends none of them.
// Every other command passes: the pans, the alarms and the timers of other channels, and every command for other
// channels. The alarms refill the buffers. Records the first level after the start command in `start`.
void ReadCommandsAfterStart(std::span<const ntdlib::ntr::snd::Command> list, ChannelStart& start, bool ends_in_run,
                            const std::function<void(std::string)>& fail);

// Why the setup of a stream's channel that plays the whole stream from memory differs from the stream's `samples`
// samples, with a loop from `loop_start` where `loop` is set. The samples count from the data's first word: 4 a word in
// PCM8, 2 in PCM16 and 8 in IMA-ADPCM, the header's word included. The setup must repeat its loop exactly when `loop`
// is set, its data must end at `samples`, and a loop must start at `loop_start`. Nothing when they agree.
std::optional<std::string> SetupLoopError(const ChannelSetup& setup, uint32_t samples, uint32_t loop_start, bool loop);

// The card reads of a stream's file in the run of its mini. The stream player fills its buffers from the file before
// the stream's channels start, and refills them from it after the start.
class StreamFileReads
{
public:
    // The file's bytes in the image: from `start` up to `end`.
    StreamFileReads(uint32_t start, uint32_t end);

    // Records a card read of `bytes` bytes at `address`, before or after the start.
    void Add(uint32_t address, uint32_t bytes, bool after_start);

    // Whether a read after the start reached a byte of the file that no read before the start reached, or the reads
    // before the start reached every byte of the file.
    bool Refilled() const;

private:
    uint32_t start_ = 0;
    uint32_t end_ = 0;
    std::map<uint32_t, uint32_t> before_; // the bytes read before the start: the first of each run, and its end
    bool refilled_ = false;
};

// The stream that a mini's run waits for, and what its run checks (RunStreamMini).
struct StreamTarget
{
    std::set<uint32_t> left_out; // the archive's files that a card read after the hook must not reach
    uint32_t file_start = 0;     // the stream file's bytes in the image: from file_start up to file_end
    uint32_t file_end = 0;
    std::vector<std::vector<uint8_t>> heads; // the heads of the stream's channels (ReadCommands)
    uint32_t channels = 0;
    std::optional<double> rate; // the rate that a record of the game gives the stream (ReadCommands)
    uint32_t samples = 0;
    uint32_t loop_start = 0;
    bool loop = false;
    bool commands_after_hook = false; // whether the lists of the driver's commands count only after the hook
    bool loop_in_setup = false;       // whether the channel's setup must give the samples and the loop (SetupLoopError)
    bool heads_after_start = false;   // whether the stream player fills the channels' buffers after their start
};

// The target of `stream` of the sound archive in `rom`, the image's file `archive_file`: the heads of the stream's
// first blocks, as the file stores them and decoded to 16-bit PCM, the stream file's bytes in the image, and the
// stream's samples and loop. The files of `reserved` (CutArchive) other than the stream's are left out: the files of
// the set's other streams contain zeros in the image.
StreamTarget ArchiveStreamTarget(const ntdlib::ntr::Rom& rom, uint32_t archive_file, uint32_t stream,
                                 const std::set<uint32_t>& reserved);

// Boots `image`, a set's image with a stream's mini and library in place, with `backup` in its card, until the channels
// of `target` start, for at most `seconds`. Then it plays on for kMiniPlaySeconds (rip/sdat_set.h). The run fails:
// - when no list of the driver's commands sets up a channel for each of the stream's channels and starts them, or when
//   the sound unit doesn't play them in time (ReadCommands);
// - when a channel that the list sets up doesn't play the stream: its data in main memory must start with a head of
//   its channel. Where the head is longer than the data that the channel plays, the data must equal the head's start.
//   Where the target sets `heads_after_start`, the data must start with a head at some step of the run after the
//   start, and the heads of every one of the stream's channels must appear;
// - when the target gives a rate, and the channel's timer gives another (ReadCommands);
// - when the channels' timers differ;
// - when the target sets `loop_in_setup`, and the setup of a channel gives other samples or another loop than the
//   target's (SetupLoopError);
// - when a command after the start changes what the stream plays (ReadCommandsAfterStart);
// - when nothing refills the buffers from the stream's file by the run's end (StreamFileReads);
// - when a card read after `hook_clock`, the clock of the hook (HookState) on the boot with `backup`, reaches none of
//   the image's files and none of the archive's, or a file of the target's `left_out`. The sound archive is the
//   image's file `archive_file`.
StreamRun RunStreamMini(std::span<const uint8_t> image, uint64_t hook_clock, uint32_t archive_file,
                        const StreamTarget& target, double seconds, const ntdlib::ntr::BackupMemory& backup = {});

// The check of the card's reads in a stream mini's run: it installs the machine's `on_card_read`, and calls `fail` with
// the reason when a read reaches what the set doesn't keep.
using CardReadCheck = std::function<void(ntdlib::ntr::Machine& machine, const std::function<void(std::string)>& fail)>;

// RunStreamMini for a set without a sound archive of the SDK, such as a set of a game on DSE: `check` installs the
// check of the card's reads in place of the archive's.
StreamRun RunStreamMini(std::span<const uint8_t> image, uint64_t hook_clock, const StreamTarget& target, double seconds,
                        const ntdlib::ntr::BackupMemory& backup, const CardReadCheck& check);

// The check of the card's reads of a set without a sound archive: from `hook_clock` on, each read must reach one of
// `kept`, the ranges that the set keeps (KeptRanges).
CardReadCheck KeptReadsCheck(std::vector<std::pair<uint32_t, uint32_t>> kept, uint64_t hook_clock);

// RunStreamMini with the target of `stream` of the set's sound archive (ArchiveStreamTarget).
StreamRun RunStreamMini(std::span<const uint8_t> image, uint64_t hook_clock, uint32_t archive_file, uint32_t stream,
                        const std::set<uint32_t>& reserved, double seconds,
                        const ntdlib::ntr::BackupMemory& backup = {});

// The timing of a stream of `samples` samples that plays at `rate` Hz from its start: a stream with a loop from
// `loop_start` plays its intro and xsflib::rip::kLengthLoops passes of its loop, then fades. Any other stream gets the
// end of its samples.
xsflib::rip::Timing StreamTiming(uint32_t samples, uint32_t loop_start, bool loop, double rate);

// The checks of a stream mini's runs, `runs`, one on each boot in the order of BootBackups, for BootsError. A run with
// a backup memory also fails when the stream's channels play at another rate than on the boot with none. The length
// comes from the boot with none.
std::vector<BootRun> StreamChecks(std::span<const StreamRun> runs);

} // namespace twosf::rip
