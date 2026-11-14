// SPDX-License-Identifier: MIT

#include "rip/set.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/arm/assembler.h"
#include "ntdlib/ntr/arm7.h"
#include "ntdlib/ntr/machine.h"
#include "ntdlib/ntr/sound.h"
#include "xsflib/bytes.h"

#include "twosf/twosf_file.h"

namespace twosf::rip
{

namespace
{

namespace arm = ntdlib::arm;

// bx lr: the first instruction of the routine that the set skips.
constexpr uint32_t kReturn = 0xE12FFF1E;

// The start of the DS's main memory, and the offset of the driver's count of loaded songs in its object.
constexpr uint32_t kMainMemory = 0x02000000;
constexpr uint32_t kSongCount = 0x400;

// The time that FirstTickMs waits for the first update.
constexpr uint64_t kBootLimitSeconds = 10;

// Writes the decimal digit of r0 / r2 at r5 and moves r5 on, unless it's a leading zero, and leaves the remainder in
// r0. r1 is not 0 once a digit is written. Uses r3.
void EmitDigit(arm::ArmAssembler& a)
{
    const int count = a.NewLabel();
    const int done = a.NewLabel();
    const int skip = a.NewLabel();
    a.Mov(3, 0);
    a.Bind(count);
    a.CmpRegister(0, 2);
    a.B(done, arm::Condition::kCc);
    a.SubRegister(0, 0, 2);
    a.Add(3, 3, 1);
    a.B(count);

    a.Bind(done);
    a.OrrRegister(1, 1, 3);
    a.Cmp(1, 0);
    a.B(skip, arm::Condition::kEq);
    a.Add(3, 3, '0');
    a.StrbPost(3, 5, 1);
    a.Bind(skip);
    a.Bx(arm::kLr);
}

// A word of four characters, the first in the low byte.
uint32_t Text(const char (&text)[5])
{
    return uint32_t(uint8_t(text[0])) | uint32_t(uint8_t(text[1])) << 8 | uint32_t(uint8_t(text[2])) << 16 |
           uint32_t(uint8_t(text[3])) << 24;
}

// Copies `size` bytes of `rom` at `offset` into `image`, which ends at the cartridge's used size.
bool Keep(const ntdlib::ntr::Rom& rom, uint32_t offset, uint32_t size, std::vector<uint8_t>& image)
{
    if (offset > image.size() || size > image.size() - offset)
    {
        return false;
    }

    const std::span<const uint8_t> source = rom.Image();
    std::copy_n(source.begin() + offset, size, image.begin() + offset);

    return true;
}

bool EndsWith(const std::string& text, std::string_view suffix)
{
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace

Stub BuildStub(const UniversomoProfile& profile)
{
    arm::ArmAssembler a(profile.main_loop.address);
    const int path = a.NewLabel();
    const int extension = a.NewLabel();
    const int song = a.NewLabel();
    const int loop = a.NewLabel();
    const int digit = a.NewLabel();
    const int copy = a.NewLabel();
    const int frame = a.NewLabel();
    const int reply = a.NewLabel();

    // The path of the song's file: its prefix, the song's number in decimal and the extension.
    a.LoadConstant(4, profile.engine);
    a.LoadAddress(1, song);
    a.Ldr(0, 1);
    a.LoadAddress(5, path);
    a.Add(5, 5, 3);
    a.Mov(1, 0);
    a.Mov(2, 100);
    a.Bl(digit);
    a.Mov(2, 10);
    a.Bl(digit);
    a.Add(0, 0, '0');
    a.StrbPost(0, 5, 1);
    a.LoadAddress(1, extension);
    a.Bind(copy);
    a.LdrbPost(0, 1, 1);
    a.StrbPost(0, 5, 1);
    a.Cmp(0, 0);
    a.B(copy, arm::Condition::kNe);

    // load(engine, path, number), then start(engine, 0, loop, -1): the first song loaded, on any free channels.
    a.MovRegister(0, 4);
    a.LoadAddress(1, path);
    a.LoadAddress(3, song);
    a.Ldr(2, 3);
    a.LoadConstant(12, profile.load.address);
    a.CallRegister(12);
    a.MovRegister(0, 4);
    a.Mov(1, 0);
    a.LoadAddress(3, loop);
    a.Ldr(2, 3);
    a.LoadConstant(3, 0xFFFFFFFF);
    a.LoadConstant(12, profile.start.address);
    a.CallRegister(12);

    // Each frame, as the main loop: the wait for the vertical blank, the replies of the sound commands until none is
    // left, the update and the flush of the commands.
    a.Bind(frame);
    a.LoadConstant(12, profile.wait_vblank.address);
    a.CallRegister(12);
    a.Bind(reply);
    a.Mov(0, 0);
    a.LoadConstant(12, profile.receive_reply.address);
    a.CallRegister(12);
    a.Cmp(0, 0);
    a.B(reply, arm::Condition::kNe);
    a.MovRegister(0, 4);
    a.LoadConstant(12, profile.update.address);
    a.CallRegister(12);
    a.Mov(0, 2);
    a.LoadConstant(12, profile.flush.address);
    a.CallRegister(12);
    a.B(frame);

    a.Bind(digit);
    EmitDigit(a);

    // The data, after the literal pool: the path with room for 3 digits and the extension, the extension alone and
    // the two words. The loop flag follows the song's number, as a mini writes them in one block.
    a.Pool();
    a.Bind(path);
    a.Word(Text("/au\0"));
    a.Word(0);
    a.Word(0);
    a.Bind(extension);
    a.Word(Text(".ums"));
    a.Word(0);
    a.Bind(song);
    a.Word(profile.title_song);
    a.Bind(loop);
    a.Word(1);

    Stub stub;
    stub.song_offset = a.Address(song) - profile.main_loop.address;
    stub.loop_offset = a.Address(loop) - profile.main_loop.address;
    stub.bytes = a.Finish();

    return stub;
}

std::optional<SetImage> MakeSetImage(const ntdlib::ntr::Rom& rom, const UniversomoProfile& profile, std::string& error)
{
    const ntdlib::ntr::RomHeader& header = rom.Header();
    const Stub stub = BuildStub(profile);
    if (stub.bytes.size() > profile.main_loop_size)
    {
        error = "the stub takes " + std::to_string(stub.bytes.size()) + " bytes, and the main loop has " +
                std::to_string(profile.main_loop_size);
        return std::nullopt;
    }

    if (profile.main_loop.address < header.arm9.ram_address ||
        profile.main_loop.address - header.arm9.ram_address + stub.bytes.size() > header.arm9.size)
    {
        error = "the stub ends past the ARM9 binary";
        return std::nullopt;
    }

    // The header, the binaries, the file system's tables and the files that the music reads.
    SetImage set;
    set.image.assign(std::min<std::size_t>(header.used_size, rom.Image().size()), 0);
    bool kept = Keep(rom, 0, header.header_size, set.image) &&
                Keep(rom, header.arm9.rom_offset, header.arm9.size, set.image) &&
                Keep(rom, header.arm7.rom_offset, header.arm7.size, set.image) &&
                Keep(rom, header.fnt_offset, header.fnt_size, set.image) &&
                Keep(rom, header.fat_offset, header.fat_size, set.image) &&
                Keep(rom, header.arm9_overlay_offset, header.arm9_overlay_size, set.image) &&
                Keep(rom, header.arm7_overlay_offset, header.arm7_overlay_size, set.image);
    for (const ntdlib::ntr::RomFile& file : rom.Files())
    {
        const bool music = std::any_of(profile.kept_suffixes.begin(), profile.kept_suffixes.end(),
                                       [&file](std::string_view suffix) { return EndsWith("/" + file.path, suffix); });
        // An overlay is a file without a path. The code may load one before the music plays.
        if (music || file.path.empty())
        {
            kept = kept && Keep(rom, file.start, file.end - file.start, set.image);
        }
    }

    if (!kept)
    {
        error = "a part of the image that the music reads lies past the cartridge's used size";
        return std::nullopt;
    }

    // The stub over the main loop, and a return at the start of the skipped routine.
    const auto image_offset = [&header](uint32_t address)
    {
        return header.arm9.rom_offset + (address - header.arm9.ram_address);
    };
    const uint32_t stub_at = image_offset(profile.main_loop.address);
    std::copy(stub.bytes.begin(), stub.bytes.end(), set.image.begin() + stub_at);
    xsflib::StoreLe32(set.image.data() + image_offset(profile.skipped.address), kReturn);
    set.song_word = stub_at + stub.song_offset;
    set.loop_word = stub_at + stub.loop_offset;

    return set;
}

void SetSong(SetImage& set, uint32_t song, bool loops)
{
    xsflib::StoreLe32(set.image.data() + set.song_word, song);
    xsflib::StoreLe32(set.image.data() + set.loop_word, loops ? 1 : 0);
}

std::vector<uint8_t> LibraryFile(const SetImage& set)
{
    return WriteTwosf({0, set.image}, {});
}

std::vector<uint8_t> MiniFile(const SetImage& set, uint32_t song, bool loops, const std::string& library,
                              xsflib::psf::Tags tags)
{
    // A song that loops keeps the library's loop flag of 1.
    if (set.loop_word != set.song_word + 4)
    {
        throw std::logic_error("the stub's loop flag doesn't follow the song's number");
    }

    Block block{set.song_word, {}};
    xsflib::AppendLe32(block.data, song);
    if (!loops)
    {
        xsflib::AppendLe32(block.data, 0);
    }

    tags["_lib"] = library;

    return WriteTwosf(block, tags);
}

LengthTags SongLengthTags(long long length_ms, bool loops)
{
    char text[32];
    if (loops)
    {
        const long long seconds = (length_ms + 500) / 1000;
        std::snprintf(text, sizeof text, "%lld:%02lld", seconds / 60, seconds % 60);
        return {text, "10"};
    }

    std::snprintf(text, sizeof text, "%lld:%02lld.%03lld", length_ms / 60000, length_ms / 1000 % 60, length_ms % 1000);

    return {text, "1"};
}

std::string FileTitle(const std::string& title)
{
    std::string name;
    for (const char c : title)
    {
        const bool refused =
            static_cast<unsigned char>(c) < 0x20 || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos;
        name += refused ? '_' : c;
    }

    // Windows drops a name's final dots and spaces.
    while (!name.empty() && (name.back() == '.' || name.back() == ' '))
    {
        name.pop_back();
    }

    return name;
}

double FirstTickMs(const SetImage& set, const UniversomoProfile& profile, uint32_t song, bool loops)
{
    SetImage played = set;
    SetSong(played, song, loops);
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(played.image);
    const auto machine = std::make_unique<ntdlib::ntr::Machine>(rom);
    machine->Arm9().SetStopAddress(profile.update.address);

    std::vector<ntdlib::ntr::SoundOutput> sound;
    const uint64_t limit = kBootLimitSeconds * ntdlib::ntr::kSystemClockHz;
    if (!machine->Run(limit, sound))
    {
        throw std::runtime_error("the driver's update doesn't run within " + std::to_string(kBootLimitSeconds) +
                                 " seconds of the boot");
    }

    const std::span<uint8_t> memory = machine->MainMemory();
    if (xsflib::LoadLe32(memory.data() + (profile.engine - kMainMemory) + kSongCount) == 0)
    {
        throw std::runtime_error("the driver has loaded no song by its first update");
    }

    return static_cast<double>(machine->Clock()) * 1000.0 / ntdlib::ntr::kSystemClockHz;
}

} // namespace twosf::rip
