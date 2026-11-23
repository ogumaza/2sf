// SPDX-License-Identifier: MIT

// Tests the NARC parts of a download-play program's set (rip/program_set.h): a member's bytes, the refusals of a file
// that isn't a NARC or has no such member, and the cut that keeps one member. Also the state that contains a piece's
// calls.

#include "rip/program_set.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

namespace
{

using xsflib::testing::Check;

void Append32(std::vector<uint8_t>& bytes, uint32_t value)
{
    bytes.resize(bytes.size() + 4);
    xsflib::StoreLe32(&bytes[bytes.size() - 4], value);
}

void AppendTag(std::vector<uint8_t>& bytes, std::string_view tag)
{
    bytes.insert(bytes.end(), tag.begin(), tag.end());
}

// A NARC of `members`, laid out as the games' are: the header, the members' table, a names section with no names
// and the data, each member at a 4-byte boundary.
std::vector<uint8_t> MakeNarc(const std::vector<std::vector<uint8_t>>& members)
{
    std::vector<uint8_t> data;
    std::vector<std::pair<uint32_t, uint32_t>> spans;
    for (const std::vector<uint8_t>& member : members)
    {
        spans.emplace_back(static_cast<uint32_t>(data.size()), static_cast<uint32_t>(data.size() + member.size()));
        data.insert(data.end(), member.begin(), member.end());
        data.resize((data.size() + 3) & ~std::size_t{3});
    }

    std::vector<uint8_t> narc;
    AppendTag(narc, "NARC");
    Append32(narc, 0x0100FFFE); // the byte order and the version
    Append32(narc, 0);          // the file's size, below
    Append32(narc, 0x00030010); // the header's size and the count of sections
    AppendTag(narc, "BTAF");
    Append32(narc, static_cast<uint32_t>(12 + 8 * members.size()));
    Append32(narc, static_cast<uint32_t>(members.size()));
    for (const auto& [start, end] : spans)
    {
        Append32(narc, start);
        Append32(narc, end);
    }
    AppendTag(narc, "BTNF");
    Append32(narc, 16);
    Append32(narc, 4);
    Append32(narc, 0x00010000);
    AppendTag(narc, "GMIF");
    Append32(narc, static_cast<uint32_t>(8 + data.size()));
    narc.insert(narc.end(), data.begin(), data.end());
    xsflib::StoreLe32(&narc[8], static_cast<uint32_t>(narc.size()));

    return narc;
}

// Why `call` refuses its NARC, or nothing when it doesn't.
std::string RefusalOf(auto&& call)
{
    try
    {
        call();
    }
    catch (const xsflib::FormatError& e)
    {
        return e.what();
    }

    return {};
}

bool Refuses(auto&& call)
{
    return !RefusalOf(call).empty();
}

// A member's bytes, and the files that NarcMember refuses.
void TestNarcMember()
{
    const std::vector<std::vector<uint8_t>> members = {{1, 2, 3}, {4, 5, 6, 7, 8}, {}, {9}};
    const std::vector<uint8_t> narc = MakeNarc(members);
    for (uint32_t i = 0; i < members.size(); i++)
    {
        const std::span<const uint8_t> member = twosf::rip::NarcMember(narc, i);
        Check(std::vector<uint8_t>(member.begin(), member.end()) == members[i],
              "each member's bytes, an empty one too");
    }

    std::vector<uint8_t> not_narc = narc;
    not_narc[0] = 'X';
    std::vector<uint8_t> no_data = narc;
    no_data[no_data.size() - 16 - 8] = 'X'; // the data's tag, before its header's size and its 16 bytes
    std::vector<uint8_t> outside = narc;
    xsflib::StoreLe32(&outside[0x10 + 12 + 8 + 4], 0x1000);
    Check(Refuses([&] { twosf::rip::NarcMember(narc, 4); }), "no member past the count");
    Check(Refuses([&] { twosf::rip::NarcMember(not_narc, 0); }), "no member of a file that isn't a NARC");
    Check(Refuses([&] { twosf::rip::NarcMember(no_data, 0); }), "no member without the data section");
    Check(Refuses([&] { twosf::rip::NarcMember(outside, 1); }), "no member that ends past the file");
    Check(Refuses([&] { twosf::rip::NarcMember(std::span(narc).first(20), 0); }), "no member of a cut-off header");
}

// ReadNarc reads each field only where the file contains it, and refuses a layout that would put a write of CutNarc
// outside the cut's bytes.
void TestNarcBounds()
{
    const std::vector<std::vector<uint8_t>> members = {{1, 2, 3}, {4, 5}};
    const std::vector<uint8_t> narc = MakeNarc(members);
    const std::size_t names = 0x10 + 12 + 8 * members.size();
    const std::size_t data = names + 16;

    // The header's size field at 0x0C ends past a 13-byte file.
    const std::span<const uint8_t> header = std::span(narc).first(13);

    // The names' size takes the data's section round to the header's bytes 4 to 7. Those bytes read "GMIF".
    std::vector<uint8_t> wrapped = narc;
    xsflib::StoreLe32(&wrapped[names + 4], static_cast<uint32_t>(0x100000004 - names));
    std::ranges::copy(std::string_view("GMIF"), wrapped.begin() + 4);

    // A count of members whose table ends past the members' section.
    std::vector<uint8_t> count = narc;
    xsflib::StoreLe32(&count[0x10 + 8], 3);

    // The file ends inside the data's header, and the member's start and end take it round to the header's last word.
    std::vector<uint8_t> short_data(narc.begin(), narc.begin() + data + 4);
    xsflib::StoreLe32(&short_data[0x10 + 12], 0xFFFFFFFC);
    xsflib::StoreLe32(&short_data[0x10 + 16], 0xFFFFFFFC);

    Check(RefusalOf([&] { twosf::rip::NarcMember(header, 0); }) == "the NARC ends inside a field",
          "no read of the header's size past the file's end");
    Check(Refuses([&] { twosf::rip::CutNarc(wrapped, 0, {}); }), "no section that ends past the file");
    Check(Refuses([&] { twosf::rip::CutNarc(count, 0, {}); }), "no table of members past its section");
    Check(Refuses([&] { twosf::rip::CutNarc(short_data, 0, {}); }), "no file that ends inside the data's header");
}

// The cut keeps one member, with other bytes, and leaves the others empty: still a NARC with every member.
void TestCutNarc()
{
    const std::vector<std::vector<uint8_t>> members = {{1, 2, 3}, {4, 5, 6, 7, 8}, {9, 10}};
    const std::vector<uint8_t> narc = MakeNarc(members);
    const std::vector<uint8_t> kept = {20, 21, 22, 23, 24, 25};
    const std::vector<uint8_t> cut = twosf::rip::CutNarc(narc, 1, kept);
    const std::span<const uint8_t> member = twosf::rip::NarcMember(cut, 1);
    Check(std::vector<uint8_t>(member.begin(), member.end()) == kept, "the kept member holds the new bytes");
    Check(twosf::rip::NarcMember(cut, 0).empty() && twosf::rip::NarcMember(cut, 2).empty(),
          "every other member is empty");
    const std::size_t data = narc.size() - 16 - 8; // the data's tag, before its header's size and its 16 bytes
    Check(xsflib::LoadLe32(&cut[8]) == cut.size() && cut.size() % 4 == 0,
          "the header gives the cut's size, at a 4-byte boundary");
    Check(xsflib::LoadLe32(&cut[data + 4]) == cut.size() - data, "the data's header gives the data's size");
    // The data's 16 bytes give way to the kept member's 6, padded to 8.
    Check(cut.size() == narc.size() - 16 + kept.size() + 2, "the data holds only the kept member, padded");
    Check(std::vector<uint8_t>(cut.begin(), cut.begin() + 4) == std::vector<uint8_t>(narc.begin(), narc.begin() + 4),
          "the header stays");
    Check(Refuses([&] { twosf::rip::CutNarc(narc, 3, kept); }), "no cut to a member past the count");
}

// A piece's calls are the one state of its label with no suffix. A label with no such state, or with two, has none.
void TestPieceState()
{
    using twosf::rip::GameState;
    twosf::rip::SdatCode code;
    code.game_start = twosf::rip::GameStart{.states = {{.label = "PIECE", .suffix = "_VARIANT"},
                                                       {.label = "PIECE", .suffix = ""},
                                                       {.label = "TWICE", .suffix = ""},
                                                       {.label = "TWICE", .suffix = ""},
                                                       {.label = "VARIANT_ONLY", .suffix = "_VARIANT"}}};

    const GameState* piece = twosf::rip::PieceState(code, "PIECE");

    Check(piece != nullptr && piece->label == "PIECE" && piece->suffix.empty(),
          "the state with no suffix, before a variant in the table");
    Check(twosf::rip::PieceState(code, "TWICE") == nullptr, "none for a label with two states with no suffix");
    Check(twosf::rip::PieceState(code, "VARIANT_ONLY") == nullptr, "none for a label with only a variant");
    Check(twosf::rip::PieceState(code, "MISSING") == nullptr, "none for a label without states");
    Check(twosf::rip::PieceState(twosf::rip::SdatCode{}, "PIECE") == nullptr, "none for code outside the table");
}

} // namespace

int main()
{
    TestNarcMember();
    TestNarcBounds();
    TestCutNarc();
    TestPieceState();

    return xsflib::testing::Summary("program_set");
}
