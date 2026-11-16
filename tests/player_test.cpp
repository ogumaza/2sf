// SPDX-License-Identifier: MIT

// The player on hand-built sets: a library whose image ends after its FAT, before the files that the FAT gives, boots
// and plays. An image that ends before its ARM7 binary is refused.

#include "twosf/player.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ntdlib/ntr/rom.h"
#include "ntdlib/testing/nds_files.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/sets.h"

#include "twosf/twosf_file.h"

namespace
{

namespace nds_files = ntdlib::testing::nds_files;

// Files in memory, by path.
using Files = std::map<std::string, std::vector<uint8_t>>;

// A binary that halts for good: swi 0x060000 (Halt), then a branch back to it.
std::vector<uint8_t> Halts()
{
    std::vector<uint8_t> bytes;
    xsflib::AppendLe32(bytes, 0xEF060000);
    xsflib::AppendLe32(bytes, 0xEAFFFFFD);

    return bytes;
}

// A whole image: both binaries halt, and the file system has one file.
std::vector<uint8_t> WholeImage()
{
    nds_files::Image spec;
    spec.arm9 = Halts();
    spec.arm7 = Halts();
    spec.files = {{"sound/sound_data.sdat", std::vector<uint8_t>(0x400, 0x5A)}};

    return nds_files::BuildImage(spec).first;
}

// A set of one library with `image`, and a mini that loads it.
Files SetOf(const std::vector<uint8_t>& image)
{
    return {{"test.2sflib", twosf::WriteTwosf(twosf::Block{0, image}, {})},
            {"test.mini2sf", twosf::WriteTwosf(twosf::Block{}, {{"_lib", "test.2sflib"}})}};
}

// The image ends after its FAT. The files that the FAT gives lie past the end. Rom::Parse refuses the image, and the
// player boots it.
void PlaysAnImageCutAfterItsFat()
{
    std::vector<uint8_t> image = WholeImage();
    const ntdlib::ntr::RomHeader header = ntdlib::ntr::Rom::Parse(image).Header();
    image.resize(std::size_t{header.fat_offset} + header.fat_size);
    XSFLIB_CHECK(!xsflib::testing::ErrorOf<xsflib::FormatError>([&] { ntdlib::ntr::Rom::Parse(image); }).empty());

    const Files files = SetOf(image);
    twosf::Player player;
    XSFLIB_REQUIRE(player.Load("test.mini2sf", xsflib::testing::ExactReader(files)));
    XSFLIB_CHECK(player.Start());
    XSFLIB_CHECK(player.Error().empty());
    std::vector<int16_t> out(2 * 4096);
    XSFLIB_CHECK(player.Render(out.data(), 4096) == 4096);
    XSFLIB_CHECK(player.Error().empty());
}

// An image that ends before its ARM7 binary has no code to boot.
void RefusesAnImageWithoutCode()
{
    std::vector<uint8_t> image = WholeImage();
    const ntdlib::ntr::RomHeader header = ntdlib::ntr::Rom::Parse(image).Header();
    image.resize(header.arm7.rom_offset);

    const Files files = SetOf(image);
    twosf::Player player;
    XSFLIB_REQUIRE(player.Load("test.mini2sf", xsflib::testing::ExactReader(files)));
    XSFLIB_CHECK(!player.Start());
    XSFLIB_CHECK(player.Error() == twosf::Player::kNoCode);
}

} // namespace

int main()
{
    PlaysAnImageCutAfterItsFat();
    RefusesAnImageWithoutCode();

    return xsflib::testing::Summary("2SF player");
}
