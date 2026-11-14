// SPDX-License-Identifier: MIT

// 2sfrip: rips a Nintendo DS game's music into a 2SF set: one .2sflib and one .mini2sf for each song. The library is
// the game's cartridge image, cut to what the music reads, with a stub that starts the song whose number the mini
// stores. The game's sound driver plays it.
//
// usage: 2sfrip [options] <image> [<image> ...]

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include "ntdlib/ntr/rom.h"
#include "ntdlib/universomo/formats.h"
#include "ntdlib/universomo/walk.h"
#include "xsflib/ascii.h"
#include "xsflib/binary.h"
#include "xsflib/cli/length_analysis.h"
#include "xsflib/cli/rip.h"
#include "xsflib/files.h"
#include "xsflib/rip/file_names.h"
#include "xsflib/rip/length.h"
#include "xsflib/rip/tool.h"

#include "rip/profile.h"
#include "rip/set.h"

namespace
{

namespace fs = std::filesystem;
namespace universomo = ntdlib::universomo;

constexpr char kUsage[] =
    "usage: 2sfrip [options] <image> [<image> ...]\n"
    "\n"
    "Rips each Nintendo DS cartridge image into a .2sflib, a .mini2sf for each song and a playlist,\n"
    "!playlist.m3u, in a folder next to it that's named after it with \"_2sf\" added. The library is the\n"
    "game's image, cut to what the music reads, with a stub that starts the song whose number the mini\n"
    "stores. The game's sound driver plays it.\n"
    "\n"
    "Games: Beat City (USA).\n"
    "\n"
    "options:\n"
    "  -o, --output DIR     put the rips in DIR\n"
    "  --list               list the songs, then stop\n"
    "  --only REGEX         rip only the songs whose name, such as \"Song 81\", or title matches\n"
    "  --effects            rip the sound effects too\n"
    "  --bgm                rip only the music (the default)\n"
    "  --no-length          omit the length and fade tags\n"
    "  --jobs N             the number of songs that run at once (default: the number of CPU threads)\n"
    "  --game NAME, --artist NAME, --year DATE, --copyright TEXT\n"
    "                       tags (for a supported game, they default to its tags; --game\n"
    "                       otherwise defaults to the image's name)\n"
    "  --by NAME            the 2sfby tag, written only when given\n"
    "  --version            show the version\n"
    "  -h, --help           show this help\n";

constexpr char kToolName[] = "2sfrip";
constexpr char kPlaylist[] = "!playlist.m3u";

// A looped song plays two passes of its loop, then fades over 10 s. A song that plays once fades over 1 s from its end.
constexpr long long kLoopFadeMs = 10000;
constexpr long long kEndFadeMs = 1000;

struct Options : xsflib::cli::RipOptions
{
    bool effects = false;
};

int ParseOption(const std::string& option, const std::function<std::string()>&, Options& o)
{
    if (option != "--effects")
    {
        return -1;
    }

    o.effects = true;

    return 0;
}

// A song to rip.
struct Job
{
    uint32_t index = 0; // the number in the mini's name
    uint32_t song = 0;
    universomo::SoundKind kind = universomo::SoundKind::kMusic;
    bool loops = true;
    std::string label;
    std::optional<xsflib::rip::Timing> timing;
};

std::string SongLabel(const twosf::rip::UniversomoProfile& profile, uint32_t song, universomo::SoundKind kind)
{
    if (song == profile.title_song)
    {
        return "Title Screen";
    }

    return (kind == universomo::SoundKind::kMusic ? "Song " : "Effect ") + std::to_string(song);
}

const char* KindName(universomo::SoundKind kind)
{
    switch (kind)
    {
    case universomo::SoundKind::kMusic:
        return "music";
    case universomo::SoundKind::kLoopedEffect:
        return "looped effect";
    case universomo::SoundKind::kEffect:
    default:
        return "effect";
    }
}

std::span<const uint8_t> FileOf(const ntdlib::ntr::Rom& rom, const std::string& path)
{
    const ntdlib::ntr::RomFile* file = rom.Find(path);
    if (file == nullptr)
    {
        throw xsflib::FormatError("the image has no file " + path);
    }

    return rom.FileData(file->id);
}

// The songs in the order of the set: the title screen's song, the other music by number, then the effects by number
// with --effects. --only picks among them by label or by "Song N".
std::vector<Job> SongJobs(const twosf::rip::UniversomoProfile& profile, const std::vector<universomo::Sound>& sounds,
                          const Options& o)
{
    std::vector<uint32_t> order;
    if (profile.title_song < sounds.size())
    {
        order.push_back(profile.title_song);
    }

    for (const bool music : {true, false})
    {
        for (uint32_t song = 0; song < sounds.size(); ++song)
        {
            if ((sounds[song].kind == universomo::SoundKind::kMusic) == music && song != profile.title_song)
            {
                order.push_back(song);
            }
        }
    }

    std::vector<Job> jobs;
    for (const uint32_t song : order)
    {
        const universomo::Sound& sound = sounds[song];
        const std::string label = SongLabel(profile, song, sound.kind);
        const std::string number = "Song " + std::to_string(song);
        const bool wanted = sound.kind == universomo::SoundKind::kMusic || (o.effects && !o.bgm);
        if (wanted && (std::regex_search(label, o.only) || std::regex_search(number, o.only)))
        {
            jobs.push_back({uint32_t(jobs.size() + 1), song, sound.kind, sound.loops, label, std::nullopt});
        }
    }

    return jobs;
}

// The length of song `song`: the boot to its first tick on the model of the DS, then two passes of its loop or its
// end, from the walk. The fade follows.
xsflib::rip::Timing SongTiming(const twosf::rip::SetImage& set, const twosf::rip::UniversomoProfile& profile,
                               const universomo::Song& song, const Job& job)
{
    const double first_tick_ms = twosf::rip::FirstTickMs(set, profile, job.song, job.loops);
    const universomo::SongLength length = universomo::MeasureSong(song, job.loops);
    const double tick_ms = 1000.0 / universomo::kTicksPerSecond;
    if (job.loops)
    {
        const double ms = first_tick_ms + double(length.intro + xsflib::rip::kLengthLoops * length.loop) * tick_ms;
        return {static_cast<long long>(ms + 0.5), kLoopFadeMs};
    }

    return {static_cast<long long>(first_tick_ms + double(length.intro) * tick_ms + 0.5), kEndFadeMs};
}

// Writes a mini for each job into `out_dir`, named "NN Title" in the jobs' order with two digits or as many as the
// count needs, and the playlist. Each mini gets `common`, its title, and its length and fade where the analysis gave
// them. Returns 1 if a file can't be written, or 0.
int WriteMinis(const twosf::rip::SetImage& set, const std::vector<Job>& jobs, const std::string& library,
               const xsflib::psf::Tags& common, const fs::path& out_dir)
{
    const int digits = std::max(2, int(std::to_string(jobs.size()).size()));
    std::set<std::string> taken;
    std::string playlist;
    for (const Job& job : jobs)
    {
        xsflib::psf::Tags tags = common;
        tags["title"] = job.label;
        std::string length;
        if (job.timing)
        {
            const twosf::rip::LengthTags times = twosf::rip::SongLengthTags(job.timing->length_ms, job.loops);
            tags["length"] = length = times.length;
            tags["fade"] = times.fade;
        }

        char number[16];
        std::snprintf(number, sizeof number, "%0*u ", digits, job.index);
        const std::string stem = number + twosf::rip::FileTitle(job.label);
        std::string name = stem + ".mini2sf";
        for (int n = 2; !taken.insert(xsflib::AsciiLower(name)).second; ++n)
        {
            name = stem + "_" + std::to_string(n) + ".mini2sf";
        }

        if (!xsflib::rip::WriteFile(out_dir / xsflib::PathFromUtf8(name),
                                    twosf::rip::MiniFile(set, job.song, job.loops, library, tags)))
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

int RipInput(const std::string& input, const Options& o, xsflib::cli::FileNameRegistries& names)
{
    std::vector<uint8_t> image;
    if (!xsflib::ReadFile(xsflib::PathFromUtf8(input), image))
    {
        std::fprintf(stderr, "error: can't read %s\n", input.c_str());
        return 1;
    }

    try
    {
        const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);
        std::string error;
        const twosf::rip::UniversomoProfile* profile = twosf::rip::FindProfile(rom, error);
        if (profile == nullptr)
        {
            std::fprintf(stderr, "error: %s: %s\n", input.c_str(), error.c_str());
            return 1;
        }

        const std::vector<universomo::Sound> sounds =
            universomo::ReadSoundTable(FileOf(rom, std::string(profile->sound_table)));
        std::fprintf(stderr, "%s: %s, Universomo's driver, %zu songs\n", input.c_str(),
                     std::string(profile->game).c_str(), sounds.size());
        if (o.list)
        {
            for (uint32_t song = 0; song < sounds.size(); ++song)
            {
                std::printf("%4u  %-13s  %s\n", song, KindName(sounds[song].kind),
                            SongLabel(*profile, song, sounds[song].kind).c_str());
            }

            std::fflush(stdout);
            return 0;
        }

        std::vector<Job> jobs = SongJobs(*profile, sounds, o);
        if (jobs.empty())
        {
            std::fprintf(stderr, "error: %s: no song to rip\n", input.c_str());
            return 1;
        }

        std::optional<twosf::rip::SetImage> set = twosf::rip::MakeSetImage(rom, *profile, error);
        if (!set)
        {
            std::fprintf(stderr, "error: %s: %s\n", input.c_str(), error.c_str());
            return 1;
        }

        // Each song's file is read before the analysis, so that a damaged one stops the rip.
        std::vector<universomo::Song> songs;
        for (const Job& job : jobs)
        {
            songs.push_back(universomo::ReadSong(FileOf(rom, universomo::SongPath(job.song).substr(1))));
        }

        const auto analyze = [&](Job& job)
        {
            if (!o.lengths)
            {
                return false;
            }

            try
            {
                job.timing = SongTiming(*set, *profile, songs[job.index - 1], job);
            }
            catch (const std::exception& e)
            {
                std::fprintf(stderr, "warning: %s: %s\n", job.label.c_str(), e.what());
            }

            return true;
        };
        xsflib::cli::AnalysisSettings analysis;
        analysis.progress = "ran";
        xsflib::cli::AnalyzeLengths(jobs, o.jobs, analyze, analysis);

        // The tags of every mini: the options' or the game's.
        Options tagged = o;
        if (tagged.game_name.empty())
        {
            tagged.game_name = profile->game;
        }

        if (tagged.year.empty())
        {
            tagged.year = profile->year;
        }

        if (tagged.copyright.empty())
        {
            tagged.copyright = profile->copyright;
        }

        xsflib::psf::Tags common = xsflib::cli::RipTags(tagged, tagged.game_name, "2sf", nullptr);
        if (o.by.empty() || o.by == kToolName)
        {
            common.erase("2sfby");
        }

        const fs::path out_dir =
            o.out_dir.empty() ? xsflib::rip::DefaultOutputDir(input, "_2sf") : xsflib::PathFromUtf8(o.out_dir);
        std::error_code fs_error;
        fs::create_directories(out_dir, fs_error);
        if (fs_error)
        {
            std::fprintf(stderr, "error: can't create %s: %s\n", xsflib::Utf8(out_dir).c_str(),
                         fs_error.message().c_str());
            return 1;
        }

        // The library, the minis and the playlist.
        xsflib::rip::FileNames& out_names = xsflib::cli::NamesIn(names, out_dir, ".2sflib", ".mini2sf");
        const std::string base(profile->product_code);
        const std::optional<std::string> library =
            xsflib::cli::WriteLibrary(out_dir, out_names, base, twosf::rip::LibraryFile(*set));
        if (!library)
        {
            return 1;
        }

        if (WriteMinis(*set, jobs, *library, common, out_dir) != 0)
        {
            return 1;
        }

        xsflib::rip::ReportOutput(out_dir);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s: %s\n", input.c_str(), e.what());
        return 1;
    }

    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    xsflib::cli::RipTool<Options> tool;
    tool.name = kToolName;
    tool.version = TWOSF_VERSION;
    tool.usage = kUsage;
    tool.modes = false;
    tool.archives = false;
    tool.parse_option = ParseOption;
    tool.rip_input = RipInput;

    return xsflib::cli::RunRipTool(tool, argc, argv);
}
