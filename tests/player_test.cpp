// SPDX-License-Identifier: MIT

// The player on hand-built sets: a library whose image ends after its FAT, before the files that the FAT gives, boots
// and plays. An image that ends before its ARM7 binary is refused.

#include "twosf/player.h"

#include <cstdint>
#include <vector>

#include "ntdlib/ntr/rom.h"
#include "xsflib/binary.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/sets.h"

#include "hand_built_set.h"

namespace
{

using twosf::testing::Files;
using twosf::testing::HaltingImage;
using twosf::testing::SetOf;

// The image ends after its FAT. The files that the FAT gives are past the end. Rom::Parse refuses the image, and the
// player boots it.
void PlaysAnImageCutAfterItsFat()
{
    std::vector<uint8_t> image = HaltingImage();
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
    std::vector<uint8_t> image = HaltingImage();
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
