// SPDX-License-Identifier: MIT

// Tests the checks of the table of games and of the table of restarts (rip/sdat_set.h) against each game's words, and
// the scene calls of the table's titles and endings.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/ntr/code.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"
#include "xsflib/text.h"

#include "rip/sdat_set.h"
#include "stub_memory.h"

namespace
{

using twosf::testing::kBase;
using xsflib::testing::Check;

// The addresses that the table of games checks for Blue Rescue Team, and the game's words there: the first words of the
// BGM start, the ME start and the wait, the three calls after the start of the sound, the cmp of the first and the last
// ME, the mov of the level and the mov of the divisor.
constexpr std::pair<uint32_t, uint32_t> kBlueRescueTeamWords[] = {
    {0x0200A5F0, 0xE92D4030}, {0x0200A564, 0xE92D4010}, {0x02072F7C, 0xE92D4070}, {0x02000CC8, 0xEB001D9D},
    {0x02000CCC, 0xEB000662}, {0x02000CD0, 0xEB00218F}, {0x0200A75C, 0xE35000C8}, {0x0200A764, 0xE35000DB},
    {0x0200B194, 0xE3A00801}, {0x0200B19C, 0xE3A01C01},
};

// A decompressed ARM9 binary from 0x02000000 with Blue Rescue Team's words.
ntdlib::ntr::LoadedCode BlueRescueTeamCode()
{
    ntdlib::ntr::LoadedCode code{.address = kBase, .bytes = std::vector<uint8_t>(0x73000, 0)};
    for (const auto& [address, word] : kBlueRescueTeamWords)
    {
        xsflib::StoreLe32(&code.bytes[address - kBase], word);
    }

    return code;
}

// A copy of `code` with one bit of the word at `address` flipped.
ntdlib::ntr::LoadedCode WithBitFlipped(const ntdlib::ntr::LoadedCode& code, uint32_t address)
{
    ntdlib::ntr::LoadedCode changed = code;
    uint8_t* const at = &changed.bytes[address - code.address];
    xsflib::StoreLe32(at, xsflib::LoadLe32(at) ^ 1);

    return changed;
}

// The table of games matches the game's words and refuses a binary in which any one of them differs, and another game.
void TestGameStartCheck()
{
    const ntdlib::ntr::LoadedCode right = BlueRescueTeamCode();
    Check(twosf::rip::MatchesGameStart(right, "APHE"), "Blue Rescue Team's words");
    for (const auto& [address, word] : kBlueRescueTeamWords)
    {
        Check(!twosf::rip::MatchesGameStart(WithBitFlipped(right, address), "APHE"),
              "another word at 0x" + xsflib::Hex(address));
    }

    Check(!twosf::rip::MatchesGameStart(right, "YFEE"), "a game outside the table");
}

// Black 2's words (Thumb starts and setters, the start of a heap, the stream manager's start-up and start, and the
// start's call of the SDK's start of a stream), and the word that contains the halfword of the patch, blx r5 (0x47A8)
// at 0x0203C05E, in its high half.
constexpr uint32_t kBlack2Arm9 = 0x02004000;
constexpr uint32_t kBlack2PatchWord = 0x0203C05C;
constexpr std::pair<uint32_t, uint32_t> kBlack2Words[] = {
    {0x02005DF4, 0x1C05B538}, {0x02005E10, 0xB081B518}, {0x02005F94, 0x1C05B538},
    {0x02005F50, 0x1C07B5F8}, {0x0203A15C, 0x1C05B538}, {0x02006FC8, 0xB081B518},
    {0x02007054, 0x1C05B570}, {0x0200707C, 0xFE32F066}, {kBlack2PatchWord, 0x47A8D000},
};

// The table checks a patch's halfword too: Black 2 matches with its words, and not with the halfword changed. The other
// halfword of the same word doesn't count.
void TestPatchCheck()
{
    ntdlib::ntr::LoadedCode right{.address = kBlack2Arm9, .bytes = std::vector<uint8_t>(0x40000, 0)};
    for (const auto& [address, word] : kBlack2Words)
    {
        xsflib::StoreLe32(&right.bytes[address - kBlack2Arm9], word);
    }
    ntdlib::ntr::LoadedCode patched = right;
    xsflib::StoreLe16(&patched.bytes[kBlack2PatchWord + 2 - kBlack2Arm9], 0x47A9);
    ntdlib::ntr::LoadedCode other_half = right;
    xsflib::StoreLe16(&other_half.bytes[kBlack2PatchWord - kBlack2Arm9], 0xD001);

    Check(twosf::rip::MatchesGameStart(right, "IREO"), "Black 2's words and the patch's halfword");
    Check(!twosf::rip::MatchesGameStart(patched, "IREO"), "another halfword where the patch goes");
    Check(twosf::rip::MatchesGameStart(other_half, "IREO"), "the other halfword of the patch's word");
    for (const auto& [address, word] : kBlack2Words)
    {
        if (address != kBlack2PatchWord)
        {
            Check(!twosf::rip::MatchesGameStart(WithBitFlipped(right, address), "IREO"),
                  "another word at 0x" + xsflib::Hex(address));
        }
    }
}

// The instruction words of a game in the table of games, and the word at each patch's address: the patch's expected
// word or halfword in its low bytes, then the bytes after it.
struct GameWords
{
    std::string_view game_code;
    uint32_t arm9 = 0;
    std::span<const std::pair<uint32_t, uint32_t>> words;
};

// Pokémon White: Black's four words of the music manager, and its patch, blx r5 at 0x020336A4.
constexpr std::pair<uint32_t, uint32_t> kWhiteWords[] = {
    {0x02005D90, 0x1C05B538}, {0x02005DAC, 0xB081B518}, {0x02005F24, 0x1C05B538},
    {0x02005EE0, 0x1C07B5F8}, {0x020336A4, 0xE12FFF35},
};

// Pokémon White 2: Black 2's words of the music manager and the stream manager, its start of a heap 0x0203A188, its
// call of NNS_SndArcStrmStart at 0x0200707C, and its patch, blx r5 (0x47A8) at 0x0203C08A, followed by adds r0, r6, #0.
constexpr std::pair<uint32_t, uint32_t> kWhite2Words[] = {
    {0x02005DF4, 0x1C05B538}, {0x02005E10, 0xB081B518}, {0x02005F94, 0x1C05B538},
    {0x02005F50, 0x1C07B5F8}, {0x0203A188, 0x1C05B538}, {0x02006FC8, 0xB081B518},
    {0x02007054, 0x1C05B570}, {0x0200707C, 0xFE48F066}, {0x0203C08A, 0x1C3047A8},
};

// Pokémon SoulSilver: HeartGold's words at HeartGold's addresses.
constexpr std::pair<uint32_t, uint32_t> kSoulSilverWords[] = {
    {0x02005D48, 0x1C04B570}, {0x02004AC8, 0x1C04B510}, {0x02005AF8, 0x2800B508},
    {0x02006B24, 0xB082B538}, {0x0200596C, 0x1C0CB510},
};

// Pokémon Diamond and Pearl: the music manager's start, the field's setter, the scene routine, the ME's start and the
// room's volume.
constexpr std::pair<uint32_t, uint32_t> kDiamondPearlWords[] = {
    {0x0200521C, 0x1C05B570}, {0x0200414C, 0x1C04B510}, {0x0200516C, 0x2800B508},
    {0x02005C28, 0x1C05B538}, {0x0200488C, 0x1C0CB510},
};

constexpr GameWords kSiblingGames[] = {
    {"IRAO", 0x02004000, kWhiteWords},        {"IRDO", 0x02004000, kWhite2Words},
    {"IPGE", 0x02000000, kSoulSilverWords},   {"ADAE", 0x02000000, kDiamondPearlWords},
    {"APAE", 0x02000000, kDiamondPearlWords},
};

// A decompressed ARM9 binary with the words of `game`.
ntdlib::ntr::LoadedCode CodeWith(const GameWords& game)
{
    ntdlib::ntr::LoadedCode code{.address = game.arm9, .bytes = std::vector<uint8_t>(0x40000, 0)};
    for (const auto& [address, word] : game.words)
    {
        xsflib::StoreLe32(&code.bytes[address - game.arm9], word);
    }

    return code;
}

// The table's entries of White, White 2, SoulSilver, Diamond and Pearl match the words and patches of each game. Each
// entry refuses a binary in which any one of them differs. White and White 2 start their pieces with the calls of Black
// and Black 2, and the entries of Black and Black 2 refuse their binaries.
void TestSiblingEntries()
{
    for (const GameWords& game : kSiblingGames)
    {
        const ntdlib::ntr::LoadedCode right = CodeWith(game);

        Check(twosf::rip::MatchesGameStart(right, game.game_code), std::string(game.game_code) + "'s words");
        for (const auto& [address, word] : game.words)
        {
            Check(!twosf::rip::MatchesGameStart(WithBitFlipped(right, address), game.game_code),
                  std::string(game.game_code) + ": another word at 0x" + xsflib::Hex(address));
        }
    }

    Check(!twosf::rip::MatchesGameStart(CodeWith(kSiblingGames[0]), "IRBO"), "White's code, Black's entry");
    Check(!twosf::rip::MatchesGameStart(CodeWith(kSiblingGames[1]), "IREO"), "White 2's code, Black 2's entry");
}

// A game whose title and ending start after a call of its scene routine: the routine and the music manager's start,
// both Thumb.
struct SceneGame
{
    std::string_view game_code;
    uint32_t scene = 0;
    uint32_t start = 0;
};

constexpr SceneGame kSceneGames[] = {
    {"ADAE", 0x0200516D, 0x0200521D},
    {"APAE", 0x0200516D, 0x0200521D},
    {"CPUE", 0x020053CD, 0x0200549D},
};

// Diamond, Pearl and Platinum start SEQ_TITLE00 (1172) and SEQ_TITLE01 (1173) after the scene routine with 1, and
// SEQ_BLD_ENDING (1186) after it with 2. The unpatched boots of the games start SEQ_TITLE00 after the call with 1. The
// ending's 2 is a reading of each game's scene function.
void TestSceneCalls()
{
    using twosf::rip::StubArgument;
    using twosf::rip::StubCall;
    using Kind = StubArgument::Kind;
    const auto calls = [](const SceneGame& game, uint32_t scene)
    {
        return std::vector<StubCall>{
            {.routine = game.scene, .arguments = {StubArgument{.kind = Kind::kConstant, .value = scene}}, .count = 1},
            {.routine = game.start, .arguments = {StubArgument{.kind = Kind::kSound}}, .count = 1}};
    };

    for (const SceneGame& game : kSceneGames)
    {
        const std::string name(game.game_code);
        for (const uint32_t title : {1172u, 1173u})
        {
            Check(twosf::rip::StartCallsOf(game.game_code, title, 7) == calls(game, 1),
                  name + ": the scene routine with 1 before " + std::to_string(title));
        }
        Check(twosf::rip::StartCallsOf(game.game_code, 1186, 7) == calls(game, 2),
              name + ": the scene routine with 2 before 1186");
    }
    Check(twosf::rip::StartCallsOf("YFEE", 1172, 7).empty(), "no calls for a game outside the table");
}

// The table of restarts gives Shin Monshou no Nazo's restart where its start-up starts with the table's two words. If
// either word differs, the lookup gives no restart and leaves the sequence out.
void TestRestartLookup()
{
    constexpr uint32_t kArm9 = 0x02004000;
    constexpr uint32_t kStartUp = 0x02015DA0;
    ntdlib::ntr::LoadedCode right{.address = kArm9, .bytes = std::vector<uint8_t>(0x12000, 0)};
    xsflib::StoreLe32(&right.bytes[kStartUp - kArm9], 0xE92D4038);
    xsflib::StoreLe32(&right.bytes[kStartUp + 4 - kArm9], 0xEBFFFFC1);

    const twosf::rip::RestartLookup found = twosf::rip::LookUpRestart(right, "VI2J");
    Check(found.restart && found.restart->sequence == 86 && found.restart->routine == kStartUp &&
              found.restart->argument == 1 && !found.left_out,
          "Shin Monshou no Nazo's restart");
    for (const uint32_t address : {kStartUp, kStartUp + 4})
    {
        const twosf::rip::RestartLookup differs = twosf::rip::LookUpRestart(WithBitFlipped(right, address), "VI2J");
        Check(!differs.restart && differs.left_out == 86u,
              "another word at 0x" + xsflib::Hex(address) + ": the sequence is left out");
    }

    const twosf::rip::RestartLookup other_game = twosf::rip::LookUpRestart(right, "YFEE");
    Check(!other_game.restart && !other_game.left_out, "a game outside the table");
}

} // namespace

int main()
{
    TestGameStartCheck();
    TestPatchCheck();
    TestSiblingEntries();
    TestSceneCalls();
    TestRestartLookup();

    return xsflib::testing::Summary("sdat_table");
}
