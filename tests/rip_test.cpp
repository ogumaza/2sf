// SPDX-License-Identifier: MIT

// Tests the stub and the files of a set (rip/set.h) on ntdlib's ARM interpreter: the path that the stub builds for
// songs of one, two and three digits, the arguments of the loader's and the start's calls, the calls of each frame,
// the stub's words, the blocks of the library and of the minis, the length tags and the files' names.

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/arm/cpu.h"
#include "xsflib/bytes.h"
#include "xsflib/psf/psf.h"
#include "xsflib/testing/check.h"

#include "rip/profile.h"
#include "rip/set.h"
#include "twosf/twosf_file.h"

namespace
{

namespace arm = ntdlib::arm;

using twosf::rip::UniversomoProfile;
using xsflib::testing::Check;

constexpr uint32_t kBase = 0x02000000;
constexpr uint32_t kSize = 0x10000;
constexpr uint32_t kStack = kBase + 0xF000;

// bx lr, and mov r0, #0.
constexpr uint32_t kReturn = 0xE12FFF1E;
constexpr uint32_t kZero = 0xE3A00000;

// 64 KiB of memory at kBase.
class Memory : public arm::Bus
{
public:
    Memory() : bytes_(kSize, 0)
    {
    }

    uint8_t* Page(uint32_t page) override
    {
        return page >= kBase && page - kBase < kSize ? bytes_.data() + (page - kBase) : nullptr;
    }

    uint32_t Read(uint32_t, int) override
    {
        return 0;
    }

    void Write(uint32_t, int, uint32_t) override
    {
    }

    uint8_t* At(uint32_t address)
    {
        return bytes_.data() + (address - kBase);
    }

private:
    std::vector<uint8_t> bytes_;
};

// A profile whose routines lie in the test's memory. Each one returns at once. The reply returns 0.
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
    profile.title_song = 105;

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
    Check(xsflib::LoadLe32(stub.bytes.data() + stub.song_offset) == 105, "the stub starts the title song");
    Check(xsflib::LoadLe32(stub.bytes.data() + stub.loop_offset) == 1, "the stub's songs loop");
}

void TestFiles()
{
    twosf::rip::SetImage set;
    set.image.assign(0x100, 0xAA);
    set.song_word = 0x40;
    set.loop_word = 0x44;

    const std::vector<uint8_t> library = twosf::rip::LibraryFile(set);
    const std::vector<uint8_t> music = twosf::rip::MiniFile(set, 81, true, "BEYE.2sflib", {{"title", "Song 81"}});
    const std::vector<uint8_t> effect = twosf::rip::MiniFile(set, 12, false, "BEYE.2sflib", {});

    xsflib::psf::PsfFile file;
    xsflib::psf::ParseOptions options;
    options.version = twosf::kTwosfVersion;
    options.decompress = xsflib::psf::InflateProgram;
    twosf::Block block;
    Check(!xsflib::psf::ParsePsf(library, file, options) && !twosf::ParseProgram(file.program, block) &&
              block.offset == 0 && block.data == set.image,
          "the library holds the whole image at offset 0");
    Check(file.reserved.empty(), "the reserved area is empty");
    Check(!xsflib::psf::ParsePsf(music, file, options) && !twosf::ParseProgram(file.program, block) &&
              block.offset == 0x40 && block.data == std::vector<uint8_t>{81, 0, 0, 0},
          "a looped song's mini holds its number");
    Check(file.tags["_lib"] == "BEYE.2sflib" && file.tags["title"] == "Song 81", "the mini names its library");
    Check(!xsflib::psf::ParsePsf(effect, file, options) && !twosf::ParseProgram(file.program, block) &&
              block.offset == 0x40 && block.data == std::vector<uint8_t>{12, 0, 0, 0, 0, 0, 0, 0},
          "the mini of a song that plays once clears the loop flag");
}

void TestTags()
{
    const twosf::rip::LengthTags loop = twosf::rip::SongLengthTags(48369, true);
    const twosf::rip::LengthTags end = twosf::rip::SongLengthTags(62499, false);

    Check(loop.length == "0:48" && loop.fade == "10", "a song that loops gets its length to the second and 10 s");
    Check(end.length == "1:02.499" && end.fade == "1", "a song that ends gets its end to the millisecond and 1 s");
    Check(twosf::rip::SongLengthTags(59500, true).length == "1:00", "a length rounds to the nearest second");
    Check(twosf::rip::FileTitle("Title Screen") == "Title Screen", "a title keeps its spaces");
    Check(twosf::rip::FileTitle("A/B: C?. ") == "A_B_ C_", "refused characters and final dots and spaces");
}

} // namespace

int main()
{
    TestStub();
    TestStubWords();
    TestFiles();
    TestTags();

    return xsflib::testing::Summary("rip");
}
