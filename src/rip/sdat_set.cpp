// SPDX-License-Identifier: MIT

#include "rip/sdat_set.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/arm/assembler.h"
#include "ntdlib/ntr/compact_image.h"
#include "ntdlib/ntr/machine.h"
#include "ntdlib/ntr/main_memory.h"
#include "ntdlib/ntr/rip.h"
#include "ntdlib/ntr/rom_writer.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/ntr/sound_routines.h"
#include "ntdlib/sdat/layout.h"
#include "xsflib/bytes.h"
#include "xsflib/text.h"

#include "rip/arm_code.h"
#include "rip/boots.h"
#include "rip/game_start_table.h"
#include "rip/stream_set.h"
#include "rip/zelda_starts.h"

namespace twosf::rip
{

namespace
{

namespace arm = ntdlib::arm;
namespace ntr = ntdlib::ntr;
namespace sdat = ntdlib::sdat;
using ntr::snd::CommandId;

// The boot's limit.
constexpr uint64_t kBootClock = 30ull * ntr::kSystemClockHz;

// A frame heap's signature, and the fields of the sound heap around it.
constexpr uint32_t kFrameHeap = 0x46524D48; // "FRMH"
constexpr uint32_t kHeapToSignature = 0x10;
constexpr uint32_t kHeapStartField = 0x18;
constexpr uint32_t kHeapEndField = 0x1C;
constexpr uint32_t kHeapHeadField = 0x24;
constexpr uint32_t kHeapTailField = 0x28;

// A sound archive's information block: its signature, its size, and the offsets of its eight tables of records from the
// block's start. NNS_SndArcInit loads the block into the heap that the game gives it.
constexpr uint32_t kInfoBlock = 0x4F464E49; // "INFO"
constexpr uint32_t kInfoSizeField = 4;
constexpr uint32_t kInfoTablesField = 8;
constexpr uint32_t kInfoTables = 8;
constexpr uint32_t kInfoHeaderBytes = 0x40;

// The IPC's tag of the sound commands. A FIFO word with it gives a list's address in its bits 6 to 31.
constexpr uint32_t kSoundTag = 7;
constexpr uint32_t kTagBits = 0x1F;
constexpr int kListShift = 6;

// A command in main memory: the link to the next, the id, then four arguments.
constexpr uint32_t kCommandId = 4;
constexpr uint32_t kCommandArgs = 8;

// A sequence file's offset of its data, and a bank's links to its wave archives: for each of its four slots, the wave
// archive's address, then a link to the next bank that uses it.
constexpr std::size_t kSequenceDataField = 0x18;
constexpr std::size_t kBankLinksStart = 0x18;
constexpr std::size_t kBankLinksEnd = 0x38;
constexpr uint32_t kBankLinkBytes = 8;

// The CPSR's mode bits, its I bit, and the modes in which a stub may run: user and system.
constexpr uint32_t kModeBits = 0x1F;
constexpr uint32_t kIrqDisabled = 0x80;
constexpr uint32_t kUserMode = 0x10;
constexpr uint32_t kSystemMode = 0x1F;

// The load's arguments without a loader: every kind of data (0xFF), the heap, 1 and a null pointer.
constexpr uint32_t kEveryKind = 0xFF;

// A sound's number that takes a literal in a stub's call. A call with it has the largest form.
constexpr uint32_t kLiteralSound = 0x12345678;

// The values of a start's arguments (StubArgument): the sound's number, and for a sequence of a sequence archive the
// archive's number, the sequence's index in it, its bank and the sound heap.
struct StartValues
{
    uint32_t sound = 0;
    uint32_t archive = 0;
    uint32_t entry = 0;
    uint32_t bank = 0;
    uint32_t heap = 0;
};

// Values that each take a literal: a block with them has its largest form.
constexpr StartValues kLiteralValues = {.sound = kLiteralSound,
                                        .archive = kLiteralSound,
                                        .entry = kLiteralSound,
                                        .bank = kLiteralSound,
                                        .heap = kLiteralSound};

// Pokémon Mystery Dungeon: Blue Rescue Team (USA). The game's request of a sound (0x0200ADB8) starts an ME, 200 to 219
// (cmp r0, #200 at 0x0200A75C, cmp r0, #219 at 0x0200A764), with 0x0200A564, and an effect, 300 to 939 (0x0200A778).
// For every other number it returns (0x0200AE70). A request of a BGM enters at 0x0200B128. For a start with no fade,
// that routine sets the level 0x10000 (mov r0, #0x10000 at 0x0200B194, stored at 0x0200B1A4). It divides the level by
// the divisor 256 (mov r1, #256 at 0x0200B19C, the division at 0x0200B1B4) and calls the start of a BGM, 0x0200A5F0,
// with the quotient, 0x100 (0x0200B1C0). The start of a BGM loads the sequence and its bank into the sound heap
// (0x02070550, 0x02070518) and starts it on player 0 with the handle at 0x0210D3EC (NNS_SndArcPlayerStartSeqEx at
// 0x02070844). It sets the player's volume to the level over 2, at most 127 (0x0200A748). Then it sets the channels of
// each track (NNS_SndPlayerSetTrackAllocatableChannel at 0x0206E410): 0xFFFA on every track of the five sequences of
// the table at 0x02086040, otherwise 0x8FFA on tracks 0 to 7 and 0x7000 on tracks 8 to 15. The start of an ME loads the
// sequence and starts it on player 1 with the handle at 0x0210D3F0. It sets no channels. A separate thread of the game
// (0x02007E54) calls the sound library's update. Between two updates the thread sleeps (OS_SleepThread(NULL) at
// 0x02072F7C) until the handler of the vertical blank (0x02007E6C) wakes it. The main function starts the sound
// (0x0200B1F8) and calls the rest of the game's start-up from 0x02000CC8 on: the park replaces its three calls.
constexpr StubCall kApheCalls[] = {MakeCall(0x0200A5F0, {Sound(), Constant(0x100)})};
constexpr std::pair<uint32_t, uint32_t> kApheMes[] = {{200, 219}};
constexpr StubCall kApheMeCalls[] = {MakeCall(0x0200A564, {Sound()})};
constexpr NumberCallsEntry kApheNumbers[] = {{.ranges = kApheMes, .calls = kApheMeCalls}};
constexpr GameWord kApheWords[] = {
    {0x0200A5F0, 0xE92D4030}, // stmfd sp!, {r4, r5, lr}
    {0x0200A564, 0xE92D4010}, // stmfd sp!, {r4, lr}
    {0x02072F7C, 0xE92D4070}, // stmfd sp!, {r4, r5, r6, lr}
    {0x02000CC8, 0xEB001D9D}, // the three calls that the park replaces
    {0x02000CCC, 0xEB000662}, {0x02000CD0, 0xEB00218F}, {0x0200A75C, 0xE35000C8}, // cmp r0, #200
    {0x0200A764, 0xE35000DB},                                                     // cmp r0, #219
    {0x0200B194, 0xE3A00801},                                                     // mov r0, #0x10000
    {0x0200B19C, 0xE3A01C01},                                                     // mov r1, #256
};

// Pokémon Black (USA). Without a backup memory, overlay 228 reads the save through 0x020334CC, the SDK's read fails,
// and 0x020335E8 calls the error callback (blx r5 at 0x0203368C), the error screen 0x0202F768. The error screen never
// returns. The patch makes the call a mov r0, r0: the read returns 0, and the game takes the path of a blank save. The
// game's music manager starts a piece with 0x02005D90(sequence, track mask), Thumb, from about 70 call sites in the
// ARM9 binary and the overlays. It resets the music's state and sets the handle's volume to 127 (0x02005F90), stops the
// handle (0x02005D14), loads the piece (0x02005924), starts it with NNS_SndArcPlayerStartSeqEx(handle, 0, -1, -1,
// sequence) at 0x02005D6C, and sets the track mask when it isn't 0xFFFF (0x02005EB4). 0x02005DAC(sequence) starts the
// five pieces whose records name player 6 on player 6 (StartSeqEx at 0x02005DDA): SEQ_BGM_TITLE (1003), TITLE01 (1004),
// POKEMON_THEME (1007), ENDING (1016) and GF_LOGO (1173). Overlays 88 and 181 call it with 1007 and 1016.
constexpr GamePatch kIrboPatches[] = {{.address = 0x0203368C, .expected = 0xE12FFF35, .value = 0xE1A00000}};
constexpr StubCall kIrboCalls[] = {MakeCall(0x02005D91, {Sound(), Constant(0xFFFF)})};
constexpr StubCall kIrboPlayer6Calls[] = {MakeCall(0x02005DAD, {Sound()})};
constexpr NumberCallsEntry kIrboNumbers[] = {{.player = 6, .calls = kIrboPlayer6Calls}};
constexpr GameWord kIrboWords[] = {
    {0x02005D90, 0x1C05B538}, // push {r3, r4, r5, lr}, adds r5, r0, #0
    {0x02005DAC, 0xB081B518}, // push {r3, r4, lr}, sub sp, #4
    {0x02005F24, 0x1C05B538}, // the setter of the tracks' volumes: push {r3, r4, r5, lr}, adds r5, r0, #0
    {0x02005EE0, 0x1C07B5F8}, // the tuning routine: push {r3, r4, r5, r6, r7, lr}, adds r7, r0, #0
};

// Black's field units set these states of the BGM's tracks: the switch unit as the player enters a zone (0x0202BF54,
// then 0x0202BFF0 with a fade of one frame), the dungeon unit (0x0202B2CC: the tempo's ratio and a pan of 0 on every
// track through 0x02005EE0, then the pitch of tracks 0xFCFF), the parts unit's first group (0x0202C710), the distance
// unit far from its point (0x0202AEBC) and the 3D unit at the sequence's start (0x0202BB48). Each calls the setter
// 0x02005F24 (tracks, volume), NNS_SndPlayerSetTrackVolume on the BGM's handle (0x0200590C). The rows are the units'
// tables, as ntdlib's profile reads them (`kTrackStates` in game_profiles.cpp). The profile is the check of the states.
constexpr TrackStateEntry kIrboTrackStates[] = {
    {"SEQ_BGM_D_CHAMPROAD", "", 0x027F, 0xFD80},
    {"SEQ_BGM_D_CHAMPROAD", "_ROUTE10_1", 0x0400, 0xFBFF},
    {"SEQ_BGM_D_CHAMPROAD", "_ROUTE10_2", 0x0600, 0xF9FF},
    {"SEQ_BGM_D_CHAMPROAD", "_ROUTE10_3", 0x0610, 0xF9EF},
    {"SEQ_BGM_D_CHAMPROAD", "_ROUTE10_4", 0x0650, 0xF9AF},
    {"SEQ_BGM_D_CHAMPROAD", "_ROUTE10_5", 0x0654, 0xF9AB},
    {"SEQ_BGM_D_CHAMPROAD", "_ROUTE10_6", 0x0674, 0xF98B},
    {"SEQ_BGM_D_CHAMPROAD", "_ROUTE10_7", 0x067C, 0xF983},
    {"SEQ_BGM_D_CHAMPROAD", "_ROUTE10_8", 0x067E, 0xF981},
    {"SEQ_BGM_D_CHAMPROAD", "_ROUTE10_9", 0x067F, 0xF980},
    {"SEQ_BGM_T_05_SP", "_INDOORS", 0x03FF, 0xFC00},
    {"SEQ_BGM_T_05_SU", "_INDOORS", 0x03FF, 0xFC00},
    {"SEQ_BGM_T_05_AU", "_INDOORS", 0x03FF, 0xFC00},
    {"SEQ_BGM_T_05_WI", "_INDOORS", 0x03FF, 0xFC00},
    {"SEQ_BGM_D_01", "_LOWER1", 0x0000, 0x0000, 235, 0xFCFF, -192},
    {"SEQ_BGM_D_02", "_LOWER1", 0x0000, 0x0000, 235, 0xFCFF, -192},
    {"SEQ_BGM_D_02", "_LOWER2", 0x0000, 0x0000, 215, 0xFCFF, -384},
    {"SEQ_BGM_T_02", "_ENTRY", 0xFC7F, 0x0380},
    {"SEQ_BGM_H_04", "_ENTRY", 0xFC05, 0x03FA},
    {"SEQ_BGM_BATTLE_SUBWAY", "_ENTRY", 0xF948, 0x06B7},
    {"SEQ_BGM_BATTLE_SUBWAY", "_ROUND1", 0xFB48, 0x04B7},
    {"SEQ_BGM_BATTLE_SUBWAY", "_ROUND2", 0xFBC8, 0x0437},
    {"SEQ_BGM_BATTLE_SUBWAY", "_ROUND3", 0xFFC8, 0x0037},
    {"SEQ_BGM_BATTLE_SUBWAY", "_ROUND4", 0xFFCE, 0x0031},
    {"SEQ_BGM_BATTLE_SUBWAY", "_ROUND5", 0xFFCF, 0x0030},
    {"SEQ_BGM_C_02", "_FAR", 0x0000, 0x0300},
    {"SEQ_BGM_GYM_C05", "_FAR", 0x0000, 0x0300},
    {"SEQ_BGM_C_07", "_FAR", 0x0000, 0x0300},
    {"SEQ_BGM_C_08_B", "_FAR", 0x0000, 0x0300},
    {"SEQ_BGM_T_03", "_FAR", 0x0000, 0x0300},
    {"SEQ_BGM_H_01", "_FAR", 0x0000, 0x03C0},
    {"SEQ_BGM_H_03", "_FAR", 0x0000, 0x0380},
};

// `rows` with the label `from` replaced by `to`. Pokémon White and White 2 play SEQ_BGM_C_08_W in Opelucid City, where
// Black and Black 2 play SEQ_BGM_C_08_B, and their field units set the same states of its tracks.
template <std::size_t N>
constexpr std::array<TrackStateEntry, N> Relabeled(const TrackStateEntry (&rows)[N], std::string_view from,
                                                   std::string_view to)
{
    std::array<TrackStateEntry, N> relabeled{};
    for (std::size_t i = 0; i < N; i++)
    {
        relabeled[i] = rows[i];
        if (rows[i].label == from)
        {
            relabeled[i].label = to;
        }
    }

    return relabeled;
}

// Pokémon White (USA): Black's music manager at Black's addresses, with Black's instruction words there (kIrboWords).
// The set therefore starts each piece with Black's calls. White's save read is Black's 0x18 bytes later: its error
// callback is blx r5 at 0x020336A4. The patch makes that call a mov r0, r0. The field units' states are Black's, as
// ntdlib's profile gives them for both games, with Opelucid City's SEQ_BGM_C_08_W in place of SEQ_BGM_C_08_B. The zone
// table names _B, and the zone's BGM function (0x02013BEC) plays _W in its place (at 0x02013C1C).
constexpr GamePatch kIraoPatches[] = {{.address = 0x020336A4, .expected = 0xE12FFF35, .value = 0xE1A00000}};
constexpr auto kIraoTrackStates = Relabeled(kIrboTrackStates, "SEQ_BGM_C_08_B", "SEQ_BGM_C_08_W");

// Pokémon Black 2 (USA): Black's code at other addresses. The save read is 0x0203BFF8, Thumb, and its error callback is
// blx r5 at 0x0203C05E. The patch makes that call a mov r8, r8. The music manager's starts are 0x02005DF4(sequence,
// track mask) and 0x02005E10(sequence) for the pieces on player 6: SEQ_BGM_TITLE (1003), TITLE01 (1004), POKEMON_THEME
// (1007), ENDING (1016) and GF_LOGO (1171). The SDK's sound library is Thumb code.
//
// The game's one stream, STRM_TITLE, is the music of the title screen. At the hook, the SDK's stream player has no
// buffers yet, and NNS_SndArcStrmStart (0x0206DCE4) returns 0: its claim of a player (0x0206DE90) finds the buffer's
// word empty (0x0206DEB2). The title screen, overlay 162, sets the player up. Its start-up (0x0219CE80) makes the
// game's heap 0x16 of 0x120000 bytes in heap 1 (0x0203A15C at 0x0219CE8E) and calls the start-up of the game's stream
// manager with that heap (0x02006FC8 at 0x0219CEAA). The manager takes 12 bytes of the heap for its state, starts the
// SDK's stream player with the game's sound heap and the thread priority 15 (NNS_SndArcStrmInit, 0x0206DB40 at
// 0x02006FEE), clears its stream handle and takes the channels of the archive's stream player 0 (0x0206DD58 at
// 0x02006FFA). The screen then starts stream 0 through the manager's start (0x02007054 at 0x0219D0B6). The manager's
// start calls NNS_SndArcStrmStart with the manager's handle, the stream and the offset 0 (0x0200707C). The screen sets
// no volume. The stream's calls are those three calls.
constexpr GamePatch kIreoPatches[] = {{.address = 0x0203C05E, .expected = 0x47A8, .value = 0x46C0, .bytes = 2}};
constexpr StubCall kIreoCalls[] = {MakeCall(0x02005DF5, {Sound(), Constant(0xFFFF)})};
constexpr StubCall kIreoPlayer6Calls[] = {MakeCall(0x02005E11, {Sound()})};
constexpr NumberCallsEntry kIreoNumbers[] = {{.player = 6, .calls = kIreoPlayer6Calls}};
constexpr StubCall kIreoStreamCalls[] = {MakeCall(0x0203A15D, {Constant(1), Constant(0x16), Constant(0x120000)}),
                                         MakeCall(0x02006FC9, {Constant(0x16)}), MakeCall(0x02007055, {Sound()})};
constexpr GameWord kIreoWords[] = {
    {0x02005DF4, 0x1C05B538}, // push {r3, r4, r5, lr}, adds r5, r0, #0
    {0x02005E10, 0xB081B518}, // push {r3, r4, lr}, sub sp, #4
    {0x02005F94, 0x1C05B538}, // the setter of the tracks' volumes
    {0x02005F50, 0x1C07B5F8}, // the tuning routine
    {0x0203A15C, 0x1C05B538}, // the start of a heap: push {r3, r4, r5, lr}, adds r5, r0, #0
    {0x02006FC8, 0xB081B518}, // the stream manager's start-up: push {r3, r4, lr}, sub sp, #4
    {0x02007054, 0x1C05B570}, // the stream manager's start: push {r4, r5, r6, lr}, adds r5, r0, #0
    {0x0200707C, 0xFE32F066}, // its call of NNS_SndArcStrmStart: bl 0x0206DCE4
};

// Black 2's field units: Black's at other addresses, with the setter 0x02005F94 and the tuning routine 0x02005F50.
// Victory Road's base plays every track in Black 2 (zones 573 on), and _INNER gives the state of Black's base.
constexpr TrackStateEntry kIreoTrackStates[] = {
    {"SEQ_BGM_D_CHAMPROAD", "_INNER", 0x027F, 0xFD80},
    {"SEQ_BGM_D_CHAMPROAD", "_ROUTE10_1", 0x0400, 0xFBFF},
    {"SEQ_BGM_D_CHAMPROAD", "_ROUTE10_2", 0x0600, 0xF9FF},
    {"SEQ_BGM_D_CHAMPROAD", "_ROUTE10_3", 0x0610, 0xF9EF},
    {"SEQ_BGM_D_CHAMPROAD", "_ROUTE10_4", 0x0650, 0xF9AF},
    {"SEQ_BGM_D_CHAMPROAD", "_ROUTE10_5", 0x0654, 0xF9AB},
    {"SEQ_BGM_D_CHAMPROAD", "_ROUTE10_6", 0x0674, 0xF98B},
    {"SEQ_BGM_D_CHAMPROAD", "_ROUTE10_7", 0x067C, 0xF983},
    {"SEQ_BGM_D_CHAMPROAD", "_ROUTE10_8", 0x067E, 0xF981},
    {"SEQ_BGM_D_CHAMPROAD", "_ROUTE10_9", 0x067F, 0xF980},
    {"SEQ_BGM_T_05_SP", "_INDOORS", 0x03FF, 0xFC00},
    {"SEQ_BGM_T_05_SU", "_INDOORS", 0x03FF, 0xFC00},
    {"SEQ_BGM_T_05_AU", "_INDOORS", 0x03FF, 0xFC00},
    {"SEQ_BGM_T_05_WI", "_INDOORS", 0x03FF, 0xFC00},
    {"SEQ_BGM_D_01", "_LOWER1", 0x0000, 0x0000, 235, 0xFCFF, -192},
    {"SEQ_BGM_D_02", "_LOWER1", 0x0000, 0x0000, 235, 0xFCFF, -192},
    {"SEQ_BGM_D_02", "_LOWER2", 0x0000, 0x0000, 215, 0xFCFF, -384},
    {"SEQ_BGM_T_02", "_ENTRY", 0xFC7F, 0x0380},
    {"SEQ_BGM_H_04", "_ENTRY", 0xFC05, 0x03FA},
    {"SEQ_BGM_BATTLE_SUBWAY", "_ENTRY", 0xF948, 0x06B7},
    {"SEQ_BGM_BATTLE_SUBWAY", "_ROUND1", 0xFB48, 0x04B7},
    {"SEQ_BGM_BATTLE_SUBWAY", "_ROUND2", 0xFBC8, 0x0437},
    {"SEQ_BGM_BATTLE_SUBWAY", "_ROUND3", 0xFFC8, 0x0037},
    {"SEQ_BGM_BATTLE_SUBWAY", "_ROUND4", 0xFFCE, 0x0031},
    {"SEQ_BGM_BATTLE_SUBWAY", "_ROUND5", 0xFFCF, 0x0030},
    {"SEQ_BGM_ERECTRIC_GYM_01", "_ENTRY", 0xFCC0, 0x033F},
    {"SEQ_BGM_ERECTRIC_GYM_01", "_STAGE1", 0xFCF2, 0x030D},
    {"SEQ_BGM_C_02", "_FAR", 0x0000, 0x0300},
    {"SEQ_BGM_C_07", "_FAR", 0x0000, 0x0300},
    {"SEQ_BGM_C_08_B", "_FAR", 0x0000, 0x0300},
    {"SEQ_BGM_T_03", "_FAR", 0x0000, 0x0300},
    {"SEQ_BGM_H_01", "_FAR", 0x0000, 0x03C0},
    {"SEQ_BGM_H_03", "_FAR", 0x0000, 0x0380},
};

// Pokémon White 2 (USA): Black 2's music manager, setters and stream manager at Black 2's addresses (0x02005DF4,
// 0x02005E10, 0x02005F94, 0x02005F50, 0x02006FC8 and 0x02007054). The set therefore starts each piece with Black 2's
// calls. The save read's error callback is blx r5 at 0x0203C08A. The patch makes that call a mov r8, r8. The title
// screen, overlay 162 (0x0219CEC0), makes the same three calls as Black 2's: the heap 0x16 of 0x120000 bytes in heap 1
// through 0x0203A188 (at 0x0219CECE), the stream manager's start-up with that heap (0x02006FC8 at 0x0219CEEA), and the
// manager's start of stream 0 (0x02007054 at 0x0219D0F6). The manager calls NNS_SndArcStrmStart, 0x0206DD10, at
// 0x0200707C. The field units' states are Black 2's, with Opelucid City's SEQ_BGM_C_08_W in place of SEQ_BGM_C_08_B:
// the zone's BGM function plays _W there (0x020189B8), as White's does.
constexpr GamePatch kIrdoPatches[] = {{.address = 0x0203C08A, .expected = 0x47A8, .value = 0x46C0, .bytes = 2}};
constexpr StubCall kIrdoStreamCalls[] = {MakeCall(0x0203A189, {Constant(1), Constant(0x16), Constant(0x120000)}),
                                         MakeCall(0x02006FC9, {Constant(0x16)}), MakeCall(0x02007055, {Sound()})};
constexpr GameWord kIrdoWords[] = {
    {0x02005DF4, 0x1C05B538}, // push {r3, r4, r5, lr}, adds r5, r0, #0
    {0x02005E10, 0xB081B518}, // push {r3, r4, lr}, sub sp, #4
    {0x02005F94, 0x1C05B538}, // the setter of the tracks' volumes
    {0x02005F50, 0x1C07B5F8}, // the tuning routine
    {0x0203A188, 0x1C05B538}, // the start of a heap: push {r3, r4, r5, lr}, adds r5, r0, #0
    {0x02006FC8, 0xB081B518}, // the stream manager's start-up: push {r3, r4, lr}, sub sp, #4
    {0x02007054, 0x1C05B570}, // the stream manager's start: push {r4, r5, r6, lr}, adds r5, r0, #0
    {0x0200707C, 0xFE48F066}, // its call of NNS_SndArcStrmStart: bl 0x0206DD10
};
constexpr auto kIrdoTrackStates = Relabeled(kIreoTrackStates, "SEQ_BGM_C_08_B", "SEQ_BGM_C_08_W");

// Pokémon HeartGold (USA). The music manager's start is 0x02005D48(sequence), Thumb, from about 30 call sites. It reads
// the record's player (0x020054D4). For player 7 it starts the piece with 0x02005DC4, and for player 1 with 0x02005DF4.
// 0x02005DF4 takes the bank from the sequence in the sound state's slot 0x20 (0x02004400 with 0x20). The game sets that
// slot with 0x02004AC8(sequence) when a map's music changes. The set plays each piece in the state of the map that
// plays it: the slot holds the piece, and the bank is the piece's. The title's scene routine 0x02005AF8, with 1, gives
// player 7 channels 0x3FFF (0x02005B0C), where the archive gives 0xA7FE. The title, the ending and the theme call it
// before their pieces: SEQ_GS_POKEMON_THEME (1008), SEQ_GS_P_POKEMON_THEME (1222), SEQ_GS_ENDING (1016) and
// SEQ_GS_ENDING2 (1017).
constexpr StubCall kIpkeCalls[] = {MakeCall(0x02005D49, {Sound()})};
constexpr std::pair<uint32_t, uint32_t> kIpkeScenes[] = {{1008, 1008}, {1016, 1017}, {1222, 1222}};
constexpr StubCall kIpkeSceneCalls[] = {MakeCall(0x02005AF9, {Constant(1)}), MakeCall(0x02005D49, {Sound()})};
constexpr StubCall kIpkeFieldCalls[] = {MakeCall(0x02004AC9, {Sound()}), MakeCall(0x02005D49, {Sound()})};
constexpr StubCall kIpkeMeCalls[] = {MakeCall(0x02006B25, {Sound()})};
constexpr NumberCallsEntry kIpkeNumbers[] = {{.ranges = kIpkeScenes, .calls = kIpkeSceneCalls},
                                             {.player = 1, .calls = kIpkeFieldCalls},
                                             {.player = 2, .calls = kIpkeMeCalls}};
constexpr StubCall kIpkeFast[] = {MakeCall(0x0200596D, {Constant(7), Constant(406)})};
constexpr GameStateEntry kIpkeStates[] = {{"SEQ_GS_WIFIGAME", "_FAST", kIpkeFast}};
constexpr GameWord kIpkeWords[] = {
    {0x02005D48, 0x1C04B570}, // push {r4, r5, r6, lr}, adds r4, r0, #0
    {0x02004AC8, 0x1C04B510}, // push {r4, lr}, adds r4, r0, #0
    {0x02005AF8, 0x2800B508}, // push {r3, lr}, cmp r0, #0
    {0x02006B24, 0xB082B538}, // the ME's start: push {r3, r4, r5, lr}, sub sp, #8
    {0x0200596C, 0x1C0CB510}, // the tempo's setter: push {r4, lr}, adds r4, r1, #0
};

// Pokémon Platinum (USA, revision 1): HeartGold's manager at other addresses. The music manager's start is
// 0x0200549C(sequence), Thumb. For player 7 it starts the piece with 0x02005508, and for player 1 with 0x02005538.
// 0x02005538 takes the bank from the sequence in the sound state's slot 0x20. The game sets that slot with
// 0x02004224(sequence) at a map's change (0x0205568E, 0x020556B4, 0x0208BE0E). For any other player the manager starts
// nothing (0x020054D0). The fanfares (PLAYER_ME, player 2) start through 0x02006150(sequence). That routine pauses the
// BGM and starts the sequence on handle 2 (0x02006182 to 0x0200618A). The scene routine 0x020053CC, with 1, gives
// player 7 channels 0x7FFF and starts the reverb at volume 30, and with 2 at volume 15. The title (overlay 63's
// equivalents) calls it with 1 before SEQ_TITLE00 (1172) and SEQ_TITLE01 (1173), and the credits with 2 before
// SEQ_BLD_ENDING (1186). The room's script sets the TV's music to volume 50 (_ROOM) through 0x02004AD4(sequence,
// volume). That routine looks up the record's player (0x02004B18) and sets the player's volume (0x02004A68). Overlay
// 115's games set the tempo's ratio of handle 7 through 0x02004FA8(handle, ratio): their ramp ends at 406 (_FAST), and
// the reset sets 256 again (0x02265A08).
constexpr StubCall kCpueCalls[] = {MakeCall(0x0200549D, {Sound()})};
constexpr std::pair<uint32_t, uint32_t> kCpueTitles[] = {{1172, 1173}};
constexpr StubCall kCpueTitleCalls[] = {MakeCall(0x020053CD, {Constant(1)}), MakeCall(0x0200549D, {Sound()})};
constexpr std::pair<uint32_t, uint32_t> kCpueEnding[] = {{1186, 1186}};
constexpr StubCall kCpueEndingCalls[] = {MakeCall(0x020053CD, {Constant(2)}), MakeCall(0x0200549D, {Sound()})};
constexpr StubCall kCpueFieldCalls[] = {MakeCall(0x02004225, {Sound()}), MakeCall(0x0200549D, {Sound()})};
constexpr StubCall kCpueMeCalls[] = {MakeCall(0x02006151, {Sound()})};
constexpr NumberCallsEntry kCpueNumbers[] = {{.ranges = kCpueTitles, .calls = kCpueTitleCalls},
                                             {.ranges = kCpueEnding, .calls = kCpueEndingCalls},
                                             {.player = 1, .calls = kCpueFieldCalls},
                                             {.player = 2, .calls = kCpueMeCalls}};
constexpr StubCall kCpueRoom[] = {MakeCall(0x02004AD5, {Sound(), Constant(50)})};
constexpr StubCall kCpueFast[] = {MakeCall(0x02004FA9, {Constant(7), Constant(406)})};
constexpr GameStateEntry kCpueStates[] = {{"SEQ_TV_HOUSOU", "_ROOM", kCpueRoom},
                                          {"SEQ_PL_WIFIGAME", "_FAST", kCpueFast}};
constexpr GameWord kCpueWords[] = {
    {0x0200549C, 0x1C05B570}, // push {r4, r5, r6, lr}, adds r5, r0, #0
    {0x02004224, 0x1C04B510}, // push {r4, lr}, adds r4, r0, #0
    {0x020053CC, 0x2800B508}, // push {r3, lr}, cmp r0, #0
    {0x02006150, 0x1C05B538}, // push {r3, r4, r5, lr}, adds r5, r0, #0
    {0x02004AD4, 0x1C0CB510}, // push {r4, lr}, adds r4, r1, #0
    {0x02004FA8, 0x1C0CB510}, // push {r4, lr}, adds r4, r1, #0
};

// Pokémon Diamond and Pearl (USA, revision 5): Platinum's manager at other addresses. Both games have these routines at
// the same addresses, with the same instruction words. The music manager's start is 0x0200521C(sequence), Thumb. It
// reads the record's player (0x020048D0). For player 7 it starts the piece with 0x02005288, and for player 1 with
// 0x020052B8. 0x020052B8 takes the bank from the sequence in the sound state's slot 0x1F (0x02003D38 with 0x1F at
// 0x020052C6). For any other player it starts nothing (0x02005250). The game sets that slot with 0x0200414C(sequence)
// at a map's change (0x0204ACBC, 0x0204ADAA, 0x0204ADD0). The fanfares (PLAYER_ME, player 2) start through
// 0x02005C28(sequence). That routine starts the sequence on handle 2 (0x02005C5A to 0x02005C62). The scene routine
// 0x0200516C, with 1, gives player 7 channels 0x7FFF and starts the reverb at volume 30, and with 2 at volume 15
// (0x02004C80 at 0x0200518C and 0x02005198). The scene function 0x0200433C calls it with 1 for the title's scene
// (0x02004462) and with 2 for the credits' (0x02004474). The title starts SEQ_TITLE00 (1172) and SEQ_TITLE01 (1173) in
// that scene, and the credits SEQ_BLD_ENDING (1186). The room's script calls 0x0200488C(sequence, volume) at 0x0204169E
// to set the TV's music to volume 50 (_ROOM). That routine looks up the record's player (0x020048D0) and sets the
// player's volume (0x0200483C). The games have no Wi-Fi Plaza, and so no _FAST.
constexpr StubCall kAdaeCalls[] = {MakeCall(0x0200521D, {Sound()})};
constexpr std::pair<uint32_t, uint32_t> kAdaeTitles[] = {{1172, 1173}};
constexpr StubCall kAdaeTitleCalls[] = {MakeCall(0x0200516D, {Constant(1)}), MakeCall(0x0200521D, {Sound()})};
constexpr std::pair<uint32_t, uint32_t> kAdaeEnding[] = {{1186, 1186}};
constexpr StubCall kAdaeEndingCalls[] = {MakeCall(0x0200516D, {Constant(2)}), MakeCall(0x0200521D, {Sound()})};
constexpr StubCall kAdaeFieldCalls[] = {MakeCall(0x0200414D, {Sound()}), MakeCall(0x0200521D, {Sound()})};
constexpr StubCall kAdaeMeCalls[] = {MakeCall(0x02005C29, {Sound()})};
constexpr NumberCallsEntry kAdaeNumbers[] = {{.ranges = kAdaeTitles, .calls = kAdaeTitleCalls},
                                             {.ranges = kAdaeEnding, .calls = kAdaeEndingCalls},
                                             {.player = 1, .calls = kAdaeFieldCalls},
                                             {.player = 2, .calls = kAdaeMeCalls}};
constexpr StubCall kAdaeRoom[] = {MakeCall(0x0200488D, {Sound(), Constant(50)})};
constexpr GameStateEntry kAdaeStates[] = {{"SEQ_TV_HOUSOU", "_ROOM", kAdaeRoom}};
constexpr GameWord kAdaeWords[] = {
    {0x0200521C, 0x1C05B570}, // push {r4, r5, r6, lr}, adds r5, r0, #0
    {0x0200414C, 0x1C04B510}, // push {r4, lr}, adds r4, r0, #0
    {0x0200516C, 0x2800B508}, // push {r3, lr}, cmp r0, #0
    {0x02005C28, 0x1C05B538}, // push {r3, r4, r5, lr}, adds r5, r0, #0
    {0x0200488C, 0x1C0CB510}, // push {r4, lr}, adds r4, r1, #0
};

// Mario Kart DS (USA). The game calls OS_WaitIrq(1, 1) itself, and its sound heap is the word at 0x0217D110. It loads a
// piece as 0x02105A68 to 0x02105A84 do: 0x0212DA38(heap, the level in the word at 0x0217C778), 0x0212DA2C(heap), then
// the load 0x0212E9CC(sequence, heap). Its start, 0x0210D7DC(sequence, volume, handle), stops the handle when it plays,
// starts the sequence with the SDK's start and sets the handle's volume. The callers 0x0210D5C0 to 0x0210D72C pass the
// word at 0x0216CB08 (105 at the hook) and the handle 0x0217D05C. 0x02105A40 starts sequences 71 and 72: it stops the
// handle 0x0217D05C with a fade of 2 frames (0x0212C6F0 at 0x02105A90), then calls the start with 127 and the handle
// 0x0217C81C.
//
// The boot queues the title's start before the hook (0x0210D720). The game's start stops it, but the stop leaves a
// trace: a set without the patch below plays the same first sound, and its samples part from those of a set with the
// patch 1.3 to 1.8 s into each piece. The patch leaves the call out, and the boot queues nothing.
constexpr uint32_t kAmceHeap = 0x0217D110;
constexpr GamePatch kAmcePatches[] = {{.address = 0x0210D720, .expected = 0xEB00002D, .value = 0xE1A00000}};
constexpr StubCall kAmceCalls[] = {MakeCall(0x0212DA38, {WordOf(kAmceHeap), WordOf(0x0217C778)}),
                                   MakeCall(0x0212DA2C, {WordOf(kAmceHeap)}),
                                   MakeCall(0x0212E9CC, {Sound(), WordOf(kAmceHeap)}),
                                   MakeCall(0x0210D7DC, {Sound(), WordOf(0x0216CB08), Constant(0x0217D05C)})};
constexpr std::pair<uint32_t, uint32_t> kAmceVoices[] = {{71, 72}};
constexpr StubCall kAmceVoiceCalls[] = {
    MakeCall(0x0212DA38, {WordOf(kAmceHeap), WordOf(0x0217C778)}), MakeCall(0x0212DA2C, {WordOf(kAmceHeap)}),
    MakeCall(0x0212E9CC, {Sound(), WordOf(kAmceHeap)}), MakeCall(0x0212C6F0, {Constant(0x0217D05C), Constant(2)}),
    MakeCall(0x0210D7DC, {Sound(), Constant(127), Constant(0x0217C81C)})};
constexpr NumberCallsEntry kAmceNumbers[] = {{.ranges = kAmceVoices, .calls = kAmceVoiceCalls}};

// On a race's final lap, the sound frame (0x021061B8) starts the course's BGM again through 0x0210D5C0, then sets the
// handle's tempo's ratio to 300 (0x02106820 to 0x02106828, NNS_SndPlayerSetTempoRatio at 0x0212C494). The courses' BGMs
// are SEQ_15 to SEQ_41 and SEQ_74. The volume that 0x0210681C sets between them is the course's, with its offset from
// the table at 0x021074E8. The set keeps the start's 105 there too.
//
// After the tempo, the same site calls 0x0210FAAC. That routine sets flag 0x8 in the word at 0x0217D22C. Each sound
// frame then calls 0x0210FB88(0x0217D224, 0x0217D05C) (0x02106670, through 0x0210FAF4). With the flag set, that routine
// raises every track by a semitone: NNS_SndPlayerSetTrackPitch at 0x0212C524 with the handle, the mask 0xFFFF and 64
// (0x0210FE58 to 0x0210FE90). The stub calls it once, after the tempo. The game sends it a frame after the start.
constexpr StubCall kAmceFinalLap[] = {MakeCall(0x0212C494, {Constant(0x0217D05C), Constant(300)}),
                                      MakeCall(0x0212C524, {Constant(0x0217D05C), Constant(0xFFFF), Constant(64)})};
constexpr GameStateEntry kAmceStates[] = {
    {"SEQ_15", "_FINAL_LAP", kAmceFinalLap}, {"SEQ_16", "_FINAL_LAP", kAmceFinalLap},
    {"SEQ_17", "_FINAL_LAP", kAmceFinalLap}, {"SEQ_18", "_FINAL_LAP", kAmceFinalLap},
    {"SEQ_19", "_FINAL_LAP", kAmceFinalLap}, {"SEQ_20", "_FINAL_LAP", kAmceFinalLap},
    {"SEQ_21", "_FINAL_LAP", kAmceFinalLap}, {"SEQ_22", "_FINAL_LAP", kAmceFinalLap},
    {"SEQ_23", "_FINAL_LAP", kAmceFinalLap}, {"SEQ_24", "_FINAL_LAP", kAmceFinalLap},
    {"SEQ_25", "_FINAL_LAP", kAmceFinalLap}, {"SEQ_26", "_FINAL_LAP", kAmceFinalLap},
    {"SEQ_27", "_FINAL_LAP", kAmceFinalLap}, {"SEQ_28", "_FINAL_LAP", kAmceFinalLap},
    {"SEQ_29", "_FINAL_LAP", kAmceFinalLap}, {"SEQ_30", "_FINAL_LAP", kAmceFinalLap},
    {"SEQ_31", "_FINAL_LAP", kAmceFinalLap}, {"SEQ_32", "_FINAL_LAP", kAmceFinalLap},
    {"SEQ_33", "_FINAL_LAP", kAmceFinalLap}, {"SEQ_34", "_FINAL_LAP", kAmceFinalLap},
    {"SEQ_35", "_FINAL_LAP", kAmceFinalLap}, {"SEQ_36", "_FINAL_LAP", kAmceFinalLap},
    {"SEQ_37", "_FINAL_LAP", kAmceFinalLap}, {"SEQ_38", "_FINAL_LAP", kAmceFinalLap},
    {"SEQ_39", "_FINAL_LAP", kAmceFinalLap}, {"SEQ_40", "_FINAL_LAP", kAmceFinalLap},
    {"SEQ_41", "_FINAL_LAP", kAmceFinalLap}, {"SEQ_74", "_FINAL_LAP", kAmceFinalLap},
};
constexpr GameWord kAmceWords[] = {
    {0x0210D7DC, 0xE92D4070}, // stmfd sp!, {r4, r5, r6, lr}
    {0x0212DA38, 0xE92D4FF0}, // stmfd sp!, {r4-r11, lr}
    {0x0212DA2C, 0xE1D000BC}, // ldrh r0, [r0, #12]
    {0x0212E9CC, 0xE92D4000}, // stmfd sp!, {lr}
    {0x0212C6F0, 0xE59FC004}, // ldr r12, [pc, #4]
    {0x0200C894, 0xE92D4070}, // OS_WaitIrq: stmfd sp!, {r4, r5, r6, lr}
    {0x0210D738, 0x0216CB08}, // the literal of the volume's word at 0x0210D714
    {0x02105AB0, kAmceHeap},  // the literals of 0x02105A40: the heap's word, the level's word and the handles
    {0x02105AB4, 0x0217C778}, {0x02105AB8, 0x0217D05C},
    {0x02105ABC, 0x0217C81C}, {0x02105A8C, 0xE3A01002}, // mov r1, #2: the fade of the stop
    {0x02105A9C, 0xE3A0107F},                           // mov r1, #127
    {0x0212C494, 0xE92D4000},                           // NNS_SndPlayerSetTempoRatio: stmdb sp!, {lr}
    {0x02106824, 0xE3A01F4B},                           // mov r1, #300
    {0x0212C524, 0xE92D4000},                           // NNS_SndPlayerSetTrackPitch: stmdb sp!, {lr}
    {0x0210FE68, 0xE3A06040},                           // mov r6, #64: the final lap's pitch
    {0x0211000C, 0x0000FFFF},                           // the literal of the mask at 0x0210FE84
};

constexpr GameStartEntry kGameStarts[] = {
    {.game_code = "APHE",
     .calls = kApheCalls,
     .numbers = kApheNumbers,
     .park = 0x02000CC8,
     .wait = MakeCall(0x02072F7C, {Constant(0)}),
     .words = kApheWords},
    {.game_code = "IRBO",
     .calls = kIrboCalls,
     .numbers = kIrboNumbers,
     .patches = kIrboPatches,
     .words = kIrboWords,
     .track_states = kIrboTrackStates,
     .track_routines = {.volume = 0x02005F25, .tuning = 0x02005EE1}},
    {.game_code = "IRAO",
     .calls = kIrboCalls,
     .numbers = kIrboNumbers,
     .patches = kIraoPatches,
     .words = kIrboWords,
     .track_states = kIraoTrackStates,
     .track_routines = {.volume = 0x02005F25, .tuning = 0x02005EE1}},
    {.game_code = "IREO",
     .calls = kIreoCalls,
     .numbers = kIreoNumbers,
     .stream_calls = kIreoStreamCalls,
     .patches = kIreoPatches,
     .words = kIreoWords,
     .track_states = kIreoTrackStates,
     .track_routines = {.volume = 0x02005F95, .tuning = 0x02005F51}},
    {.game_code = "IRDO",
     .calls = kIreoCalls,
     .numbers = kIreoNumbers,
     .stream_calls = kIrdoStreamCalls,
     .patches = kIrdoPatches,
     .words = kIrdoWords,
     .track_states = kIrdoTrackStates,
     .track_routines = {.volume = 0x02005F95, .tuning = 0x02005F51}},
    {.game_code = "IPKE", .calls = kIpkeCalls, .numbers = kIpkeNumbers, .words = kIpkeWords, .states = kIpkeStates},
    // Pokémon SoulSilver (USA): HeartGold's code at HeartGold's addresses, with every word of kIpkeWords, and
    // HeartGold's labels in its sound archive.
    {.game_code = "IPGE", .calls = kIpkeCalls, .numbers = kIpkeNumbers, .words = kIpkeWords, .states = kIpkeStates},
    {.game_code = "CPUE", .calls = kCpueCalls, .numbers = kCpueNumbers, .words = kCpueWords, .states = kCpueStates},
    {.game_code = "ADAE", .calls = kAdaeCalls, .numbers = kAdaeNumbers, .words = kAdaeWords, .states = kAdaeStates},
    {.game_code = "APAE", .calls = kAdaeCalls, .numbers = kAdaeNumbers, .words = kAdaeWords, .states = kAdaeStates},
    {.game_code = "BKIE",
     .calls = zelda::SpiritTracks<zelda::kBkie>::kCalls,
     .numbers = zelda::SpiritTracks<zelda::kBkie>::kNumbers,
     .effect_calls = zelda::SpiritTracks<zelda::kBkie>::kEffectCalls,
     .words = zelda::SpiritTracks<zelda::kBkie>::kWords,
     .states = zelda::SpiritTracks<zelda::kBkie>::kStates,
     .merge_same_starts = true},
    {.game_code = "BKIE",
     .calls = zelda::SpiritTracks<zelda::kBkieRev1>::kCalls,
     .numbers = zelda::SpiritTracks<zelda::kBkieRev1>::kNumbers,
     .effect_calls = zelda::SpiritTracks<zelda::kBkieRev1>::kEffectCalls,
     .words = zelda::SpiritTracks<zelda::kBkieRev1>::kWords,
     .states = zelda::SpiritTracks<zelda::kBkieRev1>::kStates,
     .merge_same_starts = true},
    {.game_code = "BKIP",
     .calls = zelda::SpiritTracks<zelda::kBkip>::kCalls,
     .numbers = zelda::SpiritTracks<zelda::kBkip>::kNumbers,
     .effect_calls = zelda::SpiritTracks<zelda::kBkip>::kEffectCalls,
     .words = zelda::SpiritTracks<zelda::kBkip>::kWords,
     .states = zelda::SpiritTracks<zelda::kBkip>::kStates,
     .merge_same_starts = true},
    {.game_code = "BKIP",
     .calls = zelda::SpiritTracks<zelda::kBkipRev1>::kCalls,
     .numbers = zelda::SpiritTracks<zelda::kBkipRev1>::kNumbers,
     .effect_calls = zelda::SpiritTracks<zelda::kBkipRev1>::kEffectCalls,
     .words = zelda::SpiritTracks<zelda::kBkipRev1>::kWords,
     .states = zelda::SpiritTracks<zelda::kBkipRev1>::kStates,
     .merge_same_starts = true},
    {.game_code = "AZEE",
     .calls = zelda::kPhCalls,
     .numbers = zelda::kPhNumbers,
     .effect_calls = zelda::kPhEffectCalls,
     .words = zelda::kPhWords,
     .states = zelda::kPhStates,
     .merge_same_starts = true},
    {.game_code = "AMCE",
     .calls = kAmceCalls,
     .numbers = kAmceNumbers,
     .patches = kAmcePatches,
     .heap_word = kAmceHeap,
     .words = kAmceWords,
     .states = kAmceStates},
};

// A game that plays a sequence only after it starts its sound again with another setting (SoundRestart). The entry also
// contains the first two instruction words of the start-up of its sound. FindSdatCode checks them in the ARM9 binary.
struct SoundRestartEntry
{
    std::string_view game_code;
    SoundRestart restart;
    std::array<uint32_t, 2> routine_words = {};
};

// Fire Emblem: Shin Monshou no Nazo (Japan, revision 1). The start-up of the sound, 0x02015DA0, first makes the sound
// heaps (0x02015CB0) from a static buffer at 0x0211BC80. With 0 it makes two heaps side by side, of 512000 and 143360
// bytes. With any other value it makes one heap of 0xA0000 bytes. The start-up has seven callers: the main code at
// 0x02015B74, overlay 8 at 0x0221310C, overlay 16 at 0x02249598, 0x02249ECC and 0x02249F90, and overlay 18 at
// 0x0222ECC0 and 0x022314D8. The boot's call at 0x02015B74 passes r4. The code stores r4 in both heaps' fields just
// before the call. Nobody has traced its value. A set's boot makes the two heaps. Only overlay 18's call at 0x0222ECC0
// passes a constant, 1 (mov r0, #1 at 0x0222ECBC). Overlay 18 first stops the game's three sound objects (0x0222EC78 to
// 0x0222ECB8). After the call it starts BGM_SYS_ENDING4, sequence 86, on the object for music (0x0222ECC4 to
// 0x0222ECE0). The other five callers pass r4 or r5. Nobody has traced those values. Called on the main thread from the
// hook with r0 set to 1, the start-up makes one heap of 655296 bytes with 591644 free. The game's load of sequence 86
// takes 535228 bytes of it, and its start succeeds. In the boot's first heap, with 448284 bytes free, the load fails.
// The start-up's first two words are stmfd sp!, {r3, r4, r5, lr} and bl 0x02015CB0.
constexpr SoundRestartEntry kSoundRestarts[] = {{.game_code = "VI2J",
                                                 .restart = {.sequence = 86, .routine = 0x02015DA0, .argument = 1},
                                                 .routine_words = {0xE92D4038, 0xEBFFFFC1}}};

// The stub calls the start-up again only for a game outside the table of games: no game may be in both tables.
static_assert(std::ranges::none_of(kSoundRestarts,
                                   [](const SoundRestartEntry& restart)
                                   {
                                       return std::ranges::find(kGameStarts, restart.game_code,
                                                                &GameStartEntry::game_code) !=
                                              std::ranges::end(kGameStarts);
                                   }),
              "a game is in both the table of games and the table of restarts");

// Games whose short fanfares are music although ntdlib::ntr::rip::IsMusic counts them as effects: the sequences whose
// names start with the prefix. Fire Emblem: Shadow Dragon (USA) and Shin Monshou no Nazo (Japan) name six jingles FF_:
// the fanfares of an item gained or lost, a level gained and the end of a wireless match.
struct MusicPrefix
{
    std::string_view game_code;
    std::string_view prefix;
};

constexpr MusicPrefix kMusicPrefixes[] = {{"VI2J", "FF_"}, {"YFEE", "FF_"}};

// Whether `code`, the ARM9 binary and then each ARM9 overlay in the order of its table, all decompressed, contains
// every word that `entry` expects, each in the binary or the overlay that the word names, and the binary contains each
// patch's expected word or halfword.
bool MatchesGame(std::span<const ntr::LoadedCode> code, const GameStartEntry& entry)
{
    const ntr::LoadedCode& arm9 = code.front();
    const auto word_matches = [code](const GameWord& word)
    {
        const auto at = static_cast<std::size_t>(word.overlay + 1);
        return at < code.size() && WordAt(code[at], word.address) == word.value;
    };
    const auto patch_matches = [&arm9](const GamePatch& patch)
    {
        return PatchMatches(arm9, patch);
    };

    return std::ranges::all_of(entry.words, word_matches) && std::ranges::all_of(entry.patches, patch_matches);
}

// The calls that set `state` through the game's routines `routines`: the tracks' volumes, then the tempo's ratio with a
// pan of 0 on every track, then the pitch, in the order of Black's units.
std::vector<StubCall> TrackStateCalls(const TrackStateEntry& state, const TrackRoutines& routines)
{
    constexpr uint32_t kLeave = 0xFFFFFFFF; // -1: the tuning routine leaves the value alone
    constexpr uint32_t kEveryTrack = 0xFFFF;
    std::vector<StubCall> calls;
    if (state.on != 0)
    {
        calls.push_back(MakeCall(routines.volume, {Constant(state.on), Constant(127)}));
    }
    if (state.off != 0)
    {
        calls.push_back(MakeCall(routines.volume, {Constant(state.off), Constant(0)}));
    }
    if (state.tempo_ratio != 256)
    {
        calls.push_back(MakeCall(routines.tuning,
                                 {Constant(kEveryTrack), Constant(state.tempo_ratio), Constant(kLeave), Constant(0)}));
    }
    if (state.pitch_tracks != 0)
    {
        const auto pitch = static_cast<uint32_t>(int32_t{state.pitch});
        calls.push_back(MakeCall(routines.tuning,
                                 {Constant(state.pitch_tracks), Constant(kLeave), Constant(pitch), Constant(kLeave)}));
    }

    return calls;
}

// The game's starts (GameStart) of a table entry.
GameStart GameStartOf(const GameStartEntry& entry)
{
    GameStart start{.calls = {entry.calls.begin(), entry.calls.end()},
                    .stream_calls = {entry.stream_calls.begin(), entry.stream_calls.end()},
                    .effect_calls = {entry.effect_calls.begin(), entry.effect_calls.end()},
                    .merge_same_starts = entry.merge_same_starts,
                    .park = entry.park};
    for (const NumberCallsEntry& numbers : entry.numbers)
    {
        start.numbers.push_back({.ranges = {numbers.ranges.begin(), numbers.ranges.end()},
                                 .player = numbers.player,
                                 .calls = {numbers.calls.begin(), numbers.calls.end()}});
    }

    for (const GameStateEntry& state : entry.states)
    {
        start.states.push_back({.label = std::string(state.label),
                                .suffix = std::string(state.suffix),
                                .calls = {state.calls.begin(), state.calls.end()},
                                .frame_calls = {state.frame_calls.begin(), state.frame_calls.end()},
                                .timed_calls = {state.timed_calls.begin(), state.timed_calls.end()},
                                .video_frames = state.video_frames});
    }

    for (const TrackStateEntry& state : entry.track_states)
    {
        start.states.push_back({.label = std::string(state.label),
                                .suffix = std::string(state.suffix),
                                .calls = TrackStateCalls(state, entry.track_routines)});
    }

    return start;
}

// The Thumb pair of blx <target> at `address`: the target is ARM code, a multiple of 4.
uint32_t ThumbBlx(uint32_t address, uint32_t target)
{
    const uint32_t offset = target - ((address + 4) & ~3u);
    const uint32_t high = 0xF000 | ((offset >> 12) & 0x7FF);
    const uint32_t low = 0xE800 | ((offset >> 1) & 0x7FF);

    return high | (low << 16);
}

uint32_t Word(std::span<const uint8_t> memory, uint32_t address)
{
    return xsflib::LoadLe32(&memory[(address - ntr::kMainMemory) & static_cast<uint32_t>(memory.size() - 1)]);
}

// Whether a word of main memory from `from` up to `to`, both in `memory`, starts an information block that ends by
// `to`: the signature, a size of at least the block's header, and the offsets of its tables after the header and inside
// the block.
bool InfoBlockIn(std::span<const uint8_t> memory, uint32_t from, uint32_t to)
{
    for (uint32_t at = (from + 3) & ~3u; at + kInfoHeaderBytes <= to; at += 4)
    {
        const uint8_t* block = &memory[at - ntr::kMainMemory];
        const uint32_t size = xsflib::LoadLe32(block + kInfoSizeField);
        if (xsflib::LoadLe32(block) != kInfoBlock || size < kInfoHeaderBytes || size > to - at)
        {
            continue;
        }

        const auto inside = [block, size](uint32_t table)
        {
            const uint32_t offset = xsflib::LoadLe32(block + kInfoTablesField + 4 * table);
            return offset >= kInfoHeaderBytes && offset < size;
        };
        if (std::ranges::all_of(std::views::iota(0u, kInfoTables), inside))
        {
            return true;
        }
    }

    return false;
}

// Whether main memory holds `file` at `address`. A bank's links are left out.
bool Holds(std::span<const uint8_t> memory, uint32_t address, std::span<const uint8_t> file, bool bank)
{
    if (!ntr::InMainMemory(memory, address, file.size()))
    {
        return false;
    }

    const std::span<const uint8_t> held = memory.subspan(address - ntr::kMainMemory, file.size());
    for (std::size_t i = 0; i < file.size(); i++)
    {
        if (held[i] != file[i] && !(bank && i >= kBankLinksStart && i < kBankLinksEnd))
        {
            return false;
        }
    }

    return true;
}

// Whether the sequences `a` and `b` of `archive` have the same record. Every field counts except the name. False when
// either has no record.
bool SameSequenceRecord(const sdat::Archive& archive, uint32_t a, uint32_t b)
{
    if (a >= archive.sequences.size() || b >= archive.sequences.size() || !archive.sequences[a] ||
        !archive.sequences[b])
    {
        return false;
    }

    const sdat::SequenceInfo& x = *archive.sequences[a];
    const sdat::SequenceInfo& y = *archive.sequences[b];

    return x.file_id == y.file_id && x.bank == y.bank && x.volume == y.volume &&
           x.channel_priority == y.channel_priority && x.player_priority == y.player_priority && x.player == y.player &&
           x.reserved == y.reserved;
}

// Whether the streams `a` and `b` of `archive` have the same file, volume, player priority, player and flags: the
// fields that the SDK's start of a stream reads. False when either has no record.
bool SameStreamRecord(const sdat::Archive& archive, uint32_t a, uint32_t b)
{
    if (a >= archive.streams.size() || b >= archive.streams.size() || !archive.streams[a] || !archive.streams[b])
    {
        return false;
    }

    const sdat::StreamInfo& x = *archive.streams[a];
    const sdat::StreamInfo& y = *archive.streams[b];

    return x.file_id == y.file_id && x.volume == y.volume && x.player_priority == y.player_priority &&
           x.player == y.player && x.flags == y.flags && x.reserved == y.reserved;
}

} // namespace

bool Relocatable(uint32_t word)
{
    constexpr uint32_t kPushMask = 0xFFFF0000;
    constexpr uint32_t kPush = 0xE92D0000; // stmfd sp!, {...}
    constexpr uint32_t kPc = 15;
    if ((word & kPushMask) == kPush && (word & (1u << kPc)) == 0)
    {
        return true;
    }

    constexpr uint32_t kPushOneMask = 0xFFFF0FFF;
    constexpr uint32_t kPushOne = 0xE52D0004; // str rd, [sp, #-4]!
    if ((word & kPushOneMask) == kPushOne && ((word >> 12) & 0xF) != kPc)
    {
        return true;
    }

    // Data processing, always: bits 26 and 27 clear, not a multiply, a swap or a halfword's transfer.
    constexpr uint32_t kAlways = 0xE0000000;
    const bool always = (word & 0xF0000000) == kAlways;
    const bool data = (word & 0x0C000000) == 0;
    const bool immediate = (word & 0x02000000) != 0;
    const bool extra = !immediate && (word & 0x90) == 0x90;
    const bool bx = (word & 0x0FFFFFF0) == 0x012FFF10;
    const uint32_t rn = (word >> 16) & 0xF;
    const uint32_t rd = (word >> 12) & 0xF;
    const uint32_t rm = word & 0xF;
    const uint32_t rs = (word >> 8) & 0xF;
    const bool shift_by_register = !immediate && (word & 0x10) != 0;
    return always && data && !extra && !bx && rn != kPc && rd != kPc &&
           (immediate || (rm != kPc && (!shift_by_register || rs != kPc)));
}

std::optional<std::array<uint32_t, 2>> RelocatableThumb(uint32_t halves)
{
    const auto arm = [](uint32_t half) -> std::optional<uint32_t>
    {
        if ((half & 0xFF00) == 0xB500)
        {
            return 0xE92D4000 | (half & 0xFF); // push {list, lr}: stmfd sp!, {list, lr}
        }
        if ((half & 0xF800) == 0x2000)
        {
            return 0xE3B00000 | ((half >> 8 & 7) << 12) | (half & 0xFF); // movs rd, #imm
        }
        if ((half & 0xFFC0) == 0x1C00)
        {
            return 0xE2900000 | ((half >> 3 & 7) << 16) | ((half & 7) << 12); // adds rd, rm, #0
        }
        return std::nullopt;
    };

    const std::optional<uint32_t> first = arm(halves & 0xFFFF);
    const std::optional<uint32_t> second = arm(halves >> 16);
    if (!first || !second)
    {
        return std::nullopt;
    }

    return std::array<uint32_t, 2>{*first, *second};
}

uint32_t HookWord(const SdatCode& code)
{
    const uint32_t update = code.update & ~1u;

    return (code.update & 1) != 0 ? ThumbBlx(update, code.stub) : Branch(update, code.stub);
}

std::vector<uint8_t> DsImage(const ntr::Rom& rom)
{
    // The header's unit code, and its CRC-16 over the bytes before it.
    constexpr std::size_t kUnitCodeField = 0x12;
    constexpr std::size_t kHeaderCrcField = 0x15E;

    std::vector<uint8_t> image(rom.Image().begin(), rom.Image().end());
    if (static_cast<ntr::UnitCode>(rom.Header().unit_code) == ntr::UnitCode::kDsiEnhanced)
    {
        image[kUnitCodeField] = static_cast<uint8_t>(ntr::UnitCode::kDs);
        xsflib::StoreLe16(&image[kHeaderCrcField], ntr::Crc16(std::span(image).first(kHeaderCrcField)));
    }

    return image;
}

bool MatchesGameStart(const ntr::LoadedCode& arm9, std::string_view game_code)
{
    const std::span<const ntr::LoadedCode> code(&arm9, 1);
    const auto matches = [code, game_code](const GameStartEntry& entry)
    {
        return entry.game_code == game_code && MatchesGame(code, entry);
    };

    return std::ranges::any_of(kGameStarts, matches);
}

RestartLookup LookUpRestart(const ntr::LoadedCode& arm9, std::string_view game_code)
{
    const auto entry = std::ranges::find(kSoundRestarts, game_code, &SoundRestartEntry::game_code);
    if (entry == std::ranges::end(kSoundRestarts))
    {
        return {};
    }

    const uint32_t routine = entry->restart.routine;
    const bool matches =
        WordAt(arm9, routine) == entry->routine_words[0] && WordAt(arm9, routine + 4) == entry->routine_words[1];

    return matches ? RestartLookup{.restart = entry->restart} : RestartLookup{.left_out = entry->restart.sequence};
}

namespace
{

// Whether the game's stream manager starts the streams of a set: the table of games gives its calls.
bool ManagerStartsStreams(const SdatCode& code)
{
    return code.game_start && !code.game_start->stream_calls.empty();
}

// Sets in `code` how a set of the game starts a stream: with the calls of the game's stream manager where the table of
// games gives them, or else with the SDK's start, when the search of `arm9` finds exactly one. The stream's stub must
// fit in the decompressor's body with a stream's number that takes a literal. Where the set can start no stream,
// `code.no_stream_start` says why.
void SetStreamStart(const ntr::LoadedCode& arm9, SdatCode& code)
{
    if (!ManagerStartsStreams(code))
    {
        const std::vector<uint32_t> stream_starts = FindStreamStarts(arm9);
        if (stream_starts.size() != 1)
        {
            code.no_stream_start = stream_starts.empty()
                                       ? "the search of the game's code doesn't find the start of a stream "
                                         "(NNS_SndArcStrmStart)"
                                       : "the search of the game's code finds " + std::to_string(stream_starts.size()) +
                                             " starts of a stream (NNS_SndArcStrmStart)";
            return;
        }

        code.stream_start = stream_starts[0];
    }

    if (BuildSdatStub(code, 0, kLiteralSound, SoundKind::kStream).bytes.size() > code.stub_end - code.stub)
    {
        code.stream_start.reset();
        if (code.game_start)
        {
            code.game_start->stream_calls.clear();
        }
        code.no_stream_start = "the stub that starts a stream doesn't fit in the decompressor's body";
    }
}

uint32_t LargestStartBlock(const SdatCode& code, bool with_states);

// FindSdatCode with `entry`, the table entry of the game, or null for a game outside the tables. `code` is the ARM9
// binary and its overlays, decompressed (ntr::Arm9CodeAndOverlays), or empty to decompress them here for an entry.
std::optional<SdatCode> FindCode(const ntr::Rom& rom, const GameStartEntry* entry,
                                 std::span<const ntr::LoadedCode> code_and_overlays, std::string& error)
{
    const ntr::LoadedCode arm9 = ntr::Arm9Code(rom);
    const ntr::SoundRoutines routines = ntr::FindSoundRoutines(arm9, rom.Header().arm9.entry);
    const bool listed = entry != nullptr;
    const auto count = [](std::size_t n, const char* what) -> std::optional<std::string>
    {
        if (n == 1)
        {
            return std::nullopt;
        }

        return n == 0 ? std::string("the search of the game's code doesn't find ") + what
                      : "the search of the game's code finds " + std::to_string(n) + " candidates for " + what;
    };

    // A listed game takes its starts from the table, and its wait where the table gives one. The search of the SDK's
    // start and load doesn't count then.
    const std::size_t starts = listed ? 1 : routines.starts.size();
    const std::size_t loads = listed ? 1 : routines.loads.size();
    const std::size_t waits = listed && entry->wait ? 1 : routines.waits.size();
    for (const std::optional<std::string>& problem :
         {count(routines.updates.size(), "the sound library's update (NNS_SndMain)"),
          count(starts, "the start of a sequence (NNS_SndArcPlayerStartSeq)"),
          count(loads, "the load of a sequence (NNS_SndArcLoadSeq)"),
          count(waits, "the wait for a frame (OS_WaitVBlankIntr or OS_WaitIrq)")})
    {
        if (problem)
        {
            error = *problem;
            return std::nullopt;
        }
    }

    if (!routines.decompressor)
    {
        error = "the search of the game's code finds no start-up decompressor, where the stub would go";
        return std::nullopt;
    }

    SdatCode code;
    code.update = routines.updates[0].address;
    if (listed)
    {
        code.game_start = GameStartOf(*entry);
        code.patches.assign(entry->patches.begin(), entry->patches.end());
        code.heap_word = entry->heap_word;
    }
    else
    {
        code.start = routines.starts[0].address;
        code.start_ex = routines.starts[0].ex;
        code.loader = routines.loads[0].wrapper;
        code.load = routines.loads[0].load;
    }
    if (listed && entry->wait)
    {
        code.wait = entry->wait->routine;
        for (std::size_t i = 0; i < entry->wait->count; i++)
        {
            code.wait_arguments.push_back(entry->wait->arguments[i].value);
        }
    }
    else
    {
        code.wait = routines.waits[0].address;
        code.wait_arguments = routines.waits[0].arguments;
    }
    code.stub = routines.decompressor->body;
    code.stub_end = routines.decompressor->exit;

    // The update's first instruction, or its first two in Thumb code, moves into the stub. It must be in the binary's
    // static part.
    const std::optional<ntr::ModuleParams> params = ntr::ReadModuleParams(arm9.bytes);
    const uint32_t static_end = params ? params->autoload_start : arm9.address + uint32_t(arm9.bytes.size());
    const uint32_t update = code.update & ~1u;
    if (update < arm9.address || update + 4 > static_end || code.stub_end > static_end)
    {
        error = "the update or the decompressor lies outside the ARM9 binary's static part";
        return std::nullopt;
    }

    const std::vector<ntr::LoadedCode> loaded =
        listed && code_and_overlays.empty() ? ntr::Arm9CodeAndOverlays(rom) : std::vector<ntr::LoadedCode>();
    if (listed && !MatchesGame(code_and_overlays.empty() ? loaded : code_and_overlays, *entry))
    {
        error = "the game's code doesn't contain the instructions that the table of games expects";
        return std::nullopt;
    }

    const RestartLookup restart = LookUpRestart(arm9, rom.Header().game_code);
    code.restart = restart.restart;
    code.left_out = restart.left_out;

    code.update_first = xsflib::LoadLe32(&arm9.bytes[update - arm9.address]);
    const bool thumb = (code.update & 1) != 0;
    if (thumb ? !RelocatableThumb(code.update_first) : !Relocatable(code.update_first))
    {
        error =
            "the update's first instruction (" + xsflib::Hex(code.update_first, 8, true) + ") can't move into the stub";
        return std::nullopt;
    }

    // A game in the table of games is checked with its lists alone here: the states and the effects of a set depend on
    // its sound archive (StartBlockBytes), and the rip checks them once it has read it.
    SdatCode lists_alone = code;
    if (code.game_start)
    {
        lists_alone.start_block = LargestStartBlock(code, false);
    }

    if (std::optional<std::string> problem = StubSizeProblem(lists_alone))
    {
        error = std::move(*problem);
        return std::nullopt;
    }

    SetStreamStart(arm9, code);

    return code;
}

} // namespace

std::optional<SdatCode> FindSdatCode(const ntr::Rom& rom, std::string& error)
{
    const std::string_view game_code = rom.Header().game_code;
    const auto entry = std::ranges::find(kGameStarts, game_code, &GameStartEntry::game_code);
    if (entry == std::ranges::end(kGameStarts))
    {
        return FindCode(rom, nullptr, {}, error);
    }

    // A game with an entry for each revision takes the entry whose words match, or else the first, whose check then
    // refuses the game.
    const std::vector<ntr::LoadedCode> code = ntr::Arm9CodeAndOverlays(rom);
    const auto matches = [&code, game_code](const GameStartEntry& revision)
    {
        return revision.game_code == game_code && MatchesGame(code, revision);
    };
    const auto revision = std::ranges::find_if(kGameStarts, matches);

    return FindCode(rom, revision != std::ranges::end(kGameStarts) ? &*revision : &*entry, code, error);
}

std::optional<SdatCode> FindTableCode(const ntr::Rom& rom, const GameStartEntry& entry, std::string& error)
{
    return FindCode(rom, &entry, {}, error);
}

std::optional<std::string> StubSizeProblem(const SdatCode& code)
{
    const std::size_t stub_bytes = BuildSdatStub(code, 0, 0).bytes.size();
    if (stub_bytes <= code.stub_end - code.stub)
    {
        return std::nullopt;
    }

    return "the stub takes " + std::to_string(stub_bytes) + " bytes, and the decompressor's body has " +
           std::to_string(code.stub_end - code.stub);
}

bool StartsStreams(const SdatCode& code)
{
    return code.stream_start.has_value() || ManagerStartsStreams(code);
}

std::optional<SoundArchive> FindSoundArchive(const ntr::Rom& rom, const std::set<uint32_t>& files)
{
    for (const uint32_t id : files)
    {
        if (id >= rom.Files().size())
        {
            continue;
        }

        const std::span<const uint8_t> data = rom.FileData(id);
        if (data.size() < 4 || std::string_view(reinterpret_cast<const char*>(data.data()), 4) != "SDAT")
        {
            continue;
        }

        try
        {
            sdat::Archive archive = sdat::Archive::Parse(data);
            if (std::ranges::any_of(archive.sequences, [](const auto& sequence) { return sequence.has_value(); }))
            {
                return SoundArchive{id, std::move(archive)};
            }
        }
        catch (const std::exception&)
        {
            continue;
        }
    }

    return std::nullopt;
}

namespace
{

// A frame heap's address and its start, head, tail and end.
struct FrameHeap
{
    uint32_t heap = 0;
    uint32_t start = 0;
    uint32_t head = 0;
    uint32_t tail = 0;
    uint32_t end = 0;
};

// The frame heap whose signature is at `at` in `memory`, where the heap's first word holds its address and its start,
// head, tail and end are in main memory in that order. Nothing otherwise.
std::optional<FrameHeap> FrameHeapAt(std::span<const uint8_t> memory, uint32_t at)
{
    const uint64_t memory_end = ntr::kMainMemory + uint64_t{memory.size()};
    if (at < kHeapToSignature || at + kHeapTailField + 4 > memory.size() ||
        xsflib::LoadLe32(&memory[at]) != kFrameHeap ||
        xsflib::LoadLe32(&memory[at - kHeapToSignature]) != ntr::kMainMemory + at)
    {
        return std::nullopt;
    }

    const FrameHeap heap{.heap = ntr::kMainMemory + at - kHeapToSignature,
                         .start = xsflib::LoadLe32(&memory[at + kHeapStartField]),
                         .head = xsflib::LoadLe32(&memory[at + kHeapHeadField]),
                         .tail = xsflib::LoadLe32(&memory[at + kHeapTailField]),
                         .end = xsflib::LoadLe32(&memory[at + kHeapEndField])};
    if (heap.start >= ntr::kMainMemory && heap.start <= heap.head && heap.head <= heap.tail && heap.tail <= heap.end &&
        heap.end <= memory_end)
    {
        return heap;
    }

    return std::nullopt;
}

} // namespace

std::optional<SoundHeap> SoundHeapAt(std::span<const uint8_t> memory, uint32_t word)
{
    if (!ntr::InMainMemory(memory, word))
    {
        return std::nullopt;
    }

    const uint32_t address = Word(memory, word);
    const std::optional<FrameHeap> heap = ntr::InMainMemory(memory, address)
                                              ? FrameHeapAt(memory, address - ntr::kMainMemory + kHeapToSignature)
                                              : std::nullopt;
    if (!heap)
    {
        return std::nullopt;
    }

    return SoundHeap{.address = heap->heap, .free = heap->tail - heap->head};
}

std::optional<SoundHeap> FindSoundHeap(std::span<const uint8_t> memory)
{
    std::vector<FrameHeap> heaps;
    for (uint32_t at = kHeapToSignature; at + kHeapTailField + 4 <= memory.size(); at += 4)
    {
        if (const std::optional<FrameHeap> heap = FrameHeapAt(memory, at))
        {
            heaps.push_back(*heap);
        }
    }

    for (const FrameHeap& heap : heaps)
    {
        const auto inside = [&heap](const FrameHeap& other)
        {
            return other.heap == heap.heap || (other.heap >= heap.start && other.heap < heap.end);
        };
        if (std::ranges::all_of(heaps, inside))
        {
            return SoundHeap{.address = heap.heap, .free = heap.tail - heap.head};
        }
    }

    // No heap contains the others: the heap whose used part contains the archive's information block.
    const auto holds_info = [&memory](const FrameHeap& heap)
    {
        return InfoBlockIn(memory, heap.start, heap.head);
    };
    std::vector<FrameHeap> holding;
    std::ranges::copy_if(heaps, std::back_inserter(holding), holds_info);
    if (holding.size() == 1)
    {
        return SoundHeap{.address = holding.front().heap, .free = holding.front().tail - holding.front().head};
    }

    return std::nullopt;
}

std::set<uint32_t> MusicSequences(const sdat::Archive& archive, std::string_view game_code)
{
    const auto prefix = std::ranges::find(kMusicPrefixes, game_code, &MusicPrefix::game_code);
    const auto counts_as_music = [&](uint32_t index)
    {
        if (ntr::rip::IsMusic(archive, ntr::SequenceRef{.index = index}, game_code))
        {
            return true;
        }

        const std::vector<std::optional<std::string>>& names = archive.symbols.names[0];
        return prefix != std::ranges::end(kMusicPrefixes) && index < names.size() && names[index] &&
               names[index]->starts_with(prefix->prefix);
    };

    std::set<uint32_t> all;
    std::set<uint32_t> music;
    for (uint32_t index = 0; index < archive.sequences.size(); index++)
    {
        if (archive.sequences[index])
        {
            all.insert(index);
            if (archive.has_symbols && counts_as_music(index))
            {
                music.insert(index);
            }
        }
    }

    return music.empty() ? all : music;
}

std::vector<uint32_t> FirstOfRecord(const SdatCode& code, const sdat::Archive& archive,
                                    std::span<const uint32_t> numbers, SoundKind kind)
{
    const bool game_starts = kind == SoundKind::kStream ? ManagerStartsStreams(code) : code.game_start.has_value();
    if (game_starts)
    {
        return {numbers.begin(), numbers.end()};
    }

    const auto same = kind == SoundKind::kStream ? SameStreamRecord : SameSequenceRecord;
    std::vector<uint32_t> firsts;
    firsts.reserve(numbers.size());
    for (std::size_t i = 0; i < numbers.size(); i++)
    {
        const auto same_record = [&](uint32_t other)
        {
            return same(archive, other, numbers[i]);
        };
        const std::span<const uint32_t> earlier = numbers.first(i);
        const auto first = std::ranges::find_if(earlier, same_record);
        firsts.push_back(first == earlier.end() ? numbers[i] : *first);
    }

    return firsts;
}

HookState BootToHook(const ntr::Rom& rom, uint32_t update, const ntr::BackupMemory& backup,
                     std::optional<uint32_t> heap_word)
{
    HookState hook;
    const auto machine = std::make_unique<ntr::Machine>(rom, backup);
    machine->on_card_read = [&](uint32_t address, uint32_t bytes)
    {
        hook.reads.emplace_back(address, address + bytes);
        for (const uint32_t id : rom.FilesIn(address, address + bytes))
        {
            hook.files.insert(id);
        }
    };
    machine->Arm9().SetStopAddress(update & ~1u);
    std::vector<ntr::SoundOutput> out;
    hook.reached = machine->Run(kBootClock, out);
    hook.clock = machine->Clock();
    hook.seconds = static_cast<double>(machine->Clock()) / ntr::kSystemClockHz;
    hook.cpsr = machine->Arm9().Cpsr();
    hook.heap = heap_word ? SoundHeapAt(machine->MainMemory(), *heap_word) : FindSoundHeap(machine->MainMemory());

    return hook;
}

std::optional<std::size_t> UnlikeHookBoot(const std::vector<HookState>& full, const std::vector<HookState>& library)
{
    if (const std::optional<std::size_t> different = DifferentBoot(full, library))
    {
        return different;
    }

    for (std::size_t i = 0; i < full.size(); i++)
    {
        if (library[i].reads.size() != full[i].reads.size() || library[i].heap != full[i].heap)
        {
            return i;
        }
    }

    return std::nullopt;
}

HookState MergedHook(std::span<const HookState> boots)
{
    if (boots.empty())
    {
        return {};
    }

    HookState merged = boots.front();
    for (const HookState& boot : boots.subspan(1))
    {
        merged.files.insert(boot.files.begin(), boot.files.end());
        merged.reads.insert(merged.reads.end(), boot.reads.begin(), boot.reads.end());
    }

    return merged;
}

std::optional<std::string> HookProblem(const HookState& hook)
{
    if (!hook.reached)
    {
        return "the game doesn't call the sound library's update within 30 s of its boot";
    }

    const uint32_t mode = hook.cpsr & kModeBits;
    if (mode != kSystemMode && mode != kUserMode)
    {
        return "the game calls the sound library's update in an interrupt's mode (CPSR " +
               xsflib::Hex(hook.cpsr, 8, true) + ")";
    }

    if ((hook.cpsr & kIrqDisabled) != 0)
    {
        return "the game calls the sound library's update with interrupts off";
    }

    if (!hook.heap)
    {
        return "no sound heap is in memory at the first call of the sound library's update";
    }

    return std::nullopt;
}

std::set<uint32_t> ArchiveFilesRead(const HookState& hook, uint32_t start, const sdat::Archive& archive)
{
    std::set<uint32_t> read;
    for (uint32_t id = 0; id < archive.files.size(); id++)
    {
        const uint64_t file_start = uint64_t{start} + archive.files[id].offset;
        const uint64_t file_end = file_start + archive.files[id].size;
        const auto overlaps = [&](const std::pair<uint32_t, uint32_t>& range)
        {
            return range.first < file_end && file_start < range.second;
        };
        if (archive.files[id].size > 0 && std::ranges::any_of(hook.reads, overlaps))
        {
            read.insert(id);
        }
    }

    return read;
}

BootFiles FilesRead(const ntr::Rom& rom, const SoundArchive& found, std::span<const HookState> boots)
{
    const HookState merged = MergedHook(boots);

    return {merged.files, ArchiveFilesRead(merged, rom.Files()[found.file_id].start, found.archive)};
}

BootFiles LibraryRead(const BootFiles& game, const BootFiles& full)
{
    BootFiles read = game;
    read.files.insert(full.files.begin(), full.files.end());
    read.archive_files.insert(full.archive_files.begin(), full.archive_files.end());

    return read;
}

std::optional<std::string> HookProblem(const HookState& hook, const std::optional<SoundHeap>& heap)
{
    if (std::optional<std::string> problem = HookProblem(hook))
    {
        return problem;
    }

    if (!heap || hook.heap->address != heap->address)
    {
        return "the sound heap is elsewhere at the hook";
    }

    return std::nullopt;
}

std::vector<uint32_t> PageOffsets(const sdat::Archive& archive, const std::set<uint32_t>& kept)
{
    std::size_t first_file = archive.bytes.size();
    for (const sdat::FileEntry& file : archive.files)
    {
        first_file = std::min<std::size_t>(first_file, file.offset);
    }

    std::set<uint32_t> pages;
    for (uint32_t page = 0; page * kCardPage < first_file; page++)
    {
        pages.insert(page);
    }

    for (const uint32_t id : kept)
    {
        const sdat::FileEntry& file = archive.files.at(id);
        for (uint64_t page = file.offset / kCardPage;
             file.size > 0 && page * kCardPage < uint64_t{file.offset} + file.size; page++)
        {
            pages.insert(static_cast<uint32_t>(page));
        }
    }

    std::vector<uint32_t> offsets;
    for (const sdat::FileEntry& file : archive.files)
    {
        const auto in_front = std::distance(pages.begin(), pages.lower_bound(file.offset / kCardPage));
        offsets.push_back(static_cast<uint32_t>(in_front) * kCardPage + file.offset % kCardPage);
    }

    return offsets;
}

std::optional<std::vector<uint8_t>> CutArchive(const sdat::Archive& archive,
                                               std::span<const ntr::SequenceRef> sequences,
                                               const std::set<uint32_t>& extra, const std::set<uint32_t>& reserved)
{
    const std::optional<std::vector<uint8_t>> cut = ntr::rip::KeepSequenceData(archive, sequences);
    if (!cut)
    {
        return std::nullopt;
    }

    // The files that the cut keeps and the extra ones keep their bytes, and any other reserved file keeps its size.
    const sdat::Archive kept = sdat::Archive::Parse(*cut);
    sdat::ArchiveModel model = sdat::ReadArchiveModel(archive.bytes);
    std::set<uint32_t> with_bytes;
    for (uint32_t id = 0; id < model.files.size(); id++)
    {
        if (!kept.File(id).empty() || extra.contains(id))
        {
            with_bytes.insert(id);
        }
        else if (reserved.contains(id))
        {
            std::ranges::fill(model.files[id], uint8_t{0});
            with_bytes.insert(id);
        }
        else
        {
            model.files[id].clear();
        }
    }

    std::vector<uint8_t> bytes = sdat::BuildArchive(model, PageOffsets(archive, with_bytes));
    const sdat::Archive laid_out = sdat::Archive::Parse(bytes);
    for (const uint32_t id : with_bytes)
    {
        if (!std::ranges::equal(laid_out.File(id), model.files[id]))
        {
            return std::nullopt;
        }
    }

    return bytes;
}

SetArchive MakeSetArchive(const sdat::Archive& archive, std::span<const ntr::SequenceRef> sequences,
                          const std::set<uint32_t>& extra, const std::set<uint32_t>& reserved)
{
    if (std::optional<std::vector<uint8_t>> cut = CutArchive(archive, sequences, extra, reserved))
    {
        return {.bytes = std::move(*cut), .reserved = reserved};
    }

    return {.bytes = {archive.bytes.begin(), archive.bytes.end()}, .whole = true};
}

namespace
{

// The stub's data: the sound's number, the heap and the SDK's handle.
constexpr int kData = 4; // the data's address, kept across the calls
constexpr uint32_t kSoundField = 0;
constexpr uint32_t kHeapField = 4;
constexpr uint32_t kHandleField = 8;

// The SDK's ex start reads each of the player, the bank and the priority from the record where it's -1.
constexpr uint32_t kFromRecord = 0xFFFFFFFF;

// rd = value: a mov where one holds the value, or a load from the literal pool.
void LoadValue(arm::ArmAssembler& a, int rd, uint32_t value)
{
    if (arm::RotatedImmediate(value))
    {
        a.Mov(rd, value);
    }
    else
    {
        a.LoadConstant(rd, value);
    }
}

// A direct call at `at` of the routine at `target`: bl for ARM code, blx for Thumb code (bit 0 set).
uint32_t DirectCall(uint32_t at, uint32_t target)
{
    if ((target & 1) == 0)
    {
        return BranchLink(at, target);
    }

    constexpr uint32_t kBlx = 0xFA000000;
    constexpr uint32_t kHalfBit = 0x01000000;
    const uint32_t offset = (target & ~1u) - (at + 8);

    return kBlx | ((offset & 2) != 0 ? kHalfBit : 0) | ((offset >> 2) & 0x00FFFFFF);
}

// Loads `argument` of a table's call with `values` into the register `reg`. Throws std::logic_error for the result of
// the call before in any argument but the first (`index` 0) of a call after a list's first (`first`).
void LoadArgument(arm::ArmAssembler& a, int reg, const StubArgument& argument, std::size_t index, bool first,
                  const StartValues& values)
{
    switch (argument.kind)
    {
    case StubArgument::Kind::kSound:
        LoadValue(a, reg, values.sound);
        break;

    case StubArgument::Kind::kArchive:
        LoadValue(a, reg, values.archive);
        break;

    case StubArgument::Kind::kEntry:
        LoadValue(a, reg, values.entry);
        break;

    case StubArgument::Kind::kBank:
        LoadValue(a, reg, values.bank);
        break;

    case StubArgument::Kind::kHeap:
        LoadValue(a, reg, values.heap);
        break;

    case StubArgument::Kind::kPackedEntry:
        LoadValue(a, reg, values.archive << argument.value | values.entry);
        break;

    case StubArgument::Kind::kConstant:
        LoadValue(a, reg, argument.value);
        break;

    case StubArgument::Kind::kWord:
        a.LoadConstant(reg, argument.value);
        a.Ldr(reg, reg, 0);
        break;

    case StubArgument::Kind::kResult:
        if (index != 0)
        {
            throw std::logic_error("a call's result as an argument other than the first");
        }

        if (first)
        {
            throw std::logic_error("a call's result as an argument of a list's first call");
        }
        break;
    }
}

// The calls of a table's list with `values`, each a direct call with its first arguments in r0 to r3 and the others on
// the stack, the fifth at sp. The stack's arguments go first, through r12, and the stub takes its space back after the
// call. The registers' arguments load from the last to the first. The result of the call before therefore stays in r0
// for the first. Throws std::logic_error for a result in another argument, or in the list's first call: no call comes
// before it.
void EmitCalls(arm::ArmAssembler& a, std::span<const StubCall> calls, const StartValues& values)
{
    constexpr int kScratch = 12;
    constexpr uint32_t kStackAlignment = 8;
    for (const StubCall& call : calls)
    {
        const bool first = &call == calls.data();
        const std::size_t stacked = call.count > kRegisterArguments ? call.count - kRegisterArguments : 0;
        const auto stack_bytes =
            static_cast<uint32_t>((stacked * 4 + kStackAlignment - 1) / kStackAlignment * kStackAlignment);
        if (stacked > 0)
        {
            a.Sub(arm::kSp, arm::kSp, stack_bytes);
            for (std::size_t i = kRegisterArguments; i < call.count; i++)
            {
                LoadArgument(a, kScratch, call.arguments[i], i, first, values);
                a.Str(kScratch, arm::kSp, static_cast<int>(4 * (i - kRegisterArguments)));
            }
        }

        for (std::size_t i = std::min(call.count, kRegisterArguments); i-- > 0;)
        {
            LoadArgument(a, static_cast<int>(i), call.arguments[i], i, first, values);
        }

        a.Word(DirectCall(a.Here(), call.routine));
        if (stacked > 0)
        {
            a.Add(arm::kSp, arm::kSp, stack_bytes);
        }
    }
}

// The list of the game's starts (GameStart) for `sound`, whose record names `player`: the first list of `numbers` that
// holds it, or `calls`.
std::span<const StubCall> StartCallsOf(const GameStart& game, uint32_t sound, std::optional<uint32_t> player)
{
    for (const NumberCalls& numbers : game.numbers)
    {
        const auto holds = [sound](const std::pair<uint32_t, uint32_t>& range)
        {
            return sound >= range.first && sound <= range.second;
        };
        if (std::ranges::any_of(numbers.ranges, holds) || (numbers.player && numbers.player == player))
        {
            return numbers.calls;
        }
    }

    return game.calls;
}

// Where a start block's code goes after the calls: the frames (EmitFrames) and the update's entry in them.
struct FrameTargets
{
    uint32_t frame = 0;
    uint32_t update = 0;
};

// The wait for a frame of the screen, with its arguments: a direct call where `direct`, in a start block or in frames
// that have little room (DirectWaitAfterBlocks), or else a call through r12.
void EmitWait(arm::ArmAssembler& a, const SdatCode& code, bool direct = false)
{
    for (std::size_t i = 0; i < code.wait_arguments.size(); i++)
    {
        LoadValue(a, static_cast<int>(i), code.wait_arguments[i]);
    }
    if (direct)
    {
        a.Word(DirectCall(a.Here(), code.wait));
    }
    else
    {
        Call(a, code.wait);
    }
}

// The game frames of a state that plays on (PlaysOn) with `values`, in place of the branch to the frames. The update
// first ends the game frame of the start, as the game's frame does. r4 then counts the game frames. Each game frame
// waits for `video_frames` frames of the screen, makes the state's calls of each frame, then each timed call whose
// frame has come, then the update, once a game frame as in the game. The game's routines keep r4.
void EmitGameFrames(arm::ArmAssembler& a, const SdatCode& code, const GameState& state, const StartValues& values,
                    const FrameTargets& targets)
{
    constexpr int kFrameCount = 4;
    constexpr int kScratch = 12;
    a.Word(BranchLink(a.Here(), targets.update));
    a.Mov(kFrameCount, 0);
    const int frame = a.NewLabel();
    a.Bind(frame);
    for (uint32_t i = 0; i < state.video_frames; i++)
    {
        EmitWait(a, code, true);
    }
    EmitCalls(a, state.frame_calls, values);
    a.Add(kFrameCount, kFrameCount, 1);
    for (const FrameCall& timed : state.timed_calls)
    {
        const int later = a.NewLabel();
        if (arm::RotatedImmediate(timed.frame))
        {
            a.Cmp(kFrameCount, timed.frame);
        }
        else
        {
            a.LoadConstant(kScratch, timed.frame);
            a.CmpRegister(kFrameCount, kScratch);
        }
        a.B(later, arm::Condition::kNe);
        EmitCalls(a, std::span<const StubCall>(&timed.call, 1), values);
        a.Bind(later);
    }
    a.Word(BranchLink(a.Here(), targets.update));
    a.B(frame);
}

// The start block of `calls` and then the calls of `state`, if any, with `values`, at the stub's start of `code`: the
// calls, then a branch to the frames, or the game frames of a state that plays on (EmitGameFrames), then their
// literals.
std::vector<uint8_t> AssembleStartBlock(const SdatCode& code, std::span<const StubCall> calls, const GameState* state,
                                        const StartValues& values, const FrameTargets& targets)
{
    arm::ArmAssembler a(code.stub);
    EmitCalls(a, calls, values);
    if (state)
    {
        EmitCalls(a, state->calls, values);
    }

    if (state && PlaysOn(*state))
    {
        EmitGameFrames(a, code, *state, values, targets);
    }
    else
    {
        a.Word(Branch(a.Here(), targets.frame));
    }

    return a.Finish();
}

// The targets of a start block of a game for its size: the frames and the update at the stub's start. A block's size
// doesn't depend on the targets: every call in it is direct.
FrameTargets SizeTargets(const SdatCode& code)
{
    return {.frame = code.stub, .update = code.stub};
}

// The largest start block of the game's lists, each alone, with each state's calls when `with_states`, and of the
// effects' calls, in bytes. A block's size doesn't depend on the place or the values, but for the values' constants:
// every list is measured with values that take literals (kLiteralValues).
uint32_t LargestStartBlock(const SdatCode& code, bool with_states)
{
    const GameStart& game = *code.game_start;
    const FrameTargets targets = SizeTargets(code);
    std::vector<std::span<const StubCall>> lists = {game.calls};
    for (const NumberCalls& numbers : game.numbers)
    {
        lists.emplace_back(numbers.calls);
    }

    std::size_t largest = AssembleStartBlock(code, game.effect_calls, nullptr, kLiteralValues, targets).size();
    for (const std::span<const StubCall> list : lists)
    {
        largest = std::max(largest, AssembleStartBlock(code, list, nullptr, kLiteralValues, targets).size());
        for (const GameState& state :
             with_states ? std::span<const GameState>(game.states) : std::span<const GameState>())
        {
            largest = std::max(largest, AssembleStartBlock(code, list, &state, kLiteralValues, targets).size());
        }
    }

    return static_cast<uint32_t>(largest);
}

// The size of each start block of a set of `code`: SdatCode::start_block once the rip has read the sound archive, or
// else the largest block of any list with any state (LargestStartBlock).
uint32_t BlockBytes(const SdatCode& code)
{
    return code.start_block != 0 ? code.start_block : LargestStartBlock(code, true);
}

// The update: its first instruction, or the ARM forms of its first two in Thumb code, then a jump to the rest. A load
// of pc with bit 0 set enters Thumb state.
void EmitUpdate(arm::ArmAssembler& a, const SdatCode& code)
{
    if ((code.update & 1) != 0)
    {
        const std::optional<std::array<uint32_t, 2>> moved = RelocatableThumb(code.update_first);
        if (!moved)
        {
            throw std::logic_error("the update's first Thumb instructions can't move into the stub");
        }
        a.Word((*moved)[0]);
        a.Word((*moved)[1]);
    }
    else
    {
        a.Word(code.update_first);
    }
    a.Ldr(arm::kPc, arm::kPc, -4);
    a.Word(code.update + 4);
}

// Each frame: the wait with its arguments, a direct call where `direct_wait` (EmitWait), then the update (EmitUpdate).
// Returns the update's address.
uint32_t EmitFrames(arm::ArmAssembler& a, const SdatCode& code, bool direct_wait = false)
{
    const int frame = a.NewLabel();
    const int update = a.NewLabel();
    a.Bind(frame);
    EmitWait(a, code, direct_wait);
    a.Bl(update);
    a.B(frame);

    a.Bind(update);
    const uint32_t update_at = a.Here();
    EmitUpdate(a, code);

    return update_at;
}

// Whether the frames after the start blocks of a game in the table of games call the wait directly: only where the call
// through r12 doesn't fit beside the blocks (BlockBytes) in the decompressor's body. The direct call saves the literal
// and at least one instruction. Every set whose frames fit with the call through r12 keeps it.
bool DirectWaitAfterBlocks(const SdatCode& code)
{
    const uint32_t block = BlockBytes(code);
    arm::ArmAssembler frames(code.stub + block);
    EmitFrames(frames, code);
    frames.Pool();

    return block + frames.Finish().size() > code.stub_end - code.stub;
}

// The stub of a game in the table of games: the start block of `sound` (StartBlock), padded to the largest, then the
// frames.
SdatStub BuildTableStub(const SdatCode& code, uint32_t sound, std::optional<uint32_t> player)
{
    const uint32_t block = BlockBytes(code);
    arm::ArmAssembler a(code.stub + block);
    EmitFrames(a, code, DirectWaitAfterBlocks(code));
    a.Pool();

    SdatStub stub{.bytes = StartBlock(code, sound, player), .sound_offset = 0, .sound_bytes = block};
    const std::vector<uint8_t> frames = a.Finish();
    stub.bytes.insert(stub.bytes.end(), frames.begin(), frames.end());

    return stub;
}

// The stub of a stream of a game whose stream manager starts it: the manager's calls for `stream`, with the number as a
// constant, then the frames.
SdatStub BuildManagerStreamStub(const SdatCode& code, uint32_t stream)
{
    arm::ArmAssembler a(code.stub);
    EmitCalls(a, code.game_start->stream_calls, {.sound = stream});
    EmitFrames(a, code);
    a.Pool();

    return {.bytes = a.Finish()};
}

// The SDK's load and start of the sequence, with the game's restart of its sound before the restart's sequence.
void EmitSdkStart(arm::ArmAssembler& a, const SdatCode& code)
{
    // The game's start-up of its sound again, before the one sequence that the game plays after it.
    if (code.restart)
    {
        const int load = a.NewLabel();
        a.Ldr(0, kData, kSoundField);
        a.Cmp(0, code.restart->sequence);
        a.B(load, arm::Condition::kNe);
        a.Mov(0, code.restart->argument);
        Call(a, code.restart->routine);
        a.Bind(load);
    }

    // The load: the loader's (sequence, heap), or (sequence, 0xFF, heap, 1, null) with the null on the stack.
    if (code.loader)
    {
        a.Ldr(0, kData, kSoundField);
        a.Ldr(1, kData, kHeapField);
        Call(a, *code.loader);
    }
    else
    {
        a.Sub(arm::kSp, arm::kSp, 8);
        a.Mov(0, 0);
        a.Str(0, arm::kSp);
        a.Ldr(0, kData, kSoundField);
        a.Mov(1, kEveryKind);
        a.Ldr(2, kData, kHeapField);
        a.Mov(3, 1);
        Call(a, code.load);
    }

    // NNS_SndArcPlayerStartSeq(handle, sequence), or NNS_SndArcPlayerStartSeqEx(handle, -1, -1, -1, sequence) with the
    // sequence on the stack.
    if (code.start_ex)
    {
        a.Sub(arm::kSp, arm::kSp, 8);
        a.Ldr(0, kData, kSoundField);
        a.Str(0, arm::kSp);
        a.Add(0, kData, kHandleField);
        a.LoadConstant(1, kFromRecord);
        a.MovRegister(2, 1);
        a.MovRegister(3, 1);
    }
    else
    {
        a.Add(0, kData, kHandleField);
        a.Ldr(1, kData, kSoundField);
    }
    Call(a, code.start);
}

// The start block of `calls` and then the calls of `state`, if any, with `values` (AssembleStartBlock), padded to the
// size of every start block (BlockBytes). Throws std::logic_error for a block larger than that size.
std::vector<uint8_t> PaddedBlock(const SdatCode& code, std::span<const StubCall> calls, const GameState* state,
                                 const StartValues& values)
{
    const uint32_t block = BlockBytes(code);
    arm::ArmAssembler frames(code.stub + block);
    const uint32_t update = EmitFrames(frames, code, DirectWaitAfterBlocks(code));
    const FrameTargets targets = {.frame = code.stub + block, .update = update};
    std::vector<uint8_t> bytes = AssembleStartBlock(code, calls, state, values, targets);
    if (bytes.size() > block)
    {
        throw std::logic_error("a start block of " + std::to_string(bytes.size()) + " bytes, larger than the set's " +
                               std::to_string(block));
    }
    bytes.resize(block);

    return bytes;
}

} // namespace

bool PlaysOn(const GameState& state)
{
    return !state.frame_calls.empty() || !state.timed_calls.empty();
}

std::vector<StubCall> StartCallsOf(std::string_view game_code, uint32_t sound, std::optional<uint32_t> player)
{
    const auto entry = std::ranges::find(kGameStarts, game_code, &GameStartEntry::game_code);
    if (entry == std::ranges::end(kGameStarts))
    {
        return {};
    }

    const GameStart game = GameStartOf(*entry);
    const std::span<const StubCall> calls = StartCallsOf(game, sound, player);

    return {calls.begin(), calls.end()};
}

std::vector<uint8_t> StartBlock(const SdatCode& code, uint32_t sound, std::optional<uint32_t> player,
                                const GameState* state)
{
    if (!code.game_start)
    {
        throw std::logic_error("a start block for a game outside the table of games");
    }

    return PaddedBlock(code, StartCallsOf(*code.game_start, sound, player), state, {.sound = sound});
}

uint32_t StartBlockBytes(const SdatCode& code, const sdat::Archive& archive)
{
    if (!code.game_start)
    {
        throw std::logic_error("the size of a start block for a game outside the table of games");
    }

    // The largest block of any list with any state, where it fits beside the frames.
    const uint32_t any = LargestStartBlock(code, true);
    arm::ArmAssembler frames(code.stub + any);
    EmitFrames(frames, code);
    frames.Pool();
    if (any + frames.Finish().size() <= code.stub_end - code.stub)
    {
        return any;
    }

    const GameStart& game = *code.game_start;
    const FrameTargets targets = SizeTargets(code);
    std::size_t largest = 0;
    if (!game.effect_calls.empty())
    {
        largest = AssembleStartBlock(code, game.effect_calls, nullptr, kLiteralValues, targets).size();
    }

    for (uint32_t number = 0; number < archive.sequences.size(); number++)
    {
        if (!archive.sequences[number])
        {
            continue;
        }

        const std::span<const StubCall> calls = StartCallsOf(game, number, archive.sequences[number]->player);
        largest = std::max(largest, AssembleStartBlock(code, calls, nullptr, kLiteralValues, targets).size());
        for (const GameState* state : StatesOf(code, ntr::Label(archive, {number})))
        {
            largest = std::max(largest, AssembleStartBlock(code, calls, state, kLiteralValues, targets).size());
        }
    }

    return static_cast<uint32_t>(largest);
}

std::vector<uint8_t> EffectBlock(const SdatCode& code, const sdat::Archive& archive, const ntr::SequenceRef& sequence,
                                 uint32_t heap)
{
    if (!code.game_start || code.game_start->effect_calls.empty() || !sequence.archived)
    {
        throw std::logic_error(
            "an effect's block for a game without the effects' calls, or for a sequence outside the "
            "sequence archives");
    }

    const sdat::SequenceArchive file =
        sdat::SequenceArchive::Parse(archive.File(archive.sequence_archives.at(sequence.index).value().file_id));
    const StartValues values = {.archive = sequence.index,
                                .entry = *sequence.archived,
                                .bank = file.sequences.at(*sequence.archived).bank,
                                .heap = heap};

    return PaddedBlock(code, code.game_start->effect_calls, nullptr, values);
}

std::vector<const GameState*> StatesOf(const SdatCode& code, std::string_view label)
{
    std::vector<const GameState*> states;
    if (!code.game_start)
    {
        return states;
    }

    for (const GameState& state : code.game_start->states)
    {
        if (state.label == label)
        {
            states.insert(state.suffix.empty() ? states.begin() : states.end(), &state);
        }
    }

    return states;
}

SdatStub BuildCallStub(const SdatCode& code, uint32_t routine, uint32_t argument, std::optional<uint32_t> frame_routine)
{
    arm::ArmAssembler a(code.stub);
    if (!frame_routine)
    {
        a.LoadConstant(0, argument);
        Call(a, routine);
        EmitFrames(a, code);
        a.Pool();
        return {.bytes = a.Finish()};
    }

    // The hook enters the stub once from the game, and then once a frame from the frame routine's call of the update.
    // The word `entered` distinguishes them. r12 is free at a call's entry.
    const int entered = a.NewLabel();
    const int update = a.NewLabel();
    const int frame = a.NewLabel();
    a.LoadAddress(12, entered);
    a.Ldr(12, 12);
    a.Cmp(12, 0);
    a.B(update, arm::Condition::kNe);
    a.LoadAddress(0, entered);
    a.Mov(1, 1);
    a.Str(1, 0);
    a.LoadConstant(0, argument);
    Call(a, routine);
    a.Bind(frame);
    EmitWait(a, code);
    Call(a, *frame_routine);
    a.B(frame);

    a.Bind(update);
    EmitUpdate(a, code);
    a.Bind(entered);
    a.Word(0);
    a.Pool();

    return {.bytes = a.Finish()};
}

SdatStub BuildSdatStub(const SdatCode& code, uint32_t heap, uint32_t sound, SoundKind kind,
                       std::optional<uint32_t> player)
{
    if (kind == SoundKind::kStream && !StartsStreams(code))
    {
        throw std::logic_error("a stream's stub without the start of a stream");
    }

    // A game in the table of games starts its sequences with a block of its calls at the stub's start. A mini replaces
    // the block. The frame's loop follows the largest block.
    if (kind == SoundKind::kSequence && code.game_start)
    {
        return BuildTableStub(code, sound, player);
    }

    if (kind == SoundKind::kStream && ManagerStartsStreams(code))
    {
        return BuildManagerStreamStub(code, sound);
    }

    arm::ArmAssembler a(code.stub);
    const int data = a.NewLabel();

    a.LoadAddress(kData, data);

    if (kind == SoundKind::kStream)
    {
        // NNS_SndArcStrmStart(handle, stream, 0): the stream from its start.
        a.Add(0, kData, kHandleField);
        a.Ldr(1, kData, kSoundField);
        a.Mov(2, 0);
        Call(a, *code.stream_start);
    }
    else
    {
        EmitSdkStart(a, code);
    }

    EmitFrames(a, code);

    a.Pool();
    a.Bind(data);
    a.Word(sound);
    a.Word(heap);
    a.Word(0); // the handle

    SdatStub stub;
    stub.sound_offset = a.Address(data) + kSoundField - code.stub;
    stub.bytes = a.Finish();

    return stub;
}

SdatSet MakeSdatSet(const ntr::Rom& rom, const SdatCode& code, const SdatStub& stub,
                    const std::map<uint32_t, std::vector<uint8_t>>& files)
{
    if (stub.bytes.size() > code.stub_end - code.stub)
    {
        throw std::runtime_error("the stub takes " + std::to_string(stub.bytes.size()) +
                                 " bytes, and the decompressor's body has " +
                                 std::to_string(code.stub_end - code.stub));
    }

    ntr::CompactParts parts{.arm9 = ntr::UncompressedArm9(rom), .files = files};
    const uint32_t base = rom.Header().arm9.ram_address;
    for (const GamePatch& patch : code.patches)
    {
        ApplyPatch(parts.arm9, base, patch);
    }
    std::ranges::copy(stub.bytes, parts.arm9.begin() + (code.stub - base));
    xsflib::StoreLe32(&parts.arm9[(code.update & ~1u) - base], HookWord(code));

    // A game in the table of games plays its music from a separate thread. The main thread sleeps for good after the
    // sound's start-up: mov r0, #0, then the wait, then back.
    if (code.game_start && code.game_start->park != 0)
    {
        constexpr uint32_t kMovR0Zero = 0xE3A00000;
        const uint32_t park = code.game_start->park;
        xsflib::StoreLe32(&parts.arm9[park - base], kMovR0Zero);
        xsflib::StoreLe32(&parts.arm9[park + 4 - base], BranchLink(park + 4, code.wait));
        xsflib::StoreLe32(&parts.arm9[park + 8 - base], Branch(park + 8, park));
    }

    // The SDK's overlay loader decompresses an overlay with the start-up decompressor, whose body contains the stub.
    // The kept overlays go in decompressed.
    std::vector<std::size_t> decompressed;
    const std::vector<ntr::Overlay>& overlays = rom.Arm9Overlays();
    for (std::size_t i = 0; i < overlays.size(); i++)
    {
        const auto kept = parts.files.find(overlays[i].file_id);
        if (ntr::OverlayCompressed(overlays[i]) && kept != parts.files.end())
        {
            kept->second = ntr::OverlayBytes(rom.FileData(overlays[i].file_id), overlays[i]);
            decompressed.push_back(i);
        }
    }

    SdatSet set;
    set.image = ntr::WriteCompactImage(rom, parts);
    for (const std::size_t i : decompressed)
    {
        ntr::ClearArm9OverlayFlags(set.image, i);
    }
    set.stub = rom.Header().arm9.rom_offset + (code.stub - base);
    set.sound_word = set.stub + stub.sound_offset;

    return set;
}

std::optional<ntr::snd::CommandList> SoundCommands(std::span<const uint8_t> memory, uint32_t word)
{
    if ((word & kTagBits) != kSoundTag)
    {
        return std::nullopt;
    }

    ntr::snd::CommandList list;
    for (uint32_t command = word >> kListShift; ntr::InMainMemory(memory, command); command = Word(memory, command))
    {
        ntr::snd::Command read{.id = static_cast<CommandId>(Word(memory, command + kCommandId))};
        for (uint32_t i = 0; i < read.args.size(); i++)
        {
            read.args[i] = Word(memory, command + kCommandArgs + 4 * i);
        }
        list.push_back(read);
    }

    return list;
}

namespace
{

// The ranges of the bytes of `rom` that a card read after the stub starts may reach: every file with bytes other than
// the sound archive, the file `archive_file`, the archive's header and tables, and each of the archive's files with
// bytes, but the files `left_out`. A game that runs only on a DSi checks each sector that it reads against the image's
// sector and block hash tables. The header's 0x1F0 to 0x1FF give the tables by offset and size (GBATEK's "DSi Cartridge
// Header"). Its tables count as kept, and each range widens to whole sectors of the digest.
std::vector<std::pair<uint32_t, uint32_t>> KeptRanges(const ntr::Rom& rom, uint32_t archive_file,
                                                      const sdat::Archive& archive, const std::set<uint32_t>& left_out)
{
    const bool dsi = ntr::RunsInDsiMode(rom.Header());
    const ntr::DigestSectors sectors = dsi ? ntr::DigestSectorsOf(rom) : ntr::DigestSectors{};
    std::vector<std::pair<uint32_t, uint32_t>> kept;
    if (dsi)
    {
        constexpr std::size_t kSectorHashes = 0x1F0;
        constexpr std::size_t kBlockHashes = 0x1F8;
        for (const std::size_t field : {kSectorHashes, kBlockHashes})
        {
            const uint32_t offset = xsflib::LoadLe32(&rom.Image()[field]);
            kept.emplace_back(offset, offset + xsflib::LoadLe32(&rom.Image()[field + 4]));
        }
    }

    // A DSi-only image keeps every file's place. A file that the set leaves out contains zeros, but for its bytes in a
    // sector that a kept file reaches (ntdlib's WriteCompactImage). Such a file counts as left out when its bytes in
    // the sectors that no other file reaches are zeros. A file that shares every sector that it reaches counts as left
    // out when all its bytes are zeros.
    std::vector<const ntr::RomFile*> by_start;
    for (const ntr::RomFile& file : rom.Files())
    {
        if (file.end > file.start)
        {
            by_start.push_back(&file);
        }
    }
    std::ranges::sort(by_start, {}, &ntr::RomFile::start);
    const auto left_out_of_image = [&rom, &sectors, &by_start](std::size_t i)
    {
        const ntr::RomFile& file = *by_start[i];
        const auto [first, end] = sectors.Around(file.start, file.end);
        uint32_t alone_first = file.start;
        uint32_t alone_end = file.end;
        if (i > 0 && by_start[i - 1]->end > first)
        {
            alone_first = std::max(alone_first, first + sectors.size);
        }
        if (i + 1 < by_start.size() && by_start[i + 1]->start < end)
        {
            alone_end = std::min(alone_end, end - sectors.size);
        }
        if (alone_first >= alone_end)
        {
            alone_first = file.start;
            alone_end = file.end;
        }
        const std::span<const uint8_t> alone = rom.Image().subspan(alone_first, alone_end - alone_first);
        return std::ranges::all_of(alone, [](uint8_t byte) { return byte == 0; });
    };
    for (std::size_t i = 0; i < by_start.size(); i++)
    {
        const ntr::RomFile& file = *by_start[i];
        if (file.id != archive_file && !(dsi && left_out_of_image(i)))
        {
            kept.push_back(sectors.Around(file.start, file.end));
        }
    }

    const uint32_t start = rom.Files().at(archive_file).start;
    uint32_t first_file = static_cast<uint32_t>(archive.bytes.size());
    for (uint32_t id = 0; id < archive.files.size(); id++)
    {
        const sdat::FileEntry& entry = archive.files[id];
        if (entry.size > 0)
        {
            first_file = std::min(first_file, entry.offset);
            if (!left_out.contains(id))
            {
                kept.push_back(sectors.Around(start + entry.offset, start + entry.offset + entry.size));
            }
        }
    }
    kept.push_back(sectors.Around(start, start + first_file));

    return kept;
}

} // namespace

namespace
{

// Whether a command of `id` sets what the driver's player that it addresses (PlayerKey) plays.
bool SetsPlayer(CommandId id)
{
    return id == CommandId::kPlayerParameter || id == CommandId::kTrackParameter || id == CommandId::kMuteTrack ||
           id == CommandId::kAllocatableChannels;
}

// The driver's player that `command` addresses (ntr::snd::PlayerOf), as StartFollower keys its players.
uint32_t PlayerKey(const ntr::snd::Command& command)
{
    return static_cast<uint32_t>(ntr::snd::PlayerOf(command));
}

// `command` addressed to the driver's player 0 in place of `player`. Nothing for a command that sets no player's state
// (SetsPlayer) or that addresses another player.
std::optional<ntr::snd::Command> OnPlayerZero(const ntr::snd::Command& command, uint32_t player)
{
    if (!SetsPlayer(command.id) || PlayerKey(command) != player)
    {
        return std::nullopt;
    }

    ntr::snd::Command moved = command;
    moved.args[0] =
        command.id == CommandId::kTrackParameter ? command.args[0] & ~ntr::snd::kTrackParameterPlayerBits : 0;

    return moved;
}

} // namespace

std::optional<std::string> Interference(CommandId id)
{
    switch (id)
    {
    case CommandId::kStartSequence:
    case CommandId::kPrepareSequence:
    case CommandId::kStartPreparedSequence:
        return "starts or prepares a sequence";
    case CommandId::kStopSequence:
        return "stops a sequence";
    case CommandId::kPlayerLocalVariable:
    case CommandId::kGlobalVariable:
        return "sets a variable";
    case CommandId::kPlayerParameter:
    case CommandId::kTrackParameter:
    case CommandId::kMuteTrack:
        return "sets a player's or a track's volume, mute or parameter";
    case CommandId::kMasterVolume:
    case CommandId::kChannelVolume:
        return "sets a volume";
    default:
        return std::nullopt;
    }
}

void GuardCardReads(ntr::Machine& machine, const ntr::Rom& rom, uint32_t archive_file, const sdat::Archive& archive,
                    const std::set<uint32_t>& left_out, uint64_t hook_clock, std::function<void(std::string)> fail)
{
    machine.on_card_read = [&machine, kept = KeptRanges(rom, archive_file, archive, left_out), hook_clock,
                            report = std::move(fail)](uint32_t address, uint32_t bytes)
    {
        const auto reaches = [&](const std::pair<uint32_t, uint32_t>& range)
        {
            return range.first < uint64_t{address} + bytes && address < range.second;
        };
        if (machine.Clock() >= hook_clock && !std::ranges::any_of(kept, reaches))
        {
            report(ReadOutsideSetError(address));
        }
    };
}

std::optional<ntr::snd::Command> StartFollower::Take(const ntr::snd::CommandList& list)
{
    std::optional<ntr::snd::Command> started;
    for (const ntr::snd::Command& command : list)
    {
        const CommandId id = command.id;
        if (id == CommandId::kLockChannels)
        {
            run_.locked |= static_cast<uint16_t>(command.args[0]);
        }
        else if (id == CommandId::kUnlockChannels)
        {
            run_.locked &= static_cast<uint16_t>(~command.args[0]);
        }

        if (started)
        {
            if (id == CommandId::kPlayerLocalVariable && PlayerKey(command) == player_)
            {
                Record(command, true);
            }
            else if (const std::optional<ntr::snd::Command> moved = OnPlayerZero(command, player_))
            {
                Record(*moved, true);
            }
            continue;
        }

        if (id == CommandId::kGlobalVariable && command.args[0] < run_.variables.global.size())
        {
            run_.variables.global[command.args[0]] = static_cast<int16_t>(command.args[1]);
        }
        else if (id == CommandId::kPrepareSequence || id == CommandId::kStartSequence)
        {
            pending_[PlayerKey(command)] = {.prepare = command};
        }
        else if (id == CommandId::kStopSequence)
        {
            pending_.erase(PlayerKey(command));
        }
        else if (id == CommandId::kPlayerLocalVariable && pending_.contains(PlayerKey(command)))
        {
            pending_[PlayerKey(command)].commands.push_back(command);
        }
        else if (SetsPlayer(id) && pending_.contains(PlayerKey(command)))
        {
            pending_[PlayerKey(command)].commands.push_back(*OnPlayerZero(command, PlayerKey(command)));
        }

        const bool sequence_starts = id == CommandId::kStartSequence || id == CommandId::kStartPreparedSequence;
        const auto prepared = sequence_starts ? pending_.find(PlayerKey(command)) : pending_.end();
        if (prepared != pending_.end())
        {
            player_ = prepared->first;
            started = prepared->second.prepare;
            for (const ntr::snd::Command& recorded : prepared->second.commands)
            {
                Record(recorded, false);
            }
        }
    }

    return started;
}

void StartFollower::Record(const ntr::snd::Command& command, bool started)
{
    if (command.id == CommandId::kPlayerLocalVariable)
    {
        if (command.args[1] < run_.variables.local.size())
        {
            run_.variables.local[command.args[1]] = static_cast<int16_t>(command.args[2]);
        }
    }
    else if (command.id == CommandId::kAllocatableChannels && command.args[1] == ntr::snd::kAllTracks)
    {
        run_.channels = static_cast<uint16_t>(command.args[2]);
    }
    else if (command.id == CommandId::kPlayerParameter && command.args[1] == ntr::snd::kPlayerFaderOffset && !started)
    {
        run_.volume = static_cast<int16_t>(command.args[2]);
    }
    else if (command.id == CommandId::kPlayerParameter && command.args[1] == ntr::snd::kPlayerPriorityOffset &&
             !started)
    {
        run_.priority = static_cast<int16_t>(command.args[2]);
    }
    else
    {
        run_.commands.push_back(command);
    }
}

namespace
{

// Sets the check of a run's card reads on `machine`: `fail` takes the reason of a read that fails it.
using ReadGuard = std::function<void(ntr::Machine& machine, std::function<void(std::string)> fail)>;

// RunMini's run of `rom` with its sound archive `archive`, with the check of the card reads that `guard` sets.
MiniRun RunMiniWith(const ntr::Rom& rom, const sdat::Archive& archive, const ntr::SequenceRef& sequence, double seconds,
                    const ntr::BackupMemory& backup, const ReadGuard& guard, uint32_t video_frames)
{
    MiniRun run{.video_frames = video_frames};
    const auto machine = std::make_unique<ntr::Machine>(rom, backup);
    const std::span<uint8_t> memory = machine->MainMemory();
    const auto fail = [&run](std::string error)
    {
        if (!run.error)
        {
            run.error = std::move(error);
        }
    };

    // The expected files of the sequence: its file, or its sequence archive's, and its bank's. The prepare names the
    // file's data at the offset of the data, and the offset in it where the sequence starts: the entry's offset in a
    // sequence archive.
    std::span<const uint8_t> sequence_file;
    uint32_t data_offset = 0;
    std::optional<uint32_t> entry_offset;
    uint32_t bank_number = 0;
    if (sequence.archived)
    {
        sequence_file = archive.File(archive.sequence_archives.at(sequence.index).value().file_id);
        const sdat::SequenceArchive file = sdat::SequenceArchive::Parse(sequence_file);
        const sdat::ArchivedSequence& entry = file.sequences.at(*sequence.archived);
        data_offset = file.data_offset;
        entry_offset = entry.offset;
        bank_number = entry.bank;
    }
    else
    {
        const sdat::SequenceInfo& info = *archive.sequences.at(sequence.index);
        sequence_file = archive.File(info.file_id);
        data_offset = xsflib::LoadLe32(&sequence_file[kSequenceDataField]);
        bank_number = info.bank;
    }
    const sdat::BankInfo& bank = *archive.banks.at(bank_number);
    const std::span<const uint8_t> bank_file = archive.File(bank.file_id);

    guard(*machine, fail);

    bool prepared = false;
    bool started = false;
    uint64_t start_clock = 0;
    uint32_t bank_address = 0;
    StartFollower follower(run);
    machine->on_fifo_word = [&](bool to_arm7, uint32_t word)
    {
        const std::optional<ntr::snd::CommandList> list = to_arm7 ? SoundCommands(memory, word) : std::nullopt;
        if (!list)
        {
            return;
        }

        // After the start, a state that plays on may set the sequence's player.
        if (started)
        {
            LaterList later{.clocks = machine->Clock() - start_clock};
            for (const ntr::snd::Command& command : *list)
            {
                const std::optional<ntr::snd::Command> moved =
                    video_frames != 0 ? OnPlayerZero(command, follower.Player()) : std::nullopt;
                const bool lock = command.id == CommandId::kLockChannels || command.id == CommandId::kUnlockChannels;
                if (moved)
                {
                    later.commands.push_back(*moved);
                }
                else if (lock && video_frames != 0)
                {
                    later.commands.push_back(command);
                }
                else if (const std::optional<std::string> change = Interference(command.id))
                {
                    fail("a list of the driver's commands after the start " + *change);
                }
            }
            if (!later.commands.empty())
            {
                run.later.push_back(std::move(later));
            }

            return;
        }

        const std::optional<ntr::snd::Command> prepare = follower.Take(*list);
        if (!prepare)
        {
            return;
        }

        prepared = true;
        started = true;
        start_clock = machine->Clock();
        run.start_ms = static_cast<double>(machine->Clock()) * 1000.0 / ntr::kSystemClockHz;
        bank_address = prepare->args[3];
        if (!Holds(memory, prepare->args[1] - data_offset, sequence_file, false))
        {
            fail("the driver's prepare command names " + xsflib::Hex(prepare->args[1], 8, true) +
                 ", not the sequence's bytes");
        }
        else if (entry_offset && prepare->args[2] != *entry_offset)
        {
            fail("the driver's prepare command starts at " + xsflib::Hex(prepare->args[2], 8, true) +
                 ", not at the sequence archive's entry");
        }
        else if (!Holds(memory, bank_address, bank_file, true))
        {
            fail("the driver's prepare command names " + xsflib::Hex(bank_address, 8, true) + ", not the bank's bytes");
        }
    };

    // The output counts from the first step that begins after the start.
    std::vector<ntr::SoundOutput> out;
    OutputWatch watch;
    const auto limit = static_cast<uint64_t>(seconds * ntr::kSystemClockHz);
    uint64_t end = limit;
    while (!run.error && machine->Clock() < end)
    {
        const bool playing = started;
        machine->Run(std::min(end, machine->Clock() + kRunStep), out);
        if (playing)
        {
            watch.Add(out);
        }
        out.clear();
        if (started && end == limit)
        {
            const double play_seconds = video_frames != 0 ? kPlayOnSeconds : kMiniPlaySeconds;
            end = machine->Clock() + static_cast<uint64_t>(play_seconds * ntr::kSystemClockHz);
        }
    }
    run.sounded = watch.Sounded();
    run.played_clocks = started ? machine->Clock() - start_clock : 0;

    if (!started)
    {
        run.not_started = !run.error;
        fail("the sequence doesn't start within " + std::to_string(static_cast<int>(seconds)) + " s of the boot");
    }

    run.captures_run = machine->Arm7().Sound().CapturesRun();

    // Each slot of the bank that names a wave archive links to one in main memory.
    for (uint32_t slot = 0; slot < bank.wave_archives.size() && prepared; slot++)
    {
        const uint32_t link = bank_address + static_cast<uint32_t>(kBankLinksStart) + kBankLinkBytes * slot;
        if (bank.wave_archives[slot] != sdat::kNoWaveArchive && ntr::InMainMemory(memory, link) &&
            !ntr::InMainMemory(memory, Word(memory, link)))
        {
            fail("the bank's slot " + std::to_string(slot) + " has no wave archive in memory");
        }
    }

    return run;
}

} // namespace

MiniRun RunMini(std::span<const uint8_t> image, uint64_t hook_clock, uint32_t archive_file,
                const ntr::SequenceRef& sequence, const std::set<uint32_t>& reserved, double seconds,
                const ntr::BackupMemory& backup, uint32_t video_frames)
{
    const ntr::Rom rom = ntr::Rom::Parse(image);
    const sdat::Archive archive = sdat::Archive::Parse(rom.FileData(archive_file));

    // After the hook, each card read must reach a range that the set keeps.
    const auto guard = [&](ntr::Machine& machine, std::function<void(std::string)> fail)
    {
        GuardCardReads(machine, rom, archive_file, archive, reserved, hook_clock, std::move(fail));
    };

    return RunMiniWith(rom, archive, sequence, seconds, backup, guard, video_frames);
}

MiniRun RunMemoryMini(std::span<const uint8_t> image, uint64_t hook_clock, const sdat::Archive& archive,
                      const ntr::SequenceRef& sequence, double seconds, const ntr::BackupMemory& backup)
{
    const ntr::Rom rom = ntr::Rom::Parse(image);

    // The image has no files: after the hook, any card read fails the run.
    const auto guard = [hook_clock](ntr::Machine& machine, std::function<void(std::string)> fail)
    {
        machine.on_card_read = [&machine, hook_clock, report = std::move(fail)](uint32_t address, uint32_t)
        {
            if (machine.Clock() >= hook_clock)
            {
                report("the program reads the card at " + xsflib::Hex(address, 8, true) + " after the hook");
            }
        };
    };

    return RunMiniWith(rom, archive, sequence, seconds, backup, guard, 0);
}

bool SameStart(const MiniRun& a, const MiniRun& b)
{
    return a.commands == b.commands && a.locked == b.locked && a.later == b.later &&
           a.variables.local == b.variables.local && a.variables.global == b.variables.global &&
           a.channels == b.channels && a.volume == b.volume && a.priority == b.priority &&
           a.captures_run == b.captures_run;
}

std::vector<uint32_t> FirstOfSameStart(const sdat::Archive& archive, std::span<const uint32_t> numbers,
                                       std::span<const MiniRun* const> runs)
{
    std::vector<uint32_t> firsts;
    firsts.reserve(numbers.size());
    for (std::size_t i = 0; i < numbers.size(); i++)
    {
        const auto same = [&](std::size_t other)
        {
            return runs[other] && runs[i] && SameSequenceRecord(archive, numbers[other], numbers[i]) &&
                   SameStart(*runs[other], *runs[i]);
        };
        std::size_t first = 0;
        while (first < i && !same(first))
        {
            first++;
        }
        firsts.push_back(numbers[first < i ? first : i]);
    }

    return firsts;
}

xsflib::rip::Limit AnalysisLimit(const std::set<uint32_t>& music, uint32_t sequence)
{
    return music.contains(sequence) ? xsflib::rip::Limit::kLong : xsflib::rip::Limit::kDefault;
}

ntr::GameSettings LengthSettings(const MiniRun& run, const ntr::GameSettings& profile, uint32_t player,
                                 uint16_t archive_channels)
{
    ntr::GameSettings settings;
    settings.variables = run.variables;
    if (run.channels && *run.channels != archive_channels)
    {
        settings.channels[player] = *run.channels;
    }
    if (run.captures_run)
    {
        settings.reverb = profile.reverb;
        settings.effect = profile.effect;
    }

    return settings;
}

bool EchoLeftOut(const MiniRun& run, const ntr::GameSettings& settings)
{
    return run.captures_run && !settings.reverb && !settings.effect;
}

std::vector<BootRun> MiniChecks(std::span<const MiniRun> runs)
{
    std::vector<BootRun> checks;
    for (const MiniRun& run : runs)
    {
        BootRun& check = checks.emplace_back(BootRun{run.error, run.sounded});
        if (!check.error && (run.variables.global != runs.front().variables.global ||
                             run.variables.local != runs.front().variables.local))
        {
            check.error = "the driver's commands set other variables than with no backup memory";
        }
    }

    return checks;
}

bool LeftOutUnstarted(const SdatCode& code, std::span<const MiniRun> runs,
                      const std::optional<xsflib::rip::Timing>& timing)
{
    return code.game_start && !runs.empty() && std::ranges::all_of(runs, &MiniRun::not_started) && timing &&
           timing->silent;
}

namespace
{

// What a list of the driver's commands leaves set on player 0 (ProfileDifferences): each value by its name.
using SetValues = std::map<std::string, int64_t>;

// The value of the channels locked at the start (MiniRun::locked). WithoutStartValues takes it from every track.
constexpr std::string_view kLockedChannels = "locked channels";

// Adds what `commands` set on player 0 to `values`, in their order.
void Apply(SetValues& values, const ntr::snd::CommandList& commands)
{
    for (const ntr::snd::Command& command : commands)
    {
        if (!SetsPlayer(command.id) || PlayerKey(command) != 0)
        {
            continue;
        }

        const auto& a = command.args;
        const auto each_track = [&](const std::string& name, int64_t value)
        {
            for (uint32_t track = 0; track < 16; track++)
            {
                if ((a[1] >> track & 1) != 0)
                {
                    values[name + " of track " + std::to_string(track)] = value;
                }
            }
        };
        switch (command.id)
        {
        case CommandId::kPlayerParameter:
            values["player's parameter " + xsflib::Hex(a[1], 2, true)] = static_cast<int16_t>(a[2]);
            break;
        case CommandId::kTrackParameter:
            each_track("parameter " + xsflib::Hex(a[2], 2, true), static_cast<int16_t>(a[3]));
            break;
        case CommandId::kMuteTrack:
            each_track("mute", a[2]);
            break;
        default:
            if (a[1] == ntr::snd::kAllTracks)
            {
                values["channels"] = a[2];
            }
            else
            {
                each_track("channels", a[2]);
            }
            break;
        }
    }
}

// `values` without those at the start's values: the channels `channels`, the player's volume `volume` and priority
// `priority`, a tempo's ratio of 256, and 0 for the rest. A track's channels equal to the player's are left out too.
SetValues WithoutStartValues(SetValues values, uint16_t channels, int16_t volume, int16_t priority)
{
    // Channels locked at the start: neither the player nor any of its tracks has them. A player of the archive that
    // names no channels gives its tracks all 16.
    if (const auto locked = values.find(std::string(kLockedChannels)); locked != values.end())
    {
        constexpr int64_t kEveryChannel = 0xFFFF;
        const int64_t mask = locked->second;
        values.erase(locked);
        const int64_t archive = channels != 0 ? channels : kEveryChannel;
        values["channels"] = (values.contains("channels") ? values.at("channels") : archive) & ~mask;
        for (auto& [name, value] : values)
        {
            if (name.starts_with("channels of"))
            {
                value &= ~mask;
            }
        }
    }

    const int64_t player_channels = values.contains("channels") ? values.at("channels") : channels;

    std::erase_if(values,
                  [&](const auto& value)
                  {
                      const std::string& name = value.first;
                      if (name == "channels")
                      {
                          return value.second == channels;
                      }
                      if (name.starts_with("channels of"))
                      {
                          return value.second == player_channels;
                      }
                      if (name == "player's parameter " + xsflib::Hex(ntr::snd::kPlayerFaderOffset, 2, true))
                      {
                          return value.second == volume;
                      }
                      if (name == "player's parameter " + xsflib::Hex(ntr::snd::kPlayerPriorityOffset, 2, true))
                      {
                          return value.second == priority;
                      }
                      if (name == "player's parameter " + xsflib::Hex(ntr::snd::kPlayerTempoRatioOffset, 2, true))
                      {
                          return value.second == 256;
                      }
                      return value.second == 0;
                  });

    return values;
}

// The values that a side leaves set (WithoutStartValues) after a list that changes them: on the game's side, the game
// frame of the list and its system clocks from the start; on the profile's, the list's place in its tick commands.
struct ValueStep
{
    SetValues values;
    uint64_t frame = 0;
    uint64_t clocks = 0;
    std::size_t group = 0;
    std::size_t list = 0;
};

// The game's side of `run`: the values of the start, then of each later list that changes them.
std::vector<ValueStep> GameSteps(const MiniRun& run, uint16_t archive_channels)
{
    SetValues game;
    if (run.channels)
    {
        game["channels"] = *run.channels;
    }
    if (run.locked != 0)
    {
        game[std::string(kLockedChannels)] = run.locked;
    }
    Apply(game, run.commands);
    std::vector<ValueStep> steps = {{.values = WithoutStartValues(game, archive_channels, run.volume, run.priority)}};

    const uint64_t frame_clocks = uint64_t{std::max(run.video_frames, 1u)} * ntr::kFrameCycles;
    for (const LaterList& later : run.later)
    {
        Apply(game, later.commands);
        SetValues values = WithoutStartValues(game, archive_channels, run.volume, run.priority);
        if (values != steps.back().values)
        {
            steps.push_back({.values = std::move(values),
                             .frame = (later.clocks + frame_clocks / 2) / frame_clocks,
                             .clocks = later.clocks});
        }
    }

    return steps;
}

// The tick commands of `arrangement`, if any, in the order of their ticks.
std::vector<const ntr::TickCommands*> TickCommandsOf(const ntr::ProfileArrangement* arrangement)
{
    std::vector<const ntr::TickCommands*> lists;
    if (arrangement)
    {
        for (const ntr::TickCommands& commands : arrangement->tick_commands)
        {
            lists.push_back(&commands);
        }
    }

    std::ranges::stable_sort(lists, {}, &ntr::TickCommands::tick);

    return lists;
}

// The profile's side: its channels of the sequence's player, its track channels, its commands, then the arrangement's,
// then each list of the arrangement's tick commands that changes the values.
std::vector<ValueStep> ProfileSteps(const MiniRun& run, const ntr::GameProfile& profile,
                                    const ntr::ProfileArrangement* arrangement, uint16_t archive_channels)
{
    SetValues expected;
    if (profile.settings.channels.size() == 1)
    {
        expected["channels"] = profile.settings.channels.begin()->second;
    }
    for (const ntr::TrackChannels& tracks : profile.settings.track_channels)
    {
        Apply(expected, {{.id = CommandId::kAllocatableChannels, .args = {0, tracks.tracks, tracks.channels, 0}}});
    }
    Apply(expected, profile.commands);
    if (arrangement)
    {
        Apply(expected, arrangement->commands);
    }

    // A lock in the run of channels that no lock of the profile names is the game's state outside the profile, such as
    // the channels that Mario Kart DS locks as it boots. Both sides leave those channels out.
    uint16_t named = 0;
    for (const ntr::ChannelLock& lock : profile.channel_locks)
    {
        named |= lock.channels;
    }
    auto locked = static_cast<uint16_t>(run.locked & ~named);
    for (const ntr::ChannelLock& lock : profile.channel_locks)
    {
        if (lock.frame == 0)
        {
            locked = static_cast<uint16_t>(lock.unlock ? locked & ~lock.channels : locked | lock.channels);
        }
    }
    if (locked != 0)
    {
        expected[std::string(kLockedChannels)] = locked;
    }
    std::vector<ValueStep> steps = {
        {.values = WithoutStartValues(expected, archive_channels, run.volume, run.priority)}};

    const std::vector<const ntr::TickCommands*> lists = TickCommandsOf(arrangement);
    for (std::size_t group = 0; group < lists.size(); group++)
    {
        for (std::size_t list = 0; list < lists[group]->frames.size(); list++)
        {
            Apply(expected, lists[group]->frames[list]);
            SetValues values = WithoutStartValues(expected, archive_channels, run.volume, run.priority);
            if (values != steps.back().values)
            {
                steps.push_back({.values = std::move(values), .group = group, .list = list});
            }
        }
    }

    return steps;
}

// The game's step that starts the sequence (LaterDifferences): the first up to game frame kStartFrames whose values are
// the profile's start, or else the last up to then.
std::size_t StartStep(std::span<const ValueStep> game, const SetValues& start)
{
    std::size_t last = 0;
    for (std::size_t i = 0; i < game.size() && game[i].frame <= kStartFrames; i++)
    {
        if (game[i].values == start)
        {
            return i;
        }
        last = i;
    }

    return last;
}

// A line for each value that `got` and `want` set differently, with `at` before each.
void AddValueDifferences(const SetValues& got, const SetValues& want, const std::string& at,
                         std::vector<std::string>& differences)
{
    for (const auto& [name, value] : got)
    {
        const auto other = want.find(name);
        if (other == want.end())
        {
            differences.push_back(at + "the game sets " + name + " to " + std::to_string(value) +
                                  ", the profile doesn't");
        }
        else if (other->second != value)
        {
            differences.push_back(at + "the game sets " + name + " to " + std::to_string(value) + ", the profile to " +
                                  std::to_string(other->second));
        }
    }
    for (const auto& [name, value] : want)
    {
        if (!got.contains(name))
        {
            differences.push_back(at + "the profile sets " + name + " to " + std::to_string(value) +
                                  ", the game doesn't");
        }
    }
}

// A lock or an unlock of channels after the start, in a game frame.
struct TimedLock
{
    friend bool operator==(const TimedLock&, const TimedLock&) = default;

    uint64_t frame = 0;
    uint32_t channels = 0;
    bool unlock = false;
};

// Adds a line to `differences` where the run's later locks and unlocks of channels differ from the profile's after
// frame 0, in the order of their frames. The profile's frame of the driver becomes the nearest game frame.
void AddLockDifferences(const MiniRun& run, const ntr::GameProfile& profile, std::vector<std::string>& differences)
{
    const uint64_t frame_clocks = uint64_t{std::max(run.video_frames, 1u)} * ntr::kFrameCycles;
    const auto nearest = [frame_clocks](uint64_t clocks)
    {
        return (clocks + frame_clocks / 2) / frame_clocks;
    };
    std::vector<TimedLock> got;
    for (const LaterList& later : run.later)
    {
        for (const ntr::snd::Command& command : later.commands)
        {
            if (command.id == CommandId::kLockChannels || command.id == CommandId::kUnlockChannels)
            {
                got.push_back({.frame = nearest(later.clocks),
                               .channels = command.args[0],
                               .unlock = command.id == CommandId::kUnlockChannels});
            }
        }
    }

    std::vector<ntr::ChannelLock> locks = profile.channel_locks;
    std::ranges::stable_sort(locks, {}, &ntr::ChannelLock::frame);
    std::vector<TimedLock> want;
    for (const ntr::ChannelLock& lock : locks)
    {
        if (lock.frame != 0)
        {
            want.push_back({.frame = nearest(lock.frame * ntr::snd::kFrameClocks),
                            .channels = lock.channels,
                            .unlock = lock.unlock});
        }
    }

    const auto describe = [](const std::vector<TimedLock>& side)
    {
        std::string text;
        for (const TimedLock& lock : side)
        {
            text += (text.empty() ? "" : ", ") + std::string(lock.unlock ? "an unlock of " : "a lock of ") +
                    xsflib::Hex(lock.channels, 4, true) + " in game frame " + std::to_string(lock.frame);
        }
        return text.empty() ? std::string("none") : text;
    };
    if (got != want)
    {
        differences.push_back("the game's later locks of channels are " + describe(got) + ", the profile's " +
                              describe(want));
    }
}

} // namespace

std::vector<std::string> LaterDifferences(const MiniRun& run, const ntr::GameProfile& profile,
                                          const ntr::ProfileArrangement* arrangement, uint16_t archive_channels)
{
    const std::vector<ValueStep> game = GameSteps(run, archive_channels);
    const std::vector<ValueStep> want = ProfileSteps(run, profile, arrangement, archive_channels);
    const std::vector<const ntr::TickCommands*> lists = TickCommandsOf(arrangement);
    std::vector<std::string> differences;
    AddLockDifferences(run, profile, differences);
    if (run.later.empty() && lists.empty())
    {
        return differences;
    }

    const std::size_t start = StartStep(game, want.front().values);
    const std::size_t got_changes = game.size() - start - 1;
    const std::size_t want_changes = want.size() - 1;
    if (got_changes != want_changes)
    {
        differences.push_back("the game's later lists change the values " + std::to_string(got_changes) +
                              " times, the profile's " + std::to_string(want_changes) + " times");
    }

    // The game frame of each list's first change.
    std::map<std::size_t, std::pair<uint64_t, std::size_t>> first_change;
    for (std::size_t i = 1; i <= std::min(got_changes, want_changes); i++)
    {
        const ValueStep& got = game[start + i];
        const ValueStep& step = want[i];
        const ntr::TickCommands& commands = *lists[step.group];
        const std::string at = "in game frame " + std::to_string(got.frame) + ", list " + std::to_string(step.list) +
                               " at tick " + std::to_string(commands.tick) + ": ";
        if (got.values != step.values)
        {
            AddValueDifferences(got.values, step.values, at, differences);
            break;
        }

        const auto [first, inserted] = first_change.try_emplace(step.group, got.frame, step.list);
        const uint64_t frame =
            commands.tick == 0 ? step.list + 1 : first->second.first + (step.list - first->second.second);
        if (got.frame != frame)
        {
            differences.push_back(at + "the profile's change comes in game frame " + std::to_string(frame));
            break;
        }
    }

    const auto settle = static_cast<uint64_t>(kMiniPlaySeconds * ntr::kSystemClockHz);
    if (game.back().clocks + settle > run.played_clocks)
    {
        differences.push_back("the game changes the values less than " +
                              std::to_string(static_cast<int>(kMiniPlaySeconds)) + " s before the run's end");
    }

    return differences;
}

std::vector<ntr::TickCommands> LaterTickCommands(const MiniRun& run)
{
    std::vector<ntr::TickCommands> lists;
    if (run.later.empty())
    {
        return lists;
    }

    // The lists up to the last change of a value or of the locked channels: those after it change nothing.
    const uint64_t frame_clocks = uint64_t{std::max(run.video_frames, 1u)} * ntr::kFrameCycles;
    uint64_t last_change = GameSteps(run, 0).back().clocks;
    for (const LaterList& later : run.later)
    {
        const auto locks = [](const ntr::snd::Command& command)
        {
            return command.id == CommandId::kLockChannels || command.id == CommandId::kUnlockChannels;
        };
        if (std::ranges::any_of(later.commands, locks))
        {
            last_change = std::max(last_change, later.clocks);
        }
    }

    ntr::TickCommands commands = {.tick = 0, .video_frames = std::max(run.video_frames, 1u)};
    for (const LaterList& later : run.later)
    {
        if (later.clocks > last_change)
        {
            break;
        }

        const uint64_t frame = (later.clocks + frame_clocks / 2) / frame_clocks;
        if (frame == 0)
        {
            throw std::logic_error("a later list in the game frame of the start");
        }

        commands.frames.resize(std::max<std::size_t>(commands.frames.size(), frame));
        ntr::snd::CommandList& list = commands.frames[frame - 1];
        list.insert(list.end(), later.commands.begin(), later.commands.end());
    }
    lists.push_back(std::move(commands));

    return lists;
}

std::vector<std::string> ProfileDifferences(const MiniRun& run, const ntr::GameProfile& profile,
                                            const ntr::ProfileArrangement* arrangement, uint16_t archive_channels)
{
    // The game's side: the channels, then the commands around the start, with the later lists of the start's game
    // frames for a run with later lists (LaterDifferences). The profile's side: its start.
    const std::vector<ValueStep> game_steps = GameSteps(run, archive_channels);
    const SetValues want = ProfileSteps(run, profile, arrangement, archive_channels).front().values;
    const SetValues& got = game_steps[StartStep(game_steps, want)].values;
    std::vector<std::string> differences;
    AddValueDifferences(got, want, "", differences);

    // The player's variables: the profile's, then the arrangement's. A variable that a side leaves alone keeps the
    // archive's -1.
    std::array<std::optional<int16_t>, 16> local = profile.settings.variables.local;
    if (arrangement)
    {
        for (const auto& [variable, value] : arrangement->variables)
        {
            if (variable < local.size())
            {
                local[variable] = value;
            }
        }
    }

    for (std::size_t variable = 0; variable < local.size(); variable++)
    {
        constexpr int16_t kUnset = -1;
        const int16_t got_value = run.variables.local[variable].value_or(kUnset);
        const int16_t want_value = local[variable].value_or(kUnset);
        if (got_value != want_value)
        {
            differences.push_back("the game sets variable " + std::to_string(variable) + " to " +
                                  std::to_string(got_value) + ", the profile to " + std::to_string(want_value));
        }
    }

    const bool effect = profile.settings.reverb || profile.settings.effect;
    if (run.captures_run != effect)
    {
        differences.push_back(run.captures_run ? "the game runs the capture units, and the profile has no reverb"
                                               : "the profile has a reverb or an output effect, and the game runs no "
                                                 "capture unit");
    }

    return differences;
}

} // namespace twosf::rip
