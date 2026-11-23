// SPDX-License-Identifier: MIT

#include "rip/game_streams.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/ntr/code.h"
#include "ntdlib/ntr/rom.h"
#include "xsflib/bytes.h"
#include "xsflib/text.h"

#include "rip/arm_code.h"

namespace twosf::rip
{

namespace
{

namespace ntr = ntdlib::ntr;

// A game in the table: its play routine, its frame routine, the number of its first stream, the file of the stream
// records, and the instruction words that FindGameStreams checks before it uses the entry.
struct GameStreamEntry
{
    std::string_view game_code;
    uint32_t play = 0;
    uint32_t frame = 0;
    uint32_t first_number = 0;
    std::string_view records;
    std::span<const GameWord> words;
};

// Four Swords Anniversary Edition (USA). The play routine 0x0208D240 compares its number with 490 (the literal at
// 0x0208D4F4, loaded at 0x0208D248) and takes a number from 490 on as a stream. 0x0208D32C and 0x0208D330 add 490 less
// 980 to the number. The result is the stream's index. The routine reads byte 0x12 of the stream's first record
// (0x0208D378): 0 starts one stream object, with the first record. `knife_sound.bin` contains the records.
//
// The play routine loads the stream's whole file. The frame routine 0x0208C534 updates the two stream objects
// (0x0208C048 each), calls the update of the SDK's sound library (0x0208C55C), then checks the objects' channels. The
// game calls it once a frame. A stream in IMA-ADPCM starts its channel in the update of its object.
constexpr std::array kKq9eWords = {
    GameWord{0x0208D248, 0xE59F12A4}, // ldr r1, [pc, #0x2A4]: 490
    GameWord{0x0208D4F4, 0x000001EA}, // 490
    GameWord{0x0208D32C, 0xE2410FF5}, // sub r0, r1, #980
    GameWord{0x0208D378, 0xE5D00012}, // ldrb r0, [r0, #0x12]
    GameWord{0x0208C534, 0xE92D4070}, // push {r4, r5, r6, lr}
    GameWord{0x0208C55C, 0xEB00CB1F}, // bl 0x020BF1E0, the update
};

// The European release (KQ9V): the same code, 0x20 bytes later.
constexpr std::array kKq9vWords = {
    GameWord{0x0208D268, 0xE59F12A4}, GameWord{0x0208D514, 0x000001EA}, GameWord{0x0208D34C, 0xE2410FF5},
    GameWord{0x0208D398, 0xE5D00012}, GameWord{0x0208C554, 0xE92D4070}, GameWord{0x0208C57C, 0xEB00CB1F},
};

constexpr std::array kGameStreamTable = {
    GameStreamEntry{"KQ9E", 0x0208D240, 0x0208C534, 490, "knife_sound.bin", kKq9eWords},
    GameStreamEntry{"KQ9V", 0x0208D260, 0x0208C554, 490, "knife_sound.bin", kKq9vWords},
};

// The records' file: the magic "DtLk", then at 0x10 a table of 4 sections, each with its offset from 0x10 and its size.
// Section 3 contains a pair of records of 0x30 bytes for each stream. The play routine reads the first of a pair when
// its byte 0x12 is 0. The record: the loop's first sample (a word at 0), the samples (a word at 4), the rate in Hz (a
// word at 8), the loop (a byte at 0x14), and the file's name from 0x15, after a backslash.
constexpr std::string_view kRecordsMagic = "DtLk";
constexpr std::size_t kSectionTable = 0x10;
constexpr std::size_t kStreamSection = 3;
constexpr std::size_t kRecordBytes = 0x30;
constexpr std::size_t kLoopStartField = 0x00;
constexpr std::size_t kSamplesField = 0x04;
constexpr std::size_t kRateField = 0x08;
constexpr std::size_t kSecondObjectField = 0x12;
constexpr std::size_t kLoopField = 0x14;
constexpr std::size_t kNameField = 0x15;

// The bytes of a stream file's start that the data of its channel's setup must begin with.
constexpr std::size_t kHeadBytes = 0x200;

// The streams of the records in `bytes`. Nothing, with the reason in `error`, when the records don't parse or name a
// file that the image doesn't have.
std::optional<std::vector<GameStream>> ParseRecords(const ntr::Rom& rom, std::span<const uint8_t> bytes,
                                                    uint32_t first_number, std::string& error)
{
    const std::size_t table_end = kSectionTable + 8 * (kStreamSection + 1);
    if (bytes.size() < table_end || std::string_view(reinterpret_cast<const char*>(bytes.data()), 4) != kRecordsMagic)
    {
        error = "the stream records' file has no section table";
        return std::nullopt;
    }

    const std::size_t section = kSectionTable + xsflib::LoadLe32(&bytes[kSectionTable + 8 * kStreamSection]);
    const std::size_t size = xsflib::LoadLe32(&bytes[kSectionTable + 8 * kStreamSection + 4]);
    if (section + size > bytes.size() || size % (2 * kRecordBytes) != 0)
    {
        error = "the stream records' section ends past the file's end";
        return std::nullopt;
    }

    std::vector<GameStream> streams;
    for (std::size_t at = section; at < section + size; at += 2 * kRecordBytes)
    {
        const std::span<const uint8_t> record = bytes.subspan(at, kRecordBytes);
        if (record[kSecondObjectField] != 0)
        {
            error = "a stream record starts two stream objects";
            return std::nullopt;
        }

        std::string name;
        for (std::size_t i = kNameField; i < kRecordBytes && record[i] != 0; i++)
        {
            name.push_back(static_cast<char>(record[i]));
        }
        if (name.starts_with('\\'))
        {
            name.erase(0, 1);
        }

        const auto file = std::ranges::find(rom.Files(), name, &ntr::RomFile::path);
        if (file == rom.Files().end())
        {
            error = "a stream record names " + name + ", which the image doesn't have";
            return std::nullopt;
        }

        GameStream& stream = streams.emplace_back();
        stream.label = name.substr(0, name.find('.'));
        stream.number = first_number + static_cast<uint32_t>(streams.size() - 1);
        stream.file_id = file->id;
        stream.loop_start = xsflib::LoadLe32(&record[kLoopStartField]);
        stream.samples = xsflib::LoadLe32(&record[kSamplesField]);
        stream.rate = xsflib::LoadLe32(&record[kRateField]);
        stream.loop = record[kLoopField] != 0;
        if (stream.rate == 0 || stream.samples == 0)
        {
            error = "the stream record of " + name + " has no rate or no samples";
            return std::nullopt;
        }
    }

    return streams;
}

} // namespace

std::optional<GameStreams> FindGameStreams(const ntr::Rom& rom, std::string& error)
{
    const auto entry = std::ranges::find(kGameStreamTable, rom.Header().game_code, &GameStreamEntry::game_code);
    if (entry == kGameStreamTable.end())
    {
        return std::nullopt;
    }

    const ntr::LoadedCode arm9 = ntr::Arm9Code(rom);
    if (const std::optional<GameWord> missing = MissingWord(arm9, entry->words))
    {
        error = "the game's play routine differs from the table's at " + xsflib::Hex(missing->address, 8, true);
        return std::nullopt;
    }

    const auto records = std::ranges::find(rom.Files(), entry->records, &ntr::RomFile::path);
    if (records == rom.Files().end())
    {
        error = "the image has no " + std::string(entry->records);
        return std::nullopt;
    }

    std::optional<std::vector<GameStream>> streams =
        ParseRecords(rom, rom.FileData(records->id), entry->first_number, error);
    if (!streams)
    {
        return std::nullopt;
    }

    return GameStreams{.play = entry->play, .frame = entry->frame, .streams = std::move(*streams)};
}

SdatStub BuildGameStreamStub(const SdatCode& code, const GameStreams& streams, const GameStream& stream)
{
    SdatStub stub = BuildCallStub(code, streams.play, stream.number, streams.frame);
    if (stub.bytes.size() > code.stub_end - code.stub)
    {
        throw std::runtime_error("the stub of " + stream.label + " takes " + std::to_string(stub.bytes.size()) +
                                 " bytes, and the decompressor's body has " +
                                 std::to_string(code.stub_end - code.stub));
    }

    return stub;
}

StreamTarget GameStreamTarget(const ntr::Rom& rom, const GameStream& stream, const std::set<uint32_t>& reserved)
{
    // The game's channel plays the file as it stores it: the hardware decodes IMA-ADPCM. The play routine loads the
    // whole file, and the channel's setup gives the record's samples and loop. The lists of the driver's commands count
    // from the hook on, where the stub calls the play routine.
    const ntr::RomFile& file = rom.Files().at(stream.file_id);
    const std::span<const uint8_t> head =
        rom.FileData(stream.file_id).first(std::min<std::size_t>(kHeadBytes, rom.FileData(stream.file_id).size()));
    StreamTarget target;
    target.left_out = reserved;
    target.file_start = file.start;
    target.file_end = file.end;
    target.heads = {{head.begin(), head.end()}, {head.begin(), head.end()}};
    target.channels = 1;
    target.rate = stream.rate;
    target.samples = stream.samples;
    target.loop_start = stream.loop_start;
    target.loop = stream.loop;
    target.commands_after_hook = true;
    target.loop_in_setup = true;

    return target;
}

} // namespace twosf::rip
