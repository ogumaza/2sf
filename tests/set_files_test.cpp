// SPDX-License-Identifier: MIT

// Tests the files that 2sfrip writes for a set (tools/set_files.h) in a temporary folder: the library contains the
// whole image at offset 0 and an empty reserved area, and the mini names its library and its title. Also the game tag
// that an image's name outside ASCII gives.

#include "tools/set_files.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/ntr/rom.h"
#include "ntdlib/testing/nds_files.h"
#include "xsflib/cli/rip.h"
#include "xsflib/files.h"
#include "xsflib/psf/psf.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/temp_folder.h"

#include "twosf/twosf_file.h"

namespace
{

using xsflib::testing::Check;

// Reads the 2SF file `name` in `folder` into `file`, and its program into `block`. Returns false if it can't.
bool ReadTwosf(const std::filesystem::path& folder, const std::string& name, xsflib::psf::PsfFile& file,
               twosf::Block& block)
{
    xsflib::psf::ParseOptions options;
    options.version = twosf::kTwosfVersion;
    options.decompress = xsflib::psf::InflateProgram;
    std::vector<uint8_t> bytes;

    return xsflib::ReadFile(folder / xsflib::PathFromUtf8(name), bytes) &&
           !xsflib::psf::ParsePsf(bytes, file, options) && !twosf::ParseProgram(file.program, block);
}

// A set of one mini for a game that 2sfrip doesn't know: the library is "NTR-" and the game code, and the mini is
// named "01" and its title.
void TestSet()
{
    const std::vector<uint8_t> image = ntdlib::testing::nds_files::BuildImage({.game_code = "ABCD"}).first;
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);
    twosf::tools::SetParts parts;
    parts.minis.push_back({"Song 81", {0x40, {81, 0, 0, 0}}, std::nullopt, std::nullopt});
    const xsflib::testing::TempFolder folder("twosf_set_files_test");
    xsflib::cli::RipOptions options;
    options.out_dir = xsflib::Utf8(folder.Path());
    xsflib::cli::FileNameRegistries names;

    const int result = twosf::tools::WriteSet("ビート.nds", rom, image, parts, options, names);

    Check(result == 0, "the set is written");

    xsflib::psf::PsfFile file;
    twosf::Block block;
    Check(ReadTwosf(folder.Path(), "NTR-ABCD.2sflib", file, block) && block.offset == 0 && block.data == image,
          "the library holds the whole image at offset 0");
    Check(file.reserved.empty(), "the reserved area is empty");

    Check(ReadTwosf(folder.Path(), "01 Song 81.mini2sf", file, block) && block.offset == 0x40 &&
              block.data == std::vector<uint8_t>{81, 0, 0, 0},
          "the mini holds its block");
    Check(file.tags["_lib"] == "NTR-ABCD.2sflib" && file.tags["title"] == "Song 81", "the mini names its library");
    Check(file.tags["game"] == "ビート", "the game tag is the image's name");
}

} // namespace

int main()
{
    TestSet();

    return xsflib::testing::Summary("set_files");
}
