// SPDX-License-Identifier: MIT

#include "rip/dse_set.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/arm/assembler.h"
#include "ntdlib/arm/cpu.h"
#include "ntdlib/dse/game_starts.h"
#include "ntdlib/ntr/code.h"
#include "ntdlib/ntr/compact_image.h"
#include "ntdlib/ntr/machine.h"
#include "ntdlib/ntr/main_memory.h"
#include "ntdlib/ntr/sound.h"
#include "xsflib/bytes.h"
#include "xsflib/text.h"
#include <zlib.h>

#include "rip/arm_code.h"
#include "rip/boots.h"
#include "rip/sadl.h"

namespace twosf::rip
{

namespace
{

namespace arm = ntdlib::arm;
namespace ntr = ntdlib::ntr;

// The stub's sleep between its checks of nothing: a second.
constexpr uint32_t kSleepMs = 1000;

// Where the table of members of a pack of kMembers starts.
constexpr uint32_t kMembersTable = 0x20;

// The games' routines. The addresses come from the games' ARM9 binaries (North America). Explorers of Darkness contains
// Time's code at the same addresses.
//
// The hook follows the set-up of the handler of the vertical blank in the game's start-up function (Sky 0x020033A0 to
// 0x020033BC: OS_SetIrqFunction, OS_EnableIrqMask, the IME and one more call). After the hook, the function starts the
// thread of the game's tasks (Sky 0x020033C0 to 0x020033E4). The game's start of a song waits for the frames that the
// thread of the game's frames counts (Sky 0x02003AB0), and that thread starts before the hook (Sky 0x02003358 to
// 0x02003378). The thread of the game's tasks stops the song once it runs (overlay 34's code in Sky, at 0x022DC274).
// The hook's function ends with its literal pool. The function's first load from the pool gives the pool's address.
//
// The thread of the game's frames (Sky 0x02003620, Time 0x02003438) sleeps until the frame's alarm wakes it
// (OS_SleepThread), counts the frame, runs the game's work for it, loads the frame's message queue, signals it
// (0x020029B8 in Sky) and runs the sound's work for the frame (0x0201849C in Sky): when a short piece ends, the game's
// sound resumes the music.
//
// The sound thread's start of music (PlayBgm, Sky 0x020198B8, Time 0x0205E3C0) refuses a number below 1 (Sky
// 0x02019940, Time 0x0205E448) or above the last (Sky 0x02019948, Time 0x0205E450). The game's start of music by number
// (PlayBgmById) queues 0 like any other number. Each game has a song 0 (bgm0000, B_ENV_BEACH_01) that no routine of the
// game starts. The set changes the first compare to `cmp r6, #0`, whose branch is then never taken, and its mini plays
// song 0 through the game's code. For every other number, the code runs as in the game.
//
// The games' start of music at a level (PlayBgmByIdVolume, Sky 0x02017E70, Time 0x02012490) takes the number, a fade in
// frames and a level, and queues them for the sound thread as PlayBgmById does with level 256 and no fade (Sky
// 0x02017E1C to 0x02017E5C against 0x02017EA0 to 0x02017ED4). The sound thread's PlayBgm starts DSE with the volume
// level × 127 >> 8 (Sky 0x02019AB8 to 0x02019AC0, Time 0x0205E588 to 0x0205E59C). The scripts start music through it
// (bgm_PlayFadeIn: Sky's overlay 11 at 0x022E2118, Time's overlay 4 at 0x022D9A3C), some pieces below full level. The
// stub starts every piece with it, with no fade. Like PlayBgmById, it returns at once for the number of the piece that
// plays (Sky 0x02017E7C to 0x02017E94). No piece plays at the hook.
constexpr DseCode kExplorersOfSky = {
    .game_code = "C2SE",
    .hook = 0x020033C0,
    .hook_end = 0x020035CC,
    .frame_cut = 0x0200365C,
    .frame_signal = 0x020036EC,
    .play_bgm = 0x02017DF4,
    .play_bgm_level = 0x02017E70,
    .play_me = 0x0201820C,
    .sleep = 0x02079B14,
    .load_bank = 0x0206D108,
    .load_song = 0x0206D808,
    .start_song = 0x0206D9BC,
    .stop_song = 0x0206DA9C,
    .volume_in_parameters = true,
    .bgm_first = 0x02019940,
    .bgm_limit = 0x02019948,
    .me_limit = 0x02018220,
    .bgm_format = "SOUND/BGM/bgm%04u.smd",
    .me_format = "SOUND/ME/me%04u.smd",
};

// Time's calls of DSE are the routines inside the game's wrappers. Those routines take Sky's arguments, apart from the
// start of a song: Sky's takes a pointer to the start's parameters in r1, with the volume at 0xC, and Time's takes the
// volume in r1 and its time in r2.
constexpr DseCode kExplorersOfTime = {
    .game_code = "YFTE",
    .hook = 0x020031D0,
    .hook_end = 0x020033E0,
    .frame_cut = 0x02003474,
    .frame_signal = 0x02003500,
    .play_bgm = 0x02012414,
    .play_bgm_level = 0x02012490,
    .play_me = 0x020128A8,
    .sleep = 0x0206B824,
    .load_bank = 0x020601AC,
    .load_song = 0x02060940,
    .start_song = 0x02060B88,
    .stop_song = 0x02060D1C,
    .bgm_first = 0x0205E448,
    .bgm_limit = 0x0205E450,
    .me_limit = 0x020128BC,
    .bgm_format = "SOUND/BGM/bgm%04u.smd",
    .me_format = "SOUND/ME/me%04u.smd",
};

constexpr DseCode kExplorersOfDarkness = []
{
    DseCode code = kExplorersOfTime;
    code.game_code = "YFYE";
    return code;
}();

// The Inazuma Eleven games' streams. The player's start of music by name plays a stream for a name with the extension
// .SAD (Inazuma Eleven 2's 0x0202C13C) and a song for any other. The names that the games' code and scripts give it are
// the title's music TITLE.SAD (Inazuma Eleven 2's table of names by number at 0x020A6E0C, entry 6, and overlay 3), the
// ending's music (bg_end.SAD, overlay 5; Inazuma Eleven 3's end00b.SAD and end00f.SAD, overlay 6) and the jingles
// J01.SAD to J21.SAD (the same table, the overlays and the scripts in data_iz/script). The movie player loads NAME.sad
// beside each data_iz/movie/NAME.mods (0x0202E7DC): the other streams of data_iz/sound are the movies' sound and the
// voices of the events, V....SAD, or in Inazuma Eleven NN_N.SAD.
constexpr DseStreamKind kInazumaStreams[] = {
    {.format = "data_iz/sound/TITLE"},
    {.format = "data_iz/sound/bg_end"},
    {.format = "data_iz/sound/J"},
};

constexpr DseStreamKind kInazuma3Streams[] = {
    {.format = "data_iz/sound/TITLE"},
    {.format = "data_iz/sound/end00"},
    {.format = "data_iz/sound/J"},
};

// Inazuma Eleven 2: Kyoui no Shinryakusha: Fire, Japan, revision 1. The game's start-up (0x02029E40) starts the sound
// (0x02029D78: 0x0202B6E4 with the game's sound player at 0x020B4840), loads the system's bank and effects and waits
// for them, then starts its main loop (0x0202B30C, from 0x02029E80). The main loop waits for each frame
// (OS_WaitVBlankIntr, 0x020038BC) and runs the player's work for the frame (0x0202B95C, from 0x0202B438).
//
// The player's start of music by name (0x0202C344) takes a slot for music, a name, the milliseconds of the fade-out of
// the piece that plays and, on the stack, those of the fade-in. 0x0202C4D8 calls it with no fade-out. The game's scenes
// start music through the same routine (0x0202C344): by the name in a slot (0x0202C454, from overlay 1 at 0x020F0480)
// or by a number in the table at 0x020A6E0C (0x0202C324), mostly with a fade-out of 2000 ms and a fade-in of 1000 ms. A
// name with the extension .SAD is a stream, and any other name a song. The player's work for the next frame
// (0x0202C694) sets the slot's song (0x0202D1C4) and its start (0x0202D30C). The work for the frames after that
// (0x0202D404) reads the song's bank and song, NAME.SWD and NAME.SMD, from the pack data_iz/sound/sound.pb by the index
// sound.ph (0x0202CC48), loads them into DSE (0x0208C614, 0x0208CD20) and starts the song (0x0208CE74, from
// 0x0202D68C). The volume of the start comes from the player's levels (0x0202B7F0): 126. The hook replaces the main
// loop.
constexpr DseCode kInazumaEleven2 = {
    .game_code = "BEEJ",
    .revision = 1,
    .hook = 0x0202B30C,
    .hook_end = 0x0202B46C,
    .start = DseStartKind::kName,
    .play_bgm = 0x0202C344,
    .sleep = 0x02001E78,
    .load_bank = 0x0208C614,
    .load_song = 0x0208CD20,
    .start_song = 0x0208CE74,
    .stop_song = 0x0208CF54,
    .volume_in_parameters = true,
    .player = 0x020B4840,
    .frame_wait = 0x020038BC,
    .frame_update = 0x0202B95C,
    .pack = "data_iz/sound/sound.pb",
    .pack_index = "data_iz/sound/sound.ph",
    .streams = kInazumaStreams,
    .stream_build = SadlBuild::kLater,
};

// Inazuma Eleven, Japan, revision 1: Inazuma Eleven 2's player at other addresses. The start-up (0x02038690) starts the
// sound (0x020385C0) and the main loop (0x02039920, from 0x020386D0). The main loop waits for each frame (0x0200397C)
// and runs the player's work (0x02039ECC on the player at 0x020C9068, from 0x02039A00). The start of music by name is
// 0x0203AA20. The songs and their banks are files of their own in data_iz/sound/.
constexpr DseCode kInazumaEleven = {
    .game_code = "YEEJ",
    .revision = 1,
    .hook = 0x02039920,
    .hook_end = 0x02039A34,
    .start = DseStartKind::kName,
    .play_bgm = 0x0203AA20,
    .sleep = 0x02001F38,
    .load_bank = 0x02020334,
    .load_song = 0x020209DC,
    .start_song = 0x02020C54,
    .stop_song = 0x02020D34,
    .volume_in_parameters = true,
    .player = 0x020C9068,
    .frame_wait = 0x0200397C,
    .frame_update = 0x02039ECC,
    .streams = kInazumaStreams,
};

// Inazuma Eleven 3: Sekai e no Chousen!! Bomber, Japan: the player of Inazuma Eleven 2 in a later form. The start-up
// (0x0202B51C) starts the sound (0x0202B454) and the main loop (0x0202C8D4, from 0x0202B55C). The main loop waits for
// each frame (0x02003E40) and runs the player's work (0x0202D084 on the player at 0x0209D440, from 0x0202CA74). The
// start of music by name is 0x0202DAD4. The player reads a song with its bank as one entry, NAME.SMW (0x0202EF94), of
// data_iz/sound/sound.pb, by the index sound.ph: a tree of the bits of the key's CRC-32 (0x020880BC).
constexpr DseCode kInazumaEleven3Bomber = {
    .game_code = "BEZJ",
    .hook = 0x0202C8D4,
    .hook_end = 0x0202CA84,
    .start = DseStartKind::kName,
    .play_bgm = 0x0202DAD4,
    .sleep = 0x02001EB0,
    .load_bank = 0x02077FF8,
    .load_song = 0x0207871C,
    .start_song = 0x02078884,
    .stop_song = 0x0207896C,
    .volume_in_parameters = true,
    .player = 0x0209D440,
    .frame_wait = 0x02003E40,
    .frame_update = 0x0202D084,
    .pack = "data_iz/sound/sound.pb",
    .pack_index = "data_iz/sound/sound.ph",
    .index_kind = DsePackIndex::kCrcTree,
    .song_before_bank = true,
    .streams = kInazuma3Streams,
    .stream_build = SadlBuild::kLater,
};

// Inazuma Eleven 3: Sekai e no Chousen!! The Ogre, Japan: Bomber's player at other addresses. The start-up (0x02029C08)
// starts the sound (0x02029B40) and the main loop (0x0202AFD0, from 0x02029C54). The main loop waits for each frame
// (0x02003E40) and runs the player's work (0x0202B768 on the player at 0x0209AC44, from 0x0202B180). The start of music
// by name is 0x0202C1A4. It takes the name with its extension, NAME.SMW, and finds its number in the index
// (0x02086640). The index data_iz/sound/sound.pkh of the pack sound.pkb is a table in the order of the keys' CRC-32.
// Both games load a song before its bank (Bomber 0x0202E694 and 0x0202E6B4, The Ogre 0x0202CD10 and 0x0202CD30).
constexpr DseCode kInazumaEleven3Ogre = {
    .game_code = "BOEJ",
    .hook = 0x0202AFD0,
    .hook_end = 0x0202B190,
    .start = DseStartKind::kName,
    .play_bgm = 0x0202C1A4,
    .sleep = 0x02001EB0,
    .load_bank = 0x020758C4,
    .load_song = 0x02075FE8,
    .start_song = 0x02076150,
    .stop_song = 0x02076238,
    .volume_in_parameters = true,
    .player = 0x0209AC44,
    .frame_wait = 0x02003E40,
    .frame_update = 0x0202B768,
    .pack = "data_iz/sound/sound.pkb",
    .pack_index = "data_iz/sound/sound.pkh",
    .index_kind = DsePackIndex::kCrcTable,
    .name_suffix = ".SMW",
    .song_before_bank = true,
    .streams = kInazuma3Streams,
    .stream_build = SadlBuild::kLater,
};

// Professor Layton and the Diabolical Box, North America. The main function (0x02000D98) calls the engine's main
// (overlay 2's 0x02068C38), whose first call (0x02068B2C) starts the engine's parts: the sound (0x0207F254, with the
// sound object at 0x02088A9C) and its players of SY, GE, SI and BG. The BG player is at 0x100 in the sound object. The
// hook follows that call. The scripts start music through the sound object's PlayBG (0x0207F974: 0x02068F1C,
// 0x0206925C, 0x020693AC). It adds 0x100 to the object and jumps to the BG player's PlayBG (0x0207DC50): the number,
// the level 1.0 and the fade-in 0. PlayBG loads the bank, its waves and the song in the caller's thread through DSE's
// wrappers (0x0207DF54: 0x02002378, 0x020023C8, 0x020023E4, 0x02002DAC) and starts the song at a volume of 127 times
// the level (0x0207E1A4). The stub makes that call, then sleeps.
constexpr uint32_t kLayton2Overlays[] = {2};

// Layton 2's streams. The sound object's stream manager (0x02088A9C) takes a request (0x0207FA54) of a kind, a number,
// -1, and on the stack the level 1.0 and two words of 0. Its stream object at 0x74 builds the path by the kind
// (0x0207EA30): 0 ST_%03d below 100 and ST_%04d from 100 on, 1 bgm/BG_%03d, 2 event/%03d_%d, 3 movie/M%d. It opens the
// file in the caller's thread, and the stream player's thread decodes it. The music is kind 1. ST below 100 are short
// pieces that the scenes start (0x02081240 from a scene's table, overlay 2's 0x0206B970); from 100 on they are the
// characters' voices. The events' and the movies' streams are voices and the movies' sound.
constexpr uint32_t kLayton2StreamStack[] = {0x3F800000, 0, 0};

constexpr DseStreamKind kLayton2Streams[] = {
    {.format = "data_lt2/stream/bgm/BG_%03u.SAD",
     .calls = {{{.routine = 0x0207FA54,
                 .arguments = {0x02088A9C, 1, kDseSongNumber, 0xFFFFFFFF},
                 .literals = 0b1000,
                 .stack = kLayton2StreamStack}}}},
    {.format = "data_lt2/stream/ST_%03u.SAD",
     .last = 99,
     .calls = {{{.routine = 0x0207FA54,
                 .arguments = {0x02088A9C, 0, kDseSongNumber, 0xFFFFFFFF},
                 .literals = 0b1000,
                 .stack = kLayton2StreamStack}}}},
};

constexpr DseCode kLayton2 = {
    .game_code = "YLTE",
    .overlays = kLayton2Overlays,
    .hook = 0x02068C44,
    .hook_end = 0x02068D44,
    .start = DseStartKind::kCalls,
    .sleep = 0x0201E4F0,
    .load_bank = 0x020026B4,
    .load_song = 0x02002E68,
    .start_song = 0x020030B0,
    .stop_song = 0x0200328C,
    .bgm_format = "data_lt2/sound/BG_%03u.SMD",
    .calls = {{{.routine = 0x0207DC50, .arguments = {0x02088B9C, kDseSongNumber, 0x3F800000, 0}}}},
    .streams = kLayton2Streams,
};

// Professor Layton and the Unwound Future, North America. Overlay 6's main loop registers the scenes BOOT and GAME
// (0x020EAE4C) and starts the scene BOOT, whose code is in overlay 11. The set-up of BOOT (0x0211F730) starts the file
// loader at 0x02072354 (its slots at 0x02061C74, its thread at 0x02061CB4) and the file manager at 0x02072488
// (0x020632C4, 0x020632EC, 0x0206330C). It then checks the backup memory from 0x0211F8E4 on. The hook replaces those
// checks. Overlay 0 contains the engine and the BG player, and overlay 1 contains DSE. A script's PlayBG (0x020D01A0)
// starts music in two calls. The first, 0x020D04FC, takes the number and the fade-out of the piece that plays. It has
// the BG player stop that piece (0x020602A4) and load snd/BG_NNN.csnd (0x02060264). The second, 0x0206027C, takes the
// level 1.0, the fade-in and the script's flags, and has the BG player play the song (0x0205EDC4). The loader and the
// file manager read the file in steps that a scene's work for a frame runs (0x020616C8 and 0x02063198, from 0x020F3848
// and 0x020F38F4). The BG player's work for a frame (0x0206008C) steps the BG player (0x0205F0EC). That work loads the
// bank, its waves and the song, and starts the song (0x0205F240). The stub makes both calls with no fades and flags 0.
// For each frame, it then waits (OS_WaitVBlankIntr, 0x0200BABC) and runs the three steps.
constexpr uint32_t kLayton3Overlays[] = {0, 1, 6, 11};

// Layton 3's streams. The stream player (0x020721C8) loads a stream by its path (0x02060344, from 0x0205F590) and plays
// it (0x02060364: the level 1.0, the fade-in and flags). The BG player's work for a frame (0x0206008C) steps it too
// (0x0205F9FC). 0x020D0618 loads str/bgm/BG_NNN.SAD, the music: the jukebox of overlay 31 (its table at 0x0211E54C, a
// kind and a number for each piece) starts a piece of kind 1 with it and one of kind 0 with the song's start.
// 0x020D0670 loads str/nrm/NNN.SAD, or from 110 to 150 the language's str/nrm/?/NNN.SAD, the characters' voices: the
// others are short pieces of music. The movies' and the events' streams (0x020D06EC, 0x020D0744) are their sound and
// the voices.
constexpr uint32_t kLayton3StreamPlay = 0x02060364;

constexpr DseStreamKind kLayton3Streams[] = {
    {.format = "lt3/str/bgm/BG_%03u.SAD",
     .calls = {{{.routine = 0x020D0618, .arguments = {kDseSongNumber, 0, 0, 0}},
                {.routine = kLayton3StreamPlay, .arguments = {0x3F800000, 0, 0, 0}}}}},
    {.format = "lt3/str/nrm/%03u.SAD",
     .last = 109,
     .calls = {{{.routine = 0x020D0670, .arguments = {kDseSongNumber, 0, 0, 0}},
                {.routine = kLayton3StreamPlay, .arguments = {0x3F800000, 0, 0, 0}}}}},
    {.format = "lt3/str/nrm/%03u.SAD",
     .first = 151,
     .calls = {{{.routine = 0x020D0670, .arguments = {kDseSongNumber, 0, 0, 0}},
                {.routine = kLayton3StreamPlay, .arguments = {0x3F800000, 0, 0, 0}}}}},
};

constexpr DseCode kLayton3 = {
    .game_code = "C3JE",
    .overlays = kLayton3Overlays,
    .hook = 0x0211F8E4,
    .hook_end = 0x0211F9D0,
    .start = DseStartKind::kCalls,
    .sleep = 0x0200A148,
    .load_bank = 0x020737E8,
    .load_song = 0x02073F00,
    .start_song = 0x02074064,
    .stop_song = 0x02074144,
    .volume_in_parameters = true,
    .bgm_format = "lt3/snd/BG_%03u.csnd",
    .frame_wait = 0x0200BABC,
    .calls = {{{.routine = 0x020D04FC, .arguments = {kDseSongNumber, 0, 0, 0}},
               {.routine = 0x0206027C, .arguments = {0x3F800000, 0, 0, 0}}}},
    .frame_calls = {{{.routine = 0x020616C8, .arguments = {0x02072354, 0, 0, 0}},
                     {.routine = 0x02063198, .arguments = {0x02072488, 0, 0, 0}},
                     {.routine = 0x0206008C}}},
    .streams = kLayton3Streams,
};

// Professor Layton and the Last Specter, North America. Overlay 8's boot (0x020CCD20) calls overlay 9's start of the
// engine (0x02105130, at 0x020CCD54). It starts the engine's parts (overlay 8's 0x020DCE54, at 0x02105290), among them
// the sound part at 0x02103720 (0x020F0010, at 0x020DCFB4), and the engine's threads, opens the archive lt4_main_en.fa
// and returns. The boot then runs the game's frames (0x020CCD98 on). Overlay 0 contains DSE. The hook follows the call
// of the engine's start. The sound part's PlayBG (0x020F0A84: the part, the number, the fade-out of the piece that
// plays and flags; 0x020F0A6C calls it with 0 and 0) requests the stop of the piece that plays (0x020F0BDC), the load
// of the song (0x020F0AF8) and its play at the level 1.0 and no fade-in (0x020F0B44). A scene's work for a frame runs
// the sound part's work (0x020F0420, from 0x020F8580). That work loads the bank, its waves and the song snd/BG_NNN.cse
// from the archive and starts the song (0x020F0874). The stub makes that call, then waits for each frame
// (OS_WaitVBlankIntr, 0x02018CD0) and runs the sound part's work.
constexpr uint32_t kLayton4Overlays[] = {0, 8};

// Layton 4's streams. The sound part's request of a stream (0x020F0FA4: the part, a kind, a flag, a number, and two
// words on the stack) queues it, and its play (0x020F10DC: the part, the level 1.0, the fade-in and flags) plays it.
// The scripts (overlay 13's 0x0210EDE0) pass the flag 0 and the words -1. The sound part's work for a frame opens the
// stream by the path of its kind (0x020F0E6C): 0 str/jng/%03d.SAD, or from 100 to 138 and from 600 on the language's
// str/jng/@/%03d.SAD, 1 str/bgm/%03d.SAD, 2 the movies' str/mov/@/, 3 the events' str/evt/@/. The music is kind 1. The
// language's jng are the characters' voices, and the others short pieces.
constexpr uint32_t kLayton4StreamWords[] = {0xFFFFFFFF, 0xFFFFFFFF};
constexpr DseCall kLayton4StreamPlay = {.routine = 0x020F10DC, .arguments = {0x02103720, 0x3F800000, 0, 0}};

constexpr DseStreamKind kLayton4Streams[] = {
    {.format = "lt4/str/bgm/%03u.SAD",
     .calls = {{{.routine = 0x020F0FA4, .arguments = {0x02103720, 1, 0, kDseSongNumber}, .stack = kLayton4StreamWords},
                kLayton4StreamPlay}}},
    {.format = "lt4/str/jng/%03u.SAD",
     .last = 99,
     .calls = {{{.routine = 0x020F0FA4, .arguments = {0x02103720, 0, 0, kDseSongNumber}, .stack = kLayton4StreamWords},
                kLayton4StreamPlay}}},
    {.format = "lt4/str/jng/%03u.SAD",
     .first = 139,
     .last = 599,
     .calls = {{{.routine = 0x020F0FA4, .arguments = {0x02103720, 0, 0, kDseSongNumber}, .stack = kLayton4StreamWords},
                kLayton4StreamPlay}}},
};

constexpr DseCode kLayton4 = {
    .game_code = "BLFE",
    .overlays = kLayton4Overlays,
    .hook = 0x020CCD58,
    .hook_end = 0x020CCE74,
    .start = DseStartKind::kCalls,
    .sleep = 0x02016D18,
    .load_bank = 0x0204538C,
    .load_song = 0x02045B18,
    .start_song = 0x02045C6C,
    .stop_song = 0x02045D4C,
    .volume_in_parameters = true,
    .bgm_format = "lt4_main_en.fa/snd/BG_%03u.cse",
    .frame_wait = 0x02018CD0,
    .calls = {{{.routine = 0x020F0A84, .arguments = {0x02103720, kDseSongNumber, 0, 0}}}},
    .frame_calls = {{{.routine = 0x020F0420, .arguments = {0x02103720, 0, 0, 0}}}},
    .streams = kLayton4Streams,
    .stream_build = SadlBuild::kLater,
};

// London Life, in Professor Layton and the Last Specter, North America. The main function (0x02000CA0) reads the
// parameter of the last reset (0x02000CAC, from 0x02FFFC20). With 4 in its low 3 bits, it loads overlays 0, 4, 6 and 48
// (0x02000DA0 to 0x02000DD8) and runs London Life's main (overlay 48's 0x020D9E10, from 0x02000DDC) in place of the
// main game. A set of London Life reads 4 there (mov r0, #4). London Life's main starts its parts (0x020D975C, at
// 0x020D9E6C), among them its sound (0x020BEE60, from 0x020D9762): it opens ll/common/ll_common.darc, takes a heap of
// 0x10000 bytes and starts DSE with a tick of 10 ms (0x020D809C, at 0x020BEEE2). The main then runs London Life's
// scenes (0x020D9C00, at 0x020D9E8C): it makes the scene manager (0x020D8F04), whose thread runs a state machine, and
// waits for its end (0x020D92C4). The handler of the machine's first state (0x020D92EC, registered at 0x020D8F2C) loads
// the sound effects (0x020D8104) and sets up the screens, then goes on to state 5 (0x020D9364). The hook follows its
// set-up (0x020D9360). Right after the start of the parts, the heap of operator new (0x02035E74) has no room for the
// waves of some songs, and the game's load of their waves never returns. The stub goes over the scene manager's
// destructors (0x020D9274 and 0x020D9298, in its virtual table at 0x020E64F8), and the hook jumps there. The
// destructors run only at London Life's end (0x020D9C16). London Life's scenes start music with two calls (overlay 53's
// 0x020EAACA and 0x020EAAD0, overlay 56's 0x020EEAF2 and 0x020EEB00): 0x020D824C(number) loads member number + 11 of
// the archive (0x020D8260), the song, with its bank, member number + 21 (0x020D8268), and the main bank
// /ll/common/bgm_common.SWD (0x020D8298) through 0x020CF46C, and sleeps while DSE loads (0x020D8274 to 0x020D8296);
// 0x020D832C(fade-in) starts the song (0x020CF7B4). The stub makes both calls with no fade-in, then sleeps.
constexpr uint32_t kLayton4LondonLifeOverlays[] = {0, 4, 6, 48};

// London Life's streams: 0x020D8450 (Thumb) loads /ll/common/streamNN.SAD by its number (overlay 61's 0x020F2F60 the
// first, overlay 58's 0x020F31BC the second), and 0x020D8658 plays it with a fade-in (overlay 58's 0x020F37D0). They
// are its two pieces of music beside DSE. The number 0 loads stream00_null.SAD (the name at 0x020E64DC), a short stream
// of faint noise that stands for no stream: the set leaves it out.
constexpr DseStreamKind kLayton4LondonLifeStreams[] = {
    {.format = "ll/common/stream%02u.SAD",
     .last = 99,
     .calls = {{{.routine = 0x020D8451, .arguments = {kDseSongNumber, 0, 0, 0}, .registers = 1},
                {.routine = 0x020D8659, .arguments = {0, 0, 0, 0}, .registers = 1}}}},
};

constexpr GamePatch kLayton4LondonLifePatches[] = {
    {.address = 0x02000CAC, .expected = 0xE5900000, .value = 0xE3A00004}};

constexpr DseCode kLayton4LondonLife = {
    .game_code = "BLFE",
    .overlays = kLayton4LondonLifeOverlays,
    .hook = 0x020D9361,
    .hook_end = 0x020D936C,
    .stub = 0x020D9274,
    .stub_end = 0x020D92C4,
    .start = DseStartKind::kCalls,
    .sleep = 0x02016D18,
    .load_bank = 0x0204538C,
    .load_song = 0x02045B18,
    .start_song = 0x02045C6C,
    .stop_song = 0x02045D4C,
    .volume_in_parameters = true,
    .pack = "ll/common/ll_common.darc",
    .index_kind = DsePackIndex::kDarc,
    .first_member = 11,
    .first_bank_member = 21,
    .main_bank = "ll/common/bgm_common.SWD",
    .patches = kLayton4LondonLifePatches,
    .calls = {{{.routine = 0x020D824D, .arguments = {kDseSongNumber, 0, 0, 0}, .registers = 1},
               {.routine = 0x020D832D, .arguments = {0, 0, 0, 0}, .registers = 1}}},
    .streams = kLayton4LondonLifeStreams,
    .stream_build = SadlBuild::kLater,
};

// Fushigi no Dungeon: Fuurai no Shiren DS 2, Japan. The game's main (0x0208BE98) first calls the start-up (0x02084A14).
// The start-up takes the sound's area from the OS's heap and starts the sound (0x0212DF60, at 0x02084C4C): DSE's
// start-up, its thread and its alarm. The main then starts the game's parts and runs the game (0x02119944). The hook
// follows the call of the start-up. The game's request of music (0x0212F3B8: the number and the level) does nothing for
// the song that plays. It reads the names of the song and of its bank from the table at 0x0216C570 (0x0212F4D8), and
// loads mid/SND_BGM_M_NAME.SMD (0x0212EC8C), or mid/SND_ME_M_NAME.SMD for an entry whose byte 8 is 1 (0x0212ED14), with
// BGM/BANK.SWD into the sound's area (0x0212E2AC), the song first. A name that starts with '=' gives the rest of the
// file's name (0x0212ECA4). It then starts the song (0x0212EDA0) at the volume level × 127 / 256, or 127 for the level
// -1, in the caller's thread. The scripts start music through it at a level (ntdlib/dse/game_starts.h), and number 0
// stops the music. The stub makes the request with the level of the song's state, then sleeps.
constexpr DseCode kShirenDs2 = {
    .game_code = "YWFJ",
    .hook = 0x0208BEA0,
    .hook_end = 0x0208BEF0,
    .start = DseStartKind::kCalls,
    .sleep = 0x02004E5C,
    .load_bank = 0x02126F44,
    .load_song = 0x02127600,
    .start_song = 0x021278A0,
    .stop_song = 0x02127A7C,
    .bgm_format = "mid/SND_BGM_M_%s.SMD",
    .me_format = "mid/SND_ME_M_%s.SMD",
    .song_table = 0x0216C570,
    .song_count = 49,
    .marked_format = "mid/%s.SMD",
    .song_before_bank = true,
    .calls = {{{.routine = 0x0212F3B8, .arguments = {kDseSongNumber, kDseSongLevel, 0, 0}}}},
};

// Fushigi no Dungeon: Fuurai no Shiren 4, Japan: Thumb code, apart from DSE's. NitroMain (0x02000C6C) calls the
// start-up (0x020091A0), then the game's run (0x020092F4), then the shutdown (0x0200927C). The start-up starts the
// sound object at [0x020FEBEC] (0x0200C1DC). Only the shutdown runs the sound object's shutdown (0x0200C428). The hook
// is the run's start, and the stub goes over the sound object's shutdown. The routine after the run (0x02009308) runs
// in the boot: a stub there stops the boot. The game's request of music (0x0200CA98: the sound object, the number, 127
// and 1) reads entry N of sound/bgm.bin by the offsets in sound/bgm.ent, loads its bank and its song (0x0200BE38) and
// starts the song (0x0200BDF8), in the caller's thread. The volume is the song's level in the table at 0x020C535C times
// the option's level (0x0200D850, 0x0200D07C). ntdlib::dse::GameStarts reads the table from the image. The request of a
// short piece (0x0200CD34: the object, 0x401 + N, 127, 0x40, and two words of 0 on the stack) queues entry N of
// sound/me_bgm.bin, and the sound object's work for a frame (0x0200D6B0, its third virtual routine) loads and starts
// it. The stub makes the request, then for each frame waits (OS_WaitVBlankIntr, 0x02089B20) and runs that work.
constexpr DseCode kShiren4 = {
    .game_code = "B4FJ",
    .hook = 0x020092F5,
    .hook_end = 0x02009304,
    .stub = 0x0200C428,
    .stub_end = 0x0200C508,
    .start = DseStartKind::kCalls,
    .sleep = 0x020888DD,
    .load_bank = 0x0207F738,
    .load_song = 0x0207FDD0,
    .start_song = 0x02080068,
    .stop_song = 0x02080148,
    .volume_in_parameters = true,
    .frame_wait = 0x02089B21,
    .pack = "sound/bgm.bin",
    .pack_index = "sound/bgm.ent",
    .index_kind = DsePackIndex::kOffsets,
    .me_pack = "sound/me_bgm.bin",
    .me_pack_index = "sound/me_bgm.ent",
    .calls = {{{.routine = 0x0200CA99, .arguments = {0x020FEBEC, kDseSongNumber, 127, 1}, .loads = 1}}},
    .frame_calls = {{{.routine = 0x0200D6B1, .arguments = {0x020FEBEC, 0, 0, 0}, .registers = 1, .loads = 1}}},
    .me_calls = {{{.routine = 0x0200CD35,
                   .arguments = {0x020FEBEC, kDseSongNumber, 127, 0x40},
                   .loads = 1,
                   .number_base = 0x401,
                   .stack_zeros = 2}}},
};

// Fushigi no Dungeon: Fuurai no Shiren 5, Japan: Shiren 4's structure, all in Thumb code. NitroMain (0x02000C60) calls
// the start-up (0x02009D10), then the run (0x02009E6C), then the shutdown (0x02009DF4). The start-up starts the sound
// object at [0x021242DC] (0x0200D458). Only the shutdown runs the sound object's shutdown (0x0200D688). The stub goes
// there, and over the sound's pause for a closed lid after it (0x0200D73C). Only the handler of the lid (0x02009E80)
// and overlay 13 (0x0215182A) call the pause, and the handler calls it only once the hinge's bit (bit 15 of the keys'
// word at 0x02FFFFA8) says that the lid is closed. A set plays with the lid open. The request of music is 0x0200DE60,
// with the songs' levels in the table at 0x020E032C (0x0200EF3C), and the request of a short piece 0x0200E134. The
// object's work for a frame is its fifth virtual routine (0x0200EDD8), and OS_WaitVBlankIntr is 0x0209B2D0. The game's
// change of the music's volume (0x0200E648: the object, the volume and the frames) makes its target from the song's
// levels as the request does and sets the object's fader (0x0200D264). The work for each frame moves the fader
// (0x0200D280). The fader passes the volume to DSE's change of the song's parameters (0x02091600).
constexpr DseCode kShiren5 = {
    .game_code = "B5FJ",
    .hook = 0x02009E6D,
    .hook_end = 0x02009E7C,
    .stub = 0x0200D688,
    .stub_end = 0x0200D780,
    .start = DseStartKind::kCalls,
    .sleep = 0x02099C61,
    .load_bank = 0x02090E69,
    .load_song = 0x0209133D,
    .start_song = 0x02091501,
    .stop_song = 0x0209159D,
    .volume_in_parameters = true,
    .set_song = 0x02091601,
    .frame_wait = 0x0209B2D1,
    .pack = "sound/bgm.bin",
    .pack_index = "sound/bgm.ent",
    .index_kind = DsePackIndex::kOffsets,
    .me_pack = "sound/me_bgm.bin",
    .me_pack_index = "sound/me_bgm.ent",
    .calls = {{{.routine = 0x0200DE61, .arguments = {0x021242DC, kDseSongNumber, 127, 1}, .loads = 1}}},
    .frame_calls = {{{.routine = 0x0200EDD9, .arguments = {0x021242DC, 0, 0, 0}, .registers = 1, .loads = 1}}},
    .me_calls = {{{.routine = 0x0200E135,
                   .arguments = {0x021242DC, kDseSongNumber, 127, 0x40},
                   .loads = 1,
                   .number_base = 0x401,
                   .stack_zeros = 2}}},
    .change_calls = {{{.routine = 0x0200E649,
                       .arguments = {0x021242DC, kDseChangeVolume, kDseChangeFrames, 0},
                       .registers = 3,
                       .loads = 1}}},
};

// Luminous Arc 2, North America. NitroMain (0x02000C80) calls the start-up (0x02024CF4). The start-up starts the sound
// (0x02024FE8 at 0x02024D58: DSE's start-up, its thread and its alarm) and the handler of the vertical blank. NitroMain
// then reads the buttons (0x0202453C) and runs the game's frame (0x020242E0) in a loop (0x02000CC0). The hook replaces
// that loop, and the stub goes over the frame's routine. A set never runs that routine. The game's sound requests
// (0x0206C588: the request, the number, the volume, a second volume and the fade in frames on the stack) start music
// with request 0, at a volume of 127 and no fade (0x02039A00 to 0x02039A1C). For music, the request scales the volume
// by the option for the music's level (0x0206C8DC), stops the piece that plays (0x0206C3C4), waits for a frame
// (0x02006C48), loads BGM/NAME.SWD and BGM/NAME.SMD by the name in entry N of the table at 0x0210D38F (0x0206C068) and
// starts the song (0x0206C384), in the caller's thread. The name 0 stops the music. The stub makes that request, then
// sleeps.
constexpr DseCode kLuminousArc2 = {
    .game_code = "YL2E",
    .hook = 0x02000CC0,
    .hook_end = 0x02000CC8,
    .stub = 0x020242E0,
    .stub_end = 0x02024338,
    .start = DseStartKind::kCalls,
    .sleep = 0x02005358,
    .load_bank = 0x0206DB5C,
    .load_song = 0x0206E378,
    .start_song = 0x0206E528,
    .stop_song = 0x0206E6B8,
    .bgm_format = "BGM/%s.SMD",
    .song_table = 0x0210D38F,
    .song_count = 41,
    .song_name_bytes = 8,
    .calls = {{{.routine = 0x0206C588, .arguments = {0, kDseSongNumber, 127, 127}, .stack_zeros = 2}}},
};

// Luminous Arc 3: Eyes, Japan: Luminous Arc 2's structure at other addresses. NitroMain (0x02000C94) calls the start-up
// (0x020271D8). The start-up starts the sound (0x0202CBB4 at 0x02027260), then sets up the game's scenes (0x0202D484)
// and runs the game's frame (0x0202CBC8) in a loop (0x02000CA0). The hook replaces that loop, and the stub goes over
// the frame's routine. The sound requests are 0x02053F94, with music's names in the table at 0x020C9A4C. For music
// (request 0), the request stops the piece that plays, waits for a frame (0x02008204), then steps the BGM task
// (0x02054A68) once a frame until it ends, with the game's frame work less the scenes (0x0202CC20) between the steps.
// The task loads the main bank BGM/00_HAKEI.SWD, BGM/NAME.SWD with its waves, and BGM/NAME.SMD. The request then starts
// the song (0x02054D34) with flags 4 and the volume and the fade in the parameters, in the caller's thread. The
// requests of music give a volume of 127 and no fade (0x02043694).
constexpr DseCode kLuminousArc3 = {
    .game_code = "BLUJ",
    .hook = 0x02000CA0,
    .hook_end = 0x02000CA8,
    .stub = 0x0202CBC8,
    .stub_end = 0x0202CC20,
    .start = DseStartKind::kCalls,
    .sleep = 0x020068E8,
    .load_bank = 0x0205C1B8,
    .load_song = 0x0205C92C,
    .start_song = 0x0205CA80,
    .stop_song = 0x0205CB60,
    .volume_in_parameters = true,
    .bgm_format = "BGM/%s.SMD",
    .song_table = 0x020C9A4C,
    .song_count = 44,
    .song_name_bytes = 8,
    .calls = {{{.routine = 0x02053F94, .arguments = {0, kDseSongNumber, 127, 127}, .stack_zeros = 2}}},
};

// Luminous Arc, North America: the oldest build. The game's thread (0x020704D0) creates the game's parts (0x02070DC4),
// among them the sound object (0x020968E4 at 0x02070E58, at [0x0214F330]). The sound object starts the sound
// (0x02095524: DSE's start-up, its thread and its alarm). The thread then runs the game's frames (0x020704EC on). The
// hook follows the creation of the parts, and the stub goes over the frames' loop and its literal pool. The game's
// start of music (0x02094780: the object, 3 and the number) does nothing for the song that plays. For another, it reads
// entry N of the table at 0x021104E4 (0x02095030), whose first two words give the members of data/Snd.iear that contain
// the song and its bank, and loads them, the song first, in the caller's thread (0x02094E48, 0x02095A14). The first
// load reads the archive's table of members (0x0207357C). The load's end calls the sound object's work (0x020951B8,
// 0x0209512C). That work starts the song at the volume of the music's level (0x02094D0C). The scripts start music with
// it (0x0209E310). The stub makes that call, then sleeps.
constexpr DseCode kLuminousArc = {
    .game_code = "ANIE",
    .hook = 0x020704D8,
    .hook_end = 0x0207052C,
    .start = DseStartKind::kCalls,
    .sleep = 0x02066674,
    .load_bank = 0x020BFA5C,
    .load_song = 0x020BFEC8,
    .start_song = 0x020C00AC,
    .stop_song = 0x020C0188,
    .pack = "data/Snd.iear",
    .index_kind = DsePackIndex::kMembers,
    .song_table = 0x021104E4,
    .song_count = 42,
    .song_before_bank = true,
    .calls = {{{.routine = 0x02094780, .arguments = {0x0214F330, 3, kDseSongNumber, 0}, .registers = 3, .loads = 1}}},
};

// Soma Bringer, Japan: the earlier build. NitroMain (0x02000BD8) calls the start-up (0x02040A10). The start-up starts
// the sound (0x02063430: DSE's start-up, its thread and its alarm), checks the backup memory (0x02035194) and runs the
// game's main loop (0x02000EC4). With a blank backup memory, the check waits on a message for a button. The hook
// follows the start of the sound, in place of the check, and the stub goes over the check. The game plays music through
// its music object at 0x02111340: the request (0x02079734: the object, the number, the volume, the fade-out of the
// piece that plays and the fade-in on the stack) does nothing for the piece that plays, and sets the object's request.
// The object's work for a frame (0x0207971C) passes the song's file data/snd/bg/NAME.mus in data/data.srl, a cartridge
// image, to the game's loader at 0x0210BA4C (0x02055D18 at 0x020794B8). The loader's work for a frame (0x02055A80)
// reads the file. The object's next work for a frame loads the bank and the song from the file (0x020637E8, 0x02062958)
// and starts the song at the volume and with the fade-in (0x02063828, 0x02062ACC). The game's frames run both works
// (0x0205201C, 0x02052024). The main loop runs the set-up of the game's parts (0x02050344) on the way into the game,
// after the hook. The set-up sets up the loader (0x02054C94) and the object (0x02079300: a routine that sets fields and
// pushes nothing). Without them, the loader's first work loads a halfword through its entry pointer, still 0, from
// 0x0000000A: no protection region of the game covers that address, and on a DS the ARM9 takes a data abort there. The
// number of a song's file is the number in its name plus 1000 times the place of its letter in F, D, C, B, E and W: the
// request for 1 reads F01, for 1001 D01 and for 5001 W01. A number with no file reads nothing. The stub sets up the
// loader and the object in the game's order, makes the request at a volume of 127 with no fades, then waits for each
// frame (OS_WaitVBlankIntr, 0x020166DC) and runs both works.
constexpr DseNameNumber kSomaBringerNames[] = {{"F", 0},    {"D", 1000}, {"C", 2000},
                                               {"B", 3000}, {"E", 4000}, {"W", 5000}};

constexpr DseCode kSomaBringer = {
    .game_code = "YBSJ",
    .hook = 0x02040A3C,
    .hook_end = 0x02040A4C,
    .stub = 0x02035194,
    .stub_end = 0x0203524C,
    .start = DseStartKind::kCalls,
    .sleep = 0x02015030,
    .load_bank = 0x0206B3CC,
    .load_song = 0x0206BAD4,
    .start_song = 0x0206BD1C,
    .stop_song = 0x0206BDFC,
    .frame_wait = 0x020166DC,
    .pack = "data/data.srl",
    .pack_index = "data/snd/bg/",
    .index_kind = DsePackIndex::kRom,
    .name_numbers = kSomaBringerNames,
    .calls =
        {{{.routine = 0x02054C94, .arguments = {0x0210BA4C, 0, 0, 0}, .registers = 1},
          {.routine = 0x02079300, .arguments = {0x02111340, 0, 0, 0}, .registers = 1, .first_instruction = 0xE3A02000},
          {.routine = 0x02079734, .arguments = {0x02111340, kDseSongNumber, 127, 0}, .stack_zeros = 2}}},
    .frame_calls = {{{.routine = 0x0207971C, .arguments = {0x02111340, 0, 0, 0}, .registers = 1},
                     {.routine = 0x02055A80, .arguments = {0x0210BA4C, 0, 0, 0}, .registers = 1}}},
};

constexpr DseCode kGames[] = {kExplorersOfSky,
                              kExplorersOfTime,
                              kExplorersOfDarkness,
                              kInazumaEleven2,
                              kInazumaEleven,
                              kInazumaEleven3Bomber,
                              kInazumaEleven3Ogre,
                              kLayton2,
                              kLayton3,
                              kLayton4,
                              kShirenDs2,
                              kShiren4,
                              kShiren5,
                              kLuminousArc2,
                              kLuminousArc3,
                              kLuminousArc,
                              kSomaBringer};

// The second programs of the games of kGames.
constexpr DseProgram kPrograms[] = {{.name = "ll", .code = kLayton4LondonLife}};

// Instructions of ARM code: a push of registers with the link register, a call, and a compare of a register with an
// immediate. The immediate is an operand of 8 bits, rotated right by twice the 4 bits above it. In Thumb code, a push
// of registers with the link register.
constexpr uint32_t kPushLrMask = 0xFFFF4000;
constexpr uint32_t kPushLr = 0xE92D4000;
constexpr uint32_t kThumbPushLrMask = 0xFF00;
constexpr uint32_t kThumbPushLr = 0xB500;

// The two halfwords of a Thumb call of 4 bytes, bl or blx: the first sets the high bits of the offset.
constexpr uint32_t kThumbCallFirstMask = 0xF800;
constexpr uint32_t kThumbCallFirst = 0xF000;
constexpr uint32_t kThumbCallSecondMask = 0xE800;
constexpr uint32_t kThumbCallSecond = 0xE800;

// Thumb code that goes on in ARM state at the next word: bx pc, then a nop (mov r8, r8). ARM code that jumps to the
// address in the next word: ldr pc, [pc, #-4].
constexpr uint32_t kThumbToArm = 0x46C04778;
constexpr uint32_t kJumpToNextWord = 0xE51FF004;
constexpr uint32_t kBlMask = 0xFF000000;
constexpr uint32_t kBl = 0xEB000000;
constexpr uint32_t kB = 0xEA000000;
constexpr uint32_t kCmpImmediateMask = 0xFFF0F000;
constexpr uint32_t kCmpImmediate = 0xE3500000;
constexpr uint32_t kImmediateBits = 0xFFF;

uint32_t CmpImmediate(uint32_t word)
{
    const uint32_t value = word & 0xFF;
    const uint32_t rotate = 2 * ((word >> 8) & 0xF);
    return rotate == 0 ? value : (value >> rotate) | (value << (32 - rotate));
}

// Whether `address` is of Thumb code, and the address of its first byte.
bool IsThumb(uint32_t address)
{
    return (address & 1) != 0;
}

uint32_t CodeAddress(uint32_t address)
{
    return address & ~uint32_t{1};
}

// Where the stub goes: at `code.stub`, or else at the hook.
uint32_t StubAddress(const DseCode& code)
{
    return code.stub != 0 ? code.stub : CodeAddress(code.hook);
}

// The jump from the hook to the stub at `code.stub`, in ARM state: ldr pc, [pc, #-4] and the stub's address, after a
// switch to ARM state at a hook in Thumb code.
std::vector<uint8_t> JumpToStub(const DseCode& code)
{
    std::vector<uint8_t> bytes;
    if (IsThumb(code.hook))
    {
        xsflib::AppendLe32(bytes, kThumbToArm);
    }
    xsflib::AppendLe32(bytes, kJumpToNextWord);
    xsflib::AppendLe32(bytes, code.stub);

    return bytes;
}

// The word at `address`. The caller has checked that `arm9` contains it.
uint32_t CodeWord(const ntr::LoadedCode& arm9, uint32_t address)
{
    return *WordAt(arm9, address);
}

// The overlay of `code.overlays` that contains `address` in `rom`'s table, or null.
const ntr::Overlay* OverlayAt(const ntr::Rom& rom, const DseCode& code, uint32_t address)
{
    for (const ntr::Overlay& overlay : rom.Arm9Overlays())
    {
        const bool listed = std::ranges::find(code.overlays, overlay.id) != code.overlays.end();
        if (listed && address >= overlay.ram_address && address - overlay.ram_address < overlay.ram_size)
        {
            return &overlay;
        }
    }

    return nullptr;
}

// The code that the game has loaded at the hook: the ARM9 binary, then the overlays of `code.overlays`, decompressed.
std::vector<ntr::LoadedCode> CodeAtHook(const ntr::Rom& rom, const DseCode& code)
{
    std::vector<ntr::LoadedCode> loaded = {ntr::Arm9Code(rom)};
    for (const ntr::Overlay& overlay : rom.Arm9Overlays())
    {
        if (std::ranges::find(code.overlays, overlay.id) != code.overlays.end())
        {
            loaded.push_back({.address = overlay.ram_address,
                              .bytes = ntr::OverlayBytes(rom.FileData(overlay.file_id), overlay),
                              .overlay = overlay.id});
        }
    }

    return loaded;
}

// The part of `loaded` that contains the word at `address`, or null.
const ntr::LoadedCode* CodeWith(std::span<const ntr::LoadedCode> loaded, uint32_t address)
{
    for (const ntr::LoadedCode& part : loaded)
    {
        if (address >= part.address && address - part.address + 4 <= part.bytes.size())
        {
            return &part;
        }
    }

    return nullptr;
}

} // namespace

bool ListsDseGame(std::string_view game_code)
{
    return std::ranges::find(kGames, game_code, &DseCode::game_code) != std::end(kGames);
}

bool DseTakesLevel(const DseCode& code)
{
    const auto takes = [](const DseCall& call)
    {
        return std::ranges::find(call.arguments, kDseSongLevel) != call.arguments.end();
    };

    return code.play_bgm_level != 0 || std::ranges::any_of(code.calls, takes);
}

namespace
{

// Whether `rom` contains the code that `game` expects, as FindDseCode says, and each patch's word, or in a set's
// library the patched word. Returns false, with `error` set, when it doesn't.
bool CheckDseCode(const ntr::Rom& rom, const DseCode& game, std::string& error)
{
    const std::vector<ntr::LoadedCode> loaded = CodeAtHook(rom, game);
    const auto inside = [&](uint32_t address)
    {
        return CodeWith(loaded, CodeAddress(address)) != nullptr;
    };
    const auto word_at = [&](uint32_t address)
    {
        return CodeWord(*CodeWith(loaded, CodeAddress(address)), CodeAddress(address));
    };

    // Where the table gives no start at a level or of short pieces, the checks of that routine check `play_bgm` again.
    // A game with no loop of the game's frames to cut, or with no limits, has no checks of them.
    const bool cut = game.frame_cut != 0;
    const bool limits = game.start == DseStartKind::kNumber;
    std::vector<uint32_t> routines = {game.sleep, game.load_bank, game.load_song, game.start_song, game.stop_song};
    if (game.set_song != 0)
    {
        routines.push_back(game.set_song);
    }

    // The calls of routines that push no registers, each with its first instruction.
    std::vector<std::pair<uint32_t, uint32_t>> leaves;
    if (game.start == DseStartKind::kCalls)
    {
        std::vector<std::span<const DseCall>> call_lists = {
            std::span<const DseCall>(game.calls), std::span<const DseCall>(game.frame_calls),
            std::span<const DseCall>(game.me_calls), std::span<const DseCall>(game.change_calls)};
        for (const DseStreamKind& kind : game.streams)
        {
            call_lists.emplace_back(kind.calls);
        }
        for (const std::span<const DseCall> calls : call_lists)
        {
            for (const DseCall& call : calls)
            {
                if (call.routine != 0 && call.first_instruction != 0)
                {
                    leaves.emplace_back(call.routine, call.first_instruction);
                }
                else if (call.routine != 0)
                {
                    routines.push_back(call.routine);
                }
            }
        }
    }
    else
    {
        const uint32_t play_bgm_level = game.play_bgm_level != 0 ? game.play_bgm_level : game.play_bgm;
        const uint32_t play_me = game.play_me != 0 ? game.play_me : game.play_bgm;
        routines.insert(routines.end(), {game.play_bgm, play_bgm_level, play_me});
    }

    if (game.frame_wait != 0)
    {
        routines.push_back(game.frame_wait);
    }

    if (game.frame_update != 0)
    {
        routines.push_back(game.frame_update);
    }

    std::vector<uint32_t> addresses = {CodeAddress(game.hook) - 4, game.hook, game.hook_end};
    addresses.insert(addresses.end(), routines.begin(), routines.end());
    for (const auto& [routine, first] : leaves)
    {
        addresses.push_back(routine);
    }

    if (game.stub != 0)
    {
        addresses.insert(addresses.end(), {game.stub, game.stub_end - 4});
    }
    if (cut)
    {
        addresses.insert(addresses.end(), {game.frame_cut - 16, game.frame_signal + 4, game.frame_signal + 12});
    }

    if (limits)
    {
        addresses.insert(addresses.end(), {game.bgm_first, game.bgm_limit, game.me_limit});
    }

    for (const uint32_t address : addresses)
    {
        if (!inside(address))
        {
            error = "the ARM9 binary doesn't contain " + xsflib::Hex(address, 8, true);
            return false;
        }
    }

    const auto routine_at = [&](uint32_t address)
    {
        return IsThumb(address) ? (word_at(address) & kThumbPushLrMask) == kThumbPushLr
                                : (word_at(address) & kPushLrMask) == kPushLr;
    };

    for (const uint32_t routine : routines)
    {
        if (!routine_at(routine))
        {
            error = "the game's code at " + xsflib::Hex(routine, 8, true) + " isn't the routine that 2sfrip expects";
            return false;
        }
    }

    for (const auto& [routine, first] : leaves)
    {
        if (word_at(routine) != first)
        {
            error = "the game's code at " + xsflib::Hex(routine, 8, true) + " isn't the routine that 2sfrip expects";
            return false;
        }
    }

    const auto is = [&](uint32_t address, uint32_t opcode)
    {
        return (word_at(address) & kBlMask) == opcode;
    };

    // A set's library contains the patched word (MakeDseSet).
    for (const GamePatch& patch : game.patches)
    {
        if (!PatchMatches(loaded.front(), patch) && !PatchMatches(loaded.front(), PatchedWord(patch)))
        {
            error = "the game's code at " + xsflib::Hex(patch.address, 8, true) + " isn't the code that 2sfrip patches";
            return false;
        }
    }

    // The stub at a hook in Thumb code starts with bx pc. The code goes on in ARM state 4 bytes on: a set's library has
    // it there. The hook starts a routine or follows a Thumb call: bl or blx, a pair of halfwords.
    constexpr uint32_t kWordBits = 3;
    const uint32_t before = word_at(CodeAddress(game.hook) - 4);
    const bool thumb_call = (before & kThumbCallFirstMask) == kThumbCallFirst &&
                            ((before >> 16) & kThumbCallSecondMask) == kThumbCallSecond;
    const bool thumb_start = routine_at(game.hook) || word_at(game.hook) == kThumbToArm || thumb_call;
    if (IsThumb(game.hook) && ((game.hook & kWordBits) != 1 || !thumb_start))
    {
        error = "the game's Thumb code at the hook at " + xsflib::Hex(CodeAddress(game.hook), 8, true) +
                " neither starts a routine nor follows a call at a multiple of 4";
        return false;
    }

    // The place of a stub away from the hook starts a routine of the game, ARM or Thumb, or in a set's library the
    // stub, with the jump at the hook.
    const uint32_t stub_first = game.stub != 0 ? word_at(game.stub) : 0;
    const bool stub_routine = (stub_first & kPushLrMask) == kPushLr || (stub_first & kThumbPushLrMask) == kThumbPushLr;
    const bool jump = word_at(game.hook) == kThumbToArm || word_at(game.hook) == kJumpToNextWord;
    if (game.stub != 0 && !stub_routine && !jump)
    {
        error = "the game's code at " + xsflib::Hex(game.stub, 8, true) + " isn't the routine that 2sfrip expects";
        return false;
    }

    if (!IsThumb(game.hook) && !is(game.hook - 4, kBl) && !routine_at(game.hook))
    {
        error = "the game's code at the hook at " + xsflib::Hex(game.hook, 8, true) +
                " neither follows a call nor starts a routine";
        return false;
    }

    if (cut && (!is(game.frame_cut - 16, kBl) || !is(game.frame_signal + 4, kBl) || !is(game.frame_signal + 12, kB)))
    {
        error = "the game's code at " + xsflib::Hex(game.frame_cut, 8, true) + " isn't the loop of the game's frames";
        return false;
    }

    if (!limits)
    {
        return true;
    }

    for (const uint32_t limit : {game.bgm_first, game.bgm_limit, game.me_limit})
    {
        if ((word_at(limit) & kCmpImmediateMask) != kCmpImmediate)
        {
            error = "the game's code at " + xsflib::Hex(limit, 8, true) + " isn't a compare with a number";
            return false;
        }
    }

    // A set's library compares with 0 there (MakeDseSet).
    if (CmpImmediate(word_at(game.bgm_first)) > 1)
    {
        error = "the game's code at " + xsflib::Hex(game.bgm_first, 8, true) + " isn't a compare with 1";
        return false;
    }

    return true;
}

} // namespace

std::optional<DseCode> FindDseCode(const ntr::Rom& rom, std::string& error)
{
    const ntr::RomHeader& header = rom.Header();
    const auto listed = [&header](const DseCode& code)
    {
        return code.game_code == header.game_code && code.revision == header.version;
    };
    const auto game = std::ranges::find_if(kGames, listed);
    if (game == std::end(kGames))
    {
        error = ListsDseGame(header.game_code) ? "2sfrip has no table of this revision's DSE routines"
                                               : "2sfrip has no table of this game's DSE routines";
        return std::nullopt;
    }

    if (!CheckDseCode(rom, *game, error))
    {
        return std::nullopt;
    }

    return *game;
}

std::optional<std::vector<DseProgram>> FindDsePrograms(const ntr::Rom& rom, std::string& error)
{
    const ntr::RomHeader& header = rom.Header();
    std::vector<DseProgram> programs;
    for (const DseProgram& program : kPrograms)
    {
        if (program.code.game_code != header.game_code || program.code.revision != header.version)
        {
            continue;
        }

        if (!CheckDseCode(rom, program.code, error))
        {
            error = "its program " + std::string(program.name) + ": " + error;
            return std::nullopt;
        }
        programs.push_back(program);
    }

    return programs;
}

DseLimits ReadDseLimits(const ntr::Rom& rom, const DseCode& code)
{
    // The songs' paths of a start by calls give the number in three digits.
    constexpr uint32_t kLastCallsNumber = 999;
    if (code.start == DseStartKind::kCalls)
    {
        return {.last_bgm = kLastCallsNumber};
    }

    if (code.start != DseStartKind::kNumber)
    {
        return {};
    }

    const ntr::LoadedCode arm9 = ntr::Arm9Code(rom);

    return {.first_bgm = CmpImmediate(CodeWord(arm9, code.bgm_first)),
            .last_bgm = CmpImmediate(CodeWord(arm9, code.bgm_limit)),
            .last_me = CmpImmediate(CodeWord(arm9, code.me_limit))};
}

std::optional<DseStart> DseStartOf(const DseCode& code, const DseLimits& limits, const std::string& path)
{
    const bool inside = code.start == DseStartKind::kCalls;
    const auto find = [&](const char* format, uint32_t first, uint32_t last) -> std::optional<uint32_t>
    {
        if (format == nullptr)
        {
            return std::nullopt;
        }

        for (uint32_t number = first; number <= last; number++)
        {
            char name[64];
            std::snprintf(name, sizeof name, format, number);
            const std::string_view file = name;
            if (path == file ||
                (inside && path.starts_with(file) && path.size() > file.size() && path[file.size()] == '/'))
            {
                return number;
            }
        }

        return std::nullopt;
    };

    if (const std::optional<uint32_t> number = find(code.bgm_format, 0, limits.last_bgm))
    {
        return DseStart{.number = *number, .me = false};
    }

    if (const std::optional<uint32_t> number = find(code.me_format, 0, limits.last_me))
    {
        return DseStart{.number = *number, .me = true};
    }

    return std::nullopt;
}

std::vector<DseStream> DseStreams(const ntr::Rom& rom, const DseCode& code)
{
    std::vector<DseStream> streams;
    for (std::size_t kind = 0; kind < code.streams.size(); kind++)
    {
        const DseStreamKind& stream_kind = code.streams[kind];
        std::vector<DseStream> found;
        for (const ntr::RomFile& file : rom.Files())
        {
            const std::size_t slash = file.path.rfind('/');
            const std::string file_name = file.path.substr(slash + 1);
            const std::size_t dot = file_name.rfind('.');
            const std::string label = file_name.substr(0, dot);
            if (dot == std::string::npos || file_name.substr(dot) != ".SAD" || !ReadSadlHeader(rom.FileData(file.id)))
            {
                continue;
            }

            if (code.start != DseStartKind::kCalls)
            {
                if (file.path.starts_with(stream_kind.format) && file_name.size() < kDseNameBytes)
                {
                    found.push_back(
                        {.path = file.path, .file_id = file.id, .kind = kind, .name = file_name, .label = label});
                }
                continue;
            }

            for (uint32_t number = stream_kind.first; number <= stream_kind.last; number++)
            {
                char path[64];
                std::snprintf(path, sizeof path, stream_kind.format, number);
                if (file.path == path)
                {
                    found.push_back(
                        {.path = file.path, .file_id = file.id, .kind = kind, .number = number, .label = label});
                    break;
                }
            }
        }
        std::ranges::sort(found, {}, &DseStream::path);
        streams.insert(streams.end(), found.begin(), found.end());
    }

    return streams;
}

std::map<uint32_t, DseStart> DsePackStarts(std::span<const uint8_t> index)
{
    constexpr std::size_t kEntryBytes = 32;
    constexpr std::size_t kOffsetField = 24;
    std::map<uint32_t, DseStart> starts;
    for (std::size_t at = 0; at + kEntryBytes <= index.size(); at += kEntryBytes)
    {
        const auto name_bytes = index.subspan(at, kOffsetField);
        const auto zero = std::ranges::find(name_bytes, uint8_t{0});
        std::string name(name_bytes.begin(), zero);
        name = name.substr(0, name.rfind('.'));
        if (!name.empty() && name.size() < kDseNameBytes)
        {
            starts[xsflib::LoadLe32(&index[at + kOffsetField])] = {.name = name};
        }
    }

    return starts;
}

std::optional<uint32_t> DsePackEntry(DsePackIndex kind, std::span<const uint8_t> index, std::string_view key)
{
    // The CRC-32 of the key in lowercase: zlib's, with the polynomial 0xEDB88320 of the SDK's MATH_CalcCRC32.
    std::vector<uint8_t> lowercase;
    for (const char c : key)
    {
        lowercase.push_back(static_cast<uint8_t>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
    }
    const auto crc = static_cast<uint32_t>(crc32(0, lowercase.data(), static_cast<uInt>(lowercase.size())));

    const auto word = [&index](std::size_t at) -> std::optional<uint32_t>
    {
        if (at + 4 > index.size())
        {
            return std::nullopt;
        }

        return xsflib::LoadLe32(&index[at]);
    };

    if (kind == DsePackIndex::kCrcTable)
    {
        constexpr std::size_t kEntryBytes = 16;
        for (std::size_t at = 0; at + kEntryBytes <= index.size(); at += kEntryBytes)
        {
            if (*word(at) == crc)
            {
                return word(at + 4);
            }
        }

        return std::nullopt;
    }

    // A node with bit 31 set gives the entry's place in the index. Any other node gives the place of the next node, in
    // words: in its low 16 bits for a bit of 0, and in the 15 bits above them for a bit of 1.
    std::size_t node = 0;
    for (int bit = 0; bit < 32; bit++)
    {
        const std::optional<uint32_t> value = word(node);
        if (!value)
        {
            return std::nullopt;
        }

        if ((*value & 0x80000000) != 0)
        {
            return word(*value & 0x7FFFFFFF);
        }

        node = 4 * (((crc >> bit) & 1) != 0 ? (*value >> 16) & 0x7FFF : *value & 0xFFFF);
    }

    return std::nullopt;
}

std::optional<uint32_t> DseOffsetEntry(std::span<const uint8_t> index, uint32_t offset)
{
    for (std::size_t at = 0; at + 8 <= index.size(); at += 4)
    {
        if (xsflib::LoadLe32(&index[at]) <= offset && offset < xsflib::LoadLe32(&index[at + 4]))
        {
            return static_cast<uint32_t>(at / 4);
        }
    }

    return std::nullopt;
}

std::optional<uint32_t> DseMemberAt(std::span<const uint8_t> pack, uint32_t offset)
{
    constexpr uint32_t kOffsetBase = 0x10;
    const uint32_t end = DseMembersEnd(pack);
    for (uint32_t at = kMembersTable; at + 8 <= end; at += 8)
    {
        if (xsflib::LoadLe32(&pack[at]) + kOffsetBase == offset)
        {
            return (at - kMembersTable) / 8;
        }
    }

    return std::nullopt;
}

uint32_t DseMembersEnd(std::span<const uint8_t> pack)
{
    constexpr uint32_t kWordsField = 0x14;
    if (pack.size() < kMembersTable)
    {
        return 0;
    }

    const uint64_t end = kMembersTable + uint64_t{4} * xsflib::LoadLe32(&pack[kWordsField]);

    return static_cast<uint32_t>(std::min<uint64_t>(end / 8 * 8, pack.size() / 8 * 8));
}

std::vector<std::pair<uint32_t, uint32_t>> DseDarcMembers(std::span<const uint8_t> pack)
{
    constexpr uint32_t kMagic = 0x43524144; // "DARC"
    constexpr uint32_t kCountField = 4;
    constexpr uint32_t kOffsets = 8;
    constexpr uint32_t kOffsetBase = 12;
    constexpr uint32_t kHeaderBytes = 16;
    constexpr uint32_t kSizeField = 4;
    if (pack.size() < kOffsets || xsflib::LoadLe32(pack.data()) != kMagic)
    {
        return {};
    }

    const uint64_t count = xsflib::LoadLe32(&pack[kCountField]);
    if (kOffsets + 4 * count > pack.size())
    {
        return {};
    }

    std::vector<std::pair<uint32_t, uint32_t>> members;
    for (uint64_t k = 0; k < count; k++)
    {
        const uint64_t first = kOffsetBase + 4 * k + xsflib::LoadLe32(&pack[kOffsets + 4 * k]);
        if (first + kHeaderBytes > pack.size())
        {
            return {};
        }

        const uint64_t end = first + kHeaderBytes + xsflib::LoadLe32(&pack[first + kSizeField]);
        if (end > pack.size())
        {
            return {};
        }
        members.emplace_back(static_cast<uint32_t>(first), static_cast<uint32_t>(end));
    }

    return members;
}

std::optional<uint32_t> DseDarcMemberAt(std::span<const uint8_t> pack, uint32_t offset)
{
    constexpr uint32_t kHeaderBytes = 16;
    const std::vector<std::pair<uint32_t, uint32_t>> members = DseDarcMembers(pack);
    for (std::size_t k = 0; k < members.size(); k++)
    {
        if (members[k].first + kHeaderBytes == offset)
        {
            return static_cast<uint32_t>(k);
        }
    }

    return std::nullopt;
}

std::vector<DseRomEntry> DseRomEntries(std::span<const uint8_t> pack, std::string_view folder)
{
    std::optional<ntr::Rom> image;
    try
    {
        image.emplace(ntr::Rom::Parse(pack));
    }
    catch (const std::exception&)
    {
        return {};
    }

    std::vector<DseRomEntry> entries;
    for (const ntr::RomFile& file : image->Files())
    {
        const std::string_view name = std::string_view(file.path).substr(std::min(folder.size(), file.path.size()));
        if (file.path.starts_with(folder) && !name.empty() && name.find('/') == std::string_view::npos)
        {
            entries.push_back(
                {.name = std::string(name.substr(0, name.rfind('.'))), .first = file.start, .end = file.end});
        }
    }

    return entries;
}

std::optional<uint32_t> DseNameNumberOf(const DseCode& code, std::string_view name)
{
    constexpr std::size_t kMaxDigits = 4;
    for (const DseNameNumber& prefix : code.name_numbers)
    {
        const std::string_view digits = name.substr(std::min(prefix.prefix.size(), name.size()));
        const auto digit = [](char c)
        {
            return c >= '0' && c <= '9';
        };
        if (!name.starts_with(prefix.prefix) || digits.empty() || digits.size() > kMaxDigits ||
            !std::ranges::all_of(digits, digit))
        {
            continue;
        }

        uint32_t number = 0;
        for (const char c : digits)
        {
            number = number * 10 + static_cast<uint32_t>(c - '0');
        }

        return prefix.base + number;
    }

    return std::nullopt;
}

std::map<uint32_t, uint32_t> DseMemberNumbers(const ntr::Rom& rom, const DseCode& code)
{
    constexpr uint32_t kEntryBytes = 12;
    constexpr uint32_t kMemberMask = 0xFFFF;
    const ntr::LoadedCode arm9 = ntr::Arm9Code(rom);
    std::map<uint32_t, uint32_t> numbers;
    for (uint32_t number = 0; number < code.song_count; number++)
    {
        const uint32_t entry = code.song_table + number * kEntryBytes;
        if (CodeWith(std::span(&arm9, 1), entry) == nullptr)
        {
            break;
        }

        const uint32_t word = CodeWord(arm9, entry);
        if (word != 0)
        {
            numbers.emplace(word & kMemberMask, number);
        }
    }

    return numbers;
}

std::map<std::string, DseStart> DseTableStarts(const ntr::Rom& rom, const DseCode& code)
{
    constexpr uint32_t kEntryBytes = 12;
    constexpr uint32_t kMeField = 8;
    constexpr char kMark = '=';

    // The name at `address` in the ARM9 binary, up to its terminating zero. Empty where the binary ends first.
    const ntr::LoadedCode arm9 = ntr::Arm9Code(rom);
    const auto name_at = [&arm9](uint32_t address)
    {
        std::string name;
        for (uint32_t at = address; at >= arm9.address && at - arm9.address < arm9.bytes.size(); at++)
        {
            const auto c = static_cast<char>(arm9.bytes[at - arm9.address]);
            if (c == '\0')
            {
                return name;
            }
            name.push_back(c);
        }

        return std::string();
    };

    std::map<std::string, DseStart> starts;
    for (uint32_t number = 0; number < code.song_count && code.song_name_bytes != 0; number++)
    {
        // An entry that is the name, up to its first zero or the entry's end.
        const uint32_t entry = code.song_table + number * code.song_name_bytes;
        if (entry < arm9.address || entry - arm9.address + code.song_name_bytes > arm9.bytes.size())
        {
            break;
        }

        const auto first = arm9.bytes.begin() + (entry - arm9.address);
        const std::string name(first, std::find(first, first + code.song_name_bytes, uint8_t{0}));
        if (name.empty() || code.bgm_format == nullptr)
        {
            continue;
        }

        char path[128];
        std::snprintf(path, sizeof path, code.bgm_format, name.c_str());
        starts.emplace(path, DseStart{.number = number});
    }

    for (uint32_t number = 0; number < code.song_count && code.song_name_bytes == 0; number++)
    {
        const uint32_t entry = code.song_table + number * kEntryBytes;
        if (CodeWith(std::span(&arm9, 1), entry + kMeField) == nullptr)
        {
            break;
        }

        const std::string name = name_at(CodeWord(arm9, entry));
        const bool me = arm9.bytes[entry + kMeField - arm9.address] == 1;
        const char* const format = name.starts_with(kMark) ? code.marked_format : me ? code.me_format : code.bgm_format;
        if (name.empty() || format == nullptr)
        {
            continue;
        }

        char path[128];
        std::snprintf(path, sizeof path, format, name.c_str() + (name.starts_with(kMark) ? 1 : 0));
        starts.emplace(path, DseStart{.number = number});
    }

    return starts;
}

namespace
{

// The name of `start` in kDseNameBytes, padded with zeros.
std::vector<uint8_t> NameBytes(const DseStart& start)
{
    std::vector<uint8_t> bytes(kDseNameBytes, 0);
    std::ranges::copy(start.name.substr(0, kDseNameBytes - 1), bytes.begin());

    return bytes;
}

// The stub of a game that starts a song by name: the start of music in the first slot with no fade-out, then for each
// frame the wait and the player's work.
DseStub BuildNamedStub(const DseCode& code, const DseStart& start)
{
    constexpr uint16_t kLr = 1 << 14;
    arm::ArmAssembler a(code.hook);
    const int data = a.NewLabel();
    const int frame = a.NewLabel();

    // A push like the routine's that the stub replaces: FindDseCode finds the hook in a library too.
    a.Push(kLr);
    a.LoadConstant(0, code.player);
    a.Mov(1, 0);
    a.LoadAddress(2, data);
    a.Mov(3, 0);
    a.Push(1 << 3);
    CallOnArm9(a, code.play_bgm);
    a.Add(13, 13, 4);

    a.Bind(frame);
    CallOnArm9(a, code.frame_wait);
    a.LoadConstant(0, code.player);
    CallOnArm9(a, code.frame_update);
    a.B(frame);

    a.Pool();
    a.Bind(data);
    const std::vector<uint8_t> name = NameBytes(start);
    for (std::size_t i = 0; i < name.size(); i += 4)
    {
        a.Word(xsflib::LoadLe32(&name[i]));
    }

    DseStub stub;
    stub.start_offset = a.Address(data) - code.hook;
    stub.bytes = a.Finish();

    return stub;
}

// The offsets of the word of a short piece, of the song's level and of the change's volume and frames in the data of a
// stub of kCalls.
constexpr int kCallsMeField = 4;
constexpr int kCallsLevelField = 8;
constexpr int kCallsChangeField = 12;
constexpr int kCallsFramesField = 16;

// The calls of `calls` whose routines aren't 0, each with its words of 0 on the stack and its arguments: the word at
// `data` plus the call's base for kDseSongNumber, the level at `data` for kDseSongLevel, the change's volume and frames
// at `data` for kDseChangeVolume and kDseChangeFrames, and the word at the address for an argument that the call loads.
void MakeCalls(arm::ArmAssembler& a, std::span<const DseCall> calls, int data)
{
    constexpr int kScratch = 12;
    constexpr int kSp = 13;
    for (const DseCall& call : calls)
    {
        if (call.routine == 0)
        {
            continue;
        }

        if (call.stack_zeros != 0)
        {
            a.Mov(kScratch, 0);
        }
        for (uint32_t word = 0; word < call.stack_zeros; word++)
        {
            a.Push(1 << kScratch);
        }
        for (auto word = call.stack.rbegin(); word != call.stack.rend(); ++word)
        {
            a.LoadConstant(kScratch, *word);
            a.Push(1 << kScratch);
        }

        for (std::size_t r = 0; r < call.registers && r < call.arguments.size(); r++)
        {
            const int rd = static_cast<int>(r);
            const bool literal = (call.literals >> r & 1) != 0;
            if (call.arguments[r] == kDseSongNumber && !literal)
            {
                a.LoadAddress(kScratch, data);
                a.Ldr(rd, kScratch);
                if (call.number_base != 0)
                {
                    a.LoadConstant(kScratch, 0 - call.number_base);
                    a.SubRegister(rd, rd, kScratch);
                }
            }
            else if (call.arguments[r] == kDseSongLevel && !literal)
            {
                a.LoadAddress(kScratch, data);
                a.Ldr(rd, kScratch, kCallsLevelField);
            }
            else if (call.arguments[r] == kDseChangeVolume && !literal)
            {
                a.LoadAddress(kScratch, data);
                a.Ldr(rd, kScratch, kCallsChangeField);
            }
            else if (call.arguments[r] == kDseChangeFrames && !literal)
            {
                a.LoadAddress(kScratch, data);
                a.Ldr(rd, kScratch, kCallsFramesField);
            }
            else
            {
                a.LoadConstant(rd, call.arguments[r]);
                if ((call.loads >> r & 1) != 0)
                {
                    a.Ldr(rd, rd);
                }
            }
        }
        CallOnArm9(a, call.routine);

        if (call.stack_zeros != 0 || !call.stack.empty())
        {
            a.Add(kSp, kSp, 4 * (call.stack_zeros + static_cast<uint32_t>(call.stack.size())));
        }
    }
}

// The stub of a game that starts a song by calls: each call with its arguments and the song's number, then for music
// with a change each call of `change_calls`, or for a short piece each call of `me_calls`, then for each frame the wait
// and the game's work, or else sleeps. A stub in Thumb code starts by going on in ARM state.
DseStub BuildCallsStub(const DseCode& code, const DseStart& start)
{
    constexpr int kScratch = 12;
    arm::ArmAssembler a(StubAddress(code));
    const int data = a.NewLabel();
    const int loop = a.NewLabel();
    if (IsThumb(code.hook) && code.stub == 0)
    {
        a.Word(kThumbToArm);
    }

    // Music, then its change where the change's volume isn't 0.
    const bool change = code.change_calls.front().routine != 0;
    const auto music = [&]
    {
        MakeCalls(a, code.calls, data);
        if (change)
        {
            a.LoadAddress(kScratch, data);
            a.Ldr(kScratch, kScratch, kCallsChangeField);
            a.Cmp(kScratch, 0);
            a.B(loop, arm::Condition::kEq);
            MakeCalls(a, code.change_calls, data);
        }
    };

    const bool me_calls = code.me_calls.front().routine != 0;
    if (me_calls)
    {
        const int me = a.NewLabel();
        a.LoadAddress(kScratch, data);
        a.Ldr(kScratch, kScratch, kCallsMeField);
        a.Cmp(kScratch, 0);
        a.B(me, arm::Condition::kNe);
        music();
        a.B(loop);
        a.Bind(me);
        MakeCalls(a, code.me_calls, data);
    }
    else
    {
        music();
    }

    // The main thread runs the frames or sleeps from then on.
    a.Bind(loop);
    if (code.frame_wait != 0)
    {
        CallOnArm9(a, code.frame_wait);
        MakeCalls(a, code.frame_calls, data);
    }
    else
    {
        a.LoadConstant(0, kSleepMs);
        CallOnArm9(a, code.sleep);
    }
    a.B(loop);

    a.Pool();
    a.Bind(data);
    a.Word(start.number);
    const bool level = DseTakesLevel(code);
    if (me_calls || level || change)
    {
        a.Word(start.me ? 1 : 0);
    }

    if (level || change)
    {
        a.Word(start.level);
    }

    if (change)
    {
        a.Word(start.change ? static_cast<uint32_t>(start.change->volume) : 0);
        a.Word(start.change ? static_cast<uint32_t>(start.change->frames) : 0);
    }

    DseStub stub;
    stub.start_offset = a.Address(data) - StubAddress(code);
    stub.bytes = a.Finish();

    return stub;
}

} // namespace

DseStub BuildDseStreamStub(const DseCode& code, const DseStream& stream)
{
    if (code.start != DseStartKind::kCalls)
    {
        return BuildDseStub(code, {.name = stream.name});
    }

    DseCode stream_code = code;
    stream_code.calls = code.streams[stream.kind].calls;
    stream_code.me_calls = {};

    return BuildCallsStub(stream_code, {.number = stream.number});
}

DseStub BuildDseStub(const DseCode& code, const DseStart& start)
{
    if (code.start == DseStartKind::kName)
    {
        return BuildNamedStub(code, start);
    }

    if (code.start == DseStartKind::kCalls)
    {
        return BuildCallsStub(code, start);
    }

    constexpr int kData = 4; // the data's address, kept across the calls
    constexpr uint32_t kNumberField = 0;
    constexpr uint32_t kMeField = 4;
    constexpr uint32_t kLevelField = 8;

    arm::ArmAssembler a(code.hook);
    const int data = a.NewLabel();
    const int me = a.NewLabel();
    const int sleep = a.NewLabel();

    // The start of the song: the game's start of music, or of a short piece.
    a.LoadAddress(kData, data);
    a.Ldr(0, kData, kNumberField);
    a.Ldr(1, kData, kMeField);
    a.Cmp(1, 0);
    a.B(me, arm::Condition::kNe);
    if (code.play_bgm_level != 0)
    {
        // At the level, with no fade.
        a.Mov(1, 0);
        a.Ldr(2, kData, kLevelField);
        CallOnArm9(a, code.play_bgm_level);
    }
    else
    {
        CallOnArm9(a, code.play_bgm);
    }
    a.B(sleep);
    a.Bind(me);
    CallOnArm9(a, code.play_me);

    // The main thread sleeps from then on. The game's threads play the song.
    a.Bind(sleep);
    a.LoadConstant(0, kSleepMs);
    CallOnArm9(a, code.sleep);
    a.B(sleep);

    a.Pool();
    a.Bind(data);
    a.Word(start.number);
    a.Word(start.me ? 1 : 0);
    if (code.play_bgm_level != 0)
    {
        a.Word(start.level);
    }

    DseStub stub;
    stub.start_offset = a.Address(data) + kNumberField - code.hook;
    stub.bytes = a.Finish();

    return stub;
}

std::vector<uint8_t> DseStartWords(const DseStart& start, const DseStart& library)
{
    if (!start.name.empty())
    {
        return NameBytes(start);
    }

    const bool change = !start.me && start.change != library.change;
    const bool level = (!start.me && start.level != library.level) || change;
    std::vector<uint8_t> words;
    xsflib::AppendLe32(words, start.number);
    if (start.me || library.me || level)
    {
        xsflib::AppendLe32(words, start.me ? 1 : 0);
    }

    if (level)
    {
        xsflib::AppendLe32(words, start.level);
    }

    if (change)
    {
        xsflib::AppendLe32(words, start.change ? static_cast<uint32_t>(start.change->volume) : 0);
        xsflib::AppendLe32(words, start.change ? static_cast<uint32_t>(start.change->frames) : 0);
    }

    return words;
}

bool SameDseFiles(std::span<const ntdlib::dse::NamedFile> files, const ntdlib::dse::SequenceSet& a,
                  const ntdlib::dse::SequenceSet& b)
{
    const auto same = [files](std::size_t x, std::size_t y)
    {
        return std::ranges::equal(files[x].bytes, files[y].bytes);
    };
    if (!same(a.file, b.file) || a.banks.size() != b.banks.size() || a.main_bank.has_value() != b.main_bank.has_value())
    {
        return false;
    }

    for (std::size_t i = 0; i < a.banks.size(); i++)
    {
        if (!same(a.banks[i], b.banks[i]))
        {
            return false;
        }
    }

    return !a.main_bank || same(*a.main_bank, *b.main_bank);
}

bool SameDseStart(std::span<const ntdlib::dse::NamedFile> files, const ntdlib::dse::SequenceSet& a,
                  std::span<const int> a_volumes, const ntdlib::dse::SequenceSet& b, std::span<const int> b_volumes)
{
    return std::ranges::equal(a_volumes, b_volumes) && SameDseFiles(files, a, b);
}

void CheckDseStubPlace(const DseCode& code, const DseStub& stub)
{
    // At the hook, the stub or the jump to it.
    const std::size_t at_hook = code.stub != 0 ? JumpToStub(code).size() : stub.bytes.size();
    if (at_hook > code.hook_end - CodeAddress(code.hook))
    {
        throw std::runtime_error("the stub takes " + std::to_string(at_hook) +
                                 " bytes, and the code after the hook has " +
                                 std::to_string(code.hook_end - CodeAddress(code.hook)));
    }

    if (code.stub != 0 && stub.bytes.size() > code.stub_end - code.stub)
    {
        throw std::runtime_error("the stub takes " + std::to_string(stub.bytes.size()) + " bytes, and its place has " +
                                 std::to_string(code.stub_end - code.stub));
    }
}

DseSet MakeDseSet(const ntr::Rom& rom, const DseCode& code, const DseStub& stub,
                  const std::map<uint32_t, std::vector<uint8_t>>& files)
{
    CheckDseStubPlace(code, stub);
    const std::vector<uint8_t> at_hook = code.stub != 0 ? JumpToStub(code) : stub.bytes;

    ntr::CompactParts parts{.arm9 = ntr::UncompressedArm9(rom), .files = files};
    const uint32_t base = rom.Header().arm9.ram_address;

    // The stub away from the hook goes in the code that contains the hook: the ARM9 binary's static part, or the hook's
    // overlay. The overlay goes in decompressed, with the stub.
    const ntr::Overlay* const overlay = OverlayAt(rom, code, CodeAddress(code.hook));
    if (code.stub != 0 && OverlayAt(rom, code, code.stub) != overlay)
    {
        throw std::runtime_error("a stub away from the hook requires both in the ARM9 binary or both in one overlay");
    }

    std::span<uint8_t> hook_code = parts.arm9;
    uint32_t hook_code_base = base;
    if (overlay != nullptr)
    {
        const auto file = parts.files.find(overlay->file_id);
        if (file == parts.files.end())
        {
            throw std::runtime_error("the library doesn't keep the overlay of the hook");
        }
        file->second = ntr::OverlayBytes(rom.FileData(overlay->file_id), *overlay);
        hook_code = file->second;
        hook_code_base = overlay->ram_address;
    }

    std::ranges::copy(at_hook, hook_code.begin() + (CodeAddress(code.hook) - hook_code_base));
    if (code.stub != 0)
    {
        std::ranges::copy(stub.bytes, hook_code.begin() + (code.stub - hook_code_base));
    }

    if (code.frame_cut != 0)
    {
        xsflib::StoreLe32(&parts.arm9[code.frame_cut - base], Branch(code.frame_cut, code.frame_signal));
    }

    if (code.bgm_first != 0)
    {
        uint8_t* const first_bgm = &parts.arm9[code.bgm_first - base];
        xsflib::StoreLe32(first_bgm, xsflib::LoadLe32(first_bgm) & ~kImmediateBits);
    }

    DseSet set;
    set.image = ntr::WriteCompactImage(rom, parts);
    if (overlay == nullptr)
    {
        set.stub = rom.Header().arm9.rom_offset + (StubAddress(code) - base);
        set.start_word = set.stub + stub.start_offset;
        return set;
    }

    const auto index = static_cast<std::size_t>(overlay - rom.Arm9Overlays().data());
    ntr::ClearArm9OverlayFlags(set.image, index);
    const ntr::Rom written = ntr::Rom::Parse(set.image);
    set.stub = written.Files()[overlay->file_id].start + (StubAddress(code) - overlay->ram_address);
    set.start_word = set.stub + stub.start_offset;

    return set;
}

DseMiniRun RunDseMini(std::span<const uint8_t> image, const DseCode& code, uint64_t hook_clock,
                      std::span<const uint8_t> bank, std::span<const uint8_t> song, const ntdlib::dse::SongState& state,
                      double seconds, const ntr::BackupMemory& backup, const std::map<uint32_t, FileRanges>& packs)
{
    DseMiniRun run;
    const ntr::Rom rom = ntr::Rom::Parse(image);
    const auto machine = std::make_unique<ntr::Machine>(rom, backup);
    const auto fail = [&run](std::string error)
    {
        if (!run.error)
        {
            run.error = std::move(error);
        }
    };

    // After the hook, each card read must reach the file system's tables or a file that the set keeps.
    const std::vector<std::pair<uint32_t, uint32_t>> kept = KeptRanges(rom, packs);
    bool started = false;
    machine->on_card_read = [&](uint32_t address, uint32_t bytes)
    {
        if (machine->Clock() < hook_clock)
        {
            return;
        }

        if (!Reaches(kept, address, bytes))
        {
            fail(ReadOutsideSetError(address));
        }

        if (started)
        {
            return;
        }

        for (const uint32_t id : rom.FilesIn(address, address + bytes))
        {
            const ntr::RomFile& file = rom.Files()[id];
            AddRange(run.reads[id], std::max(address, file.start) - file.start,
                     std::min(address + bytes, file.end) - file.start);
        }
    };

    // From the hook, DSE loads the bank, then the song, then starts the song: each load's first argument is the file's
    // bytes.
    struct Step
    {
        uint32_t routine;
        const char* what;
        std::span<const uint8_t> file;
    };
    const Step bank_step = {code.load_bank, "the bank", bank};
    const Step song_step = {code.load_song, "the song", song};
    const Step steps[] = {code.song_before_bank ? song_step : bank_step, code.song_before_bank ? bank_step : song_step};
    std::vector<ntr::SoundOutput> out;
    const auto limit = static_cast<uint64_t>(seconds * ntr::kSystemClockHz);
    machine->Arm9().SetStopAddress(CodeAddress(code.hook));
    if (!machine->Run(limit, out))
    {
        fail("the boot doesn't reach the hook within " + std::to_string(static_cast<int>(seconds)) + " s");
        return run;
    }

    out.clear();
    for (const Step& step : steps)
    {
        machine->Arm9().SetStopAddress(CodeAddress(step.routine));
        if (!machine->Run(limit, out))
        {
            fail(std::string("DSE doesn't load ") + step.what + " within " + std::to_string(static_cast<int>(seconds)) +
                 " s of the boot");
            return run;
        }

        if (!ntr::MainMemoryContains(machine->MainMemory(), machine->Arm9().Reg(0), step.file))
        {
            fail(std::string("DSE loads ") + step.what + " at " + xsflib::Hex(machine->Arm9().Reg(0), 8, true) +
                 ", and the bytes there aren't the set's file");
        }
        out.clear();
    }

    machine->Arm9().SetStopAddress(CodeAddress(code.start_song));
    if (!machine->Run(limit, out))
    {
        fail("DSE doesn't start the song within " + std::to_string(static_cast<int>(seconds)) + " s of the boot");
        return run;
    }
    run.start_ms = static_cast<double>(machine->Clock()) * 1000.0 / ntr::kSystemClockHz;
    started = true;
    out.clear();

    // The game's code starts the song at the level of the mini's state, or of the start before its change.
    ntdlib::arm::Cpu& cpu = machine->Arm9();
    const auto volume_of_call = [&](uint32_t parameters)
    {
        constexpr uint32_t kVolumeOffset = 0xC;
        constexpr int kVolumeBytes = 2;
        return code.volume_in_parameters ? cpu.Load(parameters + kVolumeOffset, kVolumeBytes) : parameters;
    };
    const auto check_volume = [&](const char* what, uint32_t volume, int level)
    {
        const auto expected = static_cast<uint32_t>(ntdlib::dse::StartOf({.suffix = "", .level = level}).volume);
        if (volume != expected)
        {
            fail(std::string(what) + " the volume " + std::to_string(volume) + ", and the level " +
                 std::to_string(level) + " gives " + std::to_string(expected));
        }
    };
    const uint32_t volume = volume_of_call(cpu.Reg(1));
    run.volume = static_cast<int16_t>(volume);
    check_volume("DSE starts the song at", volume, state.change ? state.change->start_level : state.level);
    const uint64_t start_clock = machine->Clock();

    // The change sets the song's volume on each of its frames, and reaches the level of the state on the last. Each
    // call of DSE's change runs to its return before the next stop at its start.
    if (state.change)
    {
        const uint64_t change_end = start_clock + static_cast<uint64_t>(kDseChangeSeconds * ntr::kSystemClockHz);
        for (int frame = 0; frame < state.change->frames; frame++)
        {
            machine->Arm9().SetStopAddress(CodeAddress(code.set_song));
            if (!machine->Run(change_end, out))
            {
                fail("DSE changes the song's volume " + std::to_string(frame) + " of " +
                     std::to_string(state.change->frames) + " times within " +
                     std::to_string(static_cast<int>(kDseChangeSeconds)) + " s of its start");
                return run;
            }
            run.volume = static_cast<int16_t>(volume_of_call(cpu.Reg(1)));
            machine->Arm9().SetStopAddress(cpu.Reg(14) & ~1U);
            machine->Run(change_end, out);
        }
        check_volume("The change gives the song", static_cast<uint32_t>(run.volume), state.level);
    }

    // The mini plays on until the game stops the song, for at most kDseMiniPlaySeconds.
    machine->Arm9().SetStopAddress(CodeAddress(code.stop_song));
    const uint64_t end = start_clock + static_cast<uint64_t>(kDseMiniPlaySeconds * ntr::kSystemClockHz);
    if (machine->Run(end, out))
    {
        run.stop_ms = static_cast<double>(machine->Clock() - start_clock) * 1000.0 / ntr::kSystemClockHz;
    }

    OutputWatch watch;
    watch.Add(out);
    run.sounded = watch.Sounded();

    return run;
}

} // namespace twosf::rip
