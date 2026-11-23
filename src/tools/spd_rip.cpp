// SPDX-License-Identifier: MIT

#include "tools/spd_rip.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/ntr/machine.h"
#include "ntdlib/spd/driver.h"
#include "ntdlib/spd/loops.h"
#include "ntdlib/spd/player.h"
#include "ntdlib/spd/profile.h"
#include "ntdlib/spd/song.h"
#include "xsflib/cli/length_analysis.h"
#include "xsflib/rip/length.h"

#include "rip/boots.h"
#include "rip/mini_render.h"
#include "rip/profile.h"
#include "rip/spd_set.h"
#include "tools/hook_library.h"
#include "tools/messages.h"
#include "tools/set_files.h"

namespace twosf::tools
{

namespace
{

namespace ntr = ntdlib::ntr;
namespace spd = ntdlib::spd;

// The time after the hook within which a mini's sound must start.
constexpr double kStartLimitSeconds = 10.0;

// The player that plays the music.
constexpr int kMusicPlayer = 0;

// A sound of the set.
struct Job
{
    int song = 0; // the index into spd::DriverInfo::songs
    std::string label;
    rip::SpdStart start;
    std::optional<xsflib::rip::Timing> timing;
    double start_ms = 0;              // the time from the boot to the stub's start of the sound on the DS
    std::optional<std::string> error; // why the mini's run on the DS fails
};

// The sound data, read.
struct SoundData
{
    spd::DriverInfo info;
    std::span<const uint8_t> bytes;
    std::vector<spd::Block> blocks;
};

std::optional<spd::Sound> SoundOf(const SoundData& data, int song)
{
    const spd::SongEntry& entry = data.info.songs[static_cast<std::size_t>(song)];
    return data.blocks[static_cast<std::size_t>(entry.block)].SoundAt(entry.sound);
}

// Why a sound of player 0 gets no mini: it only starts another sound (spd::RedirectOf), or it's a section that only a
// variant of a piece reaches (the game's profile). Nothing for a sound that gets one.
std::optional<std::string> NoMini(const SoundData& data, const spd::Profile& profile, int song)
{
    const std::string label = spd::SongName(data.info, song);
    if (const std::optional<spd::Section> section = spd::SectionOf(profile, label))
    {
        return "section of " + std::string(section->piece) + " (branch flag 1)";
    }
    if (const std::optional<int> target = spd::RedirectOf(data.bytes, data.blocks, data.info, song))
    {
        return "alias of " + spd::SongName(data.info, *target);
    }

    return std::nullopt;
}

// The sounds of player 0 that --only picks, in the order of the sound data, each followed by its variants in the game's
// profile. The sounds that get no mini go to `notes` with the reason.
std::vector<Job> SoundJobs(const SoundData& data, const spd::Profile& profile, const xsflib::cli::RipOptions& o,
                           std::vector<std::string>& notes)
{
    std::vector<Job> jobs;
    for (std::size_t i = 0; i < data.info.songs.size(); i++)
    {
        const int song = static_cast<int>(i);
        const std::optional<spd::Sound> sound = SoundOf(data, song);
        if (!sound || spd::PlayerOf(sound->flags) != kMusicPlayer)
        {
            continue;
        }

        const std::string label = spd::SongName(data.info, song);
        const bool selected = std::regex_search(label, o.only);
        if (const std::optional<std::string> why = NoMini(data, profile, song))
        {
            if (selected)
            {
                notes.push_back(label + ": " + *why);
            }
            continue;
        }

        const spd::SongEntry& entry = data.info.songs[i];
        const rip::SpdStart start = {.sound = static_cast<uint32_t>(entry.sound + 1),
                                     .slot = static_cast<uint32_t>(data.info.blocks[entry.block].slot)};
        if (selected)
        {
            jobs.push_back({.song = song, .label = label, .start = start});
        }
        for (const spd::Variant& variant : profile.variants)
        {
            const std::string variant_label = label + std::string(variant.suffix);
            if (variant.sound == label && std::regex_search(variant_label, o.only))
            {
                rip::SpdStart flagged = start;
                flagged.branch_flag = static_cast<uint32_t>(variant.branch_flag);
                jobs.push_back({.song = song, .label = variant_label, .start = flagged});
            }
        }
    }

    return jobs;
}

void ListSounds(const SoundData& data, const spd::Profile& profile)
{
    for (std::size_t i = 0; i < data.info.songs.size(); i++)
    {
        const int song = static_cast<int>(i);
        const std::optional<spd::Sound> sound = SoundOf(data, song);
        if (!sound)
        {
            continue;
        }

        const std::string label = spd::SongName(data.info, song);
        const int player = spd::PlayerOf(sound->flags);
        if (player != kMusicPlayer)
        {
            std::printf("%s  effect on player %d (not ripped)\n", label.c_str(), player);
            continue;
        }
        if (const std::optional<std::string> why = NoMini(data, profile, song))
        {
            std::printf("%s  %s\n", label.c_str(), why->c_str());
            continue;
        }

        std::printf("%s  music\n", label.c_str());
        for (const spd::Variant& variant : profile.variants)
        {
            if (variant.sound == label)
            {
                std::printf("%s%s  music, branch flag %d\n", label.c_str(), std::string(variant.suffix).c_str(),
                            variant.branch_flag);
            }
        }
    }

    std::fflush(stdout);
}

// The timing of `job`'s sound on the model of the driver (spd::MeasureSound), with set A and the sound's block as set
// B, from the frame of its start message.
xsflib::rip::Timing MeasureJob(const SoundData& data, const Job& job)
{
    const xsflib::rip::LengthRules rules;
    const double frame_ms = spd::kFrameSeconds * 1000.0;
    const auto max_frames = static_cast<uint32_t>(rules.max_ms / frame_ms);
    const spd::SongEntry& entry = data.info.songs[static_cast<std::size_t>(job.song)];
    const spd::Block* set_b = entry.block != 0 ? &data.blocks[static_cast<std::size_t>(entry.block)] : nullptr;
    spd::Player player(data.bytes, data.blocks.front(), set_b, static_cast<int>(job.start.slot),
                       spd::GameLayout(data.bytes));
    player.SetBranchFlag(static_cast<int>(job.start.branch_flag));
    const spd::SoundLength length =
        spd::MeasureSound(player, static_cast<int>(job.start.slot), static_cast<int>(job.start.sound), max_frames);
    if (!length.audible)
    {
        xsflib::rip::Timing timing = xsflib::rip::EndAt(0, frame_ms);
        timing.silent = true;
        return timing;
    }

    if (length.loop_start)
    {
        constexpr auto kPasses = static_cast<uint32_t>(xsflib::rip::kLengthLoops);
        return xsflib::rip::PassesAt(*length.loop_start + kPasses * length.loop_frames, length.loop_frames, frame_ms,
                                     rules.fade_ms);
    }

    if (length.end)
    {
        return xsflib::rip::EndAt(*length.end, frame_ms);
    }

    // A note held without end fades from the frame after the sound stops changing, as a steady sequence does.
    if (length.held)
    {
        return xsflib::rip::LoopAt(*length.held + 1, frame_ms, rules.held_fade_ms);
    }

    return {.length_ms = static_cast<long long>(rules.max_ms),
            .fade_ms = static_cast<long long>(rules.fade_ms),
            .at_limit = true};
}

// Runs each job's mini on the DS on each of `boots`: the checks confirm the set (rip::RunSpdMini on each boot, and
// rip::BootsError over them), and the run with no backup memory gives the time to the sound's start. The length comes
// from the model of the driver. The first sounds and the end of a sound that ends come from the render of 2sf's player
// (rip::TimeFromPlayer). Returns the number of minis that fail.
std::size_t RunMinis(const rip::SpdCode& code, const rip::SpdSet& set, std::span<const rip::SetBoot> boots,
                     const SoundData& data, unsigned threads, std::vector<Job>& jobs)
{
    const std::vector<ntr::BackupMemory> backups = rip::BackupsOf(boots);
    const auto analyze = [&](Job& job)
    {
        std::vector<uint8_t> image = set.image;
        const std::vector<uint8_t> words = rip::SpdStartWords(job.start);
        std::ranges::copy(words, image.begin() + set.start_word);
        std::vector<rip::BootRun> checks;
        for (const rip::SetBoot& boot : boots)
        {
            const auto run_mini = [&]
            {
                return rip::RunSpdMini(image, code, boot.hook_clock, job.start, boot.seconds, boot.backup);
            };
            const rip::SpdMiniRun run = rip::RunOnBoot(boot.backup, run_mini);
            if (checks.empty())
            {
                job.start_ms = run.start_ms;
            }
            checks.push_back({run.error, run.sounded});
        }

        job.error = rip::BootsError(backups, checks);
        if (job.error)
        {
            return false;
        }

        try
        {
            job.timing = MeasureJob(data, job);
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "warning: %s: %s\n", job.label.c_str(), e.what());
        }

        if (!job.timing)
        {
            return true;
        }

        rip::PrintRenderReport(job.label, rip::TimeFromPlayer(std::move(image), job.start_ms, job.label, *job.timing));

        return true;
    };
    xsflib::cli::AnalysisSettings analysis;
    analysis.progress = "ran";
    xsflib::cli::AnalyzeLengths(jobs, threads, analyze, analysis);

    return ReportFailures(jobs);
}

} // namespace

bool IsSpdGame(const ntr::Rom& rom)
{
    return rip::ListsSpdGame(rom.Header().game_code);
}

int RipSpd(const std::string& input, const ntr::Rom& rom, const xsflib::cli::RipOptions& o, bool effects,
           xsflib::cli::FileNameRegistries& names)
{
    std::string error;
    const std::optional<rip::SpdCode> code = rip::FindSpdCode(rom, error);
    if (!code)
    {
        return Refuse(input, "2sfrip can't rip this game: " + error);
    }

    const std::optional<spd::DriverInfo> info = spd::FindDriver(rom);
    if (!info)
    {
        return Refuse(input, "2sfrip can't rip this game: it has no sound data of Nintendo SPD's driver");
    }

    SoundData data{.info = *info, .bytes = spd::SoundData(rom, *info)};
    try
    {
        data.blocks = spd::ReadBlockData(data.bytes, data.info);
    }
    catch (const std::exception& e)
    {
        return Refuse(input, "2sfrip can't read the sound data: " + std::string(e.what()));
    }

    std::fprintf(stderr, "%s: Nintendo SPD's driver, %zu blocks, %zu sounds\n", input.c_str(), data.blocks.size(),
                 data.info.songs.size());
    const spd::Profile profile = spd::ProfileOf(rom.Header().game_code);
    if (o.list)
    {
        ListSounds(data, profile);
        return 0;
    }

    if (effects && !o.bgm)
    {
        std::fprintf(stderr, "2sfrip doesn't rip this driver's sound effects yet: the set contains the music\n");
    }

    std::vector<std::string> notes;
    std::vector<Job> jobs = SoundJobs(data, profile, o, notes);
    for (const std::string& note : notes)
    {
        std::fprintf(stderr, "%s, no mini\n", note.c_str());
    }
    if (jobs.empty())
    {
        return Refuse(input, "no sound to rip");
    }

    // The library starts the first sound, and keeps the sound data's file.
    const rip::SpdStub stub = rip::BuildSpdStub(*code, jobs.front().start);
    const auto image_of = [&](const std::map<uint32_t, std::vector<uint8_t>>& kept)
    {
        return rip::MakeSpdSet(rom, *code, stub, kept).image;
    };
    const std::optional<HookLibrary> library =
        MakeHookLibrary(rom, code->hook, {data.info.file_id}, image_of, kStartLimitSeconds, error);
    if (!library)
    {
        return Refuse(input, "2sfrip can't rip this game: " + error);
    }
    const rip::SpdSet set = rip::MakeSpdSet(rom, *code, stub, rip::WholeFiles(rom, library->files));

    const std::size_t failed = RunMinis(*code, set, library->boots, data, o.jobs, jobs);
    if (failed > 0)
    {
        return Refuse(input, "2sfrip can't rip this game: " + std::to_string(failed) + " of " +
                                 std::to_string(jobs.size()) + " minis don't play their sound on the DS");
    }

    // A sound that the model never hears gets no mini.
    const std::size_t before = jobs.size();
    std::erase_if(jobs, [](const Job& job) { return job.timing && job.timing->silent; });
    if (jobs.size() < before)
    {
        std::fprintf(stderr, "%zu sounds make no sound and are left out\n", before - jobs.size());
    }

    if (jobs.empty())
    {
        return Refuse(input, "no sound makes a sound");
    }

    std::vector<MiniSpec> minis;
    for (const Job& job : jobs)
    {
        MiniSpec& mini = minis.emplace_back();
        mini.title = job.label;
        mini.block = {set.start_word, rip::SpdStartWords(job.start)};
        if (o.lengths)
        {
            mini.timing = job.timing;
        }
    }

    return WriteSet(input, rom, set.image, {.minis = std::move(minis)}, o, names);
}

} // namespace twosf::tools
