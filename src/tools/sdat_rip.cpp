// SPDX-License-Identifier: MIT

#include "tools/sdat_rip.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "ntdlib/ntr/length_model.h"
#include "ntdlib/ntr/rip.h"
#include "ntdlib/ntr/rom_writer.h"
#include "ntdlib/ntr/snd/driver.h"
#include "xsflib/ascii.h"
#include "xsflib/bytes.h"
#include "xsflib/cli/length_analysis.h"
#include "xsflib/files.h"
#include "xsflib/rip/file_names.h"
#include "xsflib/rip/tool.h"

#include "rip/profile.h"
#include "rip/sdat_set.h"
#include "rip/set.h"
#include "twosf/twosf_file.h"

namespace twosf::tools
{

namespace
{

namespace fs = std::filesystem;
namespace ntr = ntdlib::ntr;
namespace sdat = ntdlib::sdat;

constexpr char kPlaylist[] = "!playlist.m3u";
constexpr char kToolName[] = "2sfrip";

// The time after the hook within which a mini's sequence must start.
constexpr double kStartLimitSeconds = 10.0;

// The header's unit code, and its CRC-16 over the bytes before it.
constexpr std::size_t kUnitCodeField = 0x12;
constexpr std::size_t kHeaderCrcField = 0x15E;

// The tags of the games that 2sfrip has been checked on. The year is the first release's date.
struct GameTags
{
    std::string_view game_code;
    std::string_view product_code; // the base of the library's name
    std::string_view game;
    std::string_view year;
    std::string_view copyright;
};

constexpr GameTags kGames[] = {
    {"AZEE", "NTR-AZEE-USA", "The Legend of Zelda: Phantom Hourglass", "2007-06-23", "Nintendo"},
    {"BKIE", "NTR-BKIE-USA", "The Legend of Zelda: Spirit Tracks", "2009-12-07", "Nintendo"},
    {"BKIP", "NTR-BKIP-EUR", "The Legend of Zelda: Spirit Tracks", "2009-12-07", "Nintendo"},
};

const GameTags* TagsOf(const std::string& game_code)
{
    for (const GameTags& tags : kGames)
    {
        if (tags.game_code == game_code)
        {
            return &tags;
        }
    }

    return nullptr;
}

// A sequence of the set.
struct Job
{
    uint32_t index = 0; // the analysis's order
    ntr::SequenceRef sequence;
    std::string label;
    std::optional<xsflib::rip::Timing> timing;
    double start_ms = 0;              // the time from the boot to the sequence's start on the DS
    std::optional<std::string> error; // why the mini's run on the DS fails
};

// The bytes of a mini of `sound` for `set`.
std::vector<uint8_t> MiniFile(const rip::SdatSet& set, uint32_t sound, const std::string& library,
                              xsflib::psf::Tags tags)
{
    Block block{set.sound_word, {}};
    xsflib::AppendLe32(block.data, sound);
    tags["_lib"] = library;

    return WriteTwosf(block, tags);
}

// The length and fade tags of a job. A sequence that ends gets its last sound: the timing counts the tail after it.
rip::LengthTags LengthTagsOf(const Job& job)
{
    const bool loops = job.timing->fade_ms > 0;
    const double tail = loops ? 0.0 : ntr::LengthRules().tail_ms;
    const double end = std::max(0.0, job.start_ms + static_cast<double>(job.timing->length_ms) - tail);

    return rip::SongLengthTags(static_cast<long long>(end + 0.5), loops);
}

// The set's sequences: those outside the sequence archives, by number, music only unless `effects` (`music`, from
// rip::MusicSequences). --only picks by label. A sequence that lacks data is left out with a warning. `archived`
// counts the sequences in sequence archives that --only picks, which the set leaves out.
std::vector<Job> SequenceJobs(const sdat::Archive& archive, const std::set<uint32_t>& music,
                              const xsflib::cli::RipOptions& o, bool effects, std::size_t& archived)
{
    std::vector<Job> jobs;
    archived = 0;
    for (const ntr::rip::Job& selected : ntr::rip::SelectSequences(archive, o.only, false))
    {
        if (selected.sequence.archived)
        {
            archived++;
            continue;
        }

        if (!music.contains(selected.sequence.index) && (!effects || o.bgm))
        {
            continue;
        }

        const std::vector<std::string> missing = ntr::rip::MissingData(archive, selected.sequence);
        if (!missing.empty())
        {
            std::fprintf(stderr, "warning: %s lacks %s and is left out\n", selected.label.c_str(), missing[0].c_str());
            continue;
        }

        jobs.push_back(
            {.index = static_cast<uint32_t>(jobs.size() + 1), .sequence = selected.sequence, .label = selected.label});
    }

    return jobs;
}

void ListSounds(const sdat::Archive& archive, const std::set<uint32_t>& music)
{
    for (const ntr::rip::Job& job : ntr::rip::SelectSequences(archive, std::regex(".*"), false))
    {
        const char* kind = job.sequence.archived                ? "in a sequence archive (not ripped)"
                           : music.contains(job.sequence.index) ? "music"
                                                                : "effect";
        std::printf("%s  %s\n", job.label.c_str(), kind);
    }

    std::fflush(stdout);
}

// Writes the minis and the playlist into `out_dir`, named "NN Title" in the jobs' order. `lengths` writes the length
// and fade tags. Returns 1 if a file can't be written, or 0.
int WriteMinis(const rip::SdatSet& set, const std::vector<Job>& jobs, const std::string& library,
               const xsflib::psf::Tags& common, bool lengths, const fs::path& out_dir)
{
    const int digits = std::max(2, int(std::to_string(jobs.size()).size()));
    std::set<std::string> taken;
    std::string playlist;
    for (std::size_t i = 0; i < jobs.size(); i++)
    {
        const Job& job = jobs[i];
        xsflib::psf::Tags tags = common;
        tags["title"] = job.label;
        std::string length;
        if (lengths && job.timing)
        {
            const rip::LengthTags times = LengthTagsOf(job);
            tags["length"] = length = times.length;
            tags["fade"] = times.fade;
        }

        char number[16];
        std::snprintf(number, sizeof number, "%0*zu ", digits, i + 1);
        const std::string stem = number + rip::FileTitle(job.label);
        std::string name = stem + ".mini2sf";
        for (int n = 2; !taken.insert(xsflib::AsciiLower(name)).second; ++n)
        {
            name = stem + "_" + std::to_string(n) + ".mini2sf";
        }

        const uint32_t sound = ntr::rip::SoundId(job.sequence);
        if (!xsflib::rip::WriteFile(out_dir / xsflib::PathFromUtf8(name), MiniFile(set, sound, library, tags)))
        {
            std::fprintf(stderr, "error: can't write %s\n", name.c_str());
            return 1;
        }

        std::fprintf(stderr, "%s%s%s\n", name.c_str(), length.empty() ? "" : "  length ", length.c_str());
        playlist += name + "\n";
    }

    if (!xsflib::rip::WriteFile(out_dir / kPlaylist, std::vector<uint8_t>(playlist.begin(), playlist.end())))
    {
        std::fprintf(stderr, "error: can't write %s\n", kPlaylist);
        return 1;
    }

    return 0;
}

// The files of the library: the files that the boot read, and the sound archive cut to the jobs' sequences, with the
// archive's files that the boot read.
std::map<uint32_t, std::vector<uint8_t>> LibraryFiles(const ntr::Rom& rom, const rip::SoundArchive& found,
                                                      const rip::HookState& hook, const std::vector<Job>& jobs)
{
    std::map<uint32_t, std::vector<uint8_t>> files;
    for (const uint32_t id : hook.files)
    {
        const std::span<const uint8_t> data = rom.FileData(id);
        files[id].assign(data.begin(), data.end());
    }

    std::vector<ntr::SequenceRef> sequences;
    for (const Job& job : jobs)
    {
        sequences.push_back(job.sequence);
    }

    const uint32_t archive_start = rom.Files()[found.file_id].start;
    const std::set<uint32_t> boot_files = rip::ArchiveFilesRead(hook, archive_start, found.archive);
    if (const std::optional<std::vector<uint8_t>> cut = rip::CutArchive(found.archive, sequences, boot_files))
    {
        files[found.file_id] = *cut;
    }
    else
    {
        std::fprintf(stderr, "the library keeps the whole sound archive: its layout isn't the SDK's tools' layout\n");
    }

    return files;
}

// Runs each job's mini on the DS: its checks confirm the set (rip::RunMini) and give the time to the sequence's start.
// The length comes from the model of the driver, with the global variables that the run sent. Returns the number of
// minis that fail.
std::size_t RunMinis(const ntr::Rom& rom, const rip::SdatSet& set, uint64_t hook_clock, uint32_t archive_file,
                     const sdat::Archive& archive, double seconds, unsigned threads, std::vector<Job>& jobs)
{
    const ntr::snd::Build build = ntr::snd::BuildOf(rom.Arm7());
    const auto analyze = [&](Job& job)
    {
        std::vector<uint8_t> image = set.image;
        xsflib::StoreLe32(&image[set.sound_word], ntr::rip::SoundId(job.sequence));
        const rip::MiniRun run = rip::RunMini(image, hook_clock, archive_file, job.sequence, seconds);
        job.error = run.error;
        job.start_ms = run.start_ms;
        if (job.error)
        {
            return false;
        }

        ntr::GameSettings settings;
        settings.variables = run.variables;
        try
        {
            job.timing = ntr::MeasureSequence(archive, job.sequence, settings, build);
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "warning: %s: %s\n", job.label.c_str(), e.what());
        }

        return true;
    };
    xsflib::cli::AnalysisSettings analysis;
    analysis.progress = "ran";
    xsflib::cli::AnalyzeLengths(jobs, threads, analyze, analysis);

    std::size_t failed = 0;
    for (const Job& job : jobs)
    {
        if (job.error)
        {
            std::fprintf(stderr, "%s: %s\n", job.label.c_str(), job.error->c_str());
            failed++;
        }
    }

    return failed;
}

// The tags of every mini: the options', or the game's.
xsflib::psf::Tags CommonTags(const std::string& input, const ntr::Rom& rom, const xsflib::cli::RipOptions& o)
{
    xsflib::cli::RipOptions tagged = o;
    const GameTags* game = TagsOf(rom.Header().game_code);
    if (tagged.game_name.empty())
    {
        tagged.game_name = game ? std::string(game->game) : xsflib::Utf8(fs::path(input).stem());
    }

    if (tagged.year.empty() && game)
    {
        tagged.year = game->year;
    }

    if (tagged.copyright.empty() && game)
    {
        tagged.copyright = game->copyright;
    }

    xsflib::psf::Tags common = xsflib::cli::RipTags(tagged, tagged.game_name, "2sf", nullptr);
    if (o.by.empty() || o.by == kToolName)
    {
        common.erase("2sfby");
    }

    return common;
}

// Writes the library, the minis and the playlist. Returns 1 if the rip fails, or 0.
int WriteSet(const std::string& input, const ntr::Rom& rom, const rip::SdatSet& set, const std::vector<Job>& jobs,
             const xsflib::cli::RipOptions& o, xsflib::cli::FileNameRegistries& names)
{
    const fs::path out_dir =
        o.out_dir.empty() ? xsflib::rip::DefaultOutputDir(input, "_2sf") : xsflib::PathFromUtf8(o.out_dir);
    std::error_code fs_error;
    fs::create_directories(out_dir, fs_error);
    if (fs_error)
    {
        std::fprintf(stderr, "error: can't create %s: %s\n", xsflib::Utf8(out_dir).c_str(), fs_error.message().c_str());
        return 1;
    }

    const GameTags* game = TagsOf(rom.Header().game_code);
    const std::string base = game ? std::string(game->product_code) : "NTR-" + rom.Header().game_code;
    xsflib::rip::FileNames& out_names = xsflib::cli::NamesIn(names, out_dir, ".2sflib", ".mini2sf");
    const std::optional<std::string> library =
        xsflib::cli::WriteLibrary(out_dir, out_names, base, WriteTwosf({0, set.image}, {}));
    if (!library || WriteMinis(set, jobs, *library, CommonTags(input, rom, o), o.lengths, out_dir) != 0)
    {
        return 1;
    }

    xsflib::rip::ReportOutput(out_dir);

    return 0;
}

// Rips `rom`, an image that a DS runs.
int RipDsImage(const std::string& input, const ntr::Rom& rom, const xsflib::cli::RipOptions& o, bool effects,
               xsflib::cli::FileNameRegistries& names)
{
    const auto refuse = [&input](const std::string& why)
    {
        std::fprintf(stderr, "error: %s: %s\n", input.c_str(), why.c_str());
        return 1;
    };

    if (!rip::SecureAreaDecrypted(rom))
    {
        return refuse("the image's secure area is encrypted. Decrypt the image, then rip it.");
    }

    std::set<uint32_t> every_file;
    for (const ntr::RomFile& file : rom.Files())
    {
        every_file.insert(file.id);
    }

    if (!rip::FindSoundArchive(rom, every_file))
    {
        return refuse("2sfrip doesn't support this game (game code " + rom.Header().game_code +
                      "): it has no sound archive (SDAT) with sequences at a file's start");
    }

    std::string error;
    const std::optional<rip::SdatCode> code = rip::FindSdatCode(rom, error);
    if (!code)
    {
        return refuse("2sfrip can't rip this game: " + error);
    }

    // The game's state at the hook, on the whole image, and the sound archive that the game opened by then.
    const rip::HookState hook = rip::BootToHook(rom, code->update);
    if (const std::optional<std::string> problem = rip::HookProblem(hook))
    {
        return refuse("2sfrip can't rip this game: " + *problem);
    }

    const std::optional<rip::SoundArchive> found = rip::FindSoundArchive(rom, hook.files);
    if (!found)
    {
        return refuse(
            "2sfrip can't rip this game: it reads no sound archive with sequences before its first call of "
            "the sound library's update");
    }

    const sdat::Archive& archive = found->archive;
    const std::set<uint32_t> music = rip::MusicSequences(archive);
    std::fprintf(stderr, "%s: the SDK's sound archive, %zu sequences\n", input.c_str(), archive.sequences.size());
    if (o.list)
    {
        ListSounds(archive, music);
        return 0;
    }

    std::size_t archived = 0;
    std::vector<Job> jobs = SequenceJobs(archive, music, o, effects, archived);
    if (archived > 0)
    {
        std::fprintf(stderr, "%zu sequences in sequence archives are left out: 2sfrip doesn't start them yet\n",
                     archived);
    }

    if (jobs.empty())
    {
        return refuse("no sequence to rip");
    }

    const rip::SdatStub stub = rip::BuildSdatStub(*code, hook.heap->address, ntr::rip::SoundId(jobs.front().sequence));
    const rip::SdatSet set = rip::MakeSdatSet(rom, *code, stub, LibraryFiles(rom, *found, hook, jobs));

    // The library's boot must read no file that the game's doesn't, and leave the same heap.
    const rip::HookState set_hook = rip::BootToHook(ntr::Rom::Parse(set.image), code->update);
    if (!set_hook.reached || !std::ranges::includes(hook.files, set_hook.files) || set_hook.heap != hook.heap)
    {
        return refuse("2sfrip can't rip this game: the library's boot differs from the game's at the hook");
    }

    std::fprintf(stderr, "the hook at %.3f s, the sound heap at %08X with %u bytes free, the library %zu bytes\n",
                 hook.seconds, hook.heap->address, hook.heap->free, set.image.size());

    const std::size_t failed =
        RunMinis(rom, set, set_hook.clock, found->file_id, archive, hook.seconds + kStartLimitSeconds, o.jobs, jobs);
    if (failed > 0)
    {
        return refuse("2sfrip can't rip this game: " + std::to_string(failed) + " of " + std::to_string(jobs.size()) +
                      " minis don't play their sequence on the DS");
    }

    // A sequence that plays no notes gets no mini.
    const std::size_t before = jobs.size();
    std::erase_if(jobs, [](const Job& job) { return job.timing && job.timing->silent; });
    if (jobs.size() < before)
    {
        std::fprintf(stderr, "%zu sequences play no notes and are left out\n", before - jobs.size());
    }

    if (jobs.empty())
    {
        return refuse("no sequence plays a note");
    }

    return WriteSet(input, rom, set, jobs, o, names);
}

} // namespace

int RipSdat(const std::string& input, const ntr::Rom& rom, const xsflib::cli::RipOptions& o, bool effects,
            xsflib::cli::FileNameRegistries& names)
{
    const auto unit = static_cast<ntr::UnitCode>(rom.Header().unit_code);
    if (unit == ntr::UnitCode::kDs)
    {
        return RipDsImage(input, rom, o, effects, names);
    }

    if (unit != ntr::UnitCode::kDsiEnhanced)
    {
        std::fprintf(stderr, "error: %s: the game runs only on the DSi. 2sfrip rips games that a DS runs.\n",
                     input.c_str());
        return 1;
    }

    // A DSi-enhanced game runs on a DS as a DS game. The set is made from the image with the unit code of a DS image.
    std::fprintf(stderr, "%s: a DSi-enhanced image. 2sfrip rips the game as a DS runs it.\n", input.c_str());
    std::vector<uint8_t> image(rom.Image().begin(), rom.Image().end());
    image[kUnitCodeField] = static_cast<uint8_t>(ntr::UnitCode::kDs);
    xsflib::StoreLe16(&image[kHeaderCrcField], ntr::Crc16(std::span(image).first(kHeaderCrcField)));

    return RipDsImage(input, ntr::Rom::Parse(image), o, effects, names);
}

} // namespace twosf::tools
