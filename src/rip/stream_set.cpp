// SPDX-License-Identifier: MIT

#include "rip/stream_set.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/arm/routine.h"
#include "ntdlib/ntr/arm7.h"
#include "ntdlib/ntr/length_model.h"
#include "ntdlib/ntr/machine.h"
#include "ntdlib/ntr/main_memory.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/sdat/decode.h"
#include "xsflib/bytes.h"
#include "xsflib/rip/length.h"
#include "xsflib/text.h"

#include "rip/boots.h"
#include "rip/sdat_set.h"

namespace twosf::rip
{

namespace
{

namespace arm = ntdlib::arm;
namespace ntr = ntdlib::ntr;
namespace sdat = ntdlib::sdat;
using arm::IsDirectCall;
using arm::Value;
using ntr::snd::CommandId;

// The instructions that the search reads of each routine.
constexpr std::size_t kReadLimit = 64;

// A stream's record: the player's priority (a byte at 5) and the stream player (a byte at 6).
constexpr int32_t kPriorityField = 5;
constexpr int32_t kPlayerField = 6;

// The step from the hook until the stream's channels sound: an output sample of the sound unit.
constexpr uint64_t kSampleStep = 1024;

// The clock of a channel's timer: half the system clock. The command that sets up a channel gives the ticks of a
// sample, whose negative the driver writes to the channel's timer.
constexpr double kTimerClockHz = ntr::kSystemClockHz / 2.0;

// Whether a channel's timer of `timer` ticks a sample plays at `rate` Hz, within 1 %.
bool AtRate(uint32_t timer, double rate)
{
    return timer > 0 && std::abs(kTimerClockHz / timer - rate) <= rate / 100.0;
}

// The stream's prepare: two calls, the first with the routine's second argument and the second with its first, and
// between them the loads of the priority and the player from the record that the first returns. No other load.
bool MatchStreamPrepare(const arm::Routine& routine)
{
    const std::vector<const arm::Step*> calls = routine.Calls();
    if (!routine.returns || calls.size() != 2 || !IsDirectCall(*calls[0]) || !IsDirectCall(*calls[1]) ||
        calls[0]->arguments[0] != Value::Argument(1) || calls[1]->arguments[0] != Value::Argument(0))
    {
        return false;
    }

    std::set<std::pair<int32_t, uint32_t>> fields;
    for (const arm::Step& step : routine.steps)
    {
        if (step.kind != arm::StepKind::kLoad)
        {
            continue;
        }

        if (step.address < calls[0]->address || step.address > calls[1]->address || step.base != Value::Result(0) ||
            !step.offset)
        {
            return false;
        }
        fields.insert({*step.offset, step.size});
    }

    return fields == std::set<std::pair<int32_t, uint32_t>>{{kPriorityField, 1}, {kPlayerField, 1}};
}

// The start: two calls, the first of a prepare with the routine's three arguments and the second with its first.
bool MatchStreamStart(const arm::Routine& routine, const std::set<uint32_t>& prepares)
{
    const std::vector<const arm::Step*> calls = routine.Calls();

    return routine.returns && calls.size() == 2 && IsDirectCall(*calls[0]) && IsDirectCall(*calls[1]) &&
           prepares.contains(*calls[0]->target) && calls[0]->arguments[0] == Value::Argument(0) &&
           calls[0]->arguments[1] == Value::Argument(1) && calls[0]->arguments[2] == Value::Argument(2) &&
           calls[1]->arguments[0] == Value::Argument(0);
}

// Each channel's first block of `stream`, as the file stores it and decoded to 16-bit PCM: two for each channel, in the
// channels' order. The SDK's stream player decodes blocks of IMA-ADPCM on the ARM9.
std::vector<std::vector<uint8_t>> StreamHeads(const sdat::Stream& stream)
{
    std::vector<std::vector<uint8_t>> heads;
    for (uint32_t channel = 0; channel < stream.channels; channel++)
    {
        const std::span<const uint8_t> block = stream.Block(channel, 0);
        heads.emplace_back(block.begin(), block.end());

        const std::vector<int16_t> samples = sdat::DecodeStream(stream, channel);
        std::vector<uint8_t> decoded;
        for (std::size_t i = 0; i < samples.size() && i < stream.block_samples; i++)
        {
            xsflib::AppendLe16(decoded, static_cast<uint16_t>(samples[i]));
        }
        heads.push_back(std::move(decoded));
    }

    return heads;
}

// The stream's channels whose first block is at the data of `setup` in `memory`, the DS's main memory, by the heads of
// StreamHeads. The channel plays the stream player's buffer. The buffer can be shorter than a block: a stream of PCM
// may store each channel as one block. It then contains the block's start. A silent start matches several.
std::set<uint32_t> ChannelsAt(std::span<const uint8_t> memory, const ChannelSetup& setup,
                              const std::vector<std::vector<uint8_t>>& heads)
{
    std::set<uint32_t> channels;
    for (std::size_t i = 0; i < heads.size(); i++)
    {
        const std::size_t bytes = std::min<std::size_t>(heads[i].size(), setup.bytes);
        if (bytes > 0 && ntr::MainMemoryContains(memory, setup.data, std::span(heads[i]).first(bytes)))
        {
            channels.insert(static_cast<uint32_t>(i / 2));
        }
    }

    return channels;
}

// How `command`, after the command that starts the stream's channels of `start`, changes what the stream plays, by the
// rule of ReadCommandsAfterStart. Records the first level after the start command in `start`.
std::optional<std::string> ChangeAfterStart(const ntr::snd::Command& command, ChannelStart& start, bool ends_in_run)
{
    // The setups name one channel (ntr::snd::SetUpChannel). The other channel commands name a mask of them. The
    // hardware reads 7 bits of a volume.
    constexpr uint32_t kVolumeBits = 0x7F;
    uint32_t stream_mask = 0;
    for (const uint32_t channel : start.hardware)
    {
        stream_mask |= 1u << channel;
    }
    const bool masked = (command.args[0] & stream_mask) != 0;

    switch (command.id)
    {
    case CommandId::kSetUpPcmChannel:
    case CommandId::kSetUpPsgChannel:
    case CommandId::kSetUpNoiseChannel:
        if (start.hardware.contains(static_cast<uint32_t>(ntr::snd::SetUpChannel(command))))
        {
            return "sets up one of the stream's channels";
        }
        return std::nullopt;

    case CommandId::kStartTimer:
        if (masked)
        {
            return "starts one of the stream's channels";
        }
        return std::nullopt;

    case CommandId::kStopTimer:
        if (masked && !ends_in_run)
        {
            return "stops one of the stream's channels";
        }
        return std::nullopt;

    case CommandId::kChannelTimer:
        if (masked && command.args[1] != start.timer)
        {
            return "sets another timer for one of the stream's channels";
        }
        return std::nullopt;

    case CommandId::kChannelVolume:
        {
            if (!masked)
            {
                return std::nullopt;
            }

            const ChannelLevel level = {.volume = command.args[1], .divider = command.args[2]};
            if ((level.volume & kVolumeBits) == 0)
            {
                return "silences one of the stream's channels";
            }
            if (start.level && *start.level != level)
            {
                return "sets a second level for the stream's channels";
            }

            start.level = level;
            return std::nullopt;
        }

    default:
        return Interference(command.id);
    }
}

// Calls `fail` for each channel of `start` whose setup gives other samples or another loop than `target`
// (SetupLoopError).
void CheckSetupLoops(const ChannelStart& start, const StreamTarget& target,
                     const std::function<void(std::string)>& fail)
{
    for (const uint32_t channel : start.hardware)
    {
        if (const std::optional<std::string> error =
                SetupLoopError(start.setups.at(channel), target.samples, target.loop_start, target.loop))
        {
            fail(*error);
        }
    }
}

} // namespace

std::size_t ReadCommands(const ntr::snd::CommandList& list, std::span<const uint8_t> memory,
                         const std::vector<std::vector<uint8_t>>& heads, uint32_t channels, ChannelStart& start,
                         const std::function<void(std::string)>& fail, std::optional<double> rate,
                         bool heads_after_start)
{
    for (std::size_t i = 0; i < list.size(); i++)
    {
        const ntr::snd::Command& command = list[i];
        if (command.id == CommandId::kSetUpPcmChannel)
        {
            // The loop's start and its length count words of 4 bytes. A setup at another rate than the target's plays
            // something else.
            const ntr::snd::PcmChannelSetup setup = ntr::snd::ReadPcmChannelSetup(command);
            const auto channel = static_cast<uint32_t>(setup.channel);
            if (rate && !AtRate(static_cast<uint32_t>(setup.timer), *rate))
            {
                start.setups.erase(channel);
                continue;
            }

            start.setups[channel] = {
                .data = setup.data,
                .bytes = 4 * (static_cast<uint32_t>(setup.loop_start) + static_cast<uint32_t>(setup.loop_length)),
                .timer = static_cast<uint32_t>(setup.timer),
                .format = static_cast<uint32_t>(setup.format),
                .loops = setup.loop == 1,
                .loop_start = static_cast<uint32_t>(setup.loop_start)};
            continue;
        }

        if (command.id != CommandId::kStartTimer)
        {
            continue;
        }

        // The channels that the timer starts and the stream's channels that they play.
        std::set<uint32_t> played;
        std::set<uint32_t> timers;
        std::size_t playing = 0;
        std::map<uint32_t, uint32_t> buffers; // with `heads_after_start`: each buffer, and the stream's channel
        for (const auto& [channel, setup] : start.setups)
        {
            if (heads_after_start && (command.args[0] & (1u << channel)) != 0)
            {
                buffers.try_emplace(setup.data, static_cast<uint32_t>(buffers.size()));
            }
            const std::set<uint32_t> plays =
                heads_after_start ? std::set<uint32_t>{buffers[setup.data]} : ChannelsAt(memory, setup, heads);
            if ((command.args[0] & (1u << channel)) != 0 && !plays.empty())
            {
                played.insert(plays.begin(), plays.end());
                timers.insert(setup.timer);
                start.hardware.insert(channel);
                playing++;
            }
        }
        if (played.size() != channels || playing < channels)
        {
            start.hardware.clear();
            continue;
        }

        start.started = true;
        if (timers.size() != 1 || *timers.begin() == 0)
        {
            fail("the stream's channels play at different rates");
            return i + 1;
        }
        start.timer = *timers.begin();
        start.rate = kTimerClockHz / start.timer;
        return i + 1;
    }

    return list.size();
}

void ReadCommandsAfterStart(std::span<const ntr::snd::Command> list, ChannelStart& start, bool ends_in_run,
                            const std::function<void(std::string)>& fail)
{
    for (const ntr::snd::Command& command : list)
    {
        if (const std::optional<std::string> change = ChangeAfterStart(command, start, ends_in_run))
        {
            fail("a driver command after the start " + *change);
            return;
        }
    }
}

std::optional<std::string> SetupLoopError(const ChannelSetup& setup, uint32_t samples, uint32_t loop_start, bool loop)
{
    // The samples in a word of PCM8, PCM16 and IMA-ADPCM.
    constexpr std::array<uint64_t, 3> kSamplesPerWord = {4, 2, 8};
    if (setup.format >= kSamplesPerWord.size())
    {
        return "the stream's channel plays neither PCM nor IMA-ADPCM";
    }
    if (setup.loops != loop)
    {
        return loop ? "the stream's record has a loop, and its channel doesn't repeat one"
                    : "the stream's channel repeats a loop, and its record has none";
    }

    const uint64_t per_word = kSamplesPerWord[setup.format];
    const uint64_t setup_end = per_word * (setup.bytes / 4);
    const uint64_t setup_loop = per_word * setup.loop_start;
    if (setup_end != samples || (loop && setup_loop != loop_start))
    {
        return "the stream's channel plays " + std::to_string(setup_end) + " samples with a loop from " +
               std::to_string(setup_loop) + ", and its record gives " + std::to_string(samples) + " with a loop from " +
               std::to_string(loop_start);
    }

    return std::nullopt;
}

StreamFileReads::StreamFileReads(uint32_t start, uint32_t end) : start_(start), end_(end)
{
}

void StreamFileReads::Add(uint32_t address, uint32_t bytes, bool after_start)
{
    uint32_t first = std::max(address, start_);
    uint32_t last = static_cast<uint32_t>(std::min<uint64_t>(uint64_t{address} + bytes, end_));
    if (first >= last)
    {
        return;
    }

    // After the start, a read counts when the reads before the start don't cover it.
    auto next = before_.upper_bound(first);
    if (after_start)
    {
        const bool covered = next != before_.begin() && std::prev(next)->second >= last;
        refilled_ = refilled_ || !covered;
        return;
    }

    // Before the start, the read joins the runs that it touches.
    if (next != before_.begin() && std::prev(next)->second >= first)
    {
        next = std::prev(next);
        first = next->first;
        last = std::max(last, next->second);
        next = before_.erase(next);
    }
    while (next != before_.end() && next->first <= last)
    {
        last = std::max(last, next->second);
        next = before_.erase(next);
    }
    before_[first] = last;
}

bool StreamFileReads::Refilled() const
{
    const bool whole =
        start_ < end_ && before_.size() == 1 && before_.begin()->first == start_ && before_.begin()->second == end_;

    return refilled_ || whole;
}

std::vector<uint32_t> FindStreamStarts(const ntr::LoadedCode& arm9)
{
    std::vector<arm::Routine> routines;
    std::set<uint32_t> prepares;
    std::vector<uint32_t> addresses = arm::RoutineStarts(arm9.bytes, arm9.address);
    const std::vector<uint32_t> thumb = arm::ThumbRoutineStarts(arm9.bytes, arm9.address);
    addresses.insert(addresses.end(), thumb.begin(), thumb.end());
    for (const uint32_t address : addresses)
    {
        arm::Routine routine = arm::ReadRoutine(arm9.bytes, arm9.address, address, kReadLimit);
        if (routine.steps.empty())
        {
            continue;
        }

        if (MatchStreamPrepare(routine))
        {
            prepares.insert(address);
        }
        routines.push_back(std::move(routine));
    }

    std::vector<uint32_t> starts;
    for (const arm::Routine& routine : routines)
    {
        if (MatchStreamStart(routine, prepares))
        {
            starts.push_back(routine.address);
        }
    }

    return starts;
}

std::string StreamLabel(const sdat::Archive& archive, uint32_t stream)
{
    const bool named =
        stream < archive.streams.size() && archive.streams[stream] && !archive.streams[stream]->name.empty();

    return named ? archive.streams[stream]->name : "STRM_" + std::to_string(stream);
}

std::optional<uint32_t> StreamFile(const sdat::Archive& archive, uint32_t stream)
{
    if (stream >= archive.streams.size() || !archive.streams[stream])
    {
        return std::nullopt;
    }

    const uint32_t id = archive.streams[stream]->file_id & sdat::kFileIdBits;
    if (id >= archive.files.size() || archive.files[id].size == 0)
    {
        return std::nullopt;
    }

    return id;
}

bool StreamAudible(const sdat::Archive& archive, uint32_t stream)
{
    const std::optional<uint32_t> file = StreamFile(archive, stream);
    if (!file || archive.streams[stream]->volume == 0)
    {
        return false;
    }

    try
    {
        const sdat::Stream parsed = sdat::Stream::Parse(archive.File(*file));
        for (uint32_t channel = 0; channel < parsed.channels; channel++)
        {
            if (xsflib::rip::Audible(sdat::DecodeStream(parsed, channel)))
            {
                return true;
            }
        }
    }
    catch (const std::exception&)
    {
        return false;
    }

    return false;
}

std::optional<uint32_t> ArchiveFileOffset(std::span<const uint8_t> image, uint32_t archive_file, uint32_t file_id)
{
    try
    {
        const ntr::Rom rom = ntr::Rom::Parse(image);
        const sdat::Archive archive = sdat::Archive::Parse(rom.FileData(archive_file));
        if (file_id >= archive.files.size())
        {
            return std::nullopt;
        }

        return rom.Files().at(archive_file).start + archive.files[file_id].offset;
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

StreamTarget ArchiveStreamTarget(const ntr::Rom& rom, uint32_t archive_file, uint32_t stream,
                                 const std::set<uint32_t>& reserved)
{
    const sdat::Archive archive = sdat::Archive::Parse(rom.FileData(archive_file));
    const uint32_t file = StreamFile(archive, stream).value();
    const sdat::Stream parsed = sdat::Stream::Parse(archive.File(file));
    StreamTarget target;
    target.left_out = reserved;
    target.left_out.erase(file);
    target.file_start = rom.Files().at(archive_file).start + archive.files[file].offset;
    target.file_end = target.file_start + archive.files[file].size;
    target.heads = StreamHeads(parsed);
    target.channels = parsed.channels;
    target.samples = parsed.samples;
    target.loop_start = parsed.loop_start;
    target.loop = parsed.loop != 0;

    return target;
}

StreamRun RunStreamMini(std::span<const uint8_t> image, uint64_t hook_clock, uint32_t archive_file,
                        const StreamTarget& target, double seconds, const ntr::BackupMemory& backup)
{
    const auto check = [&](ntr::Machine& machine, const std::function<void(std::string)>& fail)
    {
        const ntr::Rom rom = ntr::Rom::Parse(image);
        const sdat::Archive archive = sdat::Archive::Parse(rom.FileData(archive_file));
        GuardCardReads(machine, rom, archive_file, archive, target.left_out, hook_clock, fail);
    };

    return RunStreamMini(image, hook_clock, target, seconds, backup, check);
}

StreamRun RunStreamMini(std::span<const uint8_t> image, uint64_t hook_clock, const StreamTarget& target, double seconds,
                        const ntr::BackupMemory& backup, const CardReadCheck& check)
{
    StreamRun run;
    const ntr::Rom rom = ntr::Rom::Parse(image);
    const auto machine = std::make_unique<ntr::Machine>(rom, backup);
    const std::span<uint8_t> memory = machine->MainMemory();
    const std::function<void(std::string)> fail = [&run](std::string error)
    {
        if (!run.error)
        {
            run.error = std::move(error);
        }
    };

    // After the hook, each card read must reach a range that the set keeps. The reads of the stream's file show the
    // refills of the buffers.
    check(*machine, fail);
    StreamFileReads reads(target.file_start, target.file_end);
    ChannelStart start;
    machine->on_card_read = [guard = std::move(machine->on_card_read), &reads, &start](uint32_t address, uint32_t bytes)
    {
        guard(address, bytes);
        reads.Add(address, bytes, start.started);
    };

    // The lists of the driver's commands give the start of the stream's channels. The game must leave the stream alone
    // from the command after the start on, in the start list too. The stream player stops the channels of a stream
    // without a loop at its end.
    machine->on_fifo_word = [&](bool to_arm7, uint32_t word)
    {
        const bool counts = to_arm7 && (!target.commands_after_hook || machine->Clock() >= hook_clock);
        const std::optional<ntr::snd::CommandList> list = counts ? SoundCommands(memory, word) : std::nullopt;
        if (!list)
        {
            return;
        }

        std::span<const ntr::snd::Command> after_start = *list;
        if (!start.started)
        {
            after_start = after_start.subspan(ReadCommands(*list, memory, target.heads, target.channels, start, fail,
                                                           target.rate, target.heads_after_start));
            if (start.started && target.loop_in_setup)
            {
                CheckSetupLoops(start, target, fail);
            }
        }
        if (start.started)
        {
            const bool ends_in_run = !target.loop && target.samples <= start.rate * kMiniPlaySeconds;
            ReadCommandsAfterStart(after_start, start, ends_in_run, fail);
        }
    };

    // From the hook until the channels sound, the run goes an output sample at a time. The start is the first sample in
    // which the sound unit plays one of them. The output counts from the first step that begins after the start.
    std::vector<ntr::SoundOutput> out;
    OutputWatch watch;
    uint64_t end = static_cast<uint64_t>(seconds * ntr::kSystemClockHz);
    bool sounding = false;
    const auto sounds = [&machine](uint32_t channel)
    {
        return machine->Arm7().Sound().Busy(static_cast<int>(channel));
    };
    // Where the stream player fills the buffers after the start: the hardware channels whose buffers have started with
    // a head, and the stream's channels of those heads.
    std::set<uint32_t> filled;
    std::set<uint32_t> filled_channels;
    while (!run.error && machine->Clock() < end)
    {
        const uint64_t step = machine->Clock() >= hook_clock && !sounding ? kSampleStep : kRunStep;
        const bool playing = sounding;
        machine->Run(std::min(end, machine->Clock() + step), out);
        for (const uint32_t channel : target.heads_after_start ? start.hardware : std::set<uint32_t>{})
        {
            const std::set<uint32_t> plays = ChannelsAt(memory, start.setups.at(channel), target.heads);
            if (!plays.empty())
            {
                filled.insert(channel);
                filled_channels.insert(plays.begin(), plays.end());
            }
        }
        if (playing)
        {
            watch.Add(out);
        }
        out.clear();
        if (start.started && !sounding && std::ranges::any_of(start.hardware, sounds))
        {
            sounding = true;
            run.start_ms = static_cast<double>(machine->Clock()) * 1000.0 / ntr::kSystemClockHz;
            end = machine->Clock() + static_cast<uint64_t>(kMiniPlaySeconds * ntr::kSystemClockHz);
        }
    }

    run.rate = start.rate;
    run.sounded = watch.Sounded();
    if (!sounding)
    {
        fail("the stream's channels don't sound within " + std::to_string(static_cast<int>(seconds)) +
             " s of the boot");
    }
    else if (target.heads_after_start && (filled != start.hardware || filled_channels.size() != target.channels))
    {
        fail("the stream's channels don't play the stream: their buffers never start with the stream's samples");
    }
    else if (!reads.Refilled())
    {
        fail("the stream player reads nothing more of the stream's file after the start");
    }

    return run;
}

CardReadCheck KeptReadsCheck(std::vector<std::pair<uint32_t, uint32_t>> kept, uint64_t hook_clock)
{
    return [kept = std::move(kept), hook_clock](ntr::Machine& machine, const std::function<void(std::string)>& fail)
    {
        machine.on_card_read = [&machine, kept, hook_clock, fail](uint32_t address, uint32_t bytes)
        {
            if (machine.Clock() >= hook_clock && !Reaches(kept, address, bytes))
            {
                fail(ReadOutsideSetError(address));
            }
        };
    };
}

StreamRun RunStreamMini(std::span<const uint8_t> image, uint64_t hook_clock, uint32_t archive_file, uint32_t stream,
                        const std::set<uint32_t>& reserved, double seconds, const ntr::BackupMemory& backup)
{
    const ntr::Rom rom = ntr::Rom::Parse(image);

    return RunStreamMini(image, hook_clock, archive_file, ArchiveStreamTarget(rom, archive_file, stream, reserved),
                         seconds, backup);
}

xsflib::rip::Timing StreamTiming(uint32_t samples, uint32_t loop_start, bool loop, double rate)
{
    const auto ms = [rate](double count)
    {
        return static_cast<long long>(1000.0 * count / rate + 0.5);
    };

    if (!loop || loop_start >= samples)
    {
        return {.length_ms = ms(samples)};
    }

    const xsflib::rip::LoopPass pass{.start_ms = ms(loop_start), .length_ms = ms(samples - loop_start)};

    return {.length_ms = pass.start_ms + xsflib::rip::kLengthLoops * pass.length_ms,
            .fade_ms = static_cast<long long>(ntr::LengthRules().fade_ms),
            .loop = pass};
}

std::vector<BootRun> StreamChecks(std::span<const StreamRun> runs)
{
    std::vector<BootRun> checks;
    for (const StreamRun& run : runs)
    {
        BootRun& check = checks.emplace_back(BootRun{run.error, run.sounded});
        if (!check.error && run.rate != runs.front().rate)
        {
            check.error = "the stream's channels play at another rate than with no backup memory";
        }
    }

    return checks;
}

} // namespace twosf::rip
