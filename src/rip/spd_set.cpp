// SPDX-License-Identifier: MIT

#include "rip/spd_set.h"

#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "ntdlib/arm/assembler.h"
#include "ntdlib/ntr/code.h"
#include "ntdlib/ntr/compact_image.h"
#include "ntdlib/ntr/machine.h"
#include "ntdlib/ntr/sound.h"
#include "xsflib/bytes.h"
#include "xsflib/text.h"

#include "rip/boots.h"

namespace twosf::rip
{

namespace
{

namespace arm = ntdlib::arm;
namespace ntr = ntdlib::ntr;

// Super Princess Peach (USA). The main function 0x02000B24 calls the sound's start-up 0x02014150 at 0x02000BE8. The
// start-up starts the driver (0x020C90B0), waits for its answer (0x020CA214) and sends the first messages (0x020C94D8).
// The rest of the main function sets up the first screens, then loops from 0x02000C48 to the branch at 0x02000C70. Its
// literal pool starts at 0x02000C74.
//
// The game's start of a sound by its number in the table at 0x020CB930 (0x02014044) loads the sound's block as set B
// through 0x020140CC. That routine calls 0x020C9F9C(sound, slot) until it returns 1. Then 0x02014044 queues the start
// with 0x020C8F88(sound, slot, fade). Slot 0 is set A and requires no load: 0x020C9F9C returns 1 at once for slot 0 and
// for the block that set B already contains. The scenes' loops call 0x020C94D8 once a frame (0x02013764 and 0x0208049C,
// through 0x02014010). 0x020CA9B4 sends command 10 with 1 (the parameter 0x00010001 at 0x020CAA60). The game calls it
// right after a start, at 0x02011548.
constexpr std::array kAspeWords = {
    GameWord{0x02000BE8, 0xEB004D58}, // bl 0x02014150, the sound's start-up
    GameWord{0x02000C70, 0xEAFFFFF4}, // b 0x02000C48, the main loop
    GameWord{0x020C9F9C, 0xE92D4FF0}, // push {r4-r11, lr}, the load of set B
    GameWord{0x020C8F88, 0xE92D4070}, // push {r4, r5, r6, lr}, the start
    GameWord{0x020C94D8, 0xE92D40F0}, // push {r4, r5, r6, r7, lr}, the frame's messages
    GameWord{0x020CA9B4, 0xE92D40F0}, // push {r4, r5, r6, r7, lr}, the branch flag's 1
    GameWord{0x020CAA60, 0x00010001}, // the command's parameter: 1
};

constexpr SpdCode kSuperPrincessPeach = {
    .game_code = "ASPE",
    .hook = 0x02000BEC,
    .hook_end = 0x02000C74,
    .load_set_b = 0x020C9F9C,
    .play = 0x020C8F88,
    .sound_frame = 0x020C94D8,
    .branch_flag_1 = 0x020CA9B4,
    .words = kAspeWords,
};

constexpr SpdCode kGames[] = {kSuperPrincessPeach};

// The BIOS's VBlankIntrWait.
constexpr uint32_t kVBlankIntrWait = 5;

} // namespace

bool ListsSpdGame(std::string_view game_code)
{
    return std::ranges::find(kGames, game_code, &SpdCode::game_code) != std::end(kGames);
}

std::optional<SpdCode> FindSpdCode(const ntr::Rom& rom, std::string& error)
{
    const auto game = std::ranges::find(kGames, std::string_view(rom.Header().game_code), &SpdCode::game_code);
    if (game == std::end(kGames))
    {
        error = "2sfrip has no table of this game's routines for Nintendo SPD's driver";
        return std::nullopt;
    }

    if (const std::optional<GameWord> missing = MissingWord(ntr::Arm9Code(rom), game->words))
    {
        error = "the game's code at " + xsflib::Hex(missing->address, 8, true) + " isn't what 2sfrip expects";
        return std::nullopt;
    }

    return *game;
}

SpdStub BuildSpdStub(const SpdCode& code, const SpdStart& start)
{
    constexpr int kData = 4; // the data's address, kept across the calls
    constexpr int kSoundField = 0;
    constexpr int kSlotField = 4;
    constexpr int kFlagField = 8;

    arm::ArmAssembler a(code.hook);
    const int data = a.NewLabel();
    const int load = a.NewLabel();
    const int frame = a.NewLabel();

    // The load of the sound's block as set B, until it's done.
    a.LoadAddress(kData, data);
    a.Bind(load);
    a.Ldr(0, kData, kSoundField);
    a.Ldr(1, kData, kSlotField);
    Call(a, code.load_set_b);
    a.Cmp(0, 0);
    a.B(load, arm::Condition::kEq);

    // The start, with no fade.
    a.Ldr(0, kData, kSoundField);
    a.Ldr(1, kData, kSlotField);
    a.Mov(2, 0);
    Call(a, code.play);

    // The branch flag, where the mini sets 1. A bl reaches the routine from the hook, and keeps the stub short.
    a.Ldr(0, kData, kFlagField);
    a.Cmp(0, 0);
    a.B(frame, arm::Condition::kEq);
    a.Word(BranchLink(a.Here(), code.branch_flag_1));

    // The frames: the messages go to the driver after each vertical blank.
    a.Bind(frame);
    a.Swi(kVBlankIntrWait);
    Call(a, code.sound_frame);
    a.B(frame);

    a.Pool();
    a.Bind(data);
    a.Word(start.sound);
    a.Word(start.slot);
    a.Word(start.branch_flag);

    SpdStub stub;
    stub.start_offset = a.Address(data) - code.hook;
    stub.bytes = a.Finish();

    return stub;
}

std::vector<uint8_t> SpdStartWords(const SpdStart& start)
{
    std::vector<uint8_t> words;
    xsflib::AppendLe32(words, start.sound);
    xsflib::AppendLe32(words, start.slot);
    xsflib::AppendLe32(words, start.branch_flag);

    return words;
}

SpdSet MakeSpdSet(const ntr::Rom& rom, const SpdCode& code, const SpdStub& stub,
                  const std::map<uint32_t, std::vector<uint8_t>>& files)
{
    if (stub.bytes.size() > code.hook_end - code.hook)
    {
        throw std::runtime_error("the stub takes " + std::to_string(stub.bytes.size()) +
                                 " bytes, and the code after the hook has " +
                                 std::to_string(code.hook_end - code.hook));
    }

    ntr::CompactParts parts{.arm9 = ntr::UncompressedArm9(rom), .files = files};
    const uint32_t base = rom.Header().arm9.ram_address;
    std::ranges::copy(stub.bytes, parts.arm9.begin() + (code.hook - base));

    SpdSet set;
    set.image = ntr::WriteCompactImage(rom, parts);
    set.start_word = rom.Header().arm9.rom_offset + (code.hook - base) + stub.start_offset;

    return set;
}

SpdMiniRun RunSpdMini(std::span<const uint8_t> image, const SpdCode& code, uint64_t hook_clock, const SpdStart& start,
                      double seconds, const ntr::BackupMemory& backup)
{
    SpdMiniRun run;
    const ntr::Rom rom = ntr::Rom::Parse(image);
    const auto machine = std::make_unique<ntr::Machine>(rom, backup);
    const auto fail = [&run](std::string error)
    {
        if (!run.error)
        {
            run.error = std::move(error);
        }
    };

    // After the hook, each card read must reach the file system's tables or a file that the set keeps.
    const std::vector<std::pair<uint32_t, uint32_t>> kept = KeptRanges(rom);
    machine->on_card_read = [&](uint32_t address, uint32_t bytes)
    {
        if (machine->Clock() >= hook_clock && !Reaches(kept, address, bytes))
        {
            fail(ReadOutsideSetError(address));
        }
    };

    std::vector<ntr::SoundOutput> out;
    const auto limit = static_cast<uint64_t>(seconds * ntr::kSystemClockHz);
    const std::string within = " within " + std::to_string(static_cast<int>(seconds)) + " s of the boot";
    machine->Arm9().SetStopAddress(code.hook);
    if (!machine->Run(limit, out))
    {
        fail("the boot doesn't reach the hook" + within);
        return run;
    }

    machine->Arm9().SetStopAddress(code.play);
    if (!machine->Run(limit, out))
    {
        fail("the stub doesn't start the sound" + within);
        return run;
    }
    run.start_ms = static_cast<double>(machine->Clock()) * 1000.0 / ntr::kSystemClockHz;
    out.clear();

    // The play routine takes the sound and the slot. The stub sends the branch flag after it returns.
    const SpdStart started{.sound = machine->Arm9().Reg(0), .slot = machine->Arm9().Reg(1)};
    if (started.sound != start.sound || started.slot != start.slot)
    {
        fail("the stub starts sound " + std::to_string(started.sound) + " of slot " + std::to_string(started.slot) +
             ", not sound " + std::to_string(start.sound) + " of slot " + std::to_string(start.slot));
    }

    // The mini plays on with no stop address.
    machine->Arm9().SetStopAddress(0xFFFFFFFF);
    const uint64_t end = machine->Clock() + static_cast<uint64_t>(kSpdMiniPlaySeconds * ntr::kSystemClockHz);
    machine->Run(end, out);

    OutputWatch watch;
    watch.Add(out);
    run.sounded = watch.Sounded();

    return run;
}

} // namespace twosf::rip
