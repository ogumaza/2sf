// SPDX-License-Identifier: MIT

#include "tools/dse_rip.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/dse/archive_source.h"
#include "ntdlib/dse/build.h"
#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/files.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/dse/game_starts.h"
#include "ntdlib/dse/length_model.h"
#include "ntdlib/dse/rip.h"
#include "ntdlib/dse/sets.h"
#include "ntdlib/dse/ticks.h"
#include "ntdlib/ntr/code.h"
#include "ntdlib/ntr/machine.h"
#include "xsflib/bytes.h"
#include "xsflib/cli/length_analysis.h"
#include "xsflib/cli/parallel.h"

#include "rip/boots.h"
#include "rip/dse_set.h"
#include "rip/mini_render.h"
#include "rip/profile.h"
#include "rip/sadl.h"
#include "rip/stream_set.h"
#include "tools/hook_library.h"
#include "tools/messages.h"
#include "tools/set_files.h"

namespace twosf::tools
{

namespace
{

namespace dse = ntdlib::dse;
namespace ntr = ntdlib::ntr;

// The time after the hook within which a mini's song must start.
constexpr double kStartLimitSeconds = 10.0;

// A song of the set.
struct Job
{
    uint32_t index = 0; // the analysis's order
    dse::SequenceRef sequence;
    std::string label;
    std::string state; // the suffix of the state that the mini plays (ntdlib/dse/game_starts.h)
    rip::DseStart start;
    dse::SequenceSet files;                                 // indices into the DSE files
    int tick_microseconds = dse::Driver::kTickMicroseconds; // DSE's tick in the game's settings (dse::TickOf)
    std::optional<xsflib::rip::Timing> timing;
    double start_ms = 0;              // the time from the boot to DSE's start of the song on the DS
    std::vector<int> volumes;         // the volume of DSE's start of the song on each boot, in the boots' order
    std::optional<std::string> error; // why the mini's run on the DS fails
};

// A stream of the set (rip::DseStream): its title, the song's label with the program's suffix, its header and what its
// runs give.
struct StreamJob
{
    rip::DseStream stream;
    std::string label;
    rip::SadlHeader header;
    std::optional<xsflib::rip::Timing> timing;
    double start_ms = 0;
    std::optional<std::string> error;
};

// The game's files, then the DSE files among them (dse::FindFiles), and the states of its songs.
struct GameFiles
{
    std::vector<dse::NamedFile> named;
    std::vector<std::vector<uint8_t>> storage;
    std::vector<dse::NamedFile> dse;
    std::optional<dse::GameStarts> starts;
};

std::unique_ptr<GameFiles> ReadGameFiles(const ntr::Rom& rom)
{
    auto files = std::make_unique<GameFiles>();
    for (const ntr::RomFile& file : rom.Files())
    {
        if (!file.path.empty())
        {
            files->named.push_back({file.path, rom.FileData(file.id)});
        }
    }
    files->dse = dse::FindFiles(dse::Decompressed(files->named, files->storage));
    files->starts.emplace(rom);

    return files;
}

std::string StartName(const rip::DseStart& start)
{
    if (!start.name.empty())
    {
        return "BGM " + start.name;
    }

    const std::string level = start.level != rip::kDseFullLevel ? " at level " + std::to_string(start.level) : "";
    const std::string change = start.change ? ", then the volume " + std::to_string(start.change->volume) + " over " +
                                                  std::to_string(start.change->frames) + " frames"
                                            : "";

    return (start.me ? "ME " : "BGM ") + std::to_string(start.number) + level + change;
}

// The starts of `start`'s song in each of its states (`starts`), with their labels' suffixes. Music that the game
// starts at a level (ntdlib/dse/game_starts.h) takes the level of each state, and music that a change follows in a game
// whose stub makes it (rip::DseCode::change_calls) the change. The first state's label is the song's.
std::vector<std::pair<std::string, rip::DseStart>> StatesOf(const dse::GameStarts& starts, const rip::DseCode& code,
                                                            const rip::DseStart& start, const std::string& path)
{
    const bool level = rip::DseTakesLevel(code);
    const bool change = code.change_calls.front().routine != 0;
    if (start.me || (!level && !change) || !start.name.empty())
    {
        return {{"", start}};
    }

    std::vector<std::pair<std::string, rip::DseStart>> states;
    for (const dse::SongState& state : starts.StatesOf(path))
    {
        rip::DseStart at = start;
        if (level)
        {
            at.level = static_cast<uint32_t>(state.level);
        }
        if (change)
        {
            at.change = state.change;
        }
        states.emplace_back(state.suffix, at);
    }

    return states;
}

// The ROM file of each DSE file and its range in it: a file of its own, or a range of a file that holds it, such as a
// pack. Nothing for a file whose bytes are outside the image, such as one that FindFiles decompressed.
struct FileRange
{
    uint32_t id = 0;
    uint32_t first = 0;
    uint32_t end = 0;
    bool whole = false;
};

std::optional<FileRange> RangeOf(const ntr::Rom& rom, std::span<const uint8_t> bytes)
{
    for (const ntr::RomFile& file : rom.Files())
    {
        const std::span<const uint8_t> data = rom.FileData(file.id);
        if (!data.empty() && bytes.data() >= data.data() && bytes.data() + bytes.size() <= data.data() + data.size())
        {
            const auto first = static_cast<uint32_t>(bytes.data() - data.data());
            return FileRange{.id = file.id,
                             .first = first,
                             .end = first + static_cast<uint32_t>(bytes.size()),
                             .whole = bytes.size() == data.size()};
        }
    }

    return std::nullopt;
}

// How the set starts each song: by number from the song's path (rip::DseStartOf), from the game's table of songs, from
// the entry of a pack that contains it or from the game's number of the pack's member that contains it, or for a game
// that starts a song by name, by the name of its offset in the pack's index.
class Starts
{
public:
    Starts(const ntr::Rom& rom, const rip::DseCode& code, const rip::DseLimits& limits)
        : rom_(rom), code_(code), limits_(limits)
    {
        if (code.start == rip::DseStartKind::kCalls && code.index_kind == rip::DsePackIndex::kMembers)
        {
            pack_ = code.pack != nullptr ? rom.Find(code.pack) : nullptr;
            members_ = rip::DseMemberNumbers(rom, code);
        }
        else if (code.start == rip::DseStartKind::kCalls && code.index_kind == rip::DsePackIndex::kDarc)
        {
            pack_ = code.pack != nullptr ? rom.Find(code.pack) : nullptr;
        }
        else if (code.start == rip::DseStartKind::kCalls && code.index_kind == rip::DsePackIndex::kRom)
        {
            pack_ = code.pack != nullptr ? rom.Find(code.pack) : nullptr;
            if (pack_ != nullptr && code.pack_index != nullptr)
            {
                rom_entries_ = rip::DseRomEntries(rom.FileData(pack_->id), code.pack_index);
            }
        }
        else if (code.start == rip::DseStartKind::kCalls && code.song_table != 0)
        {
            table_ = rip::DseTableStarts(rom, code);
        }

        if (code.start == rip::DseStartKind::kCalls && code.index_kind == rip::DsePackIndex::kOffsets)
        {
            for (const auto& [pack, index, me] :
                 {std::tuple(code.pack, code.pack_index, false), std::tuple(code.me_pack, code.me_pack_index, true)})
            {
                const ntr::RomFile* const pack_file = pack != nullptr ? rom.Find(pack) : nullptr;
                const ntr::RomFile* const index_file = index != nullptr ? rom.Find(index) : nullptr;
                if (pack_file != nullptr && index_file != nullptr)
                {
                    numbered_packs_.push_back({pack_file->id, index_file->id, me});
                }
            }
        }

        if (code.start == rip::DseStartKind::kName && code.pack != nullptr && code.pack_index != nullptr)
        {
            index_ = rom.Find(code.pack_index);
            pack_ = rom.Find(code.pack);
            if (index_ != nullptr && pack_ != nullptr && code.index_kind == rip::DsePackIndex::kNames)
            {
                names_ = rip::DsePackStarts(rom.FileData(index_->id));
            }
            else if (index_ != nullptr && pack_ != nullptr)
            {
                prefixes_ = SongPrefixes(rom);
            }
        }
    }

    std::optional<rip::DseStart> Of(const dse::NamedFile& song) const
    {
        if (code_.start == rip::DseStartKind::kCalls && code_.index_kind == rip::DsePackIndex::kMembers)
        {
            return NumberOfMember(song);
        }

        if (code_.start == rip::DseStartKind::kCalls && code_.index_kind == rip::DsePackIndex::kRom)
        {
            return NumberInRom(song);
        }

        if (code_.start == rip::DseStartKind::kCalls && code_.index_kind == rip::DsePackIndex::kDarc)
        {
            return NumberInDarc(song);
        }

        if (code_.start == rip::DseStartKind::kCalls && code_.song_table != 0)
        {
            const auto start = table_.find(song.path);
            return start != table_.end() ? std::optional(start->second) : std::nullopt;
        }

        if (code_.start == rip::DseStartKind::kCalls && code_.index_kind == rip::DsePackIndex::kOffsets)
        {
            return NumberInPack(song);
        }

        if (code_.start != rip::DseStartKind::kName)
        {
            return rip::DseStartOf(code_, limits_, song.path);
        }

        const std::optional<FileRange> range = RangeOf(rom_, song.bytes);
        if (code_.pack == nullptr)
        {
            return range && range->whole ? NameOf(rom_.Files()[range->id].path) : std::nullopt;
        }

        if (pack_ == nullptr || index_ == nullptr || !range || range->id != pack_->id)
        {
            return std::nullopt;
        }

        if (code_.index_kind != rip::DsePackIndex::kNames)
        {
            return HashedStart(song.path, range->first);
        }

        const auto name = names_.find(range->first);
        if (name == names_.end())
        {
            return std::nullopt;
        }

        return name->second;
    }

private:
    // A pack whose index gives each entry's offset, and whether its songs are short pieces.
    struct NumberedPack
    {
        uint32_t pack = 0;
        uint32_t index = 0;
        bool me = false;
    };

    // The start of a song that a member of the pack contains: the number that the game's table gives the member.
    std::optional<rip::DseStart> NumberOfMember(const dse::NamedFile& song) const
    {
        const std::optional<FileRange> range = RangeOf(rom_, song.bytes);
        if (pack_ == nullptr || !range || range->id != pack_->id)
        {
            return std::nullopt;
        }

        const std::optional<uint32_t> member = rip::DseMemberAt(rom_.FileData(pack_->id), range->first);
        const auto number = member ? members_.find(*member) : members_.end();
        if (number == members_.end())
        {
            return std::nullopt;
        }

        return rip::DseStart{.number = number->second};
    }

    // The start of a song inside a file of the pack's folder `code.pack_index`, where the pack is a cartridge image:
    // the number that the file's name gives.
    std::optional<rip::DseStart> NumberInRom(const dse::NamedFile& song) const
    {
        const std::optional<FileRange> range = RangeOf(rom_, song.bytes);
        if (pack_ == nullptr || !range || range->id != pack_->id)
        {
            return std::nullopt;
        }

        const auto contains = [&range](const rip::DseRomEntry& entry)
        {
            return entry.first <= range->first && range->end <= entry.end;
        };
        const auto entry = std::ranges::find_if(rom_entries_, contains);
        const std::optional<uint32_t> number =
            entry != rom_entries_.end() ? rip::DseNameNumberOf(code_, entry->name) : std::nullopt;
        if (!number)
        {
            return std::nullopt;
        }

        return rip::DseStart{.number = *number};
    }

    // The start of a song inside a member of the pack, a DARC archive: the member's number less the first song's.
    std::optional<rip::DseStart> NumberInDarc(const dse::NamedFile& song) const
    {
        const std::optional<FileRange> range = RangeOf(rom_, song.bytes);
        if (pack_ == nullptr || !range || range->id != pack_->id)
        {
            return std::nullopt;
        }

        const std::optional<uint32_t> member = rip::DseDarcMemberAt(rom_.FileData(pack_->id), range->first);
        if (!member || *member < code_.first_member)
        {
            return std::nullopt;
        }

        return rip::DseStart{.number = *member - code_.first_member};
    }

    // The start of a song inside a pack whose index of offsets gives the entry: the entry's number.
    std::optional<rip::DseStart> NumberInPack(const dse::NamedFile& song) const
    {
        const std::optional<FileRange> range = RangeOf(rom_, song.bytes);
        if (!range)
        {
            return std::nullopt;
        }

        for (const NumberedPack& pack : numbered_packs_)
        {
            if (range->id != pack.pack)
            {
                continue;
            }

            const std::optional<uint32_t> entry = rip::DseOffsetEntry(rom_.FileData(pack.index), range->first);
            if (!entry)
            {
                return std::nullopt;
            }

            return rip::DseStart{.number = *entry, .me = pack.me};
        }

        return std::nullopt;
    }

    // The start of the song at `offset` in the pack, by an index by CRC-32: a name whose entry, NAME.SMW, contains the
    // song after its header. The index contains no names, and the header of some songs gives another name than the game
    // uses. The names tried: the song's file's name, that name without a last part after '_', then each prefix of
    // letters (and '_') of that name and of the names of songs in the ARM9 binary, followed by a number of 2 or 3
    // digits. Any name that reaches the song's entry starts the song as the game's name does.
    std::optional<rip::DseStart> HashedStart(const std::string& path, uint32_t offset) const
    {
        const std::optional<rip::DseStart> file = NameOf(path);
        if (!file)
        {
            return std::nullopt;
        }

        std::vector<std::string> names = {file->name};
        if (const std::size_t cut = file->name.rfind('_'); cut != std::string::npos && cut > 0)
        {
            names.push_back(file->name.substr(0, cut));
        }

        std::vector<std::string> prefixes = {PrefixOf(file->name)};
        for (const std::string& prefix : prefixes_)
        {
            if (prefix != prefixes.front())
            {
                prefixes.push_back(prefix);
            }
        }

        for (const std::string& prefix : prefixes)
        {
            if (prefix.empty())
            {
                continue;
            }

            for (int number = 0; number < 1000; number++)
            {
                for (const int width : {2, 3})
                {
                    char digits[8];
                    std::snprintf(digits, sizeof digits, "%0*d", width, number);
                    names.push_back(prefix + digits);
                }
            }
        }

        const std::span<const uint8_t> pack = rom_.FileData(pack_->id);
        const std::span<const uint8_t> index = rom_.FileData(index_->id);
        for (const std::string& name : names)
        {
            const std::optional<uint32_t> entry = rip::DsePackEntry(code_.index_kind, index, name + ".SMW");
            const std::string full = name + code_.name_suffix;
            if (entry && *entry + 4 <= pack.size() && *entry + xsflib::LoadLe32(&pack[*entry]) == offset &&
                full.size() < rip::kDseNameBytes)
            {
                return rip::DseStart{.name = full};
            }
        }

        return std::nullopt;
    }

    // The letters and '_' that start `name`.
    static std::string PrefixOf(const std::string& name)
    {
        const auto letter = [](char c)
        {
            return (c >= 'A' && c <= 'Z') || c == '_';
        };

        return {name.begin(), std::ranges::find_if_not(name, letter)};
    }

    // The prefixes of the names of songs, NAME.SMD or NAME.SMW, in the ARM9 binary of `rom`.
    static std::set<std::string> SongPrefixes(const ntr::Rom& rom)
    {
        const ntr::LoadedCode arm9 = ntr::Arm9Code(rom);
        const std::string text(arm9.bytes.begin(), arm9.bytes.end());
        static const std::regex kSong("([A-Z]+_?)[0-9]+\\.SM[DW]");
        std::set<std::string> prefixes;
        for (auto match = std::sregex_iterator(text.begin(), text.end(), kSong); match != std::sregex_iterator();
             ++match)
        {
            prefixes.insert((*match)[1].str());
        }

        return prefixes;
    }

    // The start of a song that is a file of its own: its file's name without the extension.
    static std::optional<rip::DseStart> NameOf(const std::string& path)
    {
        const std::string file = path.substr(path.rfind('/') + 1);
        std::string name = file.substr(0, file.rfind('.'));
        if (name.empty() || name.size() >= rip::kDseNameBytes)
        {
            return std::nullopt;
        }

        return rip::DseStart{.name = std::move(name)};
    }

    const ntr::Rom& rom_;
    const rip::DseCode& code_;
    rip::DseLimits limits_;
    const ntr::RomFile* index_ = nullptr;
    const ntr::RomFile* pack_ = nullptr;
    std::set<std::string> prefixes_;
    std::map<uint32_t, rip::DseStart> names_;
    std::map<std::string, rip::DseStart> table_;
    std::vector<NumberedPack> numbered_packs_;
    std::map<uint32_t, uint32_t> members_;
    std::vector<rip::DseRomEntry> rom_entries_;
};

// The DSE file of `files` at `path`.
const dse::NamedFile* FileAt(const GameFiles& files, const std::string& path)
{
    const auto found = std::ranges::find(files.dse, path, &dse::NamedFile::path);

    return found != files.dse.end() ? &*found : nullptr;
}

// Whether the game never starts `start`: music below the first number that the game's start of music takes.
bool IsUnused(const rip::DseStart& start, const rip::DseLimits& limits)
{
    return !start.me && start.number < limits.first_bgm;
}

// A program of the game whose songs a set rips: the game, or a second program (rip::DseProgram). A second program's rip
// boots the image with its patches (rip::PatchedImage). Its songs' titles end with "_" and its name: its songs can have
// the labels of the game's songs.
struct Program
{
    std::string name; // empty for the game
    rip::DseCode code;
    std::vector<uint8_t> image;     // the image with the program's patches, or empty for none
    std::optional<ntr::Rom> parsed; // `image` parsed
    const ntr::Rom* rom = nullptr;  // the image that the rip boots: `parsed`, or the game's
    std::unique_ptr<GameFiles> files;
    rip::DseLimits limits;
    std::unique_ptr<Starts> starts;
};

std::unique_ptr<Program> MakeProgram(const ntr::Rom& rom, std::string name, const rip::DseCode& code)
{
    auto program = std::make_unique<Program>();
    program->name = std::move(name);
    program->code = code;
    if (!code.patches.empty())
    {
        program->image = rip::PatchedImage(rom, code.patches);
        program->parsed.emplace(ntr::Rom::Parse(program->image));
    }
    program->rom = program->parsed ? &*program->parsed : &rom;
    program->files = ReadGameFiles(*program->rom);
    program->limits = rip::ReadDseLimits(*program->rom, program->code);
    program->starts = std::make_unique<Starts>(*program->rom, program->code, program->limits);

    return program;
}

// What the titles of `program`'s songs end with: nothing for the game, or "_" and the second program's name.
std::string ProgramSuffix(const Program& program)
{
    return program.name.empty() ? "" : "_" + program.name;
}

// The DSE file of `program` whose bytes are the data of member `member` of the code's pack, a DARC archive. Nothing for
// none.
std::optional<std::size_t> DarcMemberFile(const Program& program, uint32_t member)
{
    constexpr uint32_t kHeaderBytes = 16;
    const ntr::RomFile* const pack = program.rom->Find(program.code.pack);
    const std::vector<std::pair<uint32_t, uint32_t>> members =
        pack != nullptr ? rip::DseDarcMembers(program.rom->FileData(pack->id))
                        : std::vector<std::pair<uint32_t, uint32_t>>{};
    if (member >= members.size())
    {
        return std::nullopt;
    }

    const std::vector<dse::NamedFile>& dse_files = program.files->dse;
    for (std::size_t i = 0; i < dse_files.size(); i++)
    {
        const std::optional<FileRange> range = RangeOf(*program.rom, dse_files[i].bytes);
        if (range && range->id == pack->id && range->first == members[member].first + kHeaderBytes)
        {
            return i;
        }
    }

    return std::nullopt;
}

// `set`, the files of the song that `start` starts, with the banks that `program`'s code loads with it where the code
// names them: the bank in the member of the pack that the start's number gives (DseCode::first_bank_member), and the
// main bank (DseCode::main_bank) in place of a main bank with the same bytes. Nothing where the code's bank is missing,
// or where its main bank is missing or has other bytes.
std::optional<dse::SequenceSet> WithGameBanks(const Program& program, const rip::DseStart& start, dse::SequenceSet set)
{
    const rip::DseCode& code = program.code;
    if (code.index_kind == rip::DsePackIndex::kDarc && code.first_bank_member != 0)
    {
        const std::optional<std::size_t> bank = DarcMemberFile(program, code.first_bank_member + start.number);
        if (!bank)
        {
            return std::nullopt;
        }
        set.banks = {*bank};
    }

    if (code.main_bank == nullptr || !set.main_bank)
    {
        return set;
    }

    const GameFiles& files = *program.files;
    const dse::NamedFile* const named = FileAt(files, code.main_bank);
    if (named == nullptr || !std::ranges::equal(named->bytes, files.dse[*set.main_bank].bytes))
    {
        return std::nullopt;
    }
    set.main_bank = static_cast<std::size_t>(named - files.dse.data());

    return set;
}

// The songs that a set of `program` can start by number and that --only picks, in the order of
// dse::rip::SelectSequences, with the songs that the game never starts last. --only matches a song's title: the song's
// label with the state's suffix and the program's (ProgramSuffix). `unstarted` gets the paths of the others that --only
// picks by their labels. Such a song can have the label of a song that the set starts, in another place.
std::vector<Job> SongJobs(const Program& program, const xsflib::cli::RipOptions& o, std::vector<std::string>& unstarted)
{
    const GameFiles& files = *program.files;
    std::vector<Job> jobs;
    for (const dse::rip::Job& selected : dse::rip::SelectSequences(files.dse, std::regex(".*"), true))
    {
        const dse::NamedFile* song = FileAt(files, selected.sequence.path);
        const std::optional<rip::DseStart> start = song != nullptr ? program.starts->Of(*song) : std::nullopt;
        std::optional<dse::SequenceSet> set = dse::FindSequenceSet(files.dse, selected.sequence);
        if (set && start)
        {
            set = WithGameBanks(program, *start, *set);
        }

        if (!start || !set || set->banks.empty())
        {
            if (std::regex_search(selected.label, o.only))
            {
                unstarted.push_back(selected.sequence.path);
            }
            continue;
        }

        for (const auto& [suffix, at] : StatesOf(*files.starts, program.code, *start, selected.sequence.path))
        {
            const std::string label = selected.label + suffix + ProgramSuffix(program);
            if (std::regex_search(label, o.only))
            {
                jobs.push_back(
                    {.sequence = selected.sequence, .label = label, .state = suffix, .start = at, .files = *set});
            }
        }
    }

    const auto used = [&program](const Job& job)
    {
        return !IsUnused(job.start, program.limits);
    };
    std::ranges::stable_partition(jobs, used);
    for (std::size_t i = 0; i < jobs.size(); i++)
    {
        jobs[i].index = static_cast<uint32_t>(i + 1);
    }

    return jobs;
}

// Lists the game's songs and how a set starts each one: the first of `programs` that starts it, the game first. A song
// whose files repeat the bytes of a song of the same program listed before it (rip::SameDseFiles) is marked: the rip
// leaves it out where the game also starts both at the same volume (DropCopies).
void ListSongs(std::span<const std::unique_ptr<Program>> programs)
{
    // Each program's songs listed so far: each song's title with its start, and its files.
    std::vector<std::vector<std::pair<std::string, dse::SequenceSet>>> listed(programs.size());
    for (const dse::rip::Job& job : dse::rip::SelectSequences(programs.front()->files->dse, std::regex(".*"), false))
    {
        std::size_t p = 0;
        std::optional<rip::DseStart> start;
        for (std::size_t i = 0; i < programs.size() && dse::rip::IsMusic(job.sequence); i++)
        {
            const dse::NamedFile* song = FileAt(*programs[i]->files, job.sequence.path);
            start = song != nullptr ? programs[i]->starts->Of(*song) : std::nullopt;
            if (start)
            {
                p = i;
                break;
            }
        }

        if (!dse::rip::IsMusic(job.sequence) || !start)
        {
            const char* const kind =
                !dse::rip::IsMusic(job.sequence) ? "effect (not ripped)" : "music with no number (not ripped)";
            std::printf("%s  %s\n", job.label.c_str(), kind);
            continue;
        }

        const Program& program = *programs[p];
        const GameFiles& files = *program.files;
        const std::string in_program = program.name.empty() ? "" : ", in the program " + program.name;
        std::string repeats;
        if (const std::optional<dse::SequenceSet> set = dse::FindSequenceSet(files.dse, job.sequence))
        {
            const auto same = [&](const std::pair<std::string, dse::SequenceSet>& earlier)
            {
                return rip::SameDseFiles(files.dse, earlier.second, *set);
            };
            if (const auto earlier = std::ranges::find_if(listed[p], same); earlier != listed[p].end())
            {
                repeats = ", the bytes of " + earlier->first;
            }
            listed[p].emplace_back(job.label + ProgramSuffix(program) + " (" + StartName(*start) + ")", *set);
        }

        for (const auto& [suffix, at] : StatesOf(*files.starts, program.code, *start, job.sequence.path))
        {
            const std::string unused = IsUnused(at, program.limits) ? ", unused" : "";
            std::printf("%s%s%s  music, %s%s%s%s\n", job.label.c_str(), suffix.c_str(), ProgramSuffix(program).c_str(),
                        StartName(at).c_str(), in_program.c_str(), unused.c_str(), repeats.c_str());
        }
    }

    std::fflush(stdout);
}

// The streams of `program` (rip::DseStreams) that --only picks by their titles: the stream's label with the program's
// suffix (ProgramSuffix). A stream with no sound up to its end, or to its loop's end, is left out with a message.
std::vector<StreamJob> StreamJobs(const Program& program, const xsflib::cli::RipOptions& o)
{
    std::vector<StreamJob> jobs;
    for (const rip::DseStream& stream : rip::DseStreams(*program.rom, program.code))
    {
        const std::string label = stream.label + ProgramSuffix(program);
        if (!std::regex_search(label, o.only))
        {
            continue;
        }

        const std::span<const uint8_t> file = program.rom->FileData(stream.file_id);
        const rip::SadlHeader header = rip::ReadSadlHeader(file).value();
        if (!rip::SadlAudible(file, header, program.code.stream_build))
        {
            std::fprintf(stderr, "%s (%s) plays no sound and is left out\n", label.c_str(), stream.path.c_str());
            continue;
        }

        jobs.push_back({.stream = stream, .label = label, .header = header});
    }

    return jobs;
}

// Lists the streams of each of `programs` that a set plays (rip::DseStreams), after the songs (ListSongs).
void ListStreams(std::span<const std::unique_ptr<Program>> programs)
{
    for (const std::unique_ptr<Program>& program : programs)
    {
        const std::string in_program = program->name.empty() ? "" : ", in the program " + program->name;
        for (const rip::DseStream& stream : rip::DseStreams(*program->rom, program->code))
        {
            const std::span<const uint8_t> file = program->rom->FileData(stream.file_id);
            const rip::SadlHeader header = rip::ReadSadlHeader(file).value();
            const char* const silent =
                rip::SadlAudible(file, header, program->code.stream_build) ? "" : ", no sound (not ripped)";
            std::printf("%s%s  stream, %s, %u channel%s at %s Hz, %s%s%s\n", stream.label.c_str(),
                        ProgramSuffix(*program).c_str(), stream.path.c_str(), header.channels,
                        header.channels == 1 ? "" : "s", rip::SadlUpsampling(header) == 2 ? "16,364" : "32,728",
                        header.loop ? "loops" : "ends", in_program.c_str(), silent);
        }
    }

    std::fflush(stdout);
}

// The files of the songs of a set: the ROM files that are DSE files, the ranges of the packs that hold DSE files, and
// the files that hold DSE files only once the game decompresses them. Those keep the ranges that the minis' runs read.
struct SongFileSet
{
    std::set<uint32_t> whole;
    std::map<uint32_t, rip::FileRanges> packs;
    std::set<uint32_t> read;
};

// The ROM file whose path starts `path`, followed by a '/': the file that contains a DSE file that FindFiles found in
// its decompressed bytes. Null for none.
const ntr::RomFile* HolderOf(const ntr::Rom& rom, const std::string& path)
{
    for (std::size_t cut = path.rfind('/'); cut != std::string::npos && cut > 0; cut = path.rfind('/', cut - 1))
    {
        if (const ntr::RomFile* file = rom.Find(path.substr(0, cut)); file != nullptr)
        {
            return file;
        }
    }

    return nullptr;
}

// The ROM file of each DSE file that `jobs` play: their songs, their banks and their main banks. A DSE file inside a
// pack gives its range of the pack, widened to the card's pages. A DSE file that FindFiles found in a file's
// decompressed bytes gives that file. Nothing when one of them is outside the image's files.
std::optional<SongFileSet> SongFiles(const ntr::Rom& rom, const GameFiles& files, const std::vector<Job>& jobs)
{
    SongFileSet set;
    for (const Job& job : jobs)
    {
        std::vector<std::size_t> indices = job.files.banks;
        indices.push_back(job.files.file);
        if (job.files.main_bank)
        {
            indices.push_back(*job.files.main_bank);
        }

        for (const std::size_t index : indices)
        {
            const std::optional<FileRange> range = RangeOf(rom, files.dse[index].bytes);
            if (!range)
            {
                const ntr::RomFile* const holder = HolderOf(rom, files.dse[index].path);
                if (holder == nullptr)
                {
                    return std::nullopt;
                }

                set.read.insert(holder->id);
                continue;
            }

            if (range->whole)
            {
                set.whole.insert(range->id);
                continue;
            }

            rip::AddCardPages(set.packs[range->id], rom.Files()[range->id], range->first, range->end);
        }
    }

    for (const auto& [id, ranges] : set.packs)
    {
        set.whole.erase(id);
    }

    for (const uint32_t id : set.read)
    {
        set.whole.erase(id);
        set.packs.erase(id);
    }

    return set;
}

// Adds to `ranges` the card pages of each of `entries` that one of `ranges` reaches. Both give ranges of `file`.
void AddReachedEntries(const ntr::RomFile& file, const rip::FileRanges& entries, rip::FileRanges& ranges)
{
    rip::FileRanges widened = ranges;
    for (const auto& [first, end] : entries)
    {
        const auto reaches = [first, end](const std::pair<uint32_t, uint32_t>& range)
        {
            return range.first < end && first < range.second;
        };
        if (first < end && std::ranges::any_of(ranges, reaches))
        {
            rip::AddCardPages(widened, file, first, end);
        }
    }
    ranges = std::move(widened);
}

// The entries of a pack by the offsets of its index `offsets`, each up to the next offset, within the pack's `size`.
rip::FileRanges OffsetEntries(std::span<const uint8_t> offsets, uint32_t size)
{
    rip::FileRanges entries;
    for (std::size_t at = 0; at + 8 <= offsets.size(); at += 4)
    {
        entries.emplace_back(xsflib::LoadLe32(&offsets[at]), std::min(xsflib::LoadLe32(&offsets[at + 4]), size));
    }

    return entries;
}

// Widens the ranges of `set.packs` to whole entries where the game reads a pack's entry by the offsets of its index,
// the entry's header included: a song's range of the pack then grows to the entries that it reaches, widened to the
// card's pages. Where the game reads a pack's table of members from the pack (kMembers), the pack keeps the table.
// Where the pack is a cartridge image (kRom), a song's range grows to each whole file of the image that it reaches, and
// where it is a DARC archive (kDarc), to each whole member that it reaches.
void AddPackEntries(const ntr::Rom& rom, const rip::DseCode& code, SongFileSet& set)
{
    if (code.start != rip::DseStartKind::kCalls)
    {
        return;
    }

    // The ROM file of `pack` and the set's ranges of it, or null for either where the set keeps no range of it.
    const auto ranges_of = [&rom, &set](const char* pack) -> std::pair<const ntr::RomFile*, rip::FileRanges*>
    {
        const ntr::RomFile* const file = pack != nullptr ? rom.Find(pack) : nullptr;
        const auto ranges = file != nullptr ? set.packs.find(file->id) : set.packs.end();
        if (ranges == set.packs.end())
        {
            return {nullptr, nullptr};
        }

        return {file, &ranges->second};
    };

    if (code.index_kind == rip::DsePackIndex::kMembers)
    {
        if (const auto [file, ranges] = ranges_of(code.pack); ranges != nullptr)
        {
            const uint32_t size = file->end - file->start;
            rip::AddCardPages(*ranges, *file, 0, std::min(rip::DseMembersEnd(rom.FileData(file->id)), size));
        }
    }

    // A DARC archive: the game reads a member's header before its data.
    if (code.index_kind == rip::DsePackIndex::kDarc)
    {
        if (const auto [file, ranges] = ranges_of(code.pack); ranges != nullptr)
        {
            AddReachedEntries(*file, rip::DseDarcMembers(rom.FileData(file->id)), *ranges);
        }
    }

    // A pack that is a cartridge image: the game reads each song's file of the image whole.
    if (code.index_kind == rip::DsePackIndex::kRom && code.pack_index != nullptr)
    {
        if (const auto [file, ranges] = ranges_of(code.pack); ranges != nullptr)
        {
            rip::FileRanges entries;
            for (const rip::DseRomEntry& entry : rip::DseRomEntries(rom.FileData(file->id), code.pack_index))
            {
                entries.emplace_back(entry.first, entry.end);
            }
            AddReachedEntries(*file, entries, *ranges);
        }
    }

    if (code.index_kind != rip::DsePackIndex::kOffsets)
    {
        return;
    }

    for (const auto& [pack, index] :
         {std::pair(code.pack, code.pack_index), std::pair(code.me_pack, code.me_pack_index)})
    {
        const ntr::RomFile* const index_file = index != nullptr ? rom.Find(index) : nullptr;
        if (const auto [file, ranges] = ranges_of(pack); ranges != nullptr && index_file != nullptr)
        {
            AddReachedEntries(*file, OffsetEntries(rom.FileData(index_file->id), file->end - file->start), *ranges);
        }
    }
}

// `job`'s state in `starts` (ntdlib's dse::GameStarts). The check of the mini's start compares the start with that
// state. Nothing for a state that `starts` doesn't give.
std::optional<dse::SongState> TableState(const dse::GameStarts& starts, const Job& job)
{
    for (const dse::SongState& state : starts.StatesOf(job.sequence.path))
    {
        if (state.suffix == job.state)
        {
            return state;
        }
    }

    return std::nullopt;
}

// The error of a job whose state TableState doesn't give.
std::string NoStateError(const Job& job)
{
    return "ntdlib's table has no state '" + job.state + "' of the song";
}

// `set`'s image with the words of the mini that starts `start` (rip::DseStartWords).
std::vector<uint8_t> MiniImage(const rip::DseSet& set, const rip::DseStart& start, const rip::DseStart& library)
{
    std::vector<uint8_t> image = set.image;
    const std::vector<uint8_t> words = rip::DseStartWords(start, library);
    std::ranges::copy(words, image.begin() + set.start_word);

    return image;
}

// Adds to `song_files.packs` the ranges of the files of `song_files.read` that a run of each job's mini on `full`, the
// full image, reads from the hook to DSE's start of the song, widened to the card's pages. Each run checks the start at
// the state of the table, as RunMinis does. Returns the error of a run that fails.
std::optional<std::string> AddReadRanges(const rip::DseCode& code, const rip::DseSet& full,
                                         const rip::DseStart& library, const rip::SetBoot& boot, const GameFiles& files,
                                         const std::vector<Job>& jobs, unsigned threads, SongFileSet& song_files)
{
    const ntr::Rom rom = ntr::Rom::Parse(full.image);
    std::vector<rip::DseMiniRun> runs(jobs.size());
    const auto run_job = [&](std::size_t i)
    {
        const Job& job = jobs[i];
        const std::optional<dse::SongState> state = TableState(*files.starts, job);
        if (!state)
        {
            runs[i].error = NoStateError(job);
            return;
        }

        runs[i] = rip::RunDseMini(MiniImage(full, job.start, library), code, boot.hook_clock,
                                  files.dse[job.files.banks.front()].bytes, files.dse[job.files.file].bytes, *state,
                                  boot.seconds, boot.backup);
    };
    xsflib::cli::RunParallel(jobs.size(), threads, run_job);

    for (std::size_t i = 0; i < jobs.size(); i++)
    {
        if (runs[i].error)
        {
            return jobs[i].label + ": " + *runs[i].error;
        }

        for (const uint32_t id : song_files.read)
        {
            const auto reads = runs[i].reads.find(id);
            if (reads == runs[i].reads.end())
            {
                continue;
            }

            for (const auto& [first, end] : reads->second)
            {
                rip::AddCardPages(song_files.packs[id], rom.Files()[id], first, end);
            }
        }
    }

    return std::nullopt;
}

// Why the game's stops of a mini's song fail the check: `stops` gives the time of the stop on each boot with `backups`,
// in the same order (rip::DseMiniRun::stop_ms), and `timing` the piece's timing from the render, or null for none. The
// game may stop a piece that ends from the render's end of its last sound, less rip::kRenderModelMs: the games stop a
// short piece once it ends. Nothing when the mini passes.
std::optional<std::string> StopError(std::span<const ntr::BackupMemory> backups,
                                     std::span<const std::optional<double>> stops, const xsflib::rip::Timing* timing)
{
    for (std::size_t i = 0; i < stops.size(); i++)
    {
        if (!stops[i])
        {
            continue;
        }

        if (timing != nullptr && rip::Ends(*timing) &&
            *stops[i] >= static_cast<double>(timing->length_ms - rip::kRenderModelMs))
        {
            continue;
        }

        return "the game stops the song " + std::to_string(std::llround(*stops[i])) +
               " ms after its start, before the song ends" + rip::OnBoot(backups[i]);
    }

    return std::nullopt;
}

// Runs each job's mini on the DS on each of `boots`: the checks confirm the set (rip::RunDseMini on each boot, at the
// level of the table, rip::BootsError over them, and StopError), and the run with no backup memory gives the time to
// the song's start. The length comes from the model of DSE's build, at the job's tick and at the volume that the game's
// code passes to DSE's start in the run with no backup memory: the analysis plays what the set plays. The first sounds
// and the end of a piece that ends come from the render of 2sf's player (rip::TimeFromPlayer). A render that throws
// stops the rip with the song's label in the message. Returns the number of minis that fail.
std::size_t RunMinis(const rip::DseCode& code, const rip::DseSet& set, const rip::DseStart& library,
                     std::span<const rip::SetBoot> boots, const std::map<uint32_t, rip::FileRanges>& packs,
                     const GameFiles& files, dse::Build build, unsigned threads, std::vector<Job>& jobs)
{
    const std::vector<ntr::BackupMemory> backups = rip::BackupsOf(boots);
    const auto analyze = [&](Job& job)
    {
        std::vector<uint8_t> image = MiniImage(set, job.start, library);
        const std::span<const uint8_t> song = files.dse[job.files.file].bytes;
        const std::span<const uint8_t> bank = files.dse[job.files.banks.front()].bytes;
        const std::optional<dse::SongState> state = TableState(*files.starts, job);
        if (!state)
        {
            job.error = NoStateError(job);
            return false;
        }

        std::vector<rip::BootRun> checks;
        std::vector<std::optional<double>> stops;
        for (const rip::SetBoot& boot : boots)
        {
            const auto run_mini = [&]
            {
                return rip::RunDseMini(image, code, boot.hook_clock, bank, song, *state, boot.seconds, boot.backup,
                                       packs);
            };
            const rip::DseMiniRun run = rip::RunOnBoot(boot.backup, run_mini);
            if (checks.empty())
            {
                job.start_ms = run.start_ms;
            }
            checks.push_back({run.error, run.sounded});
            stops.push_back(run.stop_ms);
            job.volumes.push_back(run.volume);
        }

        job.error = rip::BootsError(backups, checks);
        if (job.error)
        {
            return false;
        }

        try
        {
            dse::LoadedFiles loaded;
            loaded.song = dse::Song::Parse(song);
            for (const std::size_t index : job.files.banks)
            {
                loaded.banks.push_back(std::make_unique<dse::WaveBank>(dse::WaveBank::Parse(files.dse[index].bytes)));
            }
            if (job.files.main_bank)
            {
                loaded.main_bank = dse::WaveBank::Parse(files.dse[*job.files.main_bank].bytes);
            }
            const dse::StartParameters song_start = {.volume = static_cast<int16_t>(job.volumes.front())};
            job.timing = dse::MeasureSequence(loaded.Files(), build, xsflib::rip::Limit::kDefault, song_start,
                                              job.tick_microseconds);
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "warning: %s: %s\n", job.label.c_str(), e.what());
        }

        if (job.timing)
        {
            rip::PrintRenderReport(job.label,
                                   rip::TimeFromPlayer(std::move(image), job.start_ms, job.label, *job.timing));
        }

        job.error = StopError(backups, stops, job.timing ? &*job.timing : nullptr);

        return !job.error;
    };
    xsflib::cli::AnalysisSettings analysis;
    analysis.progress = "ran";
    xsflib::cli::AnalyzeLengths(jobs, threads, analyze, analysis);

    return ReportFailures(jobs);
}

// Leaves out each job that repeats an earlier job under another path: the same bytes and the same volume of DSE's start
// on every boot (rip::SameDseStart). The game's code plays both the same. The states of one song aren't copies. Prints
// each copy and the job that it repeats.
void DropCopies(const GameFiles& files, std::vector<Job>& jobs)
{
    std::vector<Job> kept;
    for (Job& job : jobs)
    {
        const auto repeats = [&job, &files](const Job& earlier)
        {
            return earlier.sequence.path != job.sequence.path &&
                   rip::SameDseStart(files.dse, earlier.files, earlier.volumes, job.files, job.volumes);
        };
        if (const auto earlier = std::ranges::find_if(kept, repeats); earlier != kept.end())
        {
            std::fprintf(stderr, "%s (%s) repeats %s (%s): the same song, banks and start volume. It gets no mini\n",
                         job.label.c_str(), StartName(job.start).c_str(), earlier->label.c_str(),
                         StartName(earlier->start).c_str());
            continue;
        }

        kept.push_back(std::move(job));
    }
    jobs = std::move(kept);
}

// The block of the mini that starts `job`'s stream in `set`, a library whose stub starts `library`: for kName the words
// of the start by the stream's name (rip::DseStartWords), for kCalls the whole stub of the stream
// (rip::BuildDseStreamStub) in place of the library's. Throws std::runtime_error when the stub doesn't fit its place.
Block StreamBlock(const rip::DseCode& code, const rip::DseSet& set, const rip::DseStart& library, const StreamJob& job)
{
    if (code.start != rip::DseStartKind::kCalls)
    {
        return {set.start_word, rip::DseStartWords({.name = job.stream.name}, library)};
    }

    const rip::DseStub stub = rip::BuildDseStreamStub(code, job.stream);
    rip::CheckDseStubPlace(code, stub);

    return {set.stub, stub.bytes};
}

// The block of the second library of `job`'s stream: the stream's file at its place in `set_rom`, the library's image.
// The library keeps the file at its size, with zeros where no boot reads it.
Block StreamFileBlock(const ntr::Rom& rom, const ntr::Rom& set_rom, const StreamJob& job)
{
    const std::span<const uint8_t> file = rom.FileData(job.stream.file_id);
    const ntr::RomFile& entry = set_rom.Files().at(job.stream.file_id);
    if (entry.end - entry.start != file.size())
    {
        throw std::runtime_error(job.label + ": the library doesn't keep the stream's file at its size");
    }

    return {entry.start, {file.begin(), file.end()}};
}

// The heads of the channels of a stream, `file`, for rip::StreamTarget: the first samples that the stream player of
// `build` writes to the buffer of each channel (rip::DecodeSadl), as 16-bit PCM, each twice. The player starts the
// channels on empty buffers and fills them once they play (rip::StreamTarget::heads_after_start).
std::vector<std::vector<uint8_t>> StreamHeads(std::span<const uint8_t> file, const rip::SadlHeader& header,
                                              rip::SadlBuild build)
{
    constexpr uint32_t kHeadFrames = 9; // 270 samples or more
    constexpr std::size_t kHeadSamples = 256;
    std::vector<std::vector<uint8_t>> heads;
    for (const std::vector<int16_t>& samples : rip::DecodeSadl(file, header, kHeadFrames, build))
    {
        std::vector<uint8_t> head;
        for (std::size_t i = 0; i < std::min(samples.size(), kHeadSamples); i++)
        {
            head.push_back(static_cast<uint8_t>(samples[i] & 0xFF));
            head.push_back(static_cast<uint8_t>((samples[i] >> 8) & 0xFF));
        }
        heads.push_back(head);
        heads.push_back(std::move(head));
    }

    return heads;
}

// Runs each stream's mini on the DS on each of `boots`: the checks confirm the set (rip::RunStreamMini on each boot,
// rip::BootsError over them), and the run with no backup memory gives the time to the stream's start. After the hook,
// each card read must reach a range that the set keeps (`partial` gives the files that it keeps in part), or the
// stream's file. The length comes from the stream's samples and the rate of its channels (rip::StreamTiming), and the
// first sounds and the end of a stream that ends from the render of 2sf's player (rip::TimeFromPlayer). A run or a
// render that throws stops the rip with the stream's label in the message. Returns the number of minis that fail.
std::size_t RunStreams(const Program& program, const rip::DseSet& set, const rip::DseStart& library,
                       std::span<const rip::SetBoot> boots, const std::map<uint32_t, rip::FileRanges>& partial,
                       unsigned threads, std::vector<StreamJob>& jobs)
{
    const ntr::Rom set_rom = ntr::Rom::Parse(set.image);
    const std::vector<ntr::BackupMemory> backups = rip::BackupsOf(boots);
    const auto analyze = [&](StreamJob& job)
    {
        const std::span<const uint8_t> file = program.rom->FileData(job.stream.file_id);
        std::vector<uint8_t> image = set.image;
        for (const Block& block :
             {StreamBlock(program.code, set, library, job), StreamFileBlock(*program.rom, set_rom, job)})
        {
            std::ranges::copy(block.data, image.begin() + block.offset);
        }

        const ntr::RomFile& entry = set_rom.Files().at(job.stream.file_id);
        rip::StreamTarget target;
        target.file_start = entry.start;
        target.file_end = entry.end;
        target.heads = StreamHeads(file, job.header, program.code.stream_build);
        target.channels = job.header.channels;
        target.rate = rip::kSadlPlayRate;
        target.samples = rip::SadlSamples(job.header);
        target.loop_start = rip::SadlLoopStart(job.header);
        target.loop = job.header.loop;
        target.commands_after_hook = true;
        target.heads_after_start = true;

        std::map<uint32_t, rip::FileRanges> others = partial;
        others.erase(job.stream.file_id);
        const std::vector<std::pair<uint32_t, uint32_t>> kept = rip::KeptRanges(set_rom, others);
        std::vector<rip::StreamRun> runs;
        for (const rip::SetBoot& boot : boots)
        {
            const auto run_stream = [&]
            {
                return rip::RunStreamMini(image, boot.hook_clock, target, boot.seconds, boot.backup,
                                          rip::KeptReadsCheck(kept, boot.hook_clock));
            };
            runs.push_back(rip::RunOnBoot(boot.backup, run_stream));
        }

        job.error = rip::BootsError(backups, rip::StreamChecks(runs));
        job.start_ms = runs.front().start_ms;
        if (job.error)
        {
            return false;
        }

        job.timing = rip::StreamTiming(target.samples, target.loop_start, target.loop, runs.front().rate);
        rip::PrintRenderReport(job.label, rip::TimeFromPlayer(std::move(image), job.start_ms, job.label, *job.timing));

        return true;
    };
    xsflib::cli::AnalysisSettings analysis;
    analysis.progress = "ran the streams'";
    analysis.progress_every = 10;
    xsflib::cli::AnalyzeLengths(jobs, threads, analyze, analysis);

    return ReportFailures(jobs);
}

// A program's library, its minis and the second libraries of its streams' minis.
struct ProgramSet
{
    std::vector<uint8_t> image;
    std::vector<MiniSpec> minis;
    std::vector<SecondLibrary> libraries;
};

// Rips the songs of `jobs` (SongJobs) and the streams of `streams` (StreamJobs) of `program`: the library, a mini for
// each song that plays a note, less the copies (DropCopies), then a mini for each stream, with the stream's file as its
// second library. The library keeps each stream's file at its size, with zeros where no boot reads it. Returns nothing,
// with `error` set, when the program can't be ripped.
std::optional<ProgramSet> RipProgram(const Program& program, dse::Build build, const xsflib::cli::RipOptions& o,
                                     std::vector<Job> jobs, std::vector<StreamJob> streams, std::string& error)
{
    const ntr::Rom& rom = *program.rom;
    const rip::DseCode& code = program.code;
    const GameFiles& files = *program.files;

    // The length model plays the tick of the game's settings.
    for (Job& job : jobs)
    {
        const dse::GameTick tick = dse::TickOf(rom.Header().game_code, job.sequence.path);
        if (!tick.tick_microseconds)
        {
            error = "ntdlib gives " + job.label + " no tick";
            return std::nullopt;
        }
        job.tick_microseconds = *tick.tick_microseconds;
    }

    std::optional<SongFileSet> song_files = SongFiles(rom, files, jobs);
    if (!song_files)
    {
        error = "its songs lie outside the image's files";
        return std::nullopt;
    }
    AddPackEntries(rom, code, *song_files);

    const rip::DseStart library = jobs.empty() ? rip::DseStart{} : jobs.front().start;
    const rip::DseStub stub = rip::BuildDseStub(code, library);
    const uint32_t hook = code.hook & ~uint32_t{1}; // the hook's first byte, in ARM or Thumb code

    // Of a file whose songs are readable only once the game decompresses them, the library keeps what the minis' runs
    // read on the full image, from its hook on the boot with no backup memory.
    if (!song_files->read.empty())
    {
        const rip::DseSet full = rip::MakeDseSet(rom, code, stub, rip::WholeFiles(rom, rip::EveryFile(rom)));
        const ntr::BackupMemory none;
        const rip::HookBoot game = rip::RunOnBoot(none, [&] { return rip::BootTo(rom, hook, none); });
        const ntr::Rom full_rom = ntr::Rom::Parse(full.image);
        const rip::HookBoot full_hook = rip::RunOnBoot(none, [&] { return rip::BootTo(full_rom, hook, none); });
        const rip::SetBoot boot = {none, full_hook.clock, game.seconds + kStartLimitSeconds};
        if (std::optional<std::string> failed =
                AddReadRanges(code, full, library, boot, files, jobs, o.jobs, *song_files))
        {
            error = std::move(*failed);
            return std::nullopt;
        }
    }

    for (const StreamJob& stream : streams)
    {
        song_files->packs.try_emplace(stream.stream.file_id);
    }

    const auto image_of = [&](const std::map<uint32_t, std::vector<uint8_t>>& kept)
    {
        return rip::MakeDseSet(rom, code, stub, kept).image;
    };
    const std::optional<HookLibrary> hook_library =
        MakeHookLibrary(rom, hook, song_files->whole, image_of, kStartLimitSeconds, error, song_files->packs);
    if (!hook_library)
    {
        return std::nullopt;
    }
    const rip::DseSet set =
        rip::MakeDseSet(rom, code, stub, rip::KeptFiles(rom, hook_library->files, hook_library->partial));

    const std::vector<rip::SetBoot>& set_boots = hook_library->boots;
    const std::size_t failed =
        RunMinis(code, set, library, set_boots, hook_library->partial, files, build, o.jobs, jobs);
    if (failed > 0)
    {
        error =
            std::to_string(failed) + " of " + std::to_string(jobs.size()) + " minis don't play their song on the DS";
        return std::nullopt;
    }

    // A song that plays no notes gets no mini.
    const std::size_t before = jobs.size();
    std::erase_if(jobs, [](const Job& job) { return job.timing && job.timing->silent; });
    if (jobs.size() < before)
    {
        std::fprintf(stderr, "%zu songs play no notes and are left out\n", before - jobs.size());
    }

    // A song that the game plays as an earlier one gets no mini. The library keeps its bytes.
    DropCopies(files, jobs);

    const std::size_t failed_streams =
        RunStreams(program, set, library, set_boots, hook_library->partial, o.jobs, streams);
    if (failed_streams > 0)
    {
        error = std::to_string(failed_streams) + " of " + std::to_string(streams.size()) +
                " stream minis don't play their stream on the DS";
        return std::nullopt;
    }

    ProgramSet program_set{.image = set.image};
    for (const Job& job : jobs)
    {
        MiniSpec& mini = program_set.minis.emplace_back();
        mini.title = job.label;
        mini.block = {set.start_word, rip::DseStartWords(job.start, library)};
        if (o.lengths)
        {
            mini.timing = job.timing;
        }
    }

    const ntr::Rom set_rom = ntr::Rom::Parse(set.image);
    for (const StreamJob& stream : streams)
    {
        MiniSpec& mini = program_set.minis.emplace_back();
        mini.title = stream.label;
        mini.block = StreamBlock(code, set, library, stream);
        if (o.lengths)
        {
            mini.timing = stream.timing;
        }
        mini.library = program_set.libraries.size();
        program_set.libraries.push_back({.title = stream.label, .block = StreamFileBlock(rom, set_rom, stream)});
    }

    return program_set;
}

} // namespace

bool IsDseGame(const ntr::Rom& rom)
{
    return rip::ListsDseGame(rom.Header().game_code);
}

int RipDse(const std::string& input, const ntr::Rom& rom, const xsflib::cli::RipOptions& o, bool effects,
           xsflib::cli::FileNameRegistries& names)
{
    std::string error;
    const std::optional<rip::DseCode> code = rip::FindDseCode(rom, error);
    if (!code)
    {
        return Refuse(input, "2sfrip can't rip this game: " + error);
    }

    const std::optional<std::vector<rip::DseProgram>> second_programs = rip::FindDsePrograms(rom, error);
    if (!second_programs)
    {
        return Refuse(input, "2sfrip can't rip this game: " + error);
    }

    const std::optional<dse::Build> build = dse::BuildOf(rom);
    if (!build)
    {
        return Refuse(input, "2sfrip can't rip this game: its build of DSE isn't one that ntdlib's model covers");
    }

    // The game, then its second programs.
    std::vector<std::unique_ptr<Program>> programs;
    programs.push_back(MakeProgram(rom, "", *code));
    for (const rip::DseProgram& program : *second_programs)
    {
        programs.push_back(MakeProgram(rom, std::string(program.name), program.code));
    }

    std::fprintf(stderr, "%s: DSE, the %s build, %zu files\n", input.c_str(), dse::NameOf(*build),
                 programs.front()->files->dse.size());
    if (o.list)
    {
        ListSongs(programs);
        ListStreams(programs);
        return 0;
    }

    if (effects && !o.bgm)
    {
        std::fprintf(stderr, "2sfrip doesn't rip DSE's sound effects yet: the set contains the music\n");
    }

    // Each program's songs and streams. A song that no program starts is left out.
    std::vector<std::vector<Job>> jobs;
    std::vector<std::vector<StreamJob>> streams;
    std::vector<std::string> left_out;
    for (const std::unique_ptr<Program>& program : programs)
    {
        std::vector<std::string> unstarted;
        jobs.push_back(SongJobs(*program, o, unstarted));
        streams.push_back(StreamJobs(*program, o));
        if (program == programs.front())
        {
            left_out = std::move(unstarted);
            continue;
        }

        std::erase_if(left_out,
                      [&](const std::string& path) { return std::ranges::find(unstarted, path) == unstarted.end(); });
    }

    if (!left_out.empty())
    {
        std::string paths;
        for (const std::string& path : left_out)
        {
            paths += (paths.empty() ? "" : ", ") + path;
        }
        std::fprintf(stderr, "songs with no number or no bank are left out: %s\n", paths.c_str());
    }

    const auto none = [](const auto& program_jobs)
    {
        return program_jobs.empty();
    };
    if (std::ranges::all_of(jobs, none) && std::ranges::all_of(streams, none))
    {
        return Refuse(input, "no song to rip");
    }

    // The game's set, with the sets of its second programs beside it.
    std::vector<uint8_t> image;
    SetParts parts;
    for (std::size_t i = 0; i < programs.size(); i++)
    {
        if (jobs[i].empty() && streams[i].empty())
        {
            continue;
        }

        const Program& program = *programs[i];
        if (!program.name.empty())
        {
            std::fprintf(stderr, "the program %s\n", program.name.c_str());
        }

        std::optional<ProgramSet> set =
            RipProgram(program, *build, o, std::move(jobs[i]), std::move(streams[i]), error);
        if (!set)
        {
            const std::string of = program.name.empty() ? "" : "'s program " + program.name;
            return Refuse(input, "2sfrip can't rip this game" + of + ": " + error);
        }

        if (program.name.empty())
        {
            image = std::move(set->image);
            parts.minis = std::move(set->minis);
            parts.libraries = std::move(set->libraries);
        }
        else
        {
            parts.programs.push_back({.name = program.name,
                                      .image = std::move(set->image),
                                      .minis = std::move(set->minis),
                                      .libraries = std::move(set->libraries)});
        }
    }

    const auto no_minis = [](const ProgramParts& program)
    {
        return program.minis.empty();
    };
    if (parts.minis.empty() && std::ranges::all_of(parts.programs, no_minis))
    {
        return Refuse(input, "no song plays a note");
    }

    return WriteSet(input, rom, image, parts, o, names);
}

} // namespace twosf::tools
