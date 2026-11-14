// SPDX-License-Identifier: MIT

// 2SF files: the writer's layout of the header and the program, a round trip of a set through the writer and the
// loader, the order in which the loader applies a chain, and the files that it rejects.

#include "twosf/twosf_file.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/sets.h"

namespace
{

using twosf::Block;
using twosf::LoadedTwosf;
using xsflib::psf::Tags;
using xsflib::testing::ExactReader;

// Files in memory, by path.
using Files = std::map<std::string, std::vector<uint8_t>>;

std::optional<std::string> Load(const Files& files, const std::string& path, LoadedTwosf& set)
{
    return twosf::LoadTwosf(path, ExactReader(files), "/", set);
}

// The header has the signature, version 0x24, no reserved area and the compressed program's size. The program
// inflates to the offset, the size and the bytes.
void Layout()
{
    const Block block{0x1234, {0xAA, 0xBB, 0xCC}};
    const std::vector<uint8_t> file = twosf::WriteTwosf(block, {});
    XSFLIB_REQUIRE(file.size() > 16);
    XSFLIB_CHECK(file[0] == 'P' && file[1] == 'S' && file[2] == 'F');
    XSFLIB_CHECK(file[3] == 0x24);
    XSFLIB_CHECK(xsflib::LoadLe32(file.data() + 4) == 0);
    XSFLIB_CHECK(xsflib::LoadLe32(file.data() + 8) == file.size() - 16);

    const std::vector<uint8_t> program = twosf::WriteProgram(block);
    XSFLIB_CHECK((program == std::vector<uint8_t>{0x34, 0x12, 0, 0, 3, 0, 0, 0, 0xAA, 0xBB, 0xCC}));

    xsflib::psf::PsfFile parsed;
    XSFLIB_CHECK(!xsflib::psf::ParsePsf(file, parsed));
    XSFLIB_CHECK(parsed.version == twosf::kTwosfVersion);
    XSFLIB_CHECK(parsed.reserved.empty());
    XSFLIB_CHECK(parsed.program == program);
}

void ParsesPrograms()
{
    Block block;
    XSFLIB_CHECK(!twosf::ParseProgram({}, block));
    XSFLIB_CHECK(block == Block{});

    // Bytes after the block's size aren't part of it.
    const std::vector<uint8_t> longer = {8, 0, 0, 0, 2, 0, 0, 0, 1, 2, 3};
    XSFLIB_CHECK(!twosf::ParseProgram(longer, block));
    XSFLIB_CHECK((block == Block{8, {1, 2}}));

    const std::vector<uint8_t> short_header = {0, 0, 0, 0, 1, 0, 0};
    XSFLIB_CHECK(twosf::ParseProgram(short_header, block).has_value());

    const std::vector<uint8_t> short_block = {0, 0, 0, 0, 4, 0, 0, 0, 1, 2, 3};
    XSFLIB_CHECK(twosf::ParseProgram(short_block, block).has_value());

    // A block that ends one byte past 512 MiB.
    const std::vector<uint8_t> outside = {0xFF, 0xFF, 0xFF, 0x1F, 2, 0, 0, 0, 1, 2};
    XSFLIB_CHECK(twosf::ParseProgram(outside, block).has_value());
    const std::vector<uint8_t> inside = {0xFF, 0xFF, 0xFF, 0x1F, 1, 0, 0, 0, 1};
    XSFLIB_CHECK(!twosf::ParseProgram(inside, block));
}

// A library gives the image, and the track writes over a part of it.
void ChainOrder()
{
    Files files;
    files["set.2sflib"] = twosf::WriteTwosf({0, {1, 2, 3, 4, 5, 6, 7, 8}}, {});
    files["second.2sflib"] = twosf::WriteTwosf({6, {0x60, 0x70, 0x80, 0x90}}, {});
    files["track.mini2sf"] =
        twosf::WriteTwosf({2, {0x30, 0x40}}, {{"_lib", "set.2sflib"}, {"_lib2", "second.2sflib"}, {"title", "A"}});

    LoadedTwosf set;
    const auto error = Load(files, "track.mini2sf", set);
    XSFLIB_CHECK(!error);
    XSFLIB_CHECK((set.image == std::vector<uint8_t>{1, 2, 0x30, 0x40, 5, 6, 0x60, 0x70, 0x80, 0x90}));
    XSFLIB_CHECK((set.sources == std::vector<std::string>{"set.2sflib", "track.mini2sf", "second.2sflib"}));
    XSFLIB_CHECK(set.tags.at("title") == "A");
}

// A block past the image's end leaves zeros between.
void Gap()
{
    Files files;
    files["track.2sf"] = twosf::WriteTwosf({4, {9}}, {});

    LoadedTwosf set;
    XSFLIB_CHECK(!Load(files, "track.2sf", set));
    XSFLIB_CHECK((set.image == std::vector<uint8_t>{0, 0, 0, 0, 9}));
}

void RejectsBadFiles()
{
    Files files;
    files["other.psf"] = xsflib::psf::WritePsf(0x21, {}, twosf::WriteProgram({0, {1}}), {});
    files["short.2sf"] = xsflib::psf::WritePsf(twosf::kTwosfVersion, {}, {0, 0, 0, 0, 9, 0, 0, 0, 1}, {});
    files["missing.mini2sf"] = twosf::WriteTwosf({0, {1}}, {{"_lib", "none.2sflib"}});

    LoadedTwosf set;
    XSFLIB_CHECK(Load(files, "other.psf", set).has_value());
    XSFLIB_CHECK(Load(files, "short.2sf", set).has_value());
    XSFLIB_CHECK(Load(files, "missing.mini2sf", set).has_value());
    XSFLIB_CHECK(Load(files, "absent.2sf", set).has_value());
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("twosf_file",
                                    []
                                    {
                                        Layout();
                                        ParsesPrograms();
                                        ChainOrder();
                                        Gap();
                                        RejectsBadFiles();
                                    });
}
