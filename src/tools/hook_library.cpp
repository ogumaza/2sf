// SPDX-License-Identifier: MIT

#include "tools/hook_library.h"

#include <cstddef>
#include <cstdio>

#include "ntdlib/ntr/machine.h"

#include "tools/messages.h"

namespace twosf::tools
{

namespace
{

namespace ntr = ntdlib::ntr;

// Boots `image` on each of `backups` to `hook`.
std::vector<rip::HookBoot> BootImage(const std::vector<uint8_t>& image, uint32_t hook,
                                     const std::vector<ntr::BackupMemory>& backups)
{
    const ntr::Rom rom = ntr::Rom::Parse(image);
    std::vector<rip::HookBoot> hooks;
    for (const ntr::BackupMemory& backup : backups)
    {
        hooks.push_back(rip::RunOnBoot(backup, [&] { return rip::BootTo(rom, hook, backup); }));
    }

    return hooks;
}

} // namespace

std::optional<HookLibrary> MakeHookLibrary(const ntr::Rom& rom, uint32_t hook, const std::set<uint32_t>& song_files,
                                           const ImageOf& image_of, double start_limit_seconds, std::string& error,
                                           const std::map<uint32_t, rip::FileRanges>& partial)
{
    // The game's state at the hook on each boot (rip/boots.h), on the whole image.
    const std::vector<ntr::BackupMemory> backups = rip::BootBackups(rom.Header().game_code);
    std::vector<rip::HookBoot> hooks;
    for (const ntr::BackupMemory& backup : backups)
    {
        const auto boot = [&]
        {
            return rip::BootTo(rom, hook, backup);
        };
        if (!rip::LetsStubRun(hooks.emplace_back(rip::RunOnBoot(backup, boot))))
        {
            error = "its boot doesn't reach the hook in a thread with interrupts on" + rip::OnBoot(backup);
            return std::nullopt;
        }
    }

    const rip::HookBoot& game = hooks.front();
    const rip::HookBoot merged = rip::MergedBoot(hooks);

    // The full image's boots must let the stub run.
    const std::vector<rip::HookBoot> full_hooks =
        BootImage(image_of(rip::WholeFiles(rom, rip::EveryFile(rom))), hook, backups);
    for (std::size_t i = 0; i < backups.size(); i++)
    {
        if (!rip::LetsStubRun(full_hooks[i]))
        {
            error =
                "with every file in the set's image, its boot doesn't reach the hook in a thread with interrupts on" +
                rip::OnBoot(backups[i]);
            return std::nullopt;
        }
    }

    // The library keeps what any boot of the game or of the full image reads before the hook, and the songs' files. A
    // file kept in part also keeps what the boots read of it.
    const rip::HookBoot full_merged = rip::MergedBoot(full_hooks);
    std::set<uint32_t> kept = rip::LibraryFiles(merged, full_merged, song_files);
    HookLibrary library;
    library.partial = partial;
    for (auto& [id, ranges] : library.partial)
    {
        kept.erase(id);
        for (const rip::HookBoot* boot : {&merged, &full_merged})
        {
            if (const auto reads = boot->reads.find(id); reads != boot->reads.end())
            {
                for (const auto& [first, end] : reads->second)
                {
                    rip::AddRange(ranges, first, end);
                }
            }
        }
    }

    library.image = image_of(rip::KeptFiles(rom, kept, library.partial));
    library.files = kept;

    // On each boot, the library's boot must match the full image's, the clock included (rip::DifferentBoot). Such a
    // library has no cut archive: it differs from the full image only in the files that it leaves out.
    const std::vector<rip::HookBoot> set_hooks = BootImage(library.image, hook, backups);
    if (const std::optional<std::size_t> unlike = rip::DifferentBoot(full_hooks, set_hooks))
    {
        error = "the library's boot differs at the hook from the boot with every file in the set's image" +
                rip::OnBoot(backups[*unlike]);
        return std::nullopt;
    }

    for (std::size_t i = 0; i < backups.size(); i++)
    {
        library.boots.push_back({backups[i], set_hooks[i].clock, hooks[i].seconds + start_limit_seconds});
    }

    std::fprintf(stderr, "the hook at %.3f s, the library %zu bytes\n", game.seconds, library.image.size());
    if (merged.files != game.files)
    {
        std::fprintf(stderr, "the library keeps %s that the game reads only with a backup memory\n",
                     CountOf(merged.files.size() - game.files.size(), "file", "files").c_str());
    }

    std::size_t set_files = 0; // the files that only the full image's boots read
    for (const uint32_t id : full_merged.files)
    {
        set_files += merged.files.contains(id) ? 0 : 1;
    }

    if (set_files > 0)
    {
        std::fprintf(stderr,
                     "the library keeps %s that the set's boot reads before the hook, and the game's boot doesn't\n",
                     CountOf(set_files, "file", "files").c_str());
    }

    return library;
}

} // namespace twosf::tools
