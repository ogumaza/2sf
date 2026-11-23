// SPDX-License-Identifier: MIT

// Tests the parts of a set for a game on DSE (rip/dse_set.h) that the stubs leave: the words that a mini writes,
// whether two songs' files have the same bytes, the songs that the game starts by number, the names in a pack's index,
// the entries that an index by CRC-32 or by offsets finds, the songs of the game's table and of a table of names, the
// members of a pack and their songs, the files of a cartridge image in a pack and their numbers, and the boot to the
// hook and the run of a mini with a backup memory, up to the game's stop of the song.

#include "rip/dse_set.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/arm/assembler.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/testing/nds_files.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "backup_image.h"
#include "dse_test_code.h"
#include "stub_memory.h"

namespace
{

namespace arm = ntdlib::arm;

using twosf::rip::DseCode;
using twosf::rip::DseLimits;
using twosf::rip::DseStart;
using twosf::testing::CallsTestCode;
using twosf::testing::kBase;
using twosf::testing::TestCode;
using xsflib::testing::Check;

// A mini writes the words in which its start differs from the library's: the number always, then the word of a short
// piece, then the level of music.
void TestWords()
{
    const DseStart music = {.number = 1, .me = false};
    const DseStart me = {.number = 2, .me = true};

    Check(twosf::rip::DseStartWords({.number = 0x1234, .me = false}, music) == std::vector<uint8_t>{0x34, 0x12, 0, 0},
          "music's mini writes the number");
    Check(twosf::rip::DseStartWords({.number = 5, .me = true}, music) == std::vector<uint8_t>{5, 0, 0, 0, 1, 0, 0, 0},
          "a short piece's mini writes the number and 1");
    Check(twosf::rip::DseStartWords({.number = 5, .me = false}, me) == std::vector<uint8_t>{5, 0, 0, 0, 0, 0, 0, 0},
          "music's mini in a library of a short piece writes the number and 0");
    Check(twosf::rip::DseStartWords({.number = 86, .me = false, .level = 180}, music) ==
              std::vector<uint8_t>{86, 0, 0, 0, 0, 0, 0, 0, 180, 0, 0, 0},
          "music at another level writes the number, 0 and the level");
    Check(twosf::rip::DseStartWords({.number = 1, .me = false}, {.number = 86, .me = false, .level = 180}) ==
              std::vector<uint8_t>{1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0},
          "music at full level in a library of a lower level writes the full level");

    const ntdlib::dse::StartChange change = {.start_level = 156, .volume = 102, .frames = 1};
    const DseStart changed = {.number = 73, .me = false, .change = change};
    Check(twosf::rip::DseStartWords(changed, music) ==
              std::vector<uint8_t>{73, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 102, 0, 0, 0, 1, 0, 0, 0},
          "music with a change writes the number, 0, the level, the change's volume and its frames");
    Check(twosf::rip::DseStartWords({.number = 1, .me = false}, changed) ==
              std::vector<uint8_t>{1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
          "music without a change in a library of a change writes two words of 0 for the change");
    Check(twosf::rip::DseStartWords({.number = 74, .me = false, .change = change}, changed) ==
              std::vector<uint8_t>{74, 0, 0, 0},
          "music with the library's change writes the number");
    Check(twosf::rip::DseStartWords({.number = 5, .me = true}, changed) == std::vector<uint8_t>{5, 0, 0, 0, 1, 0, 0, 0},
          "a short piece in a library of a change writes the number and 1");

    std::vector<uint8_t> name(twosf::rip::kDseNameBytes, 0);
    std::memcpy(name.data(), "E202", 4);
    Check(twosf::rip::DseStartWords({.name = "E202"}, {.name = "P04"}) == name,
          "a start by name writes the name, padded with zeros");
}

// Two songs have the same files when the songs, the banks in their order and the main banks have the same bytes, at
// any place in the game's files.
void TestSameFiles()
{
    const std::vector<uint8_t> song = {1, 2, 3};
    const std::vector<uint8_t> other_song = {1, 2, 4};
    const std::vector<uint8_t> bank = {5, 6};
    const std::vector<uint8_t> other_bank = {5, 7};
    const std::vector<ntdlib::dse::NamedFile> files = {
        {"bgm/a.smd", song},      {"bgm/a.swd", bank},      {"me/a.smd", song}, {"me/a.swd", bank},
        {"me/b.smd", other_song}, {"me/b.swd", other_bank}, {"main.swd", bank}, {"other_main.swd", other_bank}};
    const ntdlib::dse::SequenceSet first = {.file = 0, .banks = {1}};

    const auto same = [&files, &first](const ntdlib::dse::SequenceSet& set)
    {
        return twosf::rip::SameDseFiles(files, first, set);
    };

    Check(same({.file = 2, .banks = {3}}), "a copy of the song and its bank in another place");
    Check(!same({.file = 4, .banks = {3}}), "another song");
    Check(!same({.file = 2, .banks = {5}}), "another bank");
    Check(!same({.file = 2, .banks = {3, 3}}), "another number of banks");
    Check(!same({.file = 2, .banks = {3}, .main_bank = 6}), "a main bank where the first song has none");
    Check(twosf::rip::SameDseFiles(files, {.file = 0, .banks = {1}, .main_bank = 6},
                                   {.file = 2, .banks = {3}, .main_bank = 3}),
          "main banks with the same bytes");
    Check(!twosf::rip::SameDseFiles(files, {.file = 0, .banks = {1}, .main_bank = 6},
                                    {.file = 2, .banks = {3}, .main_bank = 7}),
          "main banks with other bytes");
}

// A mini plays as another when the files have the same bytes and DSE starts both songs at the same volume on every
// boot. A copy that the game starts at another volume on one boot plays otherwise.
void TestSameStart()
{
    const std::vector<uint8_t> song = {1, 2, 3};
    const std::vector<uint8_t> other_song = {1, 2, 4};
    const std::vector<uint8_t> bank = {5, 6};
    const std::vector<ntdlib::dse::NamedFile> files = {
        {"bgm/a.smd", song}, {"bgm/a.swd", bank}, {"me/a.smd", song}, {"me/a.swd", bank}, {"me/b.smd", other_song}};
    const ntdlib::dse::SequenceSet first = {.file = 0, .banks = {1}};
    const ntdlib::dse::SequenceSet copy = {.file = 2, .banks = {3}};
    const std::vector<int> volumes = {126, 126, 126};

    const auto same = [&files, &first, &volumes](const ntdlib::dse::SequenceSet& set, const std::vector<int>& at)
    {
        return twosf::rip::SameDseStart(files, first, volumes, set, at);
    };

    Check(same(copy, {126, 126, 126}), "a copy that the game starts at the same volume on every boot");
    Check(!same(copy, {126, 100, 126}), "a copy that the game starts at another volume on one boot");
    Check(!same(copy, {127, 127, 127}), "a copy that the game starts at another volume on every boot");
    Check(!same({.file = 4, .banks = {3}}, {126, 126, 126}), "another song at the same volume");
}

// The index of names gives each file's start by its name without the extension, at the file's offset in the pack. An
// entry without a name, or with a name that the stub can't contain, gives none.
void TestPackStarts()
{
    std::vector<uint8_t> index;
    const auto add = [&index](std::string_view name, uint32_t offset)
    {
        std::vector<uint8_t> entry(32, 0);
        std::memcpy(entry.data(), name.data(), name.size());
        xsflib::StoreLe32(&entry[24], offset);
        xsflib::StoreLe32(&entry[28], 0x100);
        index.insert(index.end(), entry.begin(), entry.end());
    };
    add("E202.SMD", 0x200);
    add("E202.SWD", 0x400);
    add("", 0x600);
    add("A_NAME_OF_24_BYTES_ABCDE", 0x800);
    add("P04.SMD", 0xA00);
    index.resize(index.size() + 16); // a part of an entry at the end

    const std::map<uint32_t, DseStart> starts = twosf::rip::DsePackStarts(index);
    Check(starts.size() == 3 && starts.at(0x200).name == "E202" && starts.at(0x400).name == "E202" &&
              starts.at(0xA00).name == "P04",
          "the names without their extensions, at their offsets");
}

// An index by CRC-32 finds the entry of a key whatever the case of its letters. A table finds only a key that it
// lists; a tree finds an entry for any key, by the bits of the key's CRC-32 from the lowest.
void TestPackEntry()
{
    using twosf::rip::DsePackEntry;
    using twosf::rip::DsePackIndex;

    // The CRC-32 of each key in lowercase, from zlib.
    constexpr uint32_t kP04 = 0x29B7E9CF;   // p04.smw, lowest bit 1
    constexpr uint32_t kSr001 = 0x9497906C; // sr_001.smw, lowest bit 0

    std::vector<uint8_t> table;
    for (const auto& [crc, offset] : {std::pair{kSr001, 0x1000U}, std::pair{kP04, 0x2000U}})
    {
        for (const uint32_t word : {crc, offset, 0x100U, 0U})
        {
            xsflib::AppendLe32(table, word);
        }
    }

    Check(DsePackEntry(DsePackIndex::kCrcTable, table, "P04.SMW") == 0x2000U &&
              DsePackEntry(DsePackIndex::kCrcTable, table, "sr_001.smw") == 0x1000U,
          "a table finds a key in either case");
    Check(!DsePackEntry(DsePackIndex::kCrcTable, table, "E202.SMW"), "a table finds no key that it doesn't list");

    // The root's bit 0 leads to word 1, an entry at byte 12, and its bit 1 to word 2, an entry at byte 20.
    std::vector<uint8_t> tree;
    for (const uint32_t word :
         {(2U << 16) | 1U, 0x80000000U | 12U, 0x80000000U | 20U, 0x1000U, 0x100U, 0x2000U, 0x100U})
    {
        xsflib::AppendLe32(tree, word);
    }

    Check(DsePackEntry(DsePackIndex::kCrcTree, tree, "SR_001.SMW") == 0x1000U &&
              DsePackEntry(DsePackIndex::kCrcTree, tree, "p04.smw") == 0x2000U,
          "a tree follows the lowest bit of the key's CRC-32");
    Check(DsePackEntry(DsePackIndex::kCrcTree, tree, "E202.SMW") == 0x2000U, // CRC-32 0x1526D6F7
          "a tree gives an entry for a key that it doesn't list");
    Check(!DsePackEntry(DsePackIndex::kCrcTree, std::span(tree).first(8), "P04.SMW"),
          "a tree that ends before the node gives nothing");
}

void TestStarts()
{
    const DseCode code = TestCode();
    const DseLimits limits{.first_bgm = 1, .last_bgm = 201, .last_me = 13};
    const auto start = [&](const std::string& path)
    {
        return twosf::rip::DseStartOf(code, limits, path);
    };

    Check(start("SOUND/BGM/bgm0000.smd") == DseStart{.number = 0, .me = false},
          "music from 0, through the set's change");
    Check(start("SOUND/BGM/bgm0001.smd") == DseStart{.number = 1, .me = false}, "music from 1");
    Check(start("SOUND/BGM/bgm0201.smd") == DseStart{.number = 201, .me = false}, "music up to the limit");
    Check(!start("SOUND/BGM/bgm0202.smd"), "no music past the limit");
    Check(start("SOUND/ME/me0000.smd") == DseStart{.number = 0, .me = true}, "short pieces from 0");
    Check(start("SOUND/ME/me0013.smd") == DseStart{.number = 13, .me = true}, "short pieces up to the limit");
    Check(!start("SOUND/ME/me0014.smd"), "no short piece past the limit");
    Check(!start("SOUND/SE/motion.sed") && !start("SOUND/BGM/bgm1.smd"), "no start for other paths");
    Check(!start("SOUND/BGM/bgm0001.smd/bgm0001.smd"), "no start by number for a path inside a file");

    // A start by calls takes the songs' numbers in three digits, and a path inside the file that the format gives.
    DseCode calls = TestCode();
    calls.start = twosf::rip::DseStartKind::kCalls;
    calls.bgm_format = "lt3/snd/BG_%03u.csnd";
    calls.me_format = nullptr;
    const DseLimits three_digits{.last_bgm = 999};
    const auto start_by_calls = [&](const std::string& path)
    {
        return twosf::rip::DseStartOf(calls, three_digits, path);
    };

    Check(start_by_calls("lt3/snd/BG_007.csnd/BG_007.SMD") == DseStart{.number = 7},
          "a song inside the file that the format gives");
    Check(start_by_calls("lt3/snd/BG_999.csnd") == DseStart{.number = 999}, "the file itself, up to 999");
    Check(!start_by_calls("lt3/snd/BG_007.csndx/BG_007.SMD") && !start_by_calls("lt3/snd/BG_1000.csnd"),
          "no start for a longer name or a number of four digits");
}

void TestTable()
{
    Check(twosf::rip::ListsDseGame("C2SE") && twosf::rip::ListsDseGame("YFTE") && twosf::rip::ListsDseGame("YFYE"),
          "the table lists the three Explorers games");
    Check(twosf::rip::ListsDseGame("YEEJ") && twosf::rip::ListsDseGame("BEEJ") && twosf::rip::ListsDseGame("BEZJ") &&
              twosf::rip::ListsDseGame("BOEJ"),
          "the table lists the four Inazuma Eleven games");
    Check(twosf::rip::ListsDseGame("YLTE") && twosf::rip::ListsDseGame("C3JE") && twosf::rip::ListsDseGame("BLFE"),
          "the table lists the three Professor Layton games");
    Check(twosf::rip::ListsDseGame("YWFJ") && twosf::rip::ListsDseGame("B4FJ") && twosf::rip::ListsDseGame("B5FJ"),
          "the table lists the three Shiren games");
    Check(twosf::rip::ListsDseGame("YL2E") && twosf::rip::ListsDseGame("BLUJ") && twosf::rip::ListsDseGame("ANIE"),
          "the table lists the three Luminous Arc games");
    Check(twosf::rip::ListsDseGame("YBSJ"), "the table lists Soma Bringer");
    Check(!twosf::rip::ListsDseGame("AZEE"), "the table lists no other game");
}

// An index of offsets gives each entry's offset, then the pack's size: the entry of an offset is the one whose range
// contains it.
void TestOffsetEntry()
{
    std::vector<uint8_t> index;
    for (const uint32_t offset : {0U, 0x300U, 0x300U, 0x800U})
    {
        xsflib::AppendLe32(index, offset);
    }

    Check(twosf::rip::DseOffsetEntry(index, 0) == 0U && twosf::rip::DseOffsetEntry(index, 0x2FF) == 0U,
          "the first entry, up to its end");
    Check(twosf::rip::DseOffsetEntry(index, 0x300) == 2U, "an empty entry contains no offset");
    Check(twosf::rip::DseOffsetEntry(index, 0x7FF) == 2U, "the last entry, up to the pack's size");
    Check(!twosf::rip::DseOffsetEntry(index, 0x800) && !twosf::rip::DseOffsetEntry(std::span(index).first(4), 0),
          "no entry past the pack's size, or in an index of one word");
}

// The game's table of songs gives each song's path by the song's number: an entry of 12 bytes with the name's address
// first and the byte of a short piece at 8. A name that starts with '=' gives the rest of the path, and an empty name
// no song. The table ends with the ARM9 binary.
void TestTableStarts()
{
    constexpr uint32_t kTable = kBase + 0x100;
    constexpr uint32_t kNames = kBase + 0x200;
    std::vector<uint8_t> arm9(0x400, 0);
    uint32_t name_at = kNames;
    const auto add = [&](uint32_t number, std::string_view name, uint8_t me)
    {
        std::memcpy(&arm9[name_at - kBase], name.data(), name.size());
        xsflib::StoreLe32(&arm9[kTable + 12 * number - kBase], name_at);
        arm9[kTable + 12 * number + 8 - kBase] = me;
        name_at += static_cast<uint32_t>(name.size()) + 1;
    };
    add(0, "", 0);
    add(1, "TITLE", 0);
    add(2, "FANFARE", 1);
    add(3, "=SND_BGM_M_SOLO", 0);
    add(4, "TITLE", 0);

    DseCode code = CallsTestCode(false);
    code.bgm_format = "mid/SND_BGM_M_%s.SMD";
    code.me_format = "mid/SND_ME_M_%s.SMD";
    code.marked_format = "mid/%s.SMD";
    code.song_table = kTable;
    code.song_count = 5;
    const std::vector<uint8_t> image = twosf::testing::ImageWithArm9(arm9);
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);
    const std::map<std::string, DseStart> starts = twosf::rip::DseTableStarts(rom, code);

    Check(starts == (std::map<std::string, DseStart>{{"mid/SND_BGM_M_TITLE.SMD", {.number = 1}},
                                                     {"mid/SND_ME_M_FANFARE.SMD", {.number = 2}},
                                                     {"mid/SND_BGM_M_SOLO.SMD", {.number = 3}}}),
          "the paths of music, of a short piece and of a marked name, each by its first number");

    code.song_count = 100;
    Check(twosf::rip::DseTableStarts(rom, code).size() == 3, "the table ends with the ARM9 binary");
}

// A table of inline names: each entry is the song's name in 8 bytes, with a zero after a shorter name. An empty entry
// gives no song, and a name that fills its entry has no zero.
void TestInlineNames()
{
    constexpr uint32_t kTable = kBase + 0x100;
    constexpr uint32_t kNameBytes = 8;
    std::vector<uint8_t> arm9(0x200, 0);
    const auto add = [&](uint32_t number, std::string_view name)
    {
        std::memcpy(&arm9[kTable + kNameBytes * number - kBase], name.data(), name.size());
    };
    add(1, "BGM01");
    add(2, "ADV_LONG");
    add(3, "BGM01");

    DseCode code = CallsTestCode(false);
    code.bgm_format = "BGM/%s.SMD";
    code.song_table = kTable;
    code.song_count = 4;
    code.song_name_bytes = kNameBytes;
    const std::vector<uint8_t> image = twosf::testing::ImageWithArm9(arm9);
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);

    Check(twosf::rip::DseTableStarts(rom, code) ==
              (std::map<std::string, DseStart>{{"BGM/BGM01.SMD", {.number = 1}}, {"BGM/ADV_LONG.SMD", {.number = 2}}}),
          "the paths of the names, each by its first number");

    code.song_count = 0x100;
    Check(twosf::rip::DseTableStarts(rom, code).size() == 2, "the table of names ends with the ARM9 binary");
}

// A pack of members: at 0x14 the number of words of the table at 0x20, and for each member an offset from 0x10 and a
// size. The game's table of songs gives each song's member in the low 16 bits of an entry's first word.
void TestMembers()
{
    std::vector<uint8_t> pack(0x100, 0);
    xsflib::StoreLe32(&pack[0x14], 6);
    for (uint32_t member = 0; member < 3; member++)
    {
        xsflib::StoreLe32(&pack[0x20 + 8 * member], 0x30 + 0x20 * member);
        xsflib::StoreLe32(&pack[0x24 + 8 * member], 0x10);
    }

    Check(twosf::rip::DseMembersEnd(pack) == 0x38U, "the table of three members ends at 0x38");
    Check(twosf::rip::DseMemberAt(pack, 0x40) == 0U && twosf::rip::DseMemberAt(pack, 0x80) == 2U,
          "a member by its offset from the pack's start");
    Check(!twosf::rip::DseMemberAt(pack, 0x30) && !twosf::rip::DseMemberAt(pack, 0x48),
          "no member at an offset that starts none");

    xsflib::StoreLe32(&pack[0x14], 7);
    Check(twosf::rip::DseMembersEnd(pack) == 0x38U, "a part of an entry ends the table");
    xsflib::StoreLe32(&pack[0x14], 0x10000);
    Check(twosf::rip::DseMembersEnd(pack) == 0x100U, "the table ends with the pack");
    Check(twosf::rip::DseMembersEnd(std::span(pack).first(0x1C)) == 0U &&
              !twosf::rip::DseMemberAt(std::span(pack).first(0x1C), 0x40),
          "a pack shorter than its header has no table");

    constexpr uint32_t kTable = kBase + 0x100;
    std::vector<uint8_t> arm9(0x200, 0);
    for (const auto& [number, word] : {std::pair{1U, 0x00050002U}, std::pair{2U, 1U}, std::pair{3U, 0x00010002U}})
    {
        xsflib::StoreLe32(&arm9[kTable + 12 * number - kBase], word);
    }
    DseCode code = CallsTestCode(false);
    code.song_table = kTable;
    code.song_count = 0x100;
    const std::vector<uint8_t> image = twosf::testing::ImageWithArm9(arm9);
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);

    Check(twosf::rip::DseMemberNumbers(rom, code) == (std::map<uint32_t, uint32_t>{{2, 1}, {1, 2}}),
          "each member's first song, with no song for an entry of 0, up to the ARM9 binary's end");
}

// A DARC archive of three members: each member's range, its header included, from the word for it, and a member by
// the offset of its data.
void TestDarc()
{
    constexpr uint32_t kHeader = 16;
    std::vector<uint8_t> pack(0x100, 0);
    xsflib::StoreLe32(pack.data(), 0x43524144); // "DARC"
    xsflib::StoreLe32(&pack[4], 3);
    const std::array<std::pair<uint32_t, uint32_t>, 3> members = {{{0x20, 0x10}, {0x40, 0x0}, {0x60, 0x20}}};
    for (uint32_t k = 0; k < members.size(); k++)
    {
        xsflib::StoreLe32(&pack[8 + 4 * k], members[k].first - (12 + 4 * k));
        xsflib::StoreLe32(&pack[members[k].first + 4], members[k].second);
    }

    using Ranges = std::vector<std::pair<uint32_t, uint32_t>>;
    Check(twosf::rip::DseDarcMembers(pack) == Ranges{{0x20, 0x40}, {0x40, 0x50}, {0x60, 0x90}},
          "each member from its header to the end of its data");
    Check(twosf::rip::DseDarcMemberAt(pack, 0x30) == 0U && twosf::rip::DseDarcMemberAt(pack, 0x70) == 2U,
          "a member by the offset of its data");
    Check(!twosf::rip::DseDarcMemberAt(pack, 0x20) && !twosf::rip::DseDarcMemberAt(pack, 0x34),
          "no member at an offset where no member's data start");

    std::vector<uint8_t> other = pack;
    other[0] = 'X';
    Check(twosf::rip::DseDarcMembers(other).empty(), "no members in a pack that isn't an archive");
    other = pack;
    xsflib::StoreLe32(&other[0x60 + 4], 0x91);
    Check(twosf::rip::DseDarcMembers(other).empty(), "no members where a member's data end outside the pack");
    other = pack;
    xsflib::StoreLe32(&other[4], 0x100);
    Check(twosf::rip::DseDarcMembers(other).empty() && !twosf::rip::DseDarcMemberAt(other, 0x30),
          "no members where the table of words ends outside the pack");
    Check(twosf::rip::DseDarcMembers(std::span(pack).first(0x60 + kHeader - 1)).empty(),
          "no members where a member's header ends outside the pack");
}

// A pack that is a cartridge image: the files directly in the folder, by their names without the extension and their
// ranges in the pack. The number of each song is a prefix's base and the number after the prefix.
void TestRomPack()
{
    ntdlib::testing::nds_files::Image spec;
    spec.files = {{"data/snd/bg/F01.mus", std::vector<uint8_t>(0x30, 1)},
                  {"data/snd/bg/D12.mus", std::vector<uint8_t>(0x50, 2)},
                  {"data/snd/bg/sub/F02.mus", std::vector<uint8_t>(0x10, 3)},
                  {"data/snd/se/F03.mus", std::vector<uint8_t>(0x10, 4)}};
    const std::vector<uint8_t> pack = ntdlib::testing::nds_files::BuildImage(spec).first;
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(pack);
    const auto range = [&rom](std::string_view path)
    {
        const auto file = std::ranges::find(rom.Files(), path, &ntdlib::ntr::RomFile::path);
        return std::pair{file->start, file->end};
    };

    const std::vector<twosf::rip::DseRomEntry> entries = twosf::rip::DseRomEntries(pack, "data/snd/bg/");
    std::map<std::string, std::pair<uint32_t, uint32_t>> found;
    for (const twosf::rip::DseRomEntry& entry : entries)
    {
        found[entry.name] = {entry.first, entry.end};
    }
    Check(found == (std::map<std::string, std::pair<uint32_t, uint32_t>>{{"F01", range("data/snd/bg/F01.mus")},
                                                                         {"D12", range("data/snd/bg/D12.mus")}}),
          "the files directly in the folder, by name, at their ranges");
    Check(twosf::rip::DseRomEntries(std::vector<uint8_t>(0x100, 0xFF), "data/snd/bg/").empty(),
          "no files in a pack that isn't a cartridge image");

    constexpr twosf::rip::DseNameNumber kNames[] = {{"F", 0}, {"D", 1000}};
    DseCode code = CallsTestCode(false);
    code.name_numbers = kNames;
    Check(twosf::rip::DseNameNumberOf(code, "F01") == 1U && twosf::rip::DseNameNumberOf(code, "D12") == 1012U &&
              twosf::rip::DseNameNumberOf(code, "D0000") == 1000U,
          "the prefix's base and the number after it");
    Check(!twosf::rip::DseNameNumberOf(code, "F") && !twosf::rip::DseNameNumberOf(code, "F12345") &&
              !twosf::rip::DseNameNumberOf(code, "F0a") && !twosf::rip::DseNameNumberOf(code, "B01"),
          "no number without digits, with five digits, with another character or with another prefix");
}

// How the image of DseWithBackupImage gives the song's start its volume: in r1, or in the 16 bits at 0xC of
// parameters that r1 points to (DseCode::volume_in_parameters). `changes` gives the volume of each call of DSE's change
// of the song's parameters after the start, in the same place.
struct StartVolume
{
    uint32_t volume = 127;
    bool in_parameters = false;
    std::vector<uint32_t> changes = {};
};

// The state at `level`, with no change.
ntdlib::dse::SongState Level(int level)
{
    return {.suffix = "", .level = level};
}

// An image whose boot reaches the hook only with a backup memory. From the hook it calls DSE's load of a bank and of a
// song, each with the file's bytes, then the song's start with `start`'s volume and each of its changes, with `stops`
// the song's stop, and halts. `code` gets the addresses, and `bank` and `song` the bytes. `arm7` is the ARM7 binary.
std::vector<uint8_t> DseWithBackupImage(DseCode& code, std::vector<uint8_t>& bank, std::vector<uint8_t>& song,
                                        std::vector<uint8_t> arm7 = twosf::testing::Halts(), StartVolume start = {},
                                        bool stops = false)
{
    constexpr uint32_t kBank = twosf::testing::kArm9Base + 0x400;
    constexpr uint32_t kSong = kBank + 0x40;
    constexpr uint32_t kParameters = kSong + 0x40;
    constexpr uint32_t kParameterBytes = 16;
    bank.assign(0x40, 0xB0);
    song.assign(0x40, 0x50);

    arm::ArmAssembler a(twosf::testing::kArm9Base);
    const int halt = a.NewLabel();
    const int hook = a.NewLabel();
    const int load_bank = a.NewLabel();
    const int load_song = a.NewLabel();
    const int start_song = a.NewLabel();
    const int stop_song = a.NewLabel();
    const int set_song = a.NewLabel();
    twosf::testing::EmitBackupCheck(a, halt);
    a.Bind(hook);
    a.LoadConstant(0, kBank);
    a.Bl(load_bank);
    a.LoadConstant(0, kSong);
    a.Bl(load_song);
    a.LoadConstant(1, start.in_parameters ? kParameters : start.volume);
    a.Bl(start_song);
    for (std::size_t i = 0; i < start.changes.size(); i++)
    {
        const auto parameters = kParameters + kParameterBytes * static_cast<uint32_t>(i + 1);
        a.LoadConstant(1, start.in_parameters ? parameters : start.changes[i]);
        a.Bl(set_song);
    }
    if (stops)
    {
        a.Bl(stop_song);
    }
    twosf::testing::EmitHaltForever(a, halt);
    for (const int routine : {load_bank, load_song, start_song, stop_song, set_song})
    {
        a.Bind(routine);
        a.Bx(14);
    }

    twosf::testing::EndCodeAt(a, kBank);
    for (const std::vector<uint8_t>* file : {&bank, &song})
    {
        for (std::size_t i = 0; i < file->size(); i += 4)
        {
            a.Word(xsflib::LoadLe32(&(*file)[i]));
        }
    }
    std::vector<uint32_t> volumes = {start.volume};
    volumes.insert(volumes.end(), start.changes.begin(), start.changes.end());
    for (const uint32_t volume : volumes)
    {
        a.Word(0);
        a.Word(0);
        a.Word(0);
        a.Word(volume);
    }

    code.hook = a.Address(hook);
    code.load_bank = a.Address(load_bank);
    code.load_song = a.Address(load_song);
    code.start_song = a.Address(start_song);
    code.stop_song = a.Address(stop_song);
    code.set_song = a.Address(set_song);
    code.volume_in_parameters = start.in_parameters;

    return twosf::testing::ImageWithArm9(a.Finish(), {}, std::move(arm7));
}

// BootTo and RunDseMini boot with the backup memory that they're given: the image reaches the hook, and DSE's
// loads and start, only with one.
void TestBackup()
{
    DseCode code;
    std::vector<uint8_t> bank;
    std::vector<uint8_t> song;
    const std::vector<uint8_t> image = DseWithBackupImage(code, bank, song);
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);

    const twosf::rip::HookBoot with = twosf::rip::BootTo(rom, code.hook, twosf::testing::TestBackup());
    const twosf::rip::HookBoot without = twosf::rip::BootTo(rom, code.hook, {});
    const twosf::rip::DseMiniRun run_with =
        twosf::rip::RunDseMini(image, code, 0, bank, song, {}, 1.0, twosf::testing::TestBackup());
    const twosf::rip::DseMiniRun run_without = twosf::rip::RunDseMini(image, code, 0, bank, song, {}, 1.0, {});

    Check(with.reached, "the boot with a backup memory reaches the hook");
    Check(!without.reached, "the boot without one doesn't");
    Check(!run_with.error, "the run with a backup memory loads and starts the song: " + run_with.error.value_or(""));
    Check(run_without.error == "the boot doesn't reach the hook within 1 s", "the run without one doesn't");
}

// RunDseMini follows the sound unit's output from the song's start (DseMiniRun::sounded). An ARM7 that plays a tone
// makes it change. An ARM7 that halts leaves it at its first value.
void TestSounded()
{
    DseCode code;
    std::vector<uint8_t> bank;
    std::vector<uint8_t> song;
    const std::vector<uint8_t> tone = DseWithBackupImage(code, bank, song, twosf::testing::ToneArm7());
    const std::vector<uint8_t> quiet = DseWithBackupImage(code, bank, song);

    const twosf::rip::DseMiniRun sounds =
        twosf::rip::RunDseMini(tone, code, 0, bank, song, {}, 1.0, twosf::testing::TestBackup());
    const twosf::rip::DseMiniRun silent =
        twosf::rip::RunDseMini(quiet, code, 0, bank, song, {}, 1.0, twosf::testing::TestBackup());

    Check(!sounds.error && sounds.sounded, "the run with a tone sounds: " + sounds.error.value_or(""));
    Check(!silent.error && !silent.sounded, "the run with a halted ARM7 stays silent: " + silent.error.value_or(""));
}

// RunDseMini checks the volume that DSE's start of the song takes against the level of the mini's state: level × 127
// >> 8, in r1 or in the parameters that r1 points to. A level of 180 gives 89, and full level 127. The check fails a
// song started too quiet for its level and a song started too loud for it.
void TestStartVolume()
{
    for (const bool in_parameters : {false, true})
    {
        DseCode lowered_code;
        DseCode full_code;
        std::vector<uint8_t> bank;
        std::vector<uint8_t> song;
        const std::vector<uint8_t> lowered = DseWithBackupImage(lowered_code, bank, song, twosf::testing::Halts(),
                                                                {.volume = 89, .in_parameters = in_parameters});
        const std::vector<uint8_t> full = DseWithBackupImage(full_code, bank, song, twosf::testing::Halts(),
                                                             {.volume = 127, .in_parameters = in_parameters});

        const twosf::rip::DseMiniRun lowered_at_level =
            twosf::rip::RunDseMini(lowered, lowered_code, 0, bank, song, Level(180), 1.0, twosf::testing::TestBackup());
        const twosf::rip::DseMiniRun lowered_at_full =
            twosf::rip::RunDseMini(lowered, lowered_code, 0, bank, song, {}, 1.0, twosf::testing::TestBackup());
        const twosf::rip::DseMiniRun full_at_level =
            twosf::rip::RunDseMini(full, full_code, 0, bank, song, Level(180), 1.0, twosf::testing::TestBackup());
        const twosf::rip::DseMiniRun full_at_full =
            twosf::rip::RunDseMini(full, full_code, 0, bank, song, {}, 1.0, twosf::testing::TestBackup());

        const std::string where = in_parameters ? " (in the parameters)" : " (in r1)";
        Check(!lowered_at_level.error,
              "89 is the volume of the level 180" + where + ": " + lowered_at_level.error.value_or(""));
        Check(lowered_at_full.error == "DSE starts the song at the volume 89, and the level 256 gives 127",
              "89 isn't the volume of full level" + where + ": " + lowered_at_full.error.value_or(""));
        Check(!full_at_full.error, "127 is the volume of full level" + where + ": " + full_at_full.error.value_or(""));
        Check(full_at_level.error == "DSE starts the song at the volume 127, and the level 180 gives 89",
              "127 isn't the volume of the level 180" + where + ": " + full_at_level.error.value_or(""));
        Check(lowered_at_level.volume == 89 && lowered_at_full.volume == 89 && full_at_full.volume == 127 &&
                  full_at_level.volume == 127,
              "the run gives the volume of DSE's start, whatever the check finds" + where);
    }
}

// For a state with a change, RunDseMini checks DSE's start of the song against the level before the change, and the
// last of DSE's changes of the song's parameters, one for each of the change's frames, against the state's level. The
// state of the level 123 after a change from 156 starts at 77 and goes to 61 (over two frames, through 70). The check
// fails a change that ends elsewhere, one that DSE doesn't make as often as the change has frames, and a start at
// another volume. The run gives the volume after the change.
void TestChangeVolume()
{
    const auto state = [](int frames)
    {
        return ntdlib::dse::SongState{.suffix = "", .level = 123, .change = ntdlib::dse::StartChange{156, 102, frames}};
    };
    for (const bool in_parameters : {false, true})
    {
        DseCode changed_code;
        DseCode unchanged_code;
        DseCode full_code;
        std::vector<uint8_t> bank;
        std::vector<uint8_t> song;
        const std::vector<uint8_t> changed =
            DseWithBackupImage(changed_code, bank, song, twosf::testing::Halts(),
                               {.volume = 77, .in_parameters = in_parameters, .changes = {70, 61}});
        const std::vector<uint8_t> unchanged = DseWithBackupImage(unchanged_code, bank, song, twosf::testing::Halts(),
                                                                  {.volume = 77, .in_parameters = in_parameters});
        const std::vector<uint8_t> full = DseWithBackupImage(full_code, bank, song, twosf::testing::Halts(),
                                                             {.volume = 127, .in_parameters = in_parameters});

        const twosf::rip::DseMiniRun two =
            twosf::rip::RunDseMini(changed, changed_code, 0, bank, song, state(2), 1.0, twosf::testing::TestBackup());
        const twosf::rip::DseMiniRun one =
            twosf::rip::RunDseMini(changed, changed_code, 0, bank, song, state(1), 1.0, twosf::testing::TestBackup());
        const twosf::rip::DseMiniRun three =
            twosf::rip::RunDseMini(changed, changed_code, 0, bank, song, state(3), 1.0, twosf::testing::TestBackup());
        const twosf::rip::DseMiniRun none = twosf::rip::RunDseMini(unchanged, unchanged_code, 0, bank, song, state(1),
                                                                   1.0, twosf::testing::TestBackup());
        const twosf::rip::DseMiniRun loud =
            twosf::rip::RunDseMini(full, full_code, 0, bank, song, state(1), 1.0, twosf::testing::TestBackup());

        const std::string where = in_parameters ? " (in the parameters)" : " (in r1)";
        Check(!two.error && two.volume == 61,
              "the change over two frames reaches the state's volume" + where + ": " + two.error.value_or(""));
        Check(one.error == "The change gives the song the volume 70, and the level 123 gives 61",
              "a change over one frame ends at 70" + where + ": " + one.error.value_or(""));
        Check(three.error == "DSE changes the song's volume 2 of 3 times within 1 s of its start",
              "DSE changes the volume twice, not three times" + where + ": " + three.error.value_or(""));
        Check(none.error == "DSE changes the song's volume 0 of 1 times within 1 s of its start",
              "a start without the change" + where + ": " + none.error.value_or(""));
        Check(loud.error == "DSE starts the song at the volume 127, and the level 156 gives 77",
              "the start is checked against the level before the change" + where + ": " + loud.error.value_or(""));
    }
}

// RunDseMini gives the time from the song's start to the game's stop of it (DseMiniRun::stop_ms), and nothing where the
// game leaves the song alone. A stop isn't an error of the run: whether it fails the mini depends on the song's end.
void TestStop()
{
    DseCode stopping_code;
    DseCode playing_code;
    std::vector<uint8_t> bank;
    std::vector<uint8_t> song;
    const std::vector<uint8_t> stopping =
        DseWithBackupImage(stopping_code, bank, song, twosf::testing::Halts(), {}, true);
    const std::vector<uint8_t> playing = DseWithBackupImage(playing_code, bank, song);

    const twosf::rip::DseMiniRun stopped =
        twosf::rip::RunDseMini(stopping, stopping_code, 0, bank, song, {}, 1.0, twosf::testing::TestBackup());
    const twosf::rip::DseMiniRun plays =
        twosf::rip::RunDseMini(playing, playing_code, 0, bank, song, {}, 1.0, twosf::testing::TestBackup());

    Check(!stopped.error && stopped.stop_ms && *stopped.stop_ms < 1.0,
          "the game stops the song at once: " + stopped.error.value_or(""));
    Check(!plays.error && !plays.stop_ms, "the game leaves the song alone: " + plays.error.value_or(""));
}

} // namespace

int main()
{
    TestWords();
    TestSameFiles();
    TestSameStart();
    TestStarts();
    TestPackStarts();
    TestPackEntry();
    TestOffsetEntry();
    TestTable();
    TestTableStarts();
    TestInlineNames();
    TestMembers();
    TestDarc();
    TestRomPack();
    TestBackup();
    TestSounded();
    TestStartVolume();
    TestChangeVolume();
    TestStop();

    return xsflib::testing::Summary("dse_set");
}
