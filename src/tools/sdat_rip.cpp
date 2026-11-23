// SPDX-License-Identifier: MIT

#include "tools/sdat_rip.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iterator>
#include <map>
#include <new>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/ntr/compact_image.h"
#include "ntdlib/ntr/game_profiles.h"
#include "ntdlib/ntr/length_model.h"
#include "ntdlib/ntr/machine.h"
#include "ntdlib/ntr/rip.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/sdat/formats.h"
#include "xsflib/bytes.h"
#include "xsflib/cli/length_analysis.h"

#include "rip/boots.h"
#include "rip/game_streams.h"
#include "rip/mini_render.h"
#include "rip/profile.h"
#include "rip/program_set.h"
#include "rip/sdat_set.h"
#include "rip/stream_set.h"
#include "tools/messages.h"
#include "tools/set_files.h"

namespace twosf::tools
{

namespace
{

namespace ntr = ntdlib::ntr;
namespace sdat = ntdlib::sdat;

constexpr char kStreamExtension[] = ".strm";

// The time after the hook within which a mini's sound must start.
constexpr double kStartLimitSeconds = 10.0;

// A sequence or a stream of the set.
struct Job
{
    uint32_t index = 0; // the analysis's order
    rip::SoundKind kind = rip::SoundKind::kSequence;
    ntr::SequenceRef sequence;                    // a sequence's
    uint32_t stream = 0;                          // a stream's number
    const rip::GameStream* game_stream = nullptr; // a game stream's record (rip/game_streams.h)
    std::string label;                            // with the state's suffix
    const rip::GameState* state = nullptr; // a sequence's state, for a game in the table of games (rip::StatesOf)
    xsflib::rip::Limit limit = xsflib::rip::Limit::kDefault; // the analysis's limit (rip::AnalysisLimit)
    std::vector<std::string> same; // the later sounds of the kind with the same record, by label: this mini plays them
    std::optional<xsflib::rip::Timing> timing;
    double start_ms = 0;              // the time from the boot to the sound's start on the DS
    std::optional<std::string> error; // why the mini's run on the DS fails

    // Why the game's code doesn't start the sequence. Such a sequence plays no notes and is left out
    // (rip::LeftOutUnstarted).
    std::optional<std::string> left_out;

    // The mini's run has the capture units running, and the game's profile has no reverb and no output effect
    // (rip::EchoLeftOut).
    bool echo_left_out = false;

    // How the start of a game in the table of games differs from the game's profile (rip::ProfileDifferences).
    std::vector<std::string> profile_differences;

    // A sequence's run on the boot with no backup memory, once its runs count (RunSequenceMini).
    std::optional<rip::MiniRun> run;
};

// `candidates` are jobs of `kind` in the order of the set. Returns the job of the first sound of each record
// (rip::FirstOfRecord, with the game's routines `code`). The later sounds of the record go to its `same`.
std::vector<Job> FirstOfEachRecord(const rip::SdatCode& code, const sdat::Archive& archive, std::vector<Job> candidates,
                                   rip::SoundKind kind)
{
    std::vector<uint32_t> numbers;
    for (const Job& job : candidates)
    {
        numbers.push_back(kind == rip::SoundKind::kStream ? job.stream : job.sequence.index);
    }

    const std::vector<uint32_t> firsts = rip::FirstOfRecord(code, archive, numbers, kind);
    std::vector<Job> jobs;
    std::map<uint32_t, std::size_t> job_of; // the first sound of a record, and its job
    for (std::size_t i = 0; i < candidates.size(); i++)
    {
        if (firsts[i] != numbers[i])
        {
            jobs[job_of.at(firsts[i])].same.push_back(candidates[i].label);
            continue;
        }

        job_of[numbers[i]] = jobs.size();
        jobs.push_back(std::move(candidates[i]));
    }

    return jobs;
}

// `jobs` with the states of their sequences (rip::StatesOf): a sequence's state with no suffix goes to its job, and
// each state with a suffix gets a job after the sequence's, named after the sequence's label and the suffix.
std::vector<Job> WithStates(const rip::SdatCode& code, std::vector<Job> jobs)
{
    std::vector<Job> with_states;
    for (Job& job : jobs)
    {
        const std::vector<const rip::GameState*> states = rip::StatesOf(code, job.label);
        const std::string label = job.label;
        if (!states.empty() && states.front()->suffix.empty())
        {
            job.state = states.front();
        }
        with_states.push_back(job);
        for (const rip::GameState* state : states)
        {
            if (!state->suffix.empty())
            {
                Job& variant = with_states.emplace_back(job);
                variant.label = label + state->suffix;
                variant.state = state;
                variant.same.clear();
            }
        }
    }

    return with_states;
}

// Whether a set of a game starts the sequences of its sequence archives: a game in the table of games whose starts
// contain the effects' calls (rip::GameStart::effect_calls).
bool StartsArchived(const rip::SdatCode& code)
{
    return code.game_start && !code.game_start->effect_calls.empty();
}

// The label of `sequence`, a sequence of a sequence archive that a set starts: "Effect", the archive's number and the
// sequence's index in it, such as "Effect 0-586".
std::string EffectLabel(const ntr::SequenceRef& sequence)
{
    return "Effect " + std::to_string(sequence.index) + "-" + std::to_string(*sequence.archived);
}

// The set's sequences: those outside the sequence archives, by number, music only unless `effects` (`music`, from
// rip::MusicSequences). --only picks by label. A sequence that has no data is left out with a warning. A job goes to
// the first sequence of each record (FirstOfEachRecord). With `effects`, a game whose set starts the sequences of its
// sequence archives (StartsArchived) adds each of them after the rest, one job for each, labelled by EffectLabel.
// Otherwise `archived` counts the sequences in sequence archives that --only picks, and the set leaves them out. It
// also leaves out `code.left_out`, with a warning when the filters pick it.
std::vector<Job> SequenceJobs(const rip::SdatCode& code, const sdat::Archive& archive, const std::set<uint32_t>& music,
                              const xsflib::cli::RipOptions& o, bool effects, std::size_t& archived)
{
    std::vector<Job> picked;
    std::vector<Job> archived_jobs;
    archived = 0;
    const bool archived_effects = effects && !o.bgm && StartsArchived(code);
    for (const ntr::rip::Job& selected : ntr::rip::SelectSequences(archive, std::regex(".*"), false))
    {
        if (selected.sequence.archived && archived_effects)
        {
            const std::string label = EffectLabel(selected.sequence);
            if (!std::regex_search(label, o.only))
            {
                continue;
            }

            const std::vector<std::string> missing = ntr::rip::MissingData(archive, selected.sequence);
            if (!missing.empty())
            {
                std::fprintf(stderr, "warning: %s lacks %s and is left out\n", label.c_str(), missing[0].c_str());
                continue;
            }

            archived_jobs.push_back({.sequence = selected.sequence, .label = label});
            continue;
        }

        if (!std::regex_search(selected.label, o.only))
        {
            continue;
        }

        if (selected.sequence.archived)
        {
            archived++;
            continue;
        }

        if (!music.contains(selected.sequence.index) && (!effects || o.bgm))
        {
            continue;
        }

        if (code.left_out == selected.sequence.index)
        {
            std::fprintf(stderr,
                         "warning: %s is left out: this copy's sound start-up isn't the one that 2sfrip supports\n",
                         selected.label.c_str());
            continue;
        }

        const std::vector<std::string> missing = ntr::rip::MissingData(archive, selected.sequence);
        if (!missing.empty())
        {
            std::fprintf(stderr, "warning: %s lacks %s and is left out\n", selected.label.c_str(), missing[0].c_str());
            continue;
        }

        picked.push_back({.sequence = selected.sequence,
                          .label = selected.label,
                          .limit = rip::AnalysisLimit(music, selected.sequence.index)});
    }

    std::vector<Job> jobs =
        WithStates(code, FirstOfEachRecord(code, archive, std::move(picked), rip::SoundKind::kSequence));
    std::ranges::move(archived_jobs, std::back_inserter(jobs));

    return jobs;
}

// The set's streams: every stream with a file, by number, that --only picks by label. Every stream counts as music. A
// stream that makes no sound (rip::StreamAudible) is left out, and `silent` counts it. A job goes to the first stream
// of each record (FirstOfEachRecord).
std::vector<Job> StreamJobs(const rip::SdatCode& code, const sdat::Archive& archive, const xsflib::cli::RipOptions& o,
                            std::size_t& silent)
{
    std::vector<Job> picked;
    silent = 0;
    for (uint32_t stream = 0; stream < archive.streams.size(); stream++)
    {
        const std::string label = rip::StreamLabel(archive, stream);
        if (!rip::StreamFile(archive, stream) || !std::regex_search(label, o.only))
        {
            continue;
        }

        if (!rip::StreamAudible(archive, stream))
        {
            silent++;
            continue;
        }

        picked.push_back({.kind = rip::SoundKind::kStream, .stream = stream, .label = label});
    }

    return FirstOfEachRecord(code, archive, std::move(picked), rip::SoundKind::kStream);
}

// The set's game streams (rip/game_streams.h) that --only picks by label. Every one counts as music.
std::vector<Job> GameStreamJobs(const std::optional<rip::GameStreams>& game_streams, const xsflib::cli::RipOptions& o)
{
    std::vector<Job> jobs;
    if (!game_streams)
    {
        return jobs;
    }

    for (const rip::GameStream& stream : game_streams->streams)
    {
        if (std::regex_search(stream.label, o.only))
        {
            jobs.push_back({.kind = rip::SoundKind::kGameStream, .game_stream = &stream, .label = stream.label});
        }
    }

    return jobs;
}

// Prints each sequence's and each stream's label and kind, and after a sequence its states with a suffix
// (rip::StatesOf). A later sound of a job's record also names the first. The mini of the first plays both. `left_out`
// is the sequence that the game's code keeps out of the set (rip::SdatCode::left_out). `streams_ripped` says whether
// the rip writes the streams, as minis or as the game's files.
void ListSounds(const rip::SdatCode& code, const sdat::Archive& archive, const std::set<uint32_t>& music,
                const std::vector<Job>& jobs, std::optional<uint32_t> left_out, bool streams_ripped,
                const std::optional<rip::GameStreams>& game_streams)
{
    std::map<std::pair<rip::SoundKind, std::string>, std::string> first_of; // a later sound of a record, and the first
    for (const Job& job : jobs)
    {
        for (const std::string& label : job.same)
        {
            first_of[{job.kind, label}] = job.label;
        }
    }

    const auto same_as = [&first_of](rip::SoundKind kind, const std::string& label)
    {
        const auto first = first_of.find({kind, label});
        return first == first_of.end() ? std::string() : ", same record as " + first->second;
    };

    for (const ntr::rip::Job& job : ntr::rip::SelectSequences(archive, std::regex(".*"), false))
    {
        if (job.sequence.archived && StartsArchived(code))
        {
            std::printf("%s  effect in a sequence archive\n", EffectLabel(job.sequence).c_str());
            continue;
        }

        const char* kind = job.sequence.archived                ? "in a sequence archive"
                           : music.contains(job.sequence.index) ? "music"
                                                                : "effect";
        const bool ripped = !job.sequence.archived && left_out != job.sequence.index;
        const std::string same = same_as(rip::SoundKind::kSequence, job.label);
        std::printf("%s  %s%s%s\n", job.label.c_str(), kind, ripped ? "" : " (not ripped)", same.c_str());
        for (const rip::GameState* state : rip::StatesOf(code, job.label))
        {
            if (!state->suffix.empty() && ripped)
            {
                std::printf("%s%s  %s, a state of %s\n", job.label.c_str(), state->suffix.c_str(), kind,
                            job.label.c_str());
            }
        }
    }

    for (uint32_t stream = 0; stream < archive.streams.size(); stream++)
    {
        if (rip::StreamFile(archive, stream))
        {
            const std::string label = rip::StreamLabel(archive, stream);
            const char* kind = !rip::StreamAudible(archive, stream) ? "stream (silent, not ripped)"
                               : streams_ripped                     ? "stream"
                                                                    : "stream (not ripped)";
            const std::string same = same_as(rip::SoundKind::kStream, label);
            std::printf("%s  %s%s\n", label.c_str(), kind, same.c_str());
        }
    }

    if (game_streams)
    {
        for (const rip::GameStream& stream : game_streams->streams)
        {
            std::printf("%s  stream, in a file of the game's format\n", stream.label.c_str());
        }
    }

    std::fflush(stdout);
}

// Prints the label of each piece of `programs` (rip/program_set.h) and its program, and after a piece its states with a
// suffix (rip::StatesOf).
void ListPrograms(const std::vector<rip::Program>& programs)
{
    for (const rip::Program& program : programs)
    {
        for (const rip::ProgramPiece& piece : program.pieces)
        {
            std::printf("%s  music, in the download-play program %s\n", piece.label.c_str(), program.name.c_str());
            for (const rip::GameState* state : rip::StatesOf(program.code, piece.label))
            {
                if (!state->suffix.empty())
                {
                    std::printf("%s%s  music, a state of %s, in the download-play program %s\n", piece.label.c_str(),
                                state->suffix.c_str(), piece.label.c_str(), program.name.c_str());
                }
            }
        }
    }

    std::fflush(stdout);
}

// The archive's files that the library reserves for the streams of `jobs` (rip::CutArchive): each stream's file that
// isn't among the archive's files `read`. They keep their sizes and contain zeros, and each stream's library fills its
// file.
std::set<uint32_t> ReservedFiles(const rip::SoundArchive& found, const rip::BootFiles& read,
                                 const std::vector<Job>& jobs)
{
    std::set<uint32_t> reserved;
    for (const Job& job : jobs)
    {
        const std::optional<uint32_t> file =
            job.kind == rip::SoundKind::kStream ? rip::StreamFile(found.archive, job.stream) : std::nullopt;
        if (file && !read.archive_files.contains(*file))
        {
            reserved.insert(*file);
        }
    }

    return reserved;
}

// The boots of the full image (rip::UnlikeHookBoot), and what they read before the hook.
struct FullBoots
{
    std::vector<rip::HookState> hooks;
    rip::BootFiles read;
};

// Boots the full image of `rom` on each of `backups`: the set's image with `stub` and every file of the game kept
// whole.
FullBoots BootFullImage(const ntr::Rom& rom, const rip::SdatCode& code, const rip::SdatStub& stub,
                        const rip::SoundArchive& found, std::span<const ntr::BackupMemory> backups)
{
    const rip::SdatSet full = rip::MakeSdatSet(rom, code, stub, rip::WholeFiles(rom, rip::EveryFile(rom)));
    const ntr::Rom full_rom = ntr::Rom::Parse(full.image);
    FullBoots boots;
    for (const ntr::BackupMemory& backup : backups)
    {
        boots.hooks.push_back(
            rip::RunOnBoot(backup, [&] { return rip::BootToHook(full_rom, code.update, backup, code.heap_word); }));
    }

    boots.read = rip::FilesRead(full_rom, found, boots.hooks);

    return boots;
}

// The files of a library, and the archive's files that it reserves for the streams (rip::SetArchive).
struct LibraryParts
{
    std::map<uint32_t, std::vector<uint8_t>> files;
    std::set<uint32_t> reserved;
};

// The files of the library: the files `read`, whole, and the sound archive cut to the jobs' sequences, with the
// archive's files `read`. The files `reserved` keep their sizes and contain zeros. Where the cut fails, the library
// keeps the whole archive and reserves nothing. Where the hook runs in a separate thread, the game's boot may read a
// file before the hook that the library's boot reads only after it. The library keeps such a file: it trims only what
// is absolutely safe, and the file costs only size. A game that runs only on a DSi checks each sector that it reads
// against the image's hash tables. Its library keeps the whole archive.
LibraryParts LibraryFiles(const ntr::Rom& rom, const rip::SoundArchive& found, const rip::BootFiles& read,
                          const std::vector<Job>& jobs, const std::set<uint32_t>& reserved)
{
    LibraryParts parts{.files = rip::WholeFiles(rom, read.files)};
    if (ntr::RunsInDsiMode(rom.Header()))
    {
        parts.files.merge(rip::WholeFiles(rom, {found.file_id}));
        return parts;
    }

    std::vector<ntr::SequenceRef> sequences;
    for (const Job& job : jobs)
    {
        if (job.kind == rip::SoundKind::kSequence)
        {
            sequences.push_back(job.sequence);
        }
    }

    rip::SetArchive archive = rip::MakeSetArchive(found.archive, sequences, read.archive_files, reserved);
    if (archive.whole)
    {
        std::fprintf(stderr, "the library keeps the whole sound archive: its layout isn't the SDK's tools' layout\n");
    }
    parts.files[found.file_id] = std::move(archive.bytes);
    parts.reserved = std::move(archive.reserved);

    return parts;
}

// Drops the jobs whose sequence plays no notes, with the later sequences of their records, and prints the count. Then
// it names each sequence that the game's code doesn't start (Job::left_out), with its reason.
void DropSilent(std::vector<Job>& jobs)
{
    std::size_t silent = 0;
    std::vector<std::string> not_started;
    for (const Job& job : jobs)
    {
        if (job.left_out)
        {
            not_started.push_back(job.label + ": " + *job.left_out);
        }
        else if (job.timing && job.timing->silent)
        {
            silent += 1 + job.same.size();
        }
    }

    std::erase_if(jobs, [](const Job& job) { return job.timing && job.timing->silent; });
    if (silent > 0)
    {
        std::fprintf(stderr, "%zu %s no notes and %s left out\n", silent,
                     silent == 1 ? "sequence plays" : "sequences play", silent == 1 ? "is" : "are");
    }

    if (!not_started.empty())
    {
        std::fprintf(stderr, "%zu %s that the game's code doesn't start %s no notes and %s left out:\n",
                     not_started.size(), not_started.size() == 1 ? "sequence" : "sequences",
                     not_started.size() == 1 ? "plays" : "play", not_started.size() == 1 ? "is" : "are");
        for (const std::string& line : not_started)
        {
            std::fprintf(stderr, "  %s\n", line.c_str());
        }
    }
}

// Prints the count of the sounds of `kind` that share a record with an earlier one, then the later sounds that each
// mini of the kind also plays. `one` and `many` name the kind.
void ReportShared(const std::vector<Job>& jobs, rip::SoundKind kind, const char* one, const char* many)
{
    std::size_t shared = 0;
    for (const Job& job : jobs)
    {
        shared += job.kind == kind ? job.same.size() : 0;
    }

    if (shared == 0)
    {
        return;
    }

    std::fprintf(stderr, "%zu %s %s a record with an earlier one. The mini of the earlier one plays %s:\n", shared,
                 shared == 1 ? one : many, shared == 1 ? "shares" : "share", shared == 1 ? "it" : "them");
    for (const Job& job : jobs)
    {
        if (job.kind != kind || job.same.empty())
        {
            continue;
        }

        std::string same;
        for (const std::string& label : job.same)
        {
            same += (same.empty() ? "" : ", ") + label;
        }
        std::fprintf(stderr, "  %s: %s\n", job.label.c_str(), same.c_str());
    }
}

// What the runs of the minis on the DS read: the set and the game's routines, the sound archive, the boots (each with
// its clock of the hook and the limit of a run), each stream file's offset in the set's image, the archive's files that
// the image reserves for the streams (ReservedFiles), the game streams, and the image's files that the library keeps.
// For a download-play program's set, `piece` gives the piece whose archive is in the program's memory
// (rip/program_set.h), and `archive_file` goes unused.
struct SetRun
{
    const ntr::Rom& rom;
    const rip::SdatSet& set;
    const rip::SdatCode& code;
    uint32_t heap = 0;
    uint32_t archive_file = 0;
    const sdat::Archive& archive;
    std::vector<rip::SetBoot> boots;
    std::map<uint32_t, uint32_t> stream_offsets;
    std::set<uint32_t> reserved;
    const rip::ProgramPiece* piece = nullptr;
    const rip::GameStreams* game_streams = nullptr;
    std::set<uint32_t> kept_files; // the image's files that the library keeps
};

// The game code and the file by which ntdlib's profiles name the archive of `run`: the game's code and the archive's
// file, or for a program's piece the game that sends the program and its file of the archive.
std::pair<std::string, std::string> ProfileKey(const SetRun& run)
{
    if (run.piece)
    {
        return {run.piece->game_code, run.piece->profile_file};
    }

    return {run.rom.Header().game_code, run.rom.Files().at(run.archive_file).path};
}

// The player that the record of `sequence` names, or its entry in its sequence archive, or nothing without either.
std::optional<uint32_t> RecordPlayer(const sdat::Archive& archive, const ntr::SequenceRef& sequence)
{
    if (sequence.archived)
    {
        if (sequence.index >= archive.sequence_archives.size() || !archive.sequence_archives[sequence.index])
        {
            return std::nullopt;
        }

        const sdat::SequenceArchive file =
            sdat::SequenceArchive::Parse(archive.File(archive.sequence_archives[sequence.index]->file_id));
        if (*sequence.archived >= file.sequences.size())
        {
            return std::nullopt;
        }

        return file.sequences[*sequence.archived].player;
    }

    if (sequence.index >= archive.sequences.size() || !archive.sequences[sequence.index])
    {
        return std::nullopt;
    }

    return archive.sequences[sequence.index]->player;
}

// The channels of the archive's player `player`, or 0 without its record.
uint16_t ArchiveChannels(const sdat::Archive& archive, uint32_t player)
{
    if (player >= archive.players.size() || !archive.players[player])
    {
        return 0;
    }

    return archive.players[player]->channels;
}

// The block of `job`'s mini: the sequence's number at the stub's word for it, the sequence's start block for a game in
// the table of games, an effect's block (rip::EffectBlock) for a sequence of a sequence archive, or the stream's whole
// stub, a game stream's too.
Block MiniBlock(const SetRun& run, const Job& job)
{
    if (job.kind == rip::SoundKind::kSequence && job.sequence.archived)
    {
        return {run.set.sound_word, rip::EffectBlock(run.code, run.archive, job.sequence, run.heap)};
    }

    if (job.kind == rip::SoundKind::kSequence && run.code.game_start)
    {
        return {run.set.sound_word, rip::StartBlock(run.code, ntr::rip::SoundId(job.sequence),
                                                    RecordPlayer(run.archive, job.sequence), job.state)};
    }

    if (job.kind == rip::SoundKind::kSequence)
    {
        Block block{.offset = run.set.sound_word};
        xsflib::AppendLe32(block.data, ntr::rip::SoundId(job.sequence));
        return block;
    }

    if (job.kind == rip::SoundKind::kGameStream)
    {
        return {run.set.stub, rip::BuildGameStreamStub(run.code, *run.game_streams, *job.game_stream).bytes};
    }

    return {run.set.stub, rip::BuildSdatStub(run.code, run.heap, job.stream, rip::SoundKind::kStream).bytes};
}

// The bytes of the image's file `file` for a game stream's second library, at their offset. In an image that runs only
// on a DSi, they cover the whole sectors of the digest that the file reaches: the game checks each sector that it
// reads, and the set's library has zeros in the files that it leaves out.
Block GameStreamFileBlock(const SetRun& run, uint32_t file)
{
    const ntr::RomFile& entry = run.rom.Files().at(file);
    const ntr::DigestSectors sectors =
        ntr::RunsInDsiMode(run.rom.Header()) ? ntr::DigestSectorsOf(run.rom) : ntr::DigestSectors{};
    const auto [first, end] = sectors.Around(entry.start, entry.end);
    const std::span<const uint8_t> bytes = run.rom.Image().subspan(first, end - first);
    return {first, {bytes.begin(), bytes.end()}};
}

// The set's image with `job`'s mini in place, and for a stream its file where the library reserves it, or for a game
// stream its file where the library leaves it out. The image is then the one that a player builds from the set. A
// stream file that the library doesn't reserve has its bytes in the library already, or the stream's run fails.
std::vector<uint8_t> MiniImage(const SetRun& run, const Job& job)
{
    std::vector<uint8_t> image = run.set.image;
    const Block block = MiniBlock(run, job);
    std::ranges::copy(block.data, image.begin() + block.offset);
    if (job.kind == rip::SoundKind::kStream)
    {
        const uint32_t file = rip::StreamFile(run.archive, job.stream).value();
        if (run.reserved.contains(file))
        {
            std::ranges::copy(run.archive.File(file), image.begin() + run.stream_offsets.at(file));
        }
    }
    if (job.kind == rip::SoundKind::kGameStream && !run.kept_files.contains(job.game_stream->file_id))
    {
        const Block file = GameStreamFileBlock(run, job.game_stream->file_id);
        std::ranges::copy(file.data, image.begin() + file.offset);
    }

    return image;
}

// Returns what `f` returns. An exception that `f` throws, other than std::bad_alloc, becomes a std::runtime_error whose
// message starts with `label`.
template <typename F>
auto WithLabel(const std::string& label, const F& f)
{
    try
    {
        return f();
    }
    catch (const std::bad_alloc&)
    {
        throw;
    }
    catch (const std::exception& e)
    {
        throw std::runtime_error(label + ": " + e.what());
    }
}

// Runs a stream's mini on the DS on each of the set's boots: a stream of the sound archive or a game stream. The checks
// confirm the set (rip::RunStreamMini on each boot, rip::BootsError over them), and the run with no backup memory gives
// the time to the stream's start. Its length comes from its samples and the rate of its channels (rip::StreamTiming),
// and its first sounds and the end of a stream that ends from the render of 2sf's player (rip::TimeFromPlayer). A run
// with a backup memory fails when the channels play at another rate. A target, a run or a render that throws stops the
// rip with the stream's label in the message. Returns whether the runs count.
bool RunStream(const SetRun& run, Job& job)
{
    const std::vector<uint8_t> image = MiniImage(run, job);
    const auto make_target = [&]
    {
        const ntr::Rom rom = ntr::Rom::Parse(image);
        return job.kind == rip::SoundKind::kGameStream
                   ? rip::GameStreamTarget(rom, *job.game_stream, run.reserved)
                   : rip::ArchiveStreamTarget(rom, run.archive_file, job.stream, run.reserved);
    };
    const rip::StreamTarget target = WithLabel(job.label, make_target);
    std::vector<rip::StreamRun> runs;
    for (const rip::SetBoot& boot : run.boots)
    {
        const auto run_stream = [&]
        {
            return rip::RunStreamMini(image, boot.hook_clock, run.archive_file, target, boot.seconds, boot.backup);
        };
        runs.push_back(WithLabel(job.label, [&] { return rip::RunOnBoot(boot.backup, run_stream); }));
    }

    job.error = rip::BootsError(rip::BackupsOf(run.boots), rip::StreamChecks(runs));
    job.start_ms = runs.front().start_ms;
    if (job.error)
    {
        return false;
    }

    job.timing = rip::StreamTiming(target.samples, target.loop_start, target.loop, runs.front().rate);
    rip::PrintRenderReport(job.label, rip::TimeFromPlayer(image, job.start_ms, job.label, *job.timing));

    return true;
}

// Runs a sequence's mini on the DS on each of the set's boots: the checks confirm the set (rip::RunMini on each boot,
// rip::BootsError over them), and the run with no backup memory gives the time to the sequence's start and goes to the
// job. A run with a backup memory fails when its commands set other variables. A sequence of a game in the table of
// games that the game's code doesn't start, and that plays no notes, is left out with its reason
// (rip::LeftOutUnstarted). Returns whether the runs count.
bool RunSequenceMini(const SetRun& run, const ntr::snd::Build& build, Job& job)
{
    const std::vector<uint8_t> image = MiniImage(run, job);
    std::vector<rip::MiniRun> runs;
    for (const rip::SetBoot& boot : run.boots)
    {
        const auto run_mini = [&]
        {
            if (run.piece)
            {
                return rip::RunMemoryMini(image, boot.hook_clock, run.archive, job.sequence, boot.seconds, boot.backup);
            }

            const uint32_t video_frames = job.state && rip::PlaysOn(*job.state) ? job.state->video_frames : 0;
            return rip::RunMini(image, boot.hook_clock, run.archive_file, job.sequence, run.reserved, boot.seconds,
                                boot.backup, video_frames);
        };
        runs.push_back(rip::RunOnBoot(boot.backup, run_mini));
    }

    job.error = rip::BootsError(rip::BackupsOf(run.boots), rip::MiniChecks(runs));
    job.start_ms = runs.front().start_ms;
    if (!job.error)
    {
        job.run = std::move(runs.front());
        return true;
    }

    std::optional<xsflib::rip::Timing> timing;
    try
    {
        timing = ntr::MeasureSequence(run.archive, job.sequence, {}, build, {}, job.limit);
    }
    catch (const std::exception&)
    {
    }
    if (!rip::LeftOutUnstarted(run.code, runs, timing))
    {
        return false;
    }

    job.timing = timing;
    job.left_out = std::move(job.error);
    job.error.reset();
    return true;
}

// The analysis of a sequence whose runs count (RunSequenceMini): the length comes from the model of the driver, with
// the settings that the set gives the sequence on the run with no backup memory (rip::LengthSettings, with the game's
// profile). Music runs to the long limit. The first sounds and the end of a piece that ends come from the render of
// 2sf's player (rip::TimeFromPlayer). A game in the table of games is checked against its profile
// (rip::ProfileDifferences). A render that throws stops the rip with the sequence's label in the message.
void AnalyzeSequence(const SetRun& run, const ntr::snd::Build& build, Job& job)
{
    const rip::MiniRun& mini = *job.run;

    // The game's profile of the sequence, and of its state.
    const std::string base_label = job.state               ? job.state->label
                                   : job.sequence.archived ? ntr::Label(run.archive, job.sequence)
                                                           : job.label;
    const auto [game_code, file] = ProfileKey(run);
    const ntr::GameProfile profile = ntr::GameProfileOf(game_code, file, 0, base_label);
    const auto of_state = [&job](const ntr::ProfileArrangement& row)
    {
        return job.state && row.suffix == job.state->suffix;
    };
    const auto arrangement = std::ranges::find_if(profile.arrangements, of_state);
    const ntr::ProfileArrangement* state_profile = arrangement == profile.arrangements.end() ? nullptr : &*arrangement;
    const uint32_t player = RecordPlayer(run.archive, job.sequence).value_or(0);
    const uint16_t archive_channels = ArchiveChannels(run.archive, player);

    // A game that starts its music through its code is checked against the profile.
    if (run.code.game_start)
    {
        if (job.state && !job.state->suffix.empty() && !state_profile)
        {
            job.profile_differences.push_back("the profile has no arrangement " + job.state->suffix);
        }
        for (std::string& line : rip::ProfileDifferences(mini, profile, state_profile, archive_channels))
        {
            job.profile_differences.push_back(std::move(line));
        }

        // The game's lists after the start count only where they equal the profile's.
        const std::vector<std::string> later = rip::LaterDifferences(mini, profile, state_profile, archive_channels);
        if (!later.empty())
        {
            job.error = "the game's lists after the start differ from the profile's";
            for (const std::string& line : later)
            {
                *job.error += "; " + line;
            }
            return;
        }
    }

    const ntr::GameSettings settings = rip::LengthSettings(mini, profile.settings, player, archive_channels);
    job.echo_left_out = rip::EchoLeftOut(mini, settings);

    // Channels that the game's code locked by the start stay locked from the start.
    ntr::snd::CommandList commands;
    if (mini.locked != 0)
    {
        commands = ntr::ChannelLockCommands({.channels = mini.locked});
    }
    commands.insert(commands.end(), mini.commands.begin(), mini.commands.end());

    try
    {
        job.timing = ntr::MeasureSequence(run.archive, job.sequence, settings, build, {}, job.limit, commands,
                                          rip::LaterTickCommands(mini));
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "warning: %s: %s\n", job.label.c_str(), e.what());
    }

    if (job.timing)
    {
        const std::vector<uint8_t> image = MiniImage(run, job);
        rip::PrintRenderReport(job.label, rip::TimeFromPlayer(image, job.start_ms, job.label, *job.timing));
    }
}

// Runs a sequence's mini on the DS on each of the set's boots (RunSequenceMini), then analyzes it (AnalyzeSequence).
// Returns whether the runs count.
bool RunSequence(const SetRun& run, const ntr::snd::Build& build, Job& job)
{
    if (!RunSequenceMini(run, build, job))
    {
        return false;
    }

    if (!job.left_out)
    {
        AnalyzeSequence(run, build, job);
    }

    return true;
}

// For a game whose table entry merges numbers that start alike (rip::GameStart::merge_same_starts): the jobs of the
// sequences outside the sequence archives are grouped by the suffix of their state, with no suffix as one group. In
// each group, a job whose runs count and that starts like an earlier job of the same record (rip::FirstOfSameStart)
// moves to the `same` list of the earlier job. The jobs in the `same` list of the moved job move with it. A sequence
// and each of its states with a suffix therefore merge separately: SEQ_67_RESUME merges with SEQ_66_RESUME.
void MergeSameStarts(const SetRun& run, std::vector<Job>& jobs)
{
    // The jobs of each suffix, in the order of the set.
    std::map<std::string, std::vector<std::size_t>> of_suffix;
    for (std::size_t i = 0; i < jobs.size(); i++)
    {
        const Job& job = jobs[i];
        if (job.kind == rip::SoundKind::kSequence && !job.sequence.archived)
        {
            of_suffix[job.state ? job.state->suffix : std::string()].push_back(i);
        }
    }

    std::set<std::size_t> merged;
    for (const auto& [suffix, at] : of_suffix)
    {
        std::vector<uint32_t> numbers;
        std::vector<const rip::MiniRun*> runs;
        for (const std::size_t i : at)
        {
            numbers.push_back(jobs[i].sequence.index);
            runs.push_back(jobs[i].left_out || !jobs[i].run ? nullptr : &*jobs[i].run);
        }

        const std::vector<uint32_t> firsts = rip::FirstOfSameStart(run.archive, numbers, runs);
        std::map<uint32_t, std::size_t> job_of;
        for (std::size_t i = 0; i < numbers.size(); i++)
        {
            if (firsts[i] == numbers[i])
            {
                job_of[numbers[i]] = at[i];
                continue;
            }

            Job& first = jobs[job_of.at(firsts[i])];
            Job& later = jobs[at[i]];
            first.same.push_back(later.label);
            first.same.insert(first.same.end(), later.same.begin(), later.same.end());
            merged.insert(at[i]);
        }
    }

    std::vector<Job> kept;
    for (std::size_t i = 0; i < jobs.size(); i++)
    {
        if (!merged.contains(i))
        {
            kept.push_back(std::move(jobs[i]));
        }
    }
    jobs = std::move(kept);
}

// Runs each job's mini on the DS on each of the set's boots (RunSequence, RunStream): its checks confirm the set and
// give the time to the sound's start. Returns the number of minis that fail. Warns when a sequence's run has the
// capture units running and the profile has no reverb or output effect for the model to play.
std::size_t RunMinis(const SetRun& run, unsigned threads, std::vector<Job>& jobs)
{
    const ntr::snd::Build build = ntr::snd::BuildOf(run.rom.Arm7());
    // A game stream runs as the SDK's streams do.
    const auto is_stream = [](const Job& job)
    {
        return job.kind != rip::SoundKind::kSequence;
    };
    xsflib::cli::AnalysisSettings analysis;
    analysis.progress = "ran";
    if (run.code.game_start && run.code.game_start->merge_same_starts)
    {
        // The sequences' runs first, then the merge of the numbers that start alike, then the analysis of the jobs left
        // and the streams.
        const auto run_only = [&](Job& job)
        {
            if (!is_stream(job))
            {
                RunSequenceMini(run, build, job);
            }
            return false;
        };
        xsflib::cli::AnalyzeLengths(jobs, threads, run_only, analysis);
        MergeSameStarts(run, jobs);

        const auto analyze_only = [&](Job& job)
        {
            if (is_stream(job))
            {
                return RunStream(run, job);
            }
            if (job.error)
            {
                return false;
            }

            if (!job.left_out)
            {
                AnalyzeSequence(run, build, job);
            }
            return true;
        };
        analysis.progress = "analyzed";
        xsflib::cli::AnalyzeLengths(jobs, threads, analyze_only, analysis);
    }
    else
    {
        const auto analyze = [&](Job& job)
        {
            return is_stream(job) ? RunStream(run, job) : RunSequence(run, build, job);
        };
        xsflib::cli::AnalyzeLengths(jobs, threads, analyze, analysis);
    }

    const std::size_t failed = ReportFailures(jobs);

    for (const Job& job : jobs)
    {
        for (const std::string& line : job.profile_differences)
        {
            std::fprintf(stderr, "warning: %s: %s\n", job.label.c_str(), line.c_str());
        }
    }

    const auto echo_left_out = [](const Job& job)
    {
        return job.echo_left_out;
    };
    if (std::ranges::any_of(jobs, echo_left_out))
    {
        std::fprintf(stderr,
                     "warning: %s runs the capture units, and its profile has no reverb; pieces that end are tagged "
                     "short\n",
                     ProfileKey(run).first.c_str());
    }

    return failed;
}

// The game's files of `streams` (--stream-files), once for each file, named after the first stream that plays it. A
// game stream's file keeps its extension in the image.
std::vector<GameFile> StreamFiles(const ntr::Rom& rom, const sdat::Archive& archive, const std::vector<Job>& streams)
{
    std::vector<GameFile> files;
    std::set<uint32_t> written;
    std::set<uint32_t> written_from_image;
    for (const Job& job : streams)
    {
        if (job.kind == rip::SoundKind::kGameStream)
        {
            const uint32_t file = job.game_stream->file_id;
            const std::string& path = rom.Files().at(file).path;
            const std::size_t dot = path.rfind('.');
            if (written_from_image.insert(file).second)
            {
                files.push_back({.title = job.label,
                                 .extension = dot == std::string::npos ? std::string() : path.substr(dot),
                                 .bytes = rom.FileData(file)});
            }
            continue;
        }

        const uint32_t file = rip::StreamFile(archive, job.stream).value();
        if (written.insert(file).second)
        {
            files.push_back({.title = job.label, .extension = kStreamExtension, .bytes = archive.File(file)});
        }
    }

    return files;
}

// The minis of `jobs`, with a second library for each stream file that the set's library reserves (rip::SetArchive),
// named after the first stream that plays it. A stream file that the library keeps whole requires none: one that a boot
// reads, or every stream file where the library keeps the whole sound archive. A game stream's file gets a second
// library where the set's library leaves it out. `lengths` writes the length tags.
SetParts MiniParts(const SetRun& run, const std::vector<Job>& jobs, bool lengths)
{
    SetParts parts;
    std::map<uint32_t, std::size_t> library_of;       // a stream file, and its second library
    std::map<uint32_t, std::size_t> image_library_of; // a game stream's file of the image, and its second library
    for (const Job& job : jobs)
    {
        MiniSpec& mini = parts.minis.emplace_back();
        mini.title = job.label;
        mini.block = MiniBlock(run, job);
        if (lengths)
        {
            mini.timing = job.timing;
        }

        if (job.kind == rip::SoundKind::kGameStream)
        {
            const uint32_t file = job.game_stream->file_id;
            if (run.kept_files.contains(file))
            {
                continue;
            }
            if (!image_library_of.contains(file))
            {
                image_library_of[file] = parts.libraries.size();
                parts.libraries.push_back({.title = job.label, .block = GameStreamFileBlock(run, file)});
            }
            mini.library = image_library_of.at(file);
            continue;
        }
        if (job.kind != rip::SoundKind::kStream)
        {
            continue;
        }

        const uint32_t file = rip::StreamFile(run.archive, job.stream).value();
        if (!run.reserved.contains(file))
        {
            continue;
        }

        if (!library_of.contains(file))
        {
            const std::span<const uint8_t> data = run.archive.File(file);
            library_of[file] = parts.libraries.size();
            parts.libraries.push_back(
                {.title = job.label, .block = {run.stream_offsets.at(file), {data.begin(), data.end()}}});
        }
        mini.library = library_of.at(file);
    }

    return parts;
}

// The game's parts that a rip reads: the image, the game's routines, its state at the hook with no backup memory, the
// sound archive that it opened by then, the backup memories of the boots (rip::BootBackups), its state at the hook on
// each boot, its download-play programs (rip::FindPrograms), and its game streams (rip::FindGameStreams).
struct Game
{
    const ntr::Rom& rom;
    const ntr::Rom& boot_rom; // the image of the game's boots, `hooks` (rip::PatchedImage)
    const rip::SdatCode& code;
    const rip::HookState& hook;
    const rip::SoundArchive& found;
    const std::vector<ntr::BackupMemory>& backups;
    const std::vector<rip::HookState>& hooks;
    const std::vector<rip::Program>& programs;
    const std::optional<rip::GameStreams>& game_streams;
};

// A set's library: its image, the archive's files that it reserves for the streams (ReservedFiles), the boots of its
// minis, and the files that it keeps because a boot reads them before the hook.
struct Library
{
    rip::SdatSet set;
    std::set<uint32_t> files; // the image's files that it keeps
    std::set<uint32_t> reserved;
    std::vector<rip::SetBoot> boots;
    rip::BootFiles read;
};

// Each stream file's offset in `set`'s image, for the streams of `jobs`. Nothing when the image's sound archive has no
// place for one: `missing` then gives its stream's label.
std::optional<std::map<uint32_t, uint32_t>> StreamOffsets(const Game& game, const rip::SdatSet& set,
                                                          const std::vector<Job>& jobs, std::string& missing)
{
    std::map<uint32_t, uint32_t> offsets;
    for (const Job& job : jobs)
    {
        if (job.kind != rip::SoundKind::kStream)
        {
            continue;
        }

        const uint32_t file = rip::StreamFile(game.found.archive, job.stream).value();
        const std::optional<uint32_t> offset = rip::ArchiveFileOffset(set.image, game.found.file_id, file);
        if (!offset)
        {
            missing = job.label;
            return std::nullopt;
        }
        offsets[file] = *offset;
    }

    return offsets;
}

// Makes `library`, the library of `game` for `jobs` with `stub`. The full image's boots must let the stub run, with the
// sound heap where the game's boots put it. The library keeps what any boot of the game or of the full image reads
// before the hook. On each boot, the library's boot must match the full image's: the clock, the files, the count of
// card reads and the heap (rip::UnlikeHookBoot). Says why the library fails.
std::optional<std::string> MakeLibrary(const Game& game, const rip::SdatStub& stub, const std::vector<Job>& jobs,
                                       Library& library)
{
    const FullBoots full = BootFullImage(game.rom, game.code, stub, game.found, game.backups);
    for (std::size_t i = 0; i < game.backups.size(); i++)
    {
        if (const std::optional<std::string> problem = rip::HookProblem(full.hooks[i], game.hook.heap))
        {
            return *problem + ", with every file in the set's image" + rip::OnBoot(game.backups[i]);
        }
    }

    library.read = rip::LibraryRead(rip::FilesRead(game.boot_rom, game.found, game.hooks), full.read);
    LibraryParts parts =
        LibraryFiles(game.rom, game.found, library.read, jobs, ReservedFiles(game.found, library.read, jobs));
    library.reserved = std::move(parts.reserved);
    for (const auto& [id, bytes] : parts.files)
    {
        library.files.insert(id);
    }
    library.set = rip::MakeSdatSet(game.rom, game.code, stub, parts.files);

    const ntr::Rom set_rom = ntr::Rom::Parse(library.set.image);
    std::vector<rip::HookState> set_hooks;
    for (const ntr::BackupMemory& backup : game.backups)
    {
        set_hooks.push_back(rip::RunOnBoot(
            backup, [&] { return rip::BootToHook(set_rom, game.code.update, backup, game.code.heap_word); }));
    }

    if (const std::optional<std::size_t> unlike = rip::UnlikeHookBoot(full.hooks, set_hooks))
    {
        return "the library's boot differs at the hook from the boot with every file in the set's image" +
               rip::OnBoot(game.backups[*unlike]);
    }

    for (std::size_t i = 0; i < game.backups.size(); i++)
    {
        library.boots.push_back({game.backups[i], set_hooks[i].clock, game.hooks[i].seconds + kStartLimitSeconds});
    }

    return std::nullopt;
}

// Prints the counts of the files of the image and of the sound archive that `library` keeps because only a boot with a
// backup memory reads them, or only the boot of the full image.
void ReportBootFiles(const Game& game, const Library& library)
{
    const rip::BootFiles game_read = rip::FilesRead(game.boot_rom, game.found, game.hooks);
    const rip::BootFiles first_read = rip::FilesRead(game.boot_rom, game.found, std::span(game.hooks).first(1));
    const std::size_t gained_files = game_read.files.size() - first_read.files.size();
    const std::size_t gained_archive_files = game_read.archive_files.size() - first_read.archive_files.size();
    if (gained_files > 0 || gained_archive_files > 0)
    {
        std::fprintf(stderr,
                     "the library keeps %s of the image and %s of the sound archive that the game reads only with a "
                     "backup memory\n",
                     CountOf(gained_files, "file", "files").c_str(),
                     CountOf(gained_archive_files, "file", "files").c_str());
    }

    const std::size_t set_files = library.read.files.size() - game_read.files.size();
    const std::size_t set_archive_files = library.read.archive_files.size() - game_read.archive_files.size();
    if (set_files > 0 || set_archive_files > 0)
    {
        std::fprintf(stderr,
                     "the library keeps %s of the image and %s of the sound archive that the set's boot reads before "
                     "the hook, and the game's boot doesn't\n",
                     CountOf(set_files, "file", "files").c_str(), CountOf(set_archive_files, "file", "files").c_str());
    }
}

// The jobs of the pieces of `program` that --only picks, music only unless `effects`, as SequenceJobs picks the game's
// sequences, with their states (WithStates). Each piece's job has the piece's state with no suffix, the state that
// contains its calls. Each of its states with a suffix gets a job after it.
std::vector<Job> ProgramJobs(const rip::Program& program, const xsflib::cli::RipOptions& o, bool effects)
{
    std::vector<Job> picked;
    for (const rip::ProgramPiece& piece : program.pieces)
    {
        const sdat::Archive archive = sdat::Archive::Parse(piece.archive);
        const std::set<uint32_t> music = rip::MusicSequences(archive, piece.game_code);
        if (!std::regex_search(piece.label, o.only) || (!music.contains(piece.sequence.index) && (!effects || o.bgm)))
        {
            continue;
        }

        picked.push_back({.sequence = piece.sequence,
                          .label = piece.label,
                          .limit = rip::AnalysisLimit(music, piece.sequence.index)});
    }

    std::vector<Job> jobs = WithStates(program.code, std::move(picked));
    for (std::size_t i = 0; i < jobs.size(); i++)
    {
        jobs[i].index = static_cast<uint32_t>(i + 1);
    }

    return jobs;
}

// Boots `rom`, an image of a program's set, on each of `backups` to the hook.
std::vector<rip::HookState> BootProgram(const ntr::Rom& rom, const rip::SdatCode& code,
                                        std::span<const ntr::BackupMemory> backups)
{
    std::vector<rip::HookState> hooks;
    for (const ntr::BackupMemory& backup : backups)
    {
        hooks.push_back(
            rip::RunOnBoot(backup, [&] { return rip::BootToHook(rom, code.update, backup, code.heap_word); }));
    }

    return hooks;
}

// Rips `program`, a download-play program of the game (rip/program_set.h), into its library and the minis of `jobs`
// (ProgramJobs). The library's stub starts the first piece. The boots of the full image, the program's image with the
// stub, must let the stub run, and on each boot the library's boot must match the full image's (rip::UnlikeHookBoot):
// the library differs only in the free archives. No boot then reads those archives before the hook. Each piece's mini
// runs on the DS on each boot as a sequence of the game does (RunMinis), with its archive in the program's memory.
// Returns nothing after printing why the program fails.
std::optional<ProgramParts> RipProgram(const std::string& input, const rip::Program& program, std::vector<Job> jobs,
                                       const xsflib::cli::RipOptions& o)
{
    const auto refuse = [&](const std::string& why) -> std::optional<ProgramParts>
    {
        Refuse(input, "2sfrip can't rip the download-play program " + program.name + ": " + why);
        return std::nullopt;
    };

    // The stub, with the first piece's start block.
    const rip::SdatCode& code = program.code;
    const ntr::SequenceRef first = jobs.front().sequence;
    rip::SdatStub stub = rip::BuildSdatStub(code, 0, ntr::rip::SoundId(first));
    const std::vector<uint8_t> block =
        rip::StartBlock(code, ntr::rip::SoundId(first), std::nullopt, jobs.front().state);
    std::ranges::copy(block, stub.bytes.begin() + stub.sound_offset);

    // The full image's boots and the library's.
    const ntr::Rom image = ntr::Rom::Parse(program.image);
    const ntr::Rom library_image = ntr::Rom::Parse(program.library_image);
    const rip::SdatSet full = rip::MakeSdatSet(image, code, stub, {});
    const rip::SdatSet set = rip::MakeSdatSet(library_image, code, stub, {});
    const ntr::Rom full_rom = ntr::Rom::Parse(full.image);
    const ntr::Rom set_rom = ntr::Rom::Parse(set.image);
    const std::vector<ntr::BackupMemory> backups = rip::BootBackups(set_rom.Header().game_code);
    const std::vector<rip::HookState> full_hooks = BootProgram(full_rom, code, backups);
    const std::vector<rip::HookState> set_hooks = BootProgram(set_rom, code, backups);
    for (std::size_t i = 0; i < backups.size(); i++)
    {
        if (const std::optional<std::string> problem = rip::HookProblem(full_hooks[i], full_hooks.front().heap))
        {
            return refuse(*problem + rip::OnBoot(backups[i]));
        }
    }

    if (const std::optional<std::size_t> unlike = rip::UnlikeHookBoot(full_hooks, set_hooks))
    {
        return refuse("the library's boot differs at the hook from the boot of the program's image" +
                      rip::OnBoot(backups[*unlike]));
    }

    std::vector<rip::SetBoot> boots;
    for (std::size_t i = 0; i < backups.size(); i++)
    {
        boots.push_back({backups[i], set_hooks[i].clock, set_hooks[i].seconds + kStartLimitSeconds});
    }

    std::fprintf(stderr, "%s: the hook at %.3f s, the library %zu bytes\n", program.name.c_str(),
                 set_hooks.front().seconds, set.image.size());

    // Each piece's mini and each of its states' minis, with the piece's archive.
    ProgramParts parts{.name = program.name, .image = set.image};
    for (Job& job : jobs)
    {
        const auto piece = std::ranges::find(program.pieces, job.state->label, &rip::ProgramPiece::label);
        const sdat::Archive archive = sdat::Archive::Parse(piece->archive);
        const SetRun run{
            .rom = set_rom, .set = set, .code = code, .archive = archive, .boots = boots, .piece = &*piece};
        std::vector<Job> one = {std::move(job)};
        if (RunMinis(run, o.jobs, one) > 0)
        {
            return refuse(one.front().label + " doesn't play on the DS");
        }

        DropSilent(one);
        std::ranges::move(MiniParts(run, one, o.lengths).minis, std::back_inserter(parts.minis));
    }

    return parts;
}

// The sets of the game's download-play programs (rip/program_set.h) with the pieces that --only picks, music only
// unless `effects`. Returns nothing after printing why a program fails.
std::optional<std::vector<ProgramParts>> RipPrograms(const std::string& input,
                                                     const std::vector<rip::Program>& programs,
                                                     const xsflib::cli::RipOptions& o, bool effects)
{
    std::vector<ProgramParts> sets;
    for (const rip::Program& program : programs)
    {
        std::vector<Job> jobs = ProgramJobs(program, o, effects);
        if (jobs.empty())
        {
            continue;
        }

        std::optional<ProgramParts> set = RipProgram(input, program, std::move(jobs), o);
        if (!set)
        {
            return std::nullopt;
        }
        sets.push_back(std::move(*set));
    }

    return sets;
}

// Makes the set of `jobs`, runs each mini on the DS on each boot, and writes the set with the game's files
// `stream_files` and the sets of its download-play programs, `programs`.
int RipMinis(const std::string& input, const Game& game, std::vector<Job> jobs, const std::vector<Job>& stream_files,
             std::vector<ProgramParts> programs, const xsflib::cli::RipOptions& o,
             xsflib::cli::FileNameRegistries& names)
{
    const auto first_sequence = std::ranges::find(jobs, rip::SoundKind::kSequence, &Job::kind);
    const uint32_t first_sound = first_sequence == jobs.end() ? 0 : ntr::rip::SoundId(first_sequence->sequence);
    const std::optional<uint32_t> first_player =
        first_sequence == jobs.end() ? std::nullopt : RecordPlayer(game.found.archive, first_sequence->sequence);
    const rip::SdatStub stub =
        rip::BuildSdatStub(game.code, game.hook.heap->address, first_sound, rip::SoundKind::kSequence, first_player);
    Library library;
    if (const std::optional<std::string> problem = MakeLibrary(game, stub, jobs, library))
    {
        return Refuse(input, "2sfrip can't rip this game: " + *problem);
    }

    const rip::SdatSet& set = library.set;
    std::fprintf(stderr, "the hook at %.3f s, the sound heap at %08X with %u bytes free, the library %zu bytes\n",
                 game.hook.seconds, game.hook.heap->address, game.hook.heap->free, set.image.size());
    ReportBootFiles(game, library);

    std::string missing;
    std::optional<std::map<uint32_t, uint32_t>> offsets = StreamOffsets(game, set, jobs, missing);
    if (!offsets)
    {
        return Refuse(input, "2sfrip can't rip this game: the library's sound archive has no place for " + missing);
    }

    const SetRun run{.rom = game.rom,
                     .set = set,
                     .code = game.code,
                     .heap = game.hook.heap->address,
                     .archive_file = game.found.file_id,
                     .archive = game.found.archive,
                     .boots = library.boots,
                     .stream_offsets = std::move(*offsets),
                     .reserved = library.reserved,
                     .game_streams = game.game_streams ? &*game.game_streams : nullptr,
                     .kept_files = library.files};
    const std::size_t failed = RunMinis(run, o.jobs, jobs);
    if (failed > 0)
    {
        return Refuse(input, "2sfrip can't rip this game: " + std::to_string(failed) + " of " +
                                 std::to_string(jobs.size()) + " minis don't play their sound on the DS");
    }

    DropSilent(jobs);
    ReportShared(jobs, rip::SoundKind::kSequence, "sequence", "sequences");
    ReportShared(jobs, rip::SoundKind::kStream, "stream", "streams");
    if (jobs.empty() && stream_files.empty() && programs.empty())
    {
        return Refuse(input, "no sequence plays a note");
    }

    SetParts parts = MiniParts(run, jobs, o.lengths);
    parts.files = StreamFiles(game.rom, game.found.archive, stream_files);
    parts.programs = std::move(programs);

    return WriteSet(input, game.rom, set.image, parts, o, names);
}

// Picks the set's sequences and streams of `game`, then lists them, or rips them. --stream-files writes the game's
// stream files beside the set in place of the stream minis.
int RipSounds(const std::string& input, const Game& game, const xsflib::cli::RipOptions& o, const SdatOptions& sdat,
              xsflib::cli::FileNameRegistries& names)
{
    const sdat::Archive& archive = game.found.archive;
    const std::set<uint32_t> music = rip::MusicSequences(archive, game.rom.Header().game_code);
    std::fprintf(stderr, "%s: the SDK's sound archive, %zu sequences\n", input.c_str(), archive.sequences.size());

    std::size_t archived = 0;
    std::size_t silent_streams = 0;
    std::vector<Job> jobs = SequenceJobs(game.code, archive, music, o, sdat.effects, archived);
    std::vector<Job> streams = StreamJobs(game.code, archive, o, silent_streams);
    if (o.list)
    {
        std::ranges::move(streams, std::back_inserter(jobs));
        ListSounds(game.code, archive, music, jobs, game.code.left_out,
                   sdat.stream_files || rip::StartsStreams(game.code), game.game_streams);
        ListPrograms(game.programs);
        return 0;
    }

    // The sets of the game's download-play programs, written beside the game's.
    std::optional<std::vector<ProgramParts>> programs = RipPrograms(input, game.programs, o, sdat.effects);
    if (!programs)
    {
        return 1;
    }

    if (archived > 0)
    {
        std::fprintf(stderr, "%zu sequences in sequence archives are left out: %s\n", archived,
                     StartsArchived(game.code) ? "--effects adds them" : "2sfrip doesn't start them yet");
    }

    if (silent_streams > 0)
    {
        std::fprintf(stderr, "%zu streams make no sound and are left out\n", silent_streams);
    }

    // The streams: minis, or with --stream-files the game's files beside the set.
    std::vector<Job> stream_files;
    if (sdat.stream_files)
    {
        if (streams.empty() && GameStreamJobs(game.game_streams, o).empty())
        {
            std::fprintf(stderr, "warning: --stream-files writes nothing: the rip has no streams\n");
        }
        stream_files = std::move(streams);
        streams.clear();
    }
    else if (!streams.empty() && !rip::StartsStreams(game.code))
    {
        std::fprintf(stderr, "warning: %zu streams are left out: %s\n", streams.size(),
                     game.code.no_stream_start.c_str());
        streams.clear();
    }

    // The game streams that the game's play routine starts: minis, or with --stream-files the game's files.
    std::vector<Job> game_streams = GameStreamJobs(game.game_streams, o);
    if (sdat.stream_files)
    {
        std::ranges::move(game_streams, std::back_inserter(stream_files));
        game_streams.clear();
    }
    std::ranges::move(game_streams, std::back_inserter(streams));

    std::ranges::move(streams, std::back_inserter(jobs));
    for (std::size_t i = 0; i < jobs.size(); i++)
    {
        jobs[i].index = static_cast<uint32_t>(i + 1);
    }

    if (jobs.empty() && stream_files.empty() && programs->empty())
    {
        return Refuse(input, "no sequence or stream to rip");
    }

    if (jobs.empty())
    {
        return WriteSet(input, game.rom, {},
                        {.files = StreamFiles(game.rom, archive, stream_files), .programs = std::move(*programs)}, o,
                        names);
    }

    return RipMinis(input, game, std::move(jobs), stream_files, std::move(*programs), o, names);
}

// Rips `rom`, an image that a DS runs.
int RipDsImage(const std::string& input, const ntr::Rom& rom, const xsflib::cli::RipOptions& o, const SdatOptions& sdat,
               xsflib::cli::FileNameRegistries& names)
{
    if (!rip::FindSoundArchive(rom, rip::EveryFile(rom)))
    {
        return Refuse(input, "2sfrip doesn't support this game (game code " + rom.Header().game_code +
                                 "): it has no sound archive (SDAT) with sequences at a file's start");
    }

    std::string error;
    const std::optional<rip::SdatCode> code = rip::FindSdatCode(rom, error);
    if (!code)
    {
        return Refuse(input, "2sfrip can't rip this game: " + error);
    }

    const std::optional<std::vector<rip::Program>> programs = rip::FindPrograms(rom, error);
    if (!programs)
    {
        return Refuse(input, "2sfrip can't rip this game's download-play program: " + error);
    }

    error.clear();
    const std::optional<rip::GameStreams> game_streams = rip::FindGameStreams(rom, error);
    if (!error.empty())
    {
        return Refuse(input, "2sfrip can't rip this game's streams: " + error);
    }

    // The game's state at the hook on each boot (rip/boots.h), on the whole image with the patches of the game's code,
    // and the sound archive that the game opened by then on the boot with no backup memory. Every boot must reach the
    // hook with the same sound heap: the stub uses its address.
    const std::vector<uint8_t> boot_image = rip::PatchedImage(rom, code->patches);
    const ntr::Rom boot_rom = ntr::Rom::Parse(boot_image);
    const std::vector<ntr::BackupMemory> backups = rip::BootBackups(rom.Header().game_code);
    std::vector<rip::HookState> hooks;
    for (const ntr::BackupMemory& backup : backups)
    {
        const rip::HookState& boot = hooks.emplace_back(
            rip::RunOnBoot(backup, [&] { return rip::BootToHook(boot_rom, code->update, backup, code->heap_word); }));
        if (const std::optional<std::string> problem = rip::HookProblem(boot, hooks.front().heap))
        {
            return Refuse(input, "2sfrip can't rip this game: " + *problem + rip::OnBoot(backup));
        }
    }

    const rip::HookState& hook = hooks.front();
    const std::optional<rip::SoundArchive> found = rip::FindSoundArchive(rom, hook.files);
    if (!found)
    {
        return Refuse(input,
                      "2sfrip can't rip this game: it reads no sound archive with sequences before its first call of "
                      "the sound library's update");
    }

    // A game in the table of games sizes its start blocks by its sound archive. Its states may not fit.
    rip::SdatCode set_code = *code;
    if (set_code.game_start)
    {
        set_code.start_block = rip::StartBlockBytes(set_code, found->archive);
        if (const std::optional<std::string> problem = rip::StubSizeProblem(set_code))
        {
            return Refuse(input, "2sfrip can't rip this game: " + *problem);
        }
    }

    const Game game{.rom = rom,
                    .boot_rom = boot_rom,
                    .code = set_code,
                    .hook = hook,
                    .found = *found,
                    .backups = backups,
                    .hooks = hooks,
                    .programs = *programs,
                    .game_streams = game_streams};

    return RipSounds(input, game, o, sdat, names);
}

} // namespace

int RipSdat(const std::string& input, const ntr::Rom& rom, const xsflib::cli::RipOptions& o, const SdatOptions& sdat,
            xsflib::cli::FileNameRegistries& names)
{
    // A game that runs only on a DSi boots on ntdlib's DS in DSi mode. Its set plays only in 2sf's player.
    const auto unit = static_cast<ntr::UnitCode>(rom.Header().unit_code);
    if (unit == ntr::UnitCode::kDs || unit == ntr::UnitCode::kDsiOnly)
    {
        return RipDsImage(input, rom, o, sdat, names);
    }

    if (unit != ntr::UnitCode::kDsiEnhanced)
    {
        std::fprintf(stderr, "error: %s: unit code %u. 2sfrip rips DS, DSi-enhanced and DSi-only games.\n",
                     input.c_str(), static_cast<unsigned>(unit));
        return 1;
    }

    // A DSi-enhanced game runs on a DS as a DS game. The set is made from the image with the unit code of a DS image.
    std::fprintf(stderr, "%s: a DSi-enhanced image. 2sfrip rips the game as a DS runs it.\n", input.c_str());
    const std::vector<uint8_t> image = rip::DsImage(rom);

    return RipDsImage(input, ntr::Rom::Parse(image), o, sdat, names);
}

} // namespace twosf::tools
