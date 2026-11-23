// SPDX-License-Identifier: MIT

// Tests the stub and the files of a set (rip/set.h) on ntdlib's ARM interpreter: the path that the stub builds for
// songs of one, two and three digits, the arguments of the loader's and the start's calls, the calls of each frame,
// the stub's words, the minis' blocks, the files' names and the songs' labels, and the boots to the main loop and to
// the first tick with a backup memory. Also the libraries' names of the games that 2sfrip has been checked on
// (rip/library_name.h).

#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/arm/assembler.h"
#include "ntdlib/arm/cpu.h"
#include "ntdlib/ntr/rom.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "backup_image.h"
#include "rip/library_name.h"
#include "rip/profile.h"
#include "rip/set.h"
#include "stub_memory.h"
#include "twosf/twosf_file.h"

namespace
{

namespace arm = ntdlib::arm;

using twosf::rip::UniversomoProfile;
using twosf::testing::kBase;
using twosf::testing::kStack;
using twosf::testing::Memory;
using xsflib::testing::Check;

// bx lr, and mov r0, #0.
constexpr uint32_t kReturn = 0xE12FFF1E;
constexpr uint32_t kZero = 0xE3A00000;

// A profile whose routines are in the test's memory. Each one returns at once. The reply returns 0.
UniversomoProfile TestProfile()
{
    UniversomoProfile profile;
    profile.engine = kBase + 0x8000;
    profile.main_loop.address = kBase + 0x1000;
    profile.main_loop_size = 0x2B0;
    profile.load.address = kBase + 0x100;
    profile.start.address = kBase + 0x110;
    profile.update.address = kBase + 0x120;
    profile.wait_vblank.address = kBase + 0x130;
    profile.receive_reply.address = kBase + 0x140;
    profile.flush.address = kBase + 0x150;

    return profile;
}

// What the stub passes to the loader, the start and the update.
struct Calls
{
    std::string path;
    uint32_t load_engine = 0;
    uint32_t load_number = 0;
    uint32_t start_engine = 0;
    uint32_t start_index = 0;
    uint32_t start_loop = 0;
    uint32_t start_channel = 0;
    uint32_t update_engine = 0;
    int waits_before_update = 0;
};

// Runs the stub of `profile` with song `song` and the loop flag `loop` in its words, up to its second update.
Calls RunStub(const UniversomoProfile& profile, uint32_t song, uint32_t loop)
{
    Memory memory;
    for (const uint32_t routine : {profile.load.address, profile.start.address, profile.update.address,
                                   profile.wait_vblank.address, profile.flush.address})
    {
        xsflib::StoreLe32(memory.At(routine), kReturn);
    }

    xsflib::StoreLe32(memory.At(profile.receive_reply.address), kZero);
    xsflib::StoreLe32(memory.At(profile.receive_reply.address + 4), kReturn);
    const twosf::rip::Stub stub = twosf::rip::BuildStub(profile);
    std::memcpy(memory.At(profile.main_loop.address), stub.bytes.data(), stub.bytes.size());
    xsflib::StoreLe32(memory.At(profile.main_loop.address + stub.song_offset), song);
    xsflib::StoreLe32(memory.At(profile.main_loop.address + stub.loop_offset), loop);

    arm::Cpu cpu(memory, arm::Architecture::kArmV5TE);
    cpu.Jump(profile.main_loop.address, arm::Mode::kSystem);
    cpu.SetReg(13, kStack);
    Calls calls;
    const auto run_to = [&cpu](uint32_t address)
    {
        cpu.SetStopAddress(address);
        return cpu.Run(100000) == arm::StopReason::kStopAddress;
    };

    Check(run_to(profile.load.address), "the stub calls the loader");
    calls.load_engine = cpu.Reg(0);
    calls.path = reinterpret_cast<const char*>(memory.At(cpu.Reg(1)));
    calls.load_number = cpu.Reg(2);
    Check(run_to(profile.start.address), "the stub calls the start");
    calls.start_engine = cpu.Reg(0);
    calls.start_index = cpu.Reg(1);
    calls.start_loop = cpu.Reg(2);
    calls.start_channel = cpu.Reg(3);
    Check(run_to(profile.update.address), "the stub calls the update");
    calls.update_engine = cpu.Reg(0);
    Check(run_to(profile.wait_vblank.address), "the stub waits for the next frame");
    Check(run_to(profile.update.address), "the stub calls the update each frame");

    return calls;
}

void TestStub()
{
    const UniversomoProfile profile = TestProfile();

    const Calls title = RunStub(profile, 105, 1);
    const Calls one = RunStub(profile, 7, 0);
    const Calls two = RunStub(profile, 40, 1);
    const Calls zero = RunStub(profile, 0, 0);
    const Calls hundred = RunStub(profile, 100, 1);

    Check(title.path == "/au105.ums" && title.load_number == 105, "song 105 loads /au105.ums");
    Check(one.path == "/au7.ums" && two.path == "/au40.ums", "songs of one and two digits");
    Check(zero.path == "/au0.ums" && hundred.path == "/au100.ums", "the zeros of songs 0 and 100");
    Check(title.load_engine == profile.engine && title.start_engine == profile.engine &&
              title.update_engine == profile.engine,
          "every call takes the engine");
    Check(title.start_index == 0 && title.start_channel == 0xFFFFFFFF,
          "the start takes the first song, on any channels");
    Check(title.start_loop == 1 && one.start_loop == 0, "the start takes the loop flag");
}

void TestStubWords()
{
    const UniversomoProfile profile = TestProfile();

    const twosf::rip::Stub stub = twosf::rip::BuildStub(profile);

    Check(stub.bytes.size() <= profile.main_loop_size, "the stub fits in the main loop");
    Check(stub.loop_offset == stub.song_offset + 4, "the loop flag follows the song's number");
    Check(xsflib::LoadLe32(stub.bytes.data() + stub.song_offset) == 0, "the stub's song is 0 until a mini sets it");
    Check(xsflib::LoadLe32(stub.bytes.data() + stub.loop_offset) == 1, "the stub's songs loop");
}

void TestMiniBlocks()
{
    twosf::rip::SetImage set;
    set.image.assign(0x100, 0xAA);
    set.song_word = 0x40;
    set.loop_word = 0x44;

    const twosf::Block music = twosf::rip::MiniBlock(set, 81, true);
    const twosf::Block effect = twosf::rip::MiniBlock(set, 12, false);

    Check(music == twosf::Block{0x40, {81, 0, 0, 0}}, "a looped song's mini holds its number");
    Check(effect == twosf::Block{0x40, {12, 0, 0, 0, 0, 0, 0, 0}},
          "the mini of a song that plays once clears the loop flag");
}

void TestFileTitles()
{
    Check(twosf::rip::FileTitle("Title Screen") == "Title Screen", "a title keeps its spaces");
    Check(twosf::rip::FileTitle("A/B: C?. ") == "A_B_ C_", "refused characters and final dots and spaces");
}

// A song's label: its number after "Song" for music and "Effect" for a sound effect.
void TestSongLabels()
{
    Check(twosf::rip::SongLabel(84, true) == "Song 84", "music");
    Check(twosf::rip::SongLabel(12, false) == "Effect 12", "a sound effect");
}

// Each known game's library takes its product code: NTR- or TWL-, the game code, and a region. Any other game's takes
// "NTR-" and its game code.
void TestLibraryNames()
{
    const auto well_formed = [](const twosf::rip::KnownGame& game)
    {
        const std::string product(game.product_code);
        if (product.size() != 12)
        {
            return false;
        }

        const std::string platform = product.substr(0, 4);
        const std::string region = product.substr(8);
        return (platform == "NTR-" || platform == "TWL-") && product.substr(4, 4) == game.game_code &&
               (region == "-USA" || region == "-EUR" || region == "-JPN");
    };

    bool every_name = !twosf::rip::KnownGames().empty();
    for (const twosf::rip::KnownGame& game : twosf::rip::KnownGames())
    {
        every_name = every_name && well_formed(game) && twosf::rip::LibraryBase(game.game_code) == game.product_code;
    }

    Check(every_name, "each known game's library takes its product code");
    Check(twosf::rip::LibraryBase("VI2J") == "TWL-VI2J-JPN", "a DSi-enhanced game's product code");
    Check(twosf::rip::LibraryBase("KQ9E") == "TWL-KQ9E-USA", "a DSi-only game's product code");
    Check(twosf::rip::LibraryBase("YFEE") == "NTR-YFEE-USA", "Shadow Dragon's product code");
    Check(twosf::rip::LibraryBase("ABCD") == "NTR-ABCD", "an unknown game's library name");
}

// A set whose boot reaches the main loop, sets the driver's count of loaded songs and calls the update only with a
// backup memory. `profile` gets the addresses of the main loop, the update and the driver's object.
twosf::rip::SetImage SetWithBackup(UniversomoProfile& profile)
{
    constexpr uint32_t kEngine = 0x02010000;
    constexpr uint32_t kSongCount = 0x400;
    constexpr uint32_t kWords = twosf::testing::kArm9Base + 0x400;

    arm::ArmAssembler a(twosf::testing::kArm9Base);
    const int halt = a.NewLabel();
    const int main_loop = a.NewLabel();
    const int update = a.NewLabel();
    twosf::testing::EmitBackupCheck(a, halt);
    a.LoadConstant(1, kEngine + kSongCount);
    a.Mov(0, 1);
    a.Str(0, 1);
    a.Bind(main_loop);
    a.Bl(update);
    twosf::testing::EmitHaltForever(a, halt);
    a.Bind(update);
    a.Bx(14);
    twosf::testing::EndCodeAt(a, kWords);
    a.Word(0);
    a.Word(0);
    profile.engine = kEngine;
    profile.main_loop.address = a.Address(main_loop);
    profile.update.address = a.Address(update);

    twosf::rip::SetImage set;
    set.image = twosf::testing::ImageWithArm9(a.Finish());
    const uint32_t arm9_offset = xsflib::LoadLe32(&set.image[0x20]);
    set.song_word = arm9_offset + (kWords - twosf::testing::kArm9Base);
    set.loop_word = set.song_word + 4;

    return set;
}

// BootToMainLoop and FirstTickMs boot with the backup memory that they're given: the set reaches the main loop and the
// update only with one.
void TestBackup()
{
    UniversomoProfile profile;
    const twosf::rip::SetImage set = SetWithBackup(profile);
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(set.image);

    const twosf::rip::MainLoopBoot with = twosf::rip::BootToMainLoop(rom, profile, twosf::testing::TestBackup());
    const twosf::rip::MainLoopBoot without = twosf::rip::BootToMainLoop(rom, profile, {});
    bool ticks_with = true;
    try
    {
        twosf::rip::FirstTickMs(set, profile, 1, true, twosf::testing::TestBackup());
    }
    catch (const std::runtime_error&)
    {
        ticks_with = false;
    }
    bool ticks_without = true;
    try
    {
        twosf::rip::FirstTickMs(set, profile, 1, true, {});
    }
    catch (const std::runtime_error&)
    {
        ticks_without = false;
    }

    Check(with.reached, "the boot with a backup memory reaches the main loop");
    Check(!without.reached, "the boot without one doesn't");
    Check(ticks_with, "the boot with a backup memory reaches the first tick");
    Check(!ticks_without, "the boot without one doesn't");
}

} // namespace

int main()
{
    TestStub();
    TestStubWords();
    TestMiniBlocks();
    TestFileTitles();
    TestSongLabels();
    TestLibraryNames();
    TestBackup();

    return xsflib::testing::Summary("rip");
}
