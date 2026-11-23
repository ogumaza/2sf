// SPDX-License-Identifier: MIT

// Tests the game streams (rip/game_streams.h) on images that ntdlib's nds_files builds: the table's check of the play
// routine's words, the stream records, the refusals, and the target of a stream's run.

#include "rip/game_streams.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/ntr/rom.h"
#include "ntdlib/testing/nds_files.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "backup_image.h"

namespace
{

namespace nds_files = ntdlib::testing::nds_files;
using Bytes = std::vector<uint8_t>;
using xsflib::testing::Check;

// The words that the table expects of KQ9E's play routine and frame routine, by address.
const std::vector<std::pair<uint32_t, uint32_t>> kPlayWords = {{0x0208D248, 0xE59F12A4}, {0x0208D4F4, 0x000001EA},
                                                               {0x0208D32C, 0xE2410FF5}, {0x0208D378, 0xE5D00012},
                                                               {0x0208C534, 0xE92D4070}, {0x0208C55C, 0xEB00CB1F}};

// A record of the records' file: the loop's start, the samples, the rate, whether the second stream object plays, the
// loop and the file's name.
Bytes Record(uint32_t loop_start, uint32_t samples, uint32_t rate, uint8_t second_object, bool loop,
             const std::string& name)
{
    Bytes record(0x30, 0);
    xsflib::StoreLe32(&record[0x00], loop_start);
    xsflib::StoreLe32(&record[0x04], samples);
    xsflib::StoreLe32(&record[0x08], rate);
    record[0x12] = second_object;
    record[0x14] = loop ? 1 : 0;
    const std::string stored = "\\" + name;
    for (std::size_t i = 0; i < stored.size(); i++)
    {
        record[0x15 + i] = static_cast<uint8_t>(stored[i]);
    }
    return record;
}

// The records' file: the magic, the table of 4 sections at 0x10, and section 3 with a pair of records for each stream.
// The second record of a pair differs only in its loop.
Bytes RecordsFile(const std::vector<Bytes>& firsts)
{
    Bytes file(0x30, 0);
    file[0] = 'D';
    file[1] = 't';
    file[2] = 'L';
    file[3] = 'k';
    xsflib::StoreLe32(&file[0x10 + 8 * 3], 0x20);
    xsflib::StoreLe32(&file[0x10 + 8 * 3 + 4], static_cast<uint32_t>(0x60 * firsts.size()));
    for (const Bytes& first : firsts)
    {
        file.insert(file.end(), first.begin(), first.end());
        Bytes second = first;
        second[0x14] = 0;
        file.insert(file.end(), second.begin(), second.end());
    }
    return file;
}

// An image with the game code `game_code`, an ARM9 binary with the play routine's words, `records` as the records'
// file, and two stream files.
Bytes StreamImage(const std::string& game_code, const Bytes& records,
                  const std::vector<std::pair<uint32_t, uint32_t>>& words = kPlayWords)
{
    Bytes arm9(0x8E000, 0);
    for (const auto& [address, value] : words)
    {
        xsflib::StoreLe32(&arm9[address - twosf::testing::kArm9Base], value);
    }

    nds_files::Image spec;
    spec.game_code = game_code;
    spec.arm9 = std::move(arm9);
    spec.arm7 = twosf::testing::Halts();
    spec.files = {{"knife_sound.bin", records}, {"zelda_tit.wave", Bytes(0x100, 1)}, {"tri_gnd.wave", Bytes(0x80, 2)}};
    return nds_files::BuildImage(spec).first;
}

const Bytes kRecords = RecordsFile(
    {Record(0, 0x49A5C, 16384, 0, true, "zelda_tit.wave"), Record(0x2B878, 0xF78D0, 23802, 0, false, "tri_gnd.wave")});

// The table's game gets its streams in the records' order, from 490, with each one's file and record.
void TestStreams()
{
    const Bytes image = StreamImage("KQ9E", kRecords);
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);
    std::string error;
    const std::optional<twosf::rip::GameStreams> found = twosf::rip::FindGameStreams(rom, error);
    if (!Check(found.has_value() && error.empty(), "KQ9E's streams: " + error) ||
        !Check(found->streams.size() == 2, "two streams"))
    {
        return;
    }

    const twosf::rip::GameStream& first = found->streams[0];
    const twosf::rip::GameStream& second = found->streams[1];
    Check(found->play == 0x0208D240 && found->frame == 0x0208C534, "the play routine and the frame routine");
    Check(first.label == "zelda_tit" && first.number == 490 && first.file_id == rom.Find("zelda_tit.wave")->id,
          "the first stream's label, number and file");
    Check(first.rate == 16384 && first.samples == 0x49A5C && first.loop_start == 0 && first.loop,
          "the first stream's record");
    Check(second.label == "tri_gnd" && second.number == 491 && second.file_id == rom.Find("tri_gnd.wave")->id,
          "the second stream's label, number and file");
    Check(second.rate == 23802 && second.samples == 0xF78D0 && second.loop_start == 0x2B878 && !second.loop,
          "the second stream's record");
}

// A game outside the table has no streams and no error. The table's game is refused when a word of the play routine
// differs, when the records' file has another magic, when a record names a file that the image doesn't have, and when
// a record starts the second stream object.
void TestRefusals()
{
    std::string error;
    const Bytes other = StreamImage("ABCE", kRecords);
    Check(!twosf::rip::FindGameStreams(ntdlib::ntr::Rom::Parse(other), error) && error.empty(),
          "a game outside the table");

    std::vector<std::pair<uint32_t, uint32_t>> words = kPlayWords;
    words[1].second = 0x1EB;
    const Bytes changed = StreamImage("KQ9E", kRecords, words);
    Check(!twosf::rip::FindGameStreams(ntdlib::ntr::Rom::Parse(changed), error) &&
              error == "the game's play routine differs from the table's at 0x0208D4F4",
          "a changed word: " + error);

    error.clear();
    Bytes other_magic = kRecords;
    other_magic[0] = 'X';
    const Bytes no_table = StreamImage("KQ9E", other_magic);
    Check(!twosf::rip::FindGameStreams(ntdlib::ntr::Rom::Parse(no_table), error) &&
              error == "the stream records' file has no section table",
          "another magic: " + error);

    error.clear();
    const Bytes missing = StreamImage("KQ9E", RecordsFile({Record(0, 0x100, 16384, 0, true, "yum_fie.wave")}));
    Check(!twosf::rip::FindGameStreams(ntdlib::ntr::Rom::Parse(missing), error) &&
              error == "a stream record names yum_fie.wave, which the image doesn't have",
          "a missing file: " + error);

    error.clear();
    const Bytes stereo = StreamImage("KQ9E", RecordsFile({Record(0, 0x100, 16384, 1, true, "zelda_tit.wave")}));
    Check(!twosf::rip::FindGameStreams(ntdlib::ntr::Rom::Parse(stereo), error) &&
              error == "a stream record starts two stream objects",
          "the second stream object: " + error);
}

// A game stream's target for its mini's run: the stream file's bytes in the image, its head of at most 0x200 bytes for
// its one channel, the record's rate, samples and loop, the reserved files, the commands from the hook on, and the
// check of the setup's loop.
void TestTarget()
{
    const Bytes image = StreamImage("KQ9E", kRecords);
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);
    std::string error;
    const std::optional<twosf::rip::GameStreams> found = twosf::rip::FindGameStreams(rom, error);
    if (!Check(found.has_value() && found->streams.size() == 2, "KQ9E's streams: " + error))
    {
        return;
    }

    const ntdlib::ntr::RomFile& file = *rom.Find("tri_gnd.wave");
    const std::set<uint32_t> reserved = {7, 9};

    const twosf::rip::StreamTarget target = twosf::rip::GameStreamTarget(rom, found->streams[1], reserved);

    Check(target.file_start == file.start && target.file_end == file.end, "the stream file's bytes in the image");
    Check(target.heads == std::vector<Bytes>{Bytes(0x80, 2), Bytes(0x80, 2)} && target.channels == 1,
          "the head of the one channel");
    Check(target.rate == 23802.0 && target.samples == 0xF78D0 && target.loop_start == 0x2B878 && !target.loop,
          "the record's rate, samples and loop");
    Check(target.left_out == reserved && target.commands_after_hook, "the reserved files and the commands");
    Check(target.loop_in_setup, "the channel's setup gives the samples and the loop");
}

} // namespace

int main()
{
    TestStreams();
    TestRefusals();
    TestTarget();
    return xsflib::testing::Summary("game streams");
}
