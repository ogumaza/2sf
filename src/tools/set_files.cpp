// SPDX-License-Identifier: MIT

#include "tools/set_files.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "xsflib/files.h"
#include "xsflib/rip/file_names.h"
#include "xsflib/rip/length_tags.h"
#include "xsflib/rip/tool.h"

#include "rip/library_name.h"

namespace twosf::tools
{

namespace
{

namespace fs = std::filesystem;

constexpr char kPlaylist[] = "!playlist.m3u";
constexpr char kToolName[] = "2sfrip";

// The tags of every mini: the options', and the image's name for a game that no option names.
xsflib::psf::Tags CommonTags(const std::string& input, const xsflib::cli::RipOptions& o)
{
    xsflib::cli::RipOptions tagged = o;
    if (tagged.game_name.empty())
    {
        tagged.game_name = xsflib::Utf8(xsflib::PathFromUtf8(input).stem());
    }

    xsflib::psf::Tags common = xsflib::cli::RipTags(tagged, tagged.game_name, "2sf", nullptr);
    if (o.by.empty() || o.by == kToolName)
    {
        common.erase("2sfby");
    }

    return common;
}

// Writes each of `libraries` into `out_dir`, named after the set's library `base` and the library's title. Returns
// their names, or nothing after printing an error.
std::optional<std::vector<std::string>> WriteSecondLibraries(std::span<const SecondLibrary> libraries,
                                                             const std::string& base, xsflib::rip::FileNames& names,
                                                             const fs::path& out_dir)
{
    std::vector<std::string> written;
    for (const SecondLibrary& library : libraries)
    {
        const std::optional<std::string> name = xsflib::cli::WriteLibrary(
            out_dir, names, base + "_" + rip::FileTitle(library.title), WriteTwosf(library.block, {}));
        if (!name)
        {
            return std::nullopt;
        }

        written.push_back(*name);
    }

    return written;
}

// A mini for xsflib::cli::WriteMinis: its number in the set, its title, its timing, its block, and the names of the
// libraries that it loads.
struct MiniJob
{
    uint32_t index = 0;
    std::string label;
    const xsflib::rip::Timing* timing = nullptr;
    const Block* block = nullptr;
    std::string library;
    std::optional<std::string> second_library;
};

// Writes the minis and the playlist into `out_dir` with xsflib::cli::WriteMinis, named "NN Title" in the order of
// `minis` by `names`. Each mini names the library of the same index in `library_of`. A mini with a second library names
// it from `libraries`, by its index. Returns 1 if a file can't be written, or 0.
int WriteMinis(std::span<const MiniSpec> minis, std::span<const std::string> library_of,
               const std::vector<std::string>& libraries, const xsflib::psf::Tags& common,
               xsflib::rip::FileNames& names, const fs::path& out_dir)
{
    std::vector<MiniJob> jobs;
    for (std::size_t i = 0; i < minis.size(); i++)
    {
        const MiniSpec& mini = minis[i];
        MiniJob& job = jobs.emplace_back();
        job.index = static_cast<uint32_t>(i + 1);
        job.label = mini.title;
        job.timing = mini.timing ? &*mini.timing : nullptr;
        job.block = &mini.block;
        job.library = library_of[i];
        if (mini.library)
        {
            job.second_library = libraries.at(*mini.library);
        }
    }

    std::vector<fs::path> paths;
    xsflib::cli::MiniSettings settings;
    settings.title_for_file = rip::FileTitle;
    settings.paths = &paths;
    const auto build = [](const MiniJob& job, xsflib::psf::Tags& tags)
    {
        tags["_lib"] = job.library;
        if (job.second_library)
        {
            tags["_lib2"] = *job.second_library;
        }

        return WriteTwosf(*job.block, tags);
    };
    const int written =
        xsflib::cli::WriteMinis(out_dir, names, jobs, common, xsflib::rip::LengthFrom::kFirstSound, settings, build);
    if (written < 0)
    {
        return 1;
    }

    std::string playlist;
    for (const fs::path& path : paths)
    {
        playlist += xsflib::Utf8(path.filename()) + "\n";
    }
    if (!xsflib::rip::WriteFile(out_dir / kPlaylist, std::vector<uint8_t>(playlist.begin(), playlist.end())))
    {
        std::fprintf(stderr, "error: can't write %s\n", kPlaylist);
        return 1;
    }

    return 0;
}

// Writes `files` into `out_dir`, each named after its title with its extension. The names come from the registry of the
// extension in `names` (xsflib::cli::NamesIn). The rips into one folder share it. Returns 1 if a file can't be written,
// or 0.
int WriteGameFiles(std::span<const GameFile> files, xsflib::cli::FileNameRegistries& names, const fs::path& out_dir)
{
    for (const GameFile& file : files)
    {
        const std::string name = xsflib::cli::NamesIn(names, out_dir, file.extension)
                                     .LibraryIn(out_dir, rip::FileTitle(file.title), file.bytes);
        if (!xsflib::rip::WriteFile(out_dir / xsflib::PathFromUtf8(name),
                                    std::vector<uint8_t>(file.bytes.begin(), file.bytes.end())))
        {
            std::fprintf(stderr, "error: can't write %s\n", name.c_str());
            return 1;
        }

        std::fprintf(stderr, "%s\n", name.c_str());
    }

    return 0;
}

} // namespace

int WriteSet(const std::string& input, const ntdlib::ntr::Rom& rom, const std::vector<uint8_t>& image,
             const SetParts& parts, const xsflib::cli::RipOptions& o, xsflib::cli::FileNameRegistries& names)
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

    // The libraries of the game and of its programs that minis name, then the minis and the playlist.
    const std::string base = rip::LibraryBase(rom.Header().game_code);
    xsflib::rip::FileNames& out_names = xsflib::cli::NamesIn(names, out_dir, ".2sflib", ".mini2sf");
    std::vector<MiniSpec> minis;
    std::vector<std::string> library_of;
    std::vector<std::string> libraries;
    const auto add_library = [&](const std::string& title, const std::vector<uint8_t>& bytes,
                                 std::span<const MiniSpec> its_minis, std::span<const SecondLibrary> its_libraries)
    {
        const std::optional<std::string> library =
            xsflib::cli::WriteLibrary(out_dir, out_names, title, WriteTwosf({0, bytes}, {}));
        const std::optional<std::vector<std::string>> second =
            library ? WriteSecondLibraries(its_libraries, base, out_names, out_dir) : std::nullopt;
        if (!second)
        {
            return false;
        }

        // The minis' second libraries, as indices into `libraries`.
        const std::size_t first = libraries.size();
        libraries.insert(libraries.end(), second->begin(), second->end());
        for (MiniSpec mini : its_minis)
        {
            if (mini.library)
            {
                *mini.library += first;
            }
            minis.push_back(std::move(mini));
        }
        library_of.insert(library_of.end(), its_minis.size(), *library);

        return true;
    };

    if (!parts.minis.empty() && !add_library(base, image, parts.minis, parts.libraries))
    {
        return 1;
    }

    for (const ProgramParts& program : parts.programs)
    {
        if (!program.minis.empty() &&
            !add_library(base + "_" + program.name, program.image, program.minis, program.libraries))
        {
            return 1;
        }
    }

    // A game that runs only on a DSi plays in DSi mode. `_dsi` marks such a set for other players (the specification).
    xsflib::psf::Tags common = CommonTags(input, o);
    if (ntdlib::ntr::RunsInDsiMode(rom.Header()))
    {
        common["_dsi"] = "1";
    }
    if (!minis.empty() && WriteMinis(minis, library_of, libraries, common, out_names, out_dir) != 0)
    {
        return 1;
    }

    if (WriteGameFiles(parts.files, names, out_dir) != 0)
    {
        return 1;
    }

    xsflib::rip::ReportOutput(out_dir);

    return 0;
}

} // namespace twosf::tools
