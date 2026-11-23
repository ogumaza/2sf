// SPDX-License-Identifier: MIT

#include "rip/program_set.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/ntr/code.h"
#include "ntdlib/ntr/compact_image.h"
#include "ntdlib/ntr/rip.h"
#include "ntdlib/sdat/formats.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "rip/game_start_table.h"

namespace twosf::rip
{

namespace
{

namespace ntr = ntdlib::ntr;
namespace sdat = ntdlib::sdat;

// A NARC: its header's size field, then the sections: the members' table (BTAF: its size, the count of members, then
// each member's start and end in the data), the names (BTNF) and the data (GMIF), each with its signature and size.
constexpr uint32_t kNarcSizeField = 0x08;
constexpr uint32_t kNarcHeaderSizeField = 0x0C;
constexpr uint32_t kSectionSizeField = 4;
constexpr uint32_t kSectionHeader = 8;
constexpr uint32_t kMemberCountField = 8;
constexpr uint32_t kMembersField = 12;
constexpr uint32_t kMemberBytes = 8;

// Where a NARC's sections are.
struct NarcLayout
{
    uint32_t table = 0; // BTAF
    uint32_t count = 0;
    uint32_t names = 0; // BTNF
    uint32_t data = 0;  // GMIF
};

uint16_t Load16(std::span<const uint8_t> bytes, std::size_t at)
{
    if (at + 2 > bytes.size())
    {
        throw xsflib::FormatError("the NARC ends inside a field");
    }

    return xsflib::LoadLe16(&bytes[at]);
}

uint32_t Load32(std::span<const uint8_t> bytes, std::size_t at)
{
    if (at + 4 > bytes.size())
    {
        throw xsflib::FormatError("the NARC ends inside a field");
    }

    return xsflib::LoadLe32(&bytes[at]);
}

// The layout of `narc`, with each section's signature checked. Every section is inside the file after the one before,
// the members' table is inside its section, and the data's header is inside the file.
NarcLayout ReadNarc(std::span<const uint8_t> narc)
{
    const auto signature = [&narc](std::size_t at, std::string_view expected)
    {
        if (at + 4 > narc.size() ||
            !std::ranges::equal(narc.subspan(at, 4), expected, [](uint8_t a, char b) { return a == uint8_t(b); }))
        {
            throw xsflib::FormatError("not a NARC: no " + std::string(expected) + " where the format puts it");
        }
    };

    // The end of the section at `at`, from its size.
    const auto end_of = [&narc](uint32_t at)
    {
        const uint64_t end = uint64_t{at} + Load32(narc, at + kSectionSizeField);
        if (end > narc.size())
        {
            throw xsflib::FormatError("a section of the NARC ends past the file");
        }

        return static_cast<uint32_t>(end);
    };

    signature(0, "NARC");
    NarcLayout layout;
    layout.table = Load16(narc, kNarcHeaderSizeField);
    signature(layout.table, "BTAF");
    layout.count = Load32(narc, layout.table + kMemberCountField) & 0xFFFF;
    layout.names = end_of(layout.table);
    if (layout.table + kMembersField + uint64_t{kMemberBytes} * layout.count > layout.names)
    {
        throw xsflib::FormatError("the NARC's table of members ends past its section");
    }

    signature(layout.names, "BTNF");
    layout.data = end_of(layout.names);
    signature(layout.data, "GMIF");
    if (layout.data + uint64_t{kSectionHeader} > narc.size())
    {
        throw xsflib::FormatError("the NARC ends inside the data's header");
    }

    return layout;
}

// A program in the table of programs: the games that send it, its file in their file systems, its starts (a state of
// each piece with no suffix, the states of its variants, the patches and the words, GameStartEntry), the table of its
// embedded archives (each archive's start and end in the ARM9 binary), the embedded archive and its member that holds
// the first piece's sound archive, the embedded archives `free_first` to `free_last` that no boot and no mini reads,
// and the game's file that it sends, a NARC whose member `sent_member` holds the second piece's sound archive. Both
// pieces play `sequence`.
struct ProgramEntry
{
    std::span<const std::string_view> game_codes;
    std::string_view path;
    const GameStartEntry& start;
    uint32_t archive_table = 0;
    uint32_t box_archive = 0;
    uint32_t box_member = 0;
    std::string_view box_file; // the game's copy of the embedded archive, for the profile
    uint32_t free_first = 0;
    uint32_t free_last = 0;
    std::string_view sent_path;
    uint32_t sent_member = 0;
    uint32_t sequence = 0;
};

// `calls` followed by `call`.
template <std::size_t N>
constexpr std::array<StubCall, N + 1> Then(const StubCall (&calls)[N], const StubCall& call)
{
    std::array<StubCall, N + 1> all{};
    std::ranges::copy(calls, all.begin());
    all[N] = call;

    return all;
}

// The arguments of a tuning routine's call that leave a value alone (-1), and that set every track.
constexpr uint32_t kLeave = 0xFFFFFFFF;
constexpr uint32_t kEveryTrack = 0xFFFF;

// Poké Transfer's download-play program of Pokémon Black and White (USA), dl_rom/child_r_eng.srl, ARM code. Both games
// send the same program. Its ARM9 binary loads at 0x02004000 and contains 15 NARCs, listed at 0x02064100. Archive 11
// holds the game's a/1/4/0, byte for byte. The box screen's process start (0x0201534C) makes heap 93 of 0xF8000 bytes
// in heap 1 (mov r4, #93 at 0x02015350) and calls the box's set-up 0x02013AF4 with its work. The set-up loads member 6
// of archive 11 into heap 93 (0x02035A10 at 0x02013B64, mov r0, #11 at 0x02013B58, mov r1, #6 at 0x02013B5C), starts
// the sound with it (0x02010AB8 at 0x02013B78) and starts sequence 1000, SEQ_BGM_PALPARK_BOX, through the music start
// 0x02010D40(sequence, track mask) at 0x02013C40 with 0xFFFF. The music start stops the handle, loads the piece and
// starts it with NNS_SndArcPlayerStartSeqEx at 0x02010D08. The sound's start-up 0x02010AB8(archive) makes the sound
// heap (0x0204329C at 0x02010AC8) and calls NNS_SndArcInitOnMemory (0x02042DF8 at 0x02010ADC).
//
// The minigame's archive is the game's a/1/4/2. The game sends it whole: the program contains no copy. The box opens a
// handle on the received NARC in memory with 0x02034EC4(address, size, heap 93) at 0x02014530. That routine allocates
// the handle in the heap and calls FS_CreateFileFromMemory (0x02048E94 at 0x02034F04). The minigame's process start
// 0x0201A048 makes heap 95 of 0x60000 bytes in the box's heap (0x020231EC at 0x0201A064, mov r4, #95 at 0x0201A054, mov
// r2, #0x60000 at 0x0201A060) and calls its set-up 0x02018764. The set-up copies member 0 into heap 95
// (0x02035AEC(handle, 0, 0, heap) at 0x02018790: mov r4, #0 at 0x02018770, mov r1, r4 and mov r2, r4 at 0x02018788 and
// 0x0201878C; r2 0 copies the member as stored), starts the sound with the copy (0x02010AB8 at 0x020187AC) and starts
// sequence 1000, SEQ_BGM_PALPARK_GAME, with 0xFFFF (0x02010D40 at 0x02018904, mov r0, #1000 at 0x02018900, the literal
// 0xFFFF at 0x02018934).
//
// The set's library writes the minigame's NARC over archives 7 to 10, from 0x020CAE30 to 0x020E5AD4. No boot and no
// mini reads those archives. The second piece's handle covers those bytes. The patch makes the boot's start of the
// box's piece at 0x02013C40 a mov r0, r0: the first piece's mini starts the piece from the hook, as the set-up would.
//
// The minigame's countdown 0x02019C8C counts the round's timer at work+0x1A4 down once a frame, from 7200 frames (the
// literal 0x1C20 at 0x02019C84). When the timer falls from above 1200 to 1200, 20 s before the round's end, and no
// slow-down runs (work+0x1A6 is 0), the countdown calls the tuning routine 0x02010DA4(tracks, tempo ratio, pitch, pan)
// at 0x02019DF0 with 0xFFFF, 285, 85 and -1. The tests are cmp r5, #1200 at 0x02019DBC and cmp r1, #1200 at 0x02019DCC.
// The arguments are the literals at 0x02019F20 and 0x02019F24, mov r2, #85 at 0x02019DE0 and sub r3, r2, #86 at
// 0x02019DEC. The routine sets the BGM's tempo ratio to 285/256 and the pitch of every track to 85/64 of a semitone
// through the SDK (NNS_SndPlayerSetTempoRatio at 0x02042320, NNS_SndPlayerSetTrackPitch at 0x020422E0). It leaves the
// pan alone. The state stays set to the round's end: the end of a slow-down sets it again (0x02019DB8). The state
// _HURRY of the second piece makes the minigame's calls, then the tuning routine's.
constexpr uint32_t kIrboFreeStart = 0x020CAE30;
constexpr uint32_t kIrboFreeEnd = 0x020E5AD4;
constexpr StubCall kIrboBoxCalls[] = {MakeCall(0x02010D40, {Sound(), Constant(0xFFFF)})};
constexpr StubCall kIrboGameCalls[] = {
    MakeCall(0x020231EC, {Constant(93), Constant(95), Constant(0x60000)}),
    MakeCall(0x02034EC4, {Constant(kIrboFreeStart), Constant(kIrboFreeEnd - kIrboFreeStart), Constant(93)}),
    MakeCall(0x02035AEC, {Result(), Constant(0), Constant(0), Constant(95)}), MakeCall(0x02010AB8, {Result()}),
    MakeCall(0x02010D40, {Sound(), Constant(0xFFFF)})};
constexpr auto kIrboHurryCalls =
    Then(kIrboGameCalls, MakeCall(0x02010DA4, {Constant(kEveryTrack), Constant(285), Constant(85), Constant(kLeave)}));
constexpr GameStateEntry kIrboStates[] = {{"SEQ_BGM_PALPARK_BOX", "", kIrboBoxCalls},
                                          {"SEQ_BGM_PALPARK_GAME", "", kIrboGameCalls},
                                          {"SEQ_BGM_PALPARK_GAME", "_HURRY", kIrboHurryCalls}};
constexpr GamePatch kIrboPatches[] = {{.address = 0x02013C40, .expected = 0xEBFFF43E, .value = 0xE1A00000}};
constexpr GameWord kIrboWords[] = {
    {0x02010D40, 0xE92D4038}, // the music start: stmfd sp!, {r3, r4, r5, lr}
    {0x02010AB8, 0xE92D4010}, // the sound's start-up: stmfd sp!, {r4, lr}
    {0x020231EC, 0xE92D4038}, // the start of a heap: stmfd sp!, {r3, r4, r5, lr}
    {0x02034EC4, 0xE92D4078}, // the handle on a NARC in memory: stmfd sp!, {r3, r4, r5, r6, lr}
    {0x02035AEC, 0xE92D4008}, // the copy of a member: stmfd sp!, {r3, lr}
    {0x02015350, 0xE3A0405D}, // mov r4, #93
    {0x02014530, 0xEB008263}, // bl 0x02034EC4
    {0x0201A054, 0xE3A0405F}, // mov r4, #95
    {0x0201A060, 0xE3A02806}, // mov r2, #0x60000
    {0x02018770, 0xE3A04000}, // mov r4, #0
    {0x02018788, 0xE1A01004}, // mov r1, r4
    {0x0201878C, 0xE1A02004}, // mov r2, r4
    {0x02018790, 0xEB0074D5}, // bl 0x02035AEC
    {0x020187AC, 0xEBFFE0C1}, // bl 0x02010AB8
    {0x02018900, 0xE3A00FFA}, // mov r0, #1000
    {0x02018904, 0xEBFFE10D}, // bl 0x02010D40
    {0x02018934, 0x0000FFFF}, // the track mask
    {0x02013B58, 0xE3A0000B}, // mov r0, #11
    {0x02013B5C, 0xE3A01006}, // mov r1, #6
    {0x02010DA4, 0xE92D41F0}, // the tuning routine: stmfd sp!, {r4, r5, r6, r7, r8, lr}
    {0x02019DBC, 0xE3550E4B}, // cmp r5, #1200
    {0x02019DCC, 0xE3510E4B}, // cmp r1, #1200
    {0x02019DE0, 0xE3A02055}, // mov r2, #85
    {0x02019DEC, 0xE2423056}, // sub r3, r2, #86
    {0x02019DF0, 0xEBFFDBEB}, // bl 0x02010DA4
    {0x02019F20, 0x0000FFFF}, // the tracks
    {0x02019F24, 0x0000011D}, // the tempo ratio
    // The table's words of the free archives' bounds and of archive 11.
    {0x02064138, kIrboFreeStart}, // archive 7's start
    {0x02064154, kIrboFreeEnd},   // archive 10's end
    {0x02064158, 0x020E5AD4},     // archive 11's start
    {0x0206415C, 0x020FC8B8},     // archive 11's end
};
constexpr GameStartEntry kIrboProgramStart = {
    .game_code = "IRBO", .patches = kIrboPatches, .words = kIrboWords, .states = kIrboStates};

// Poké Transfer's download-play program of Pokémon Black 2 and White 2 (USA), dl_rom/child_r_eng.srl: Black's program
// at other addresses, with the heap's and the archive's routines in Thumb code. Its ARM9 binary loads at 0x02000000 and
// lists its 15 NARCs at 0x02057A80. Archive 11 contains the game's a/1/4/0. The box's process start makes heap 93
// (0x02011700) and calls the set-up 0x0200FEA0. The set-up loads member 6 of archive 11 (0x0200FF04, 0x0200FF08),
// starts the sound with it (0x0200C668 at 0x0200FF24) and starts sequence 1000 at 0x0200FFEC through the music start
// 0x0200C8F0. The box opens the received NARC with 0x0202BB88 at 0x020108DC. The minigame's process start 0x020164B0
// makes heap 95 (0x0201FB88, 0x020164BC, 0x020164C8) and calls its set-up 0x02014BCC. That set-up copies member 0 into
// heap 95 (0x0202C358 at 0x02014BF8, 0x02014BD8, 0x02014BF0, 0x02014BF4), starts the sound with the copy (0x0200C668 at
// 0x02014C14) and starts sequence 1000 with 0xFFFF (0x0200C8F0 at 0x02014D6C, 0x02014D68, the literal at 0x02014D9C).
// The library writes the minigame's NARC over archives 7 to 10, from 0x020B7124 to 0x020D2C78, and the patch makes the
// boot's start at 0x0200FFEC a mov r0, r0. The minigame's countdown is 0x020160F4, with Black's tests and arguments
// (0x02016224, 0x02016234, 0x02016248, 0x02016254 and the literals at 0x02016388 and 0x0201638C). It calls the tuning
// routine 0x0200C954 at 0x02016258.
constexpr uint32_t kIreoFreeStart = 0x020B7124;
constexpr uint32_t kIreoFreeEnd = 0x020D2C78;
constexpr StubCall kIreoBoxCalls[] = {MakeCall(0x0200C8F0, {Sound(), Constant(0xFFFF)})};
constexpr StubCall kIreoGameCalls[] = {
    MakeCall(0x0201FB89, {Constant(93), Constant(95), Constant(0x60000)}),
    MakeCall(0x0202BB89, {Constant(kIreoFreeStart), Constant(kIreoFreeEnd - kIreoFreeStart), Constant(93)}),
    MakeCall(0x0202C359, {Result(), Constant(0), Constant(0), Constant(95)}), MakeCall(0x0200C668, {Result()}),
    MakeCall(0x0200C8F0, {Sound(), Constant(0xFFFF)})};
constexpr auto kIreoHurryCalls =
    Then(kIreoGameCalls, MakeCall(0x0200C954, {Constant(kEveryTrack), Constant(285), Constant(85), Constant(kLeave)}));
constexpr GameStateEntry kIreoStates[] = {{"SEQ_BGM_PALPARK_BOX", "", kIreoBoxCalls},
                                          {"SEQ_BGM_PALPARK_GAME", "", kIreoGameCalls},
                                          {"SEQ_BGM_PALPARK_GAME", "_HURRY", kIreoHurryCalls}};
constexpr GamePatch kIreoPatches[] = {{.address = 0x0200FFEC, .expected = 0xEBFFF23F, .value = 0xE1A00000}};
constexpr GameWord kIreoWords[] = {
    {0x0200C8F0, 0xE92D4038}, // the music start: stmfd sp!, {r3, r4, r5, lr}
    {0x0200C668, 0xE92D4010}, // the sound's start-up: stmfd sp!, {r4, lr}
    {0x0201FB88, 0x1C05B538}, // the start of a heap: push {r3, r4, r5, lr}, adds r5, r0, #0
    {0x0202BB88, 0xB081B578}, // the handle on a NARC in memory: push {r3, r4, r5, r6, lr}, sub sp, #4
    {0x0202C358, 0xB082B510}, // the copy of a member: push {r4, lr}, sub sp, #8
    {0x02011700, 0xE3A0405D}, // mov r4, #93
    {0x020108DC, 0xFA006CA9}, // blx 0x0202BB88
    {0x020164BC, 0xE3A0405F}, // mov r4, #95
    {0x020164C8, 0xE3A02806}, // mov r2, #0x60000
    {0x02014BD8, 0xE3A04000}, // mov r4, #0
    {0x02014BF0, 0xE1A01004}, // mov r1, r4
    {0x02014BF4, 0xE1A02004}, // mov r2, r4
    {0x02014BF8, 0xFA005DD6}, // blx 0x0202C358
    {0x02014C14, 0xEBFFDE93}, // bl 0x0200C668
    {0x02014D68, 0xE3A00FFA}, // mov r0, #1000
    {0x02014D6C, 0xEBFFDEDF}, // bl 0x0200C8F0
    {0x02014D9C, 0x0000FFFF}, // the track mask
    {0x0200FF04, 0xE3A0000B}, // mov r0, #11
    {0x0200FF08, 0xE3A01006}, // mov r1, #6
    {0x0200C954, 0xE92D41F0}, // the tuning routine: stmfd sp!, {r4, r5, r6, r7, r8, lr}
    {0x02016224, 0xE3550E4B}, // cmp r5, #1200
    {0x02016234, 0xE3510E4B}, // cmp r1, #1200
    {0x02016248, 0xE3A02055}, // mov r2, #85
    {0x02016254, 0xE2423056}, // sub r3, r2, #86
    {0x02016258, 0xEBFFD9BD}, // bl 0x0200C954
    {0x02016388, 0x0000FFFF}, // the tracks
    {0x0201638C, 0x0000011D}, // the tempo ratio
    // The table's words of the free archives' bounds and of archive 11.
    {0x02057AB8, kIreoFreeStart}, // archive 7's start
    {0x02057AD4, kIreoFreeEnd},   // archive 10's end
    {0x02057AD8, 0x020D2C78},     // archive 11's start
    {0x02057ADC, 0x020E9A5C},     // archive 11's end
};
constexpr GameStartEntry kIreoProgramStart = {
    .game_code = "IREO", .patches = kIreoPatches, .words = kIreoWords, .states = kIreoStates};

constexpr std::string_view kBlackWhite[] = {"IRBO", "IRAO"};
constexpr std::string_view kBlackWhite2[] = {"IREO", "IRDO"};

const ProgramEntry kPrograms[] = {
    {.game_codes = kBlackWhite,
     .path = "dl_rom/child_r_eng.srl",
     .start = kIrboProgramStart,
     .archive_table = 0x02064100,
     .box_archive = 11,
     .box_member = 6,
     .box_file = "a/1/4/0",
     .free_first = 7,
     .free_last = 10,
     .sent_path = "a/1/4/2",
     .sent_member = 0,
     .sequence = 1000},
    {.game_codes = kBlackWhite2,
     .path = "dl_rom/child_r_eng.srl",
     .start = kIreoProgramStart,
     .archive_table = 0x02057A80,
     .box_archive = 11,
     .box_member = 6,
     .box_file = "a/1/4/0",
     .free_first = 7,
     .free_last = 10,
     .sent_path = "a/1/4/2",
     .sent_member = 0,
     .sequence = 1000},
};

// The bytes of `arm9` from `start` up to `end`. `arm9` starts at `base`. Nothing outside it.
std::optional<std::span<uint8_t>> Bytes(std::span<uint8_t> arm9, uint32_t base, uint32_t start, uint32_t end)
{
    if (start < base || end < start || end - base > arm9.size())
    {
        return std::nullopt;
    }

    return arm9.subspan(start - base, end - start);
}

// The label of `sequence` in `archive`, or nothing when the archive has no such sequence.
std::optional<std::string> LabelOf(const sdat::Archive& archive, uint32_t sequence)
{
    const ntr::SequenceRef ref{.index = sequence};
    for (const ntr::rip::Job& job : ntr::rip::SelectSequences(archive, std::regex(".*"), false))
    {
        if (job.sequence == ref)
        {
            return job.label;
        }
    }

    return std::nullopt;
}

// The program of `entry` in `rom`, or nothing with `error` set.
std::optional<Program> ReadProgram(const ntr::Rom& rom, const ProgramEntry& entry, std::string& error)
{
    const ntr::RomFile* file = rom.Find(entry.path);
    const ntr::RomFile* sent = rom.Find(entry.sent_path);
    if (file == nullptr || sent == nullptr)
    {
        error = "the game has no " + std::string(file == nullptr ? entry.path : entry.sent_path);
        return std::nullopt;
    }

    // The program's image as a DS runs it, and its routines.
    Program program{.name = std::string(entry.path.substr(entry.path.rfind('/') + 1))};
    program.name.resize(program.name.rfind('.'));
    const ntr::Rom original = ntr::Rom::Parse(rom.FileData(file->id));
    const std::vector<uint8_t> ds_image = DsImage(original);
    const ntr::Rom program_rom = ntr::Rom::Parse(ds_image);
    std::optional<SdatCode> code = FindTableCode(program_rom, entry.start, error);
    if (!code)
    {
        error = std::string(entry.path) + ": " + error;
        return std::nullopt;
    }
    program.code = std::move(*code);

    // The ARM9 binary decompressed, the first piece's archive in it and the free archives' bytes. FindTableCode has
    // checked the table's words of the archives that these are.
    ntr::CompactParts parts{.arm9 = ntr::UncompressedArm9(program_rom)};
    const uint32_t base = program_rom.Header().arm9.ram_address;
    const auto word = [&](uint32_t address)
    {
        return xsflib::LoadLe32(&parts.arm9.at(address - base));
    };
    const uint32_t box = entry.archive_table + 8 * entry.box_archive;
    const std::optional<std::span<uint8_t>> box_narc = Bytes(parts.arm9, base, word(box), word(box + 4));
    const std::optional<std::span<uint8_t>> free =
        Bytes(parts.arm9, base, word(entry.archive_table + 8 * entry.free_first),
              word(entry.archive_table + 8 * entry.free_last + 4));
    if (!box_narc || !free)
    {
        error = std::string(entry.path) + ": an embedded archive lies outside the ARM9 binary";
        return std::nullopt;
    }
    program.image = ntr::WriteCompactImage(program_rom, parts);

    // The pieces: the first from the embedded archive, the second from the sent NARC's archive cut to the sequence's
    // data. The cut NARC goes over the free archives, followed by zeros.
    const ntr::SequenceRef sequence{.index = entry.sequence};
    try
    {
        const std::span<const uint8_t> box_archive = NarcMember(*box_narc, entry.box_member);
        const sdat::Archive sent_archive = sdat::Archive::Parse(NarcMember(rom.FileData(sent->id), entry.sent_member));
        const std::optional<std::vector<uint8_t>> cut = CutArchive(sent_archive, std::span(&sequence, 1), {});
        const std::optional<std::string> box_label = LabelOf(sdat::Archive::Parse(box_archive), entry.sequence);
        const std::optional<std::string> sent_label = LabelOf(sent_archive, entry.sequence);
        if (!cut || !box_label || !sent_label)
        {
            error = std::string(entry.path) + ": a sound archive has no sequence " + std::to_string(entry.sequence) +
                    " with its data";
            return std::nullopt;
        }

        const std::vector<uint8_t> narc = CutNarc(rom.FileData(sent->id), entry.sent_member, *cut);
        if (narc.size() > free->size())
        {
            error = std::string(entry.path) + ": the cut of " + std::string(entry.sent_path) + " takes " +
                    std::to_string(narc.size()) + " bytes, and the free archives have " + std::to_string(free->size());
            return std::nullopt;
        }

        std::ranges::fill(*free, uint8_t{0});
        std::ranges::copy(narc, free->begin());
        program.library_image = ntr::WriteCompactImage(program_rom, parts);
        program.pieces.push_back({.label = *box_label,
                                  .archive = {box_archive.begin(), box_archive.end()},
                                  .sequence = sequence,
                                  .game_code = rom.Header().game_code,
                                  .profile_file = std::string(entry.box_file)});
        program.pieces.push_back({.label = *sent_label,
                                  .archive = *cut,
                                  .sequence = sequence,
                                  .game_code = rom.Header().game_code,
                                  .profile_file = std::string(entry.sent_path)});
    }
    catch (const std::exception& e)
    {
        error = std::string(entry.path) + ": " + e.what();
        return std::nullopt;
    }

    // Each piece's calls are the one state of its label with no suffix.
    for (const ProgramPiece& piece : program.pieces)
    {
        if (PieceState(program.code, piece.label) == nullptr)
        {
            error = std::string(entry.path) + ": the table of programs has no calls for " + piece.label;
            return std::nullopt;
        }
    }

    return program;
}

} // namespace

std::span<const uint8_t> NarcMember(std::span<const uint8_t> narc, uint32_t member)
{
    const NarcLayout layout = ReadNarc(narc);
    if (member >= layout.count)
    {
        throw xsflib::FormatError("the NARC has no member " + std::to_string(member));
    }

    const std::size_t entry = layout.table + kMembersField + kMemberBytes * member;
    const std::size_t start = layout.data + kSectionHeader + Load32(narc, entry);
    const std::size_t end = layout.data + kSectionHeader + Load32(narc, entry + 4);
    if (end < start || end > narc.size())
    {
        throw xsflib::FormatError("member " + std::to_string(member) + " lies outside the NARC");
    }

    return narc.subspan(start, end - start);
}

std::vector<uint8_t> CutNarc(std::span<const uint8_t> narc, uint32_t member, std::span<const uint8_t> bytes)
{
    NarcMember(narc, member);
    const NarcLayout layout = ReadNarc(narc);

    // The header, the members' table and the names as they are, then the data's header and the member.
    std::vector<uint8_t> cut(narc.begin(), narc.begin() + layout.data + kSectionHeader);
    const auto size = static_cast<uint32_t>(bytes.size());
    for (uint32_t i = 0; i < layout.count; i++)
    {
        const std::size_t entry = layout.table + kMembersField + kMemberBytes * i;
        xsflib::StoreLe32(&cut[entry], i == member ? 0 : size);
        xsflib::StoreLe32(&cut[entry + 4], size);
    }
    cut.insert(cut.end(), bytes.begin(), bytes.end());
    cut.resize((cut.size() + 3) & ~std::size_t{3});
    xsflib::StoreLe32(&cut[layout.data + kSectionSizeField], static_cast<uint32_t>(cut.size() - layout.data));
    xsflib::StoreLe32(&cut[kNarcSizeField], static_cast<uint32_t>(cut.size()));

    return cut;
}

const GameState* PieceState(const SdatCode& code, std::string_view label)
{
    const std::vector<const GameState*> states = StatesOf(code, label);
    const auto base = [](const GameState* state)
    {
        return state->suffix.empty();
    };

    return std::ranges::count_if(states, base) == 1 ? states.front() : nullptr;
}

std::optional<std::vector<Program>> FindPrograms(const ntr::Rom& rom, std::string& error)
{
    std::vector<Program> programs;
    for (const ProgramEntry& entry : kPrograms)
    {
        if (std::ranges::find(entry.game_codes, std::string_view(rom.Header().game_code)) == entry.game_codes.end())
        {
            continue;
        }

        std::optional<Program> program = ReadProgram(rom, entry, error);
        if (!program)
        {
            return std::nullopt;
        }
        programs.push_back(std::move(*program));
    }

    return programs;
}

} // namespace twosf::rip
