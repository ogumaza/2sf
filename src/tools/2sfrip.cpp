// SPDX-License-Identifier: MIT

// 2sfrip: rips a Nintendo DS game's music into a 2SF set: one .2sflib and one .mini2sf for each song. The library is
// the game's cartridge image, cut to what the music reads, with a stub that starts the song whose number the mini
// stores. The game's sound driver plays it.
//
// usage: 2sfrip [options] <image> [<image> ...]

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/ntr/machine.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/universomo/formats.h"
#include "ntdlib/universomo/walk.h"
#include "xsflib/binary.h"
#include "xsflib/cli/length_analysis.h"
#include "xsflib/cli/rip.h"
#include "xsflib/files.h"
#include "xsflib/rip/length.h"

#include "rip/boots.h"
#include "rip/library_name.h"
#include "rip/mini_render.h"
#include "rip/profile.h"
#include "rip/set.h"
#include "tools/dse_rip.h"
#include "tools/messages.h"
#include "tools/sdat_rip.h"
#include "tools/set_files.h"
#include "tools/spd_rip.h"

namespace
{

namespace universomo = ntdlib::universomo;

constexpr char kUsage[] =
    "usage: 2sfrip [options] <image> [<image> ...]\n"
    "\n"
    "Rips each Nintendo DS cartridge image into a .2sflib, a .mini2sf for each song and a playlist,\n"
    "!playlist.m3u, in a folder next to it that's named after it with \"_2sf\" added. The library is the\n"
    "game's image, cut to what the music reads, with a stub that starts the song whose number the mini\n"
    "stores. The game's sound driver plays it.\n"
    "\n"
    "Games: Beat City (USA), the three Pokémon Mystery Dungeon: Explorers games (USA), Super Princess\n"
    "Peach (USA), and games on the SDK's sound archives (SDAT) whose sound routines 2sfrip finds, such\n"
    "as The Legend of Zelda: Phantom Hourglass (USA), Spirit Tracks (USA, Europe), Four Swords\n"
    "Anniversary Edition (USA, Europe), Pokémon Mystery Dungeon: Blue Rescue Team (USA), Pokémon\n"
    "Diamond, Pearl, Platinum, HeartGold, SoulSilver, Black, White, Black 2 and White 2 (USA) and Mario\n"
    "Kart DS (USA). 2sfrip runs every mini of a game on the SDK's archives on its model of the DS, and\n"
    "refuses the game when one fails. Such a set contains the archive's streams too: each stream file\n"
    "goes in a library of its own. A game that runs only on a DSi runs in the model's DSi mode, and its\n"
    "set plays only in 2sf's player.\n"
    "\n"
    "options:\n"
    "  -o, --output DIR     put the rips in DIR\n"
    "  --list               list the songs, then stop\n"
    "  --only REGEX         rip only the songs whose name, such as \"Song 81\" or \"SEQ_12\", or title matches\n"
    "  --effects            rip the sound effects too\n"
    "  --stream-files       write the game's stream files (.strm) beside the set, in place of stream minis\n"
    "  --bgm                rip only the music (the default)\n"
    "  --no-length          omit the length and fade tags\n"
    "  --jobs N             the number of songs that run at once (default: the number of CPU threads)\n"
    "  --game NAME, --artist NAME, --year DATE, --copyright TEXT\n"
    "                       tags (--game defaults to the image's name)\n"
    "  --by NAME            the 2sfby tag, written only when given\n"
    "  --version            show the version\n"
    "  -h, --help           show this help\n";

constexpr char kToolName[] = "2sfrip";

struct Options : xsflib::cli::RipOptions
{
    bool effects = false;
    bool stream_files = false;
};

int ParseOption(const std::string& option, const std::function<std::string()>&, Options& o)
{
    if (option == "--effects")
    {
        o.effects = true;
        return 0;
    }

    if (option == "--stream-files")
    {
        o.stream_files = true;
        return 0;
    }

    return -1;
}

// A song to rip.
struct Job
{
    uint32_t index = 0; // the job's place in the list of jobs, from 1: its song file is songs[index - 1]
    uint32_t song = 0;
    universomo::SoundKind kind = universomo::SoundKind::kMusic;
    bool loops = true;
    std::string label;
    std::optional<xsflib::rip::Timing> timing;
};

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

// The songs in the order of the set: the music by number, then the effects by number with --effects. --only picks among
// them by label or by "Song N".
std::vector<Job> SongJobs(const std::vector<universomo::Sound>& sounds, const Options& o)
{
    std::vector<uint32_t> order;
    for (const bool music : {true, false})
    {
        for (uint32_t song = 0; song < sounds.size(); ++song)
        {
            if ((sounds[song].kind == universomo::SoundKind::kMusic) == music)
            {
                order.push_back(song);
            }
        }
    }

    std::vector<Job> jobs;
    for (const uint32_t song : order)
    {
        const universomo::Sound& sound = sounds[song];
        const std::string label = twosf::rip::SongLabel(song, sound.kind == universomo::SoundKind::kMusic);
        const std::string number = "Song " + std::to_string(song);
        const bool wanted = sound.kind == universomo::SoundKind::kMusic || (o.effects && !o.bgm);
        if (wanted && (std::regex_search(label, o.only) || std::regex_search(number, o.only)))
        {
            jobs.push_back({uint32_t(jobs.size() + 1), song, sound.kind, sound.loops, label, std::nullopt});
        }
    }

    return jobs;
}

// The timing of song `song` from the boot: the boot to its first tick on the model of the DS, then its intro and two
// passes of its loop, or its end, from the walk. A loop fades over xsflib's fade. The boot has no backup memory. The
// boot with each of `backups` after the first must reach the first tick too. The first sounds, and the end of a song
// that ends, come from the render of 2sf's player (twosf::rip::TimeFromPlayer).
xsflib::rip::Timing SongTiming(const twosf::rip::SetImage& set, const twosf::rip::UniversomoProfile& profile,
                               const universomo::Song& song, const Job& job,
                               std::span<const ntdlib::ntr::BackupMemory> backups)
{
    for (const ntdlib::ntr::BackupMemory& backup : backups.subspan(1))
    {
        const auto first_tick = [&]
        {
            return twosf::rip::FirstTickMs(set, profile, job.song, job.loops, backup);
        };
        twosf::rip::RunOnBoot(backup, first_tick);
    }

    const double first_tick_ms = twosf::rip::FirstTickMs(set, profile, job.song, job.loops);
    const universomo::SongLength length = universomo::MeasureSong(song, job.loops);
    const auto ms = [](uint64_t ticks, double from)
    {
        return static_cast<long long>(from + double(ticks) * 1000.0 / universomo::kTicksPerSecond + 0.5);
    };

    if (!job.loops)
    {
        return {.length_ms = ms(length.intro, first_tick_ms)};
    }

    const xsflib::rip::LoopPass pass{.start_ms = ms(length.intro, first_tick_ms), .length_ms = ms(length.loop, 0.0)};

    return {.length_ms = pass.start_ms + xsflib::rip::kLengthLoops * pass.length_ms,
            .fade_ms = static_cast<long long>(xsflib::rip::LengthRules().fade_ms),
            .loop = pass};
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
        if (!twosf::rip::SecureAreaDecrypted(rom))
        {
            return twosf::tools::Refuse(input, "the image's secure area is encrypted. Decrypt the image, then rip it.");
        }

        const std::vector<std::string_view> universomo_games = twosf::rip::SupportedGames();
        const bool dse = twosf::tools::IsDseGame(rom);
        const bool spd = twosf::tools::IsSpdGame(rom);
        if (!dse && !spd && std::ranges::find(universomo_games, rom.Header().game_code) == universomo_games.end())
        {
            return twosf::tools::RipSdat(input, rom, o, {.effects = o.effects, .stream_files = o.stream_files}, names);
        }

        // Only a game on the SDK's sound archive player has stream files to write.
        if (o.stream_files)
        {
            std::fprintf(stderr, "warning: --stream-files writes nothing: the rip has no streams\n");
        }

        if (dse)
        {
            return twosf::tools::RipDse(input, rom, o, o.effects, names);
        }

        if (spd)
        {
            return twosf::tools::RipSpd(input, rom, o, o.effects, names);
        }

        std::string error;
        const twosf::rip::UniversomoProfile* profile = twosf::rip::FindProfile(rom, error);
        if (profile == nullptr)
        {
            return twosf::tools::Refuse(input, error);
        }

        const std::vector<universomo::Sound> sounds =
            universomo::ReadSoundTable(FileOf(rom, std::string(profile->sound_table)));
        std::fprintf(stderr, "%s: %s, Universomo's driver, %zu songs\n", input.c_str(),
                     twosf::rip::LibraryBase(profile->game_code).c_str(), sounds.size());
        if (o.list)
        {
            for (uint32_t song = 0; song < sounds.size(); ++song)
            {
                std::printf("%4u  %-13s  %s\n", song, KindName(sounds[song].kind),
                            twosf::rip::SongLabel(song, sounds[song].kind == universomo::SoundKind::kMusic).c_str());
            }

            std::fflush(stdout);
            return 0;
        }

        std::vector<Job> jobs = SongJobs(sounds, o);
        if (jobs.empty())
        {
            return twosf::tools::Refuse(input, "no song to rip");
        }

        // The game's boot as a set runs it: the set's image with every file. The library keeps the files that any of
        // its boots (rip/boots.h) reads before the main loop. On each boot, the library's boot must then match that
        // boot exactly (rip::DifferentBoot). The two images differ only in the files that the library leaves out.
        const std::optional<twosf::rip::SetImage> whole =
            twosf::rip::MakeSetImage(rom, *profile, twosf::rip::EveryFile(rom), error);
        if (!whole)
        {
            return twosf::tools::Refuse(input, error);
        }

        const ntdlib::ntr::Rom whole_rom = ntdlib::ntr::Rom::Parse(whole->image);
        const std::vector<ntdlib::ntr::BackupMemory> backups = twosf::rip::BootBackups(rom.Header().game_code);
        std::vector<twosf::rip::MainLoopBoot> boots;
        std::set<uint32_t> boot_files;
        for (const ntdlib::ntr::BackupMemory& backup : backups)
        {
            const auto boot_whole = [&]
            {
                return twosf::rip::BootToMainLoop(whole_rom, *profile, backup);
            };
            const twosf::rip::MainLoopBoot& boot = boots.emplace_back(twosf::rip::RunOnBoot(backup, boot_whole));
            if (!boot.reached)
            {
                return twosf::tools::Refuse(
                    input, "the boot doesn't reach the main loop within 10 seconds" + twosf::rip::OnBoot(backup));
            }
            boot_files.insert(boot.files.begin(), boot.files.end());
        }

        std::optional<twosf::rip::SetImage> set = twosf::rip::MakeSetImage(rom, *profile, boot_files, error);
        if (!set)
        {
            return twosf::tools::Refuse(input, error);
        }

        const ntdlib::ntr::Rom set_rom = ntdlib::ntr::Rom::Parse(set->image);
        std::vector<twosf::rip::MainLoopBoot> set_boots;
        for (const ntdlib::ntr::BackupMemory& backup : backups)
        {
            const auto boot_set = [&]
            {
                return twosf::rip::BootToMainLoop(set_rom, *profile, backup);
            };
            set_boots.push_back(twosf::rip::RunOnBoot(backup, boot_set));
        }

        if (const std::optional<std::size_t> unlike = twosf::rip::DifferentBoot(boots, set_boots))
        {
            return twosf::tools::Refuse(input, "the library's boot differs from the game's at the main loop" +
                                                   twosf::rip::OnBoot(backups[*unlike]));
        }

        // Each song's file is read before the analysis. A damaged one then stops the rip.
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
                job.timing = SongTiming(*set, *profile, songs[job.index - 1], job, backups);
            }
            catch (const std::exception& e)
            {
                std::fprintf(stderr, "warning: %s: %s\n", job.label.c_str(), e.what());
                return true;
            }

            twosf::rip::SetImage mini = *set;
            twosf::rip::SetSong(mini, job.song, job.loops);
            twosf::rip::PrintRenderReport(
                job.label, twosf::rip::TimeFromPlayer(std::move(mini.image), 0.0, job.label, *job.timing));

            return true;
        };
        xsflib::cli::AnalysisSettings analysis;
        analysis.progress = "ran";
        xsflib::cli::AnalyzeLengths(jobs, o.jobs, analyze, analysis);

        // The minis, by the order of the jobs.
        std::vector<twosf::tools::MiniSpec> minis;
        for (const Job& job : jobs)
        {
            minis.push_back(
                {.title = job.label, .block = twosf::rip::MiniBlock(*set, job.song, job.loops), .timing = job.timing});
        }

        return twosf::tools::WriteSet(input, rom, set->image, {.minis = std::move(minis)}, o, names);
    }
    catch (const std::exception& e)
    {
        return twosf::tools::Refuse(input, e.what());
    }
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
