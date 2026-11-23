// SPDX-License-Identifier: MIT

// The entries of the table of games (rip/game_start_table.h) for The Legend of Zelda: Spirit Tracks (USA and Europe)
// and Phantom Hourglass (USA). Each game starts its music through a manager of its own, and a scene of the game can set
// a state of the piece that plays: its mixer's state, its tracks' volumes and mutes, its tempo, or the section that the
// piece starts at. A set calls the same routines with the same arguments. ntdlib's profiles of the two games
// (ntdlib::ntr::GameProfileOf) are the check of what each mini sends.

#pragma once

#include <array>
#include <cstdint>

#include "rip/game_start_table.h"

namespace twosf::rip::zelda
{

// The Legend of Zelda: Spirit Tracks. The BGM manager (0x020B5198 in the USA release, 0x020B51B8 in the European one:
// the literal at 0x02069C50 or 0x02069C70 of overlay 0: 0x02069C24 passes it to the manager's set-up) starts every
// piece of player 0 with 0x0206C988(manager, sequence, fade, volume, state), the state on the stack. It returns for a
// sequence whose record names another player (0x0206C9C0). It swaps some requests for another sequence in another state
// of its mixer, and gives SEQ_27, 39, 87, 88, 96 and 97 a state of its own (ntdlib's profile has the rules). It starts
// the piece through 0x0206BB10(handle, sequence, fade, volume) on its handle at 8 (add r0, r9, #8 at 0x0206D02C), then
// sets the mixer's state at once (the mixer's vtable at +0x0C, 0x0206B32C). The mixer's state sets the player's volume,
// the tempo's ratio, every track's pitch and each track's volume, in the start's list of commands. A request of SEQ_39
// that doesn't follow SEQ_37 or 0 (the last request at +0x20, 0x0206CF50 to 0x0206CF60), and of SEQ_37 that follows
// SEQ_39 or 54, also writes the kept section to variable 0 (NNS_SndPlayerWriteVariable at 0x02002F9C, from 0x0206D07C).
// At the hook nothing has played. A set's start therefore writes no variable.
//
// Before a start, the manager plays some requests as another sequence by the story's flags and the scene (0x0206C9CC to
// 0x0206CB70): SEQ_44 and 45 play SEQ_46 and 47 without flag 0x20, SEQ_50 and 51 play SEQ_52 and 53 without flag 0x80,
// and SEQ_76 plays SEQ_77 outside scenes 19 and 66. At the hook the pointers to the flags and the scene (0x027E09B8 and
// 0x027E09A4) are still 0. The manager therefore plays the other sequence. A set starts those five as the manager
// starts them where the game plays them, after the swap: 0x0206BB10 on the manager's handle at 8 with the sequence that
// the manager plays, then the mixer's state of the start (0x0206CBC8 to 0x0206CF44). SEQ_44, 50 and 76 play themselves
// in state 0, SEQ_45 plays SEQ_44 in state 10 and SEQ_51 plays SEQ_50 in state 13.
//
// The manager's other rules of that kind, for SEQ_18, 19, 37 to 40 and 60, read their flags through the same null
// pointers at the hook: they read the bytes at 0x00 to 0x1C of the instruction TCM. Those bytes happen to leave the
// requests alone, and a set's start plays the sequence that the mini names. A change of those bytes could swap a
// request. The run's check of the prepared sequence and the profile's check would then fail the rip.
//
// The pieces of player 1 start through 0x0206D18C. That routine calls 0x0206BB10 on the manager's handle at 0x0C (add
// r0, r5, #12 at 0x0206D1AC) at volume 127, with a fade-in of 70 frames for SEQ_28 alone. A set starts every piece of
// another player than 0 with 0x0206BB10 on that handle, with no fade: a fade at a start is the scene's action, not a
// state of the piece. 0x0206BB10 loads the piece through the game's loader (0x0206A014) when it isn't loaded.
//
// The states that the scenes set (ntdlib's kSpiritTracksPieces and kSpiritTracksScenes):
// - SEQ_65 starts in state 22 (the state of its handler's height bands, 0x0206B668), SEQ_80 in state 15 (overlay 91's
//   frame fades it there from 0) and SEQ_153 in state 26 (overlay 23, 0x020EB470). A set starts each in that state.
// - State 0 turns the mixer off (0x0206A82C stores 0 at +0x0C), and the mixer then sends no track's volume: only the
//   frames of a fade take the tracks back to 127. A set therefore starts SEQ_6 and SEQ_18 in state 0, as the game
//   does with nothing playing, and then sets the state of each mini: the set of a state from state 0 sends every value,
//   as a start in the state does. SEQ_6 starts in state 19 (overlay 96, 0x02179D04), and its scene fades it to state
//   0 and back (0x02179D2E): _FULL keeps state 0. SEQ_18 starts in the manager's train state at +0x48, 1 at first.
// - A state with a suffix: the mixer's state set at once through 0x0206B32C(mixer, state) after the start, as a scene
//   sets it through the manager's 0x0206D160 (a fade that a set leaves out). The mixer is the word at the manager's
//   address (ldr r0, [r0] at 0x0206D164).
// - SEQ_18's handler (0x0206B3C8) mutes tracks by the train's state in each frame (0x0206B4A0 to 0x0206B510): state 1
//   unmutes 0xFFDF and mutes 0x0020, state 2 unmutes 0xFFFF, state 3 unmutes 0x02E2 and mutes 0x3101, each through
//   NNS_SndPlayerSetTrackMute (0x02002DE8, from 0x0206B4B8) on the manager's handle. A set makes the same calls once
//   after the start. Nothing else in a set changes the mutes.
// - _RESUME: the manager's start of a piece at the kept section 0 (0x0206D0CC, and 0x0206C988 for the requests above)
//   writes variable 0 with NNS_SndPlayerWriteVariable(handle, 0, 0). SEQ_39 and SEQ_40 take that start as their base:
//   the manager writes it on every start but those that follow SEQ_37 or 38.
// - _INTRO: SEQ_39's and SEQ_18's handlers play the intro in state 1's level and tempo with every track at 127, then
//   fade to the mixer's state at a tick (504 and 456). The handlers run in the mixer's update (0x0206B348, the vtable's
//   +0x08), once in each game frame, while the mixer's flags at 0x98 and 0x99 are clear: a start through 0x0206C988
//   clears them, and a write of the kept section sets 0x99 (0x0206D088). The game runs a frame in every second frame of
//   the screen. A set calls the mixer's update in each game frame after the start. SEQ_18's intro rests on the station
//   assumption of ntdlib's profile: SEQ_18 starts in state 0 with nothing playing, as from
//   0x0206C2A0, where the train has just stopped. Overlay 26's train sound update (0x0212E584) then sets state 2 in
//   the first game frame and state 3 once the train has stood for 60 frames, each through the manager's
//   0x0206D160(manager, state), after the mixer's update of that frame. A set makes those two calls in frames 1 and 60.
//   No run of the real scene shows that the game starts SEQ_18 that way.
// - Streams in cutscenes (ntdlib's kUnderStereoStream and kStreamLocks): the preparation of a stereo stream locks
//   channels 6 and 7 through NNS_SndLockChannel (0x020036A8, called from 0x02006560), and the stream's stop unlocks
//   them through NNS_SndUnlockChannel (0x020036EC, from 0x02006590). A set makes the same calls: before the start for
//   SEQ_279 to SEQ_282 and SEQ_304, and in a later game frame for SEQ_184 (frame 82), SEQ_194 (292), SEQ_251 (795) and
//   SEQ_304 (an unlock in frame 1, a lock in frame 40). Those are the ticks of the cutscenes, one in each game frame.
//   The profile gives each lock's frame of the driver. A list from the ARM9 reaches the driver only at a game frame,
//   6.42 of the driver's frames, and the check takes the game frame nearest the profile's frame.
//
// Effects: the game requests an effect by an id, the sequence archive times 1,024 plus the index (0x0206D304 and
// 0x0206D30C), through 0x0206DB24(handle, id), from 259 call sites, 192 of them on the manager's handle at 0x5C (the
// literal at 0x02069C54: the manager's set-up passes it to the effects' set-up). The request calls the effects' start
// 0x0206D31C(handle, id) (0x0206DBC0). The effects' start calls the SDK's NNS_SndArcPlayerStartSeqArc (0x02004AE0) with
// the archive and the index unless a byte at 0x9E of 0x020B50A0 is set. After a start of id 2, the request writes 180
// to variable 0 (0x0206DBC4 to 0x0206DBD8): effect 0-2 waits that many ticks before its end. While an effect plays, the
// request plays the next one of ids 8, 131 and 0x901B to 0x901D in its place. With nothing playing, it plays the id.
// The game's scenes load the archives and the banks of their effects through groups. A set loads the entry's archive
// and its bank with the SDK's internal loaders (0x02004190 and 0x020041F0, with every kind of data, the heap, 1 and a
// null pointer, as the group loader 0x02004010 calls them), then calls the request.
//
// Revision 1 of each release has the sound manager's code at the same addresses, and the SDK's routines that a set
// calls in the ARM9 binary are the same. In overlay 0 of revision 1, the data from 0x020B1934 of the USA release on is
// 0x40 bytes higher, the mixer's vtable among it, and so does the manager. The table has an entry for each revision,
// and the words of an entry pick it.
struct SpiritTracksAddresses
{
    uint32_t shift = 0;      // the European overlay 0 is 0x20 bytes higher
    uint32_t data_shift = 0; // the shift of the mixer's vtable
    uint32_t manager = 0;    // the BGM manager
};

constexpr SpiritTracksAddresses kBkie = {.shift = 0, .data_shift = 0, .manager = 0x020B5198};
constexpr SpiritTracksAddresses kBkip = {.shift = 0x20, .data_shift = 0x20, .manager = 0x020B51B8};
constexpr SpiritTracksAddresses kBkieRev1 = {.shift = 0, .data_shift = 0x40, .manager = 0x020B51D8};
constexpr SpiritTracksAddresses kBkipRev1 = {.shift = 0x20, .data_shift = 0x60, .manager = 0x020B51F8};

constexpr uint32_t kStManagerStart = 0x0206C988;
constexpr uint32_t kStHandleStart = 0x0206BB10;
constexpr uint32_t kStMixerState = 0x0206B32C;
constexpr uint32_t kStMixerUpdate = 0x0206B348;
constexpr uint32_t kStStateFade = 0x0206D160;
constexpr uint32_t kStSetTrackMute = 0x02002DE8; // the SDK's, in the ARM9 binary: the same in both releases
constexpr uint32_t kStWriteVariable = 0x02002F9C;
constexpr uint32_t kStMusicHandle = 8;
constexpr uint32_t kStSecondHandle = 0x0C;
constexpr uint32_t kStVideoFrames = 2;
constexpr uint32_t kStLoadSequenceArchive = 0x02004190; // the SDK's, in the ARM9 binary: the same in both releases
constexpr uint32_t kStLoadBank = 0x020041F0;
constexpr uint32_t kStEffectRequest = 0x0206DB24;
constexpr uint32_t kStEffectStart = 0x0206D31C;
constexpr uint32_t kStEffectHandle = 0x5C;
constexpr uint32_t kStEffectIdShift = 10;
constexpr uint32_t kEveryKind = 0xFF;
constexpr uint32_t kStTrainStarts = 1;
constexpr uint32_t kStTrainStands = 60;
constexpr uint32_t kStLockChannels = 0x020036A8;   // NNS_SndLockChannel: the same in both releases
constexpr uint32_t kStUnlockChannels = 0x020036EC; // NNS_SndUnlockChannel
constexpr uint32_t kStStreamChannels = 0xC0;

constexpr StubCall StStart(const SpiritTracksAddresses& at, uint32_t state)
{
    return MakeCall(kStManagerStart + at.shift,
                    {Constant(at.manager), Sound(), Constant(0), Constant(127), Constant(state)});
}

constexpr StubCall StOtherPlayer(const SpiritTracksAddresses& at)
{
    return MakeCall(kStHandleStart + at.shift,
                    {Constant(at.manager + kStSecondHandle), Sound(), Constant(0), Constant(127)});
}

constexpr StubCall StOnMusicHandle(const SpiritTracksAddresses& at, StubArgument sequence)
{
    return MakeCall(kStHandleStart + at.shift,
                    {Constant(at.manager + kStMusicHandle), sequence, Constant(0), Constant(127)});
}

constexpr StubCall StState(const SpiritTracksAddresses& at, uint32_t state)
{
    return MakeCall(kStMixerState + at.shift, {WordOf(at.manager), Constant(state)});
}

constexpr StubCall StMixerUpdate(const SpiritTracksAddresses& at)
{
    return MakeCall(kStMixerUpdate + at.shift, {WordOf(at.manager)});
}

constexpr StubCall StStateFade(const SpiritTracksAddresses& at, uint32_t state)
{
    return MakeCall(kStStateFade + at.shift, {Constant(at.manager), Constant(state)});
}

constexpr StubCall StMute(const SpiritTracksAddresses& at, uint32_t tracks, uint32_t on)
{
    return MakeCall(kStSetTrackMute, {Constant(at.manager + kStMusicHandle), Constant(tracks), Constant(on)});
}

// A load of the SDK's internal loaders: (number, every kind of data, heap, 1, null).
constexpr StubCall SdkLoad(uint32_t loader, StubArgument number)
{
    return MakeCall(loader, {number, Constant(kEveryKind), Heap(), Constant(1), Constant(0)});
}

// The lock or the unlock of the channels of a stereo stream, as the stream's preparation and its stop make them.
constexpr StubCall StStreamChannels(bool lock)
{
    return MakeCall(lock ? kStLockChannels : kStUnlockChannels, {Constant(kStStreamChannels)});
}

constexpr StubCall StResume(const SpiritTracksAddresses& at)
{
    return MakeCall(kStWriteVariable, {Constant(at.manager + kStMusicHandle), Constant(0), Constant(0)});
}

// The lists of a release's starts and states.
template <const SpiritTracksAddresses& At>
struct SpiritTracks
{
    static constexpr std::array<StubCall, 1> kCalls = {StStart(At, 0)};
    static constexpr std::array<StubCall, 1> kInState15 = {StStart(At, 15)};
    static constexpr std::array<StubCall, 1> kInState22 = {StStart(At, 22)};
    static constexpr std::array<StubCall, 1> kInState26 = {StStart(At, 26)};
    static constexpr std::array<StubCall, 1> kOtherPlayers = {StOtherPlayer(At)};
    static constexpr std::array<StubCall, 3> kEffectCalls = {
        SdkLoad(kStLoadSequenceArchive, ArchiveNumber()), SdkLoad(kStLoadBank, BankNumber()),
        MakeCall(kStEffectRequest + At.shift, {Constant(At.manager + kStEffectHandle), PackedEntry(kStEffectIdShift)})};
    static constexpr std::array<StubCall, 2> kItself = {StOnMusicHandle(At, Sound()), StState(At, 0)};
    static constexpr std::array<StubCall, 2> kAsSeq44 = {StOnMusicHandle(At, Constant(44)), StState(At, 10)};
    static constexpr std::array<StubCall, 2> kAsSeq50 = {StOnMusicHandle(At, Constant(50)), StState(At, 13)};

    static constexpr std::array<std::pair<uint32_t, uint32_t>, 1> kSeq65 = {{{65, 65}}};
    static constexpr std::array<std::pair<uint32_t, uint32_t>, 1> kSeq80 = {{{80, 80}}};
    static constexpr std::array<std::pair<uint32_t, uint32_t>, 1> kSeq153 = {{{153, 153}}};
    static constexpr std::array<std::pair<uint32_t, uint32_t>, 3> kSwappedAlone = {{{44, 44}, {50, 50}, {76, 76}}};
    static constexpr std::array<std::pair<uint32_t, uint32_t>, 1> kSeq45 = {{{45, 45}}};
    static constexpr std::array<std::pair<uint32_t, uint32_t>, 1> kSeq51 = {{{51, 51}}};

    // SEQ_65, 80 and 153 in their states, the requests that the manager plays as another sequence at the hook, then the
    // players of the archive's music besides 0: 1, 2, 3, 5, 8, 11 and 15.
    static constexpr std::array<NumberCallsEntry, 13> kNumbers = {{
        {.ranges = kSeq65, .calls = kInState22},
        {.ranges = kSeq80, .calls = kInState15},
        {.ranges = kSeq153, .calls = kInState26},
        {.ranges = kSwappedAlone, .calls = kItself},
        {.ranges = kSeq45, .calls = kAsSeq44},
        {.ranges = kSeq51, .calls = kAsSeq50},
        {.player = 1, .calls = kOtherPlayers},
        {.player = 2, .calls = kOtherPlayers},
        {.player = 3, .calls = kOtherPlayers},
        {.player = 5, .calls = kOtherPlayers},
        {.player = 8, .calls = kOtherPlayers},
        {.player = 11, .calls = kOtherPlayers},
        {.player = 15, .calls = kOtherPlayers},
    }};

    static constexpr std::array<StubCall, 3> kSeq18 = {StState(At, 1), StMute(At, 0xFFDF, 0), StMute(At, 0x0020, 1)};
    static constexpr std::array<StubCall, 4> kSeq18Resume = {StState(At, 1), StMute(At, 0xFFDF, 0),
                                                             StMute(At, 0x0020, 1), StResume(At)};
    static constexpr std::array<StubCall, 2> kSeq18Slow = {StState(At, 2), StMute(At, 0xFFFF, 0)};
    static constexpr std::array<StubCall, 3> kSeq18Stopped = {StState(At, 3), StMute(At, 0x02E2, 0),
                                                              StMute(At, 0x3101, 1)};
    static constexpr std::array<StubCall, 1> kMixerUpdate = {StMixerUpdate(At)};
    static constexpr std::array<FrameCall, 2> kTrainAtStation = {
        {{.frame = kStTrainStarts, .call = StStateFade(At, 2)}, {.frame = kStTrainStands, .call = StStateFade(At, 3)}}};
    static constexpr std::array<StubCall, 1> kResume = {StResume(At)};
    static constexpr std::array<StubCall, 1> kState4 = {StState(At, 4)};
    static constexpr std::array<StubCall, 1> kState16 = {StState(At, 16)};
    static constexpr std::array<StubCall, 1> kState17 = {StState(At, 17)};
    static constexpr std::array<StubCall, 1> kState19 = {StState(At, 19)};
    static constexpr std::array<StubCall, 1> kState21 = {StState(At, 21)};
    static constexpr std::array<StubCall, 1> kState23 = {StState(At, 23)};
    static constexpr std::array<StubCall, 1> kState24 = {StState(At, 24)};
    static constexpr std::array<StubCall, 1> kState25 = {StState(At, 25)};
    static constexpr std::array<StubCall, 1> kState27 = {StState(At, 27)};
    static constexpr std::array<StubCall, 1> kState29 = {StState(At, 29)};
    static constexpr std::array<StubCall, 1> kState31 = {StState(At, 31)};
    static constexpr std::array<StubCall, 1> kUnderStream = {StStreamChannels(true)};
    static constexpr std::array<FrameCall, 1> kSeq184Stream = {{{.frame = 82, .call = StStreamChannels(true)}}};
    static constexpr std::array<FrameCall, 1> kSeq194Stream = {{{.frame = 292, .call = StStreamChannels(true)}}};
    static constexpr std::array<FrameCall, 1> kSeq251Stream = {{{.frame = 795, .call = StStreamChannels(true)}}};
    static constexpr std::array<FrameCall, 2> kSeq304Streams = {
        {{.frame = 1, .call = StStreamChannels(false)}, {.frame = 40, .call = StStreamChannels(true)}}};

    // In the order of ntdlib's profile for each label: the variables' rows, then the scenes' rows.
    static constexpr std::array<GameStateEntry, 45> kStates = {{
        {"SEQ_6", "", kState19},
        {"SEQ_6", "_FULL", {}},
        {"SEQ_18", "", kSeq18},
        {"SEQ_18", "_RESUME", kSeq18Resume},
        {.label = "SEQ_18",
         .suffix = "_INTRO",
         .frame_calls = kMixerUpdate,
         .timed_calls = kTrainAtStation,
         .video_frames = kStVideoFrames},
        {"SEQ_18", "_SLOW", kSeq18Slow},
        {"SEQ_18", "_STOPPED", kSeq18Stopped},
        {"SEQ_23", "_RESUME", kResume},
        {"SEQ_23", "_STOPPED", kState4},
        {"SEQ_24", "_RESUME", kResume},
        {"SEQ_27", "_PHASE2", kState21},
        {"SEQ_37", "_RESUME", kResume},
        {"SEQ_38", "_RESUME", kResume},
        {"SEQ_39", "", kResume},
        {.label = "SEQ_39", .suffix = "_INTRO", .frame_calls = kMixerUpdate, .video_frames = kStVideoFrames},
        {"SEQ_40", "", kResume},
        {.label = "SEQ_40", .suffix = "_INTRO", .frame_calls = kMixerUpdate, .video_frames = kStVideoFrames},
        {"SEQ_41", "_RESUME", kResume},
        {"SEQ_59", "_RESUME", kResume},
        {"SEQ_64", "_RESUME", kResume},
        {"SEQ_65", "_HEIGHT2", kState23},
        {"SEQ_65", "_HEIGHT3", kState24},
        {"SEQ_65", "_HEIGHT4", kState25},
        {"SEQ_66", "_RESUME", kResume},
        {"SEQ_67", "_RESUME", kResume},
        {"SEQ_68", "_RESUME", kResume},
        {"SEQ_69", "_RESUME", kResume},
        {"SEQ_70", "_RESUME", kResume},
        {"SEQ_71", "_RESUME", kResume},
        {"SEQ_80", "_CLOSER", kState16},
        {"SEQ_80", "_CLOSEST", kState17},
        {"SEQ_87", "_PHASE2", kState31},
        {"SEQ_88", "_PHASE2", kState31},
        {"SEQ_96", "_PHASE2", kState29},
        {"SEQ_97", "_PHASE2", kState29},
        {"SEQ_153", "_PHASE2", kState27},
        {.label = "SEQ_184", .suffix = "", .timed_calls = kSeq184Stream, .video_frames = kStVideoFrames},
        {.label = "SEQ_194", .suffix = "", .timed_calls = kSeq194Stream, .video_frames = kStVideoFrames},
        {.label = "SEQ_251", .suffix = "", .timed_calls = kSeq251Stream, .video_frames = kStVideoFrames},
        {"SEQ_279", "", kUnderStream},
        {"SEQ_280", "", kUnderStream},
        {"SEQ_281", "", kUnderStream},
        {"SEQ_282", "", kUnderStream},
        {.label = "SEQ_304",
         .suffix = "",
         .calls = kUnderStream,
         .timed_calls = kSeq304Streams,
         .video_frames = kStVideoFrames},
        {"SEQ_351", "_RESUME", kResume},
    }};

    // The first word of each routine that the calls name, the words that give the handles' offsets, the manager's
    // literal, the checks of SEQ_39's request (cmp r8, #39 and cmp r0, #37), the handler's mutes (ldr r0, [r4, #8] at
    // 0x0206B4A0, mov r1, #32 at 0x0206B4C0) and their call of the SDK's setter, the manager's call of the variable's
    // write, the mixer's vtable (the update at +0x08, the set of a state at +0x0C), the effects' handle, the SDK's
    // loaders, the locks of the channels, the request's call of the effects' start and its write of variable 0 for id
    // 2, and the start's split of the id and its call of NNS_SndArcPlayerStartSeqArc.
    static constexpr std::array<GameWord, 33> kWords = {{
        {kStManagerStart + At.shift, 0xE92D4FF8, 0},                // stmfd sp!, {r3-r11, lr}
        {kStHandleStart + At.shift, 0xE92D43F8, 0},                 // stmfd sp!, {r3-r9, lr}
        {kStMixerState + At.shift, 0xE92D4010, 0},                  // stmfd sp!, {r4, lr}
        {kStMixerUpdate + At.shift, 0xE92D4010, 0},                 // stmfd sp!, {r4, lr}
        {kStStateFade + At.shift, 0xE92D4008, 0},                   // stmfd sp!, {r3, lr}
        {kStSetTrackMute, 0xE92D4008},                              // stmfd sp!, {r3, lr}
        {kStWriteVariable, 0xE92D4008},                             // stmfd sp!, {r3, lr}
        {0x0206D02C + At.shift, 0xE2890008, 0},                     // add r0, r9, #8
        {0x0206D1AC + At.shift, 0xE285000C, 0},                     // add r0, r5, #12
        {0x02069C50 + At.shift, At.manager, 0},                     // the manager's literal
        {0x0206D164 + At.shift, 0xE5900000, 0},                     // ldr r0, [r0]
        {0x0206CF50 + At.shift, 0xE3580027, 0},                     // cmp r8, #39
        {0x0206CF5C + At.shift, 0xE3500025, 0},                     // cmp r0, #37
        {0x0206B4A0 + At.shift, 0xE5940008, 0},                     // ldr r0, [r4, #8]
        {0x0206B4C0 + At.shift, 0xE3A01020, 0},                     // mov r1, #32
        {0x0206B4B8 + At.shift, 0xEBFE5E4A - At.shift / 4, 0},      // bl NNS_SndPlayerSetTrackMute
        {0x0206D07C + At.shift, 0xEBFE57C6 - At.shift / 4, 0},      // bl NNS_SndPlayerWriteVariable
        {0x020B2144 + At.data_shift, kStMixerUpdate + At.shift, 0}, // the vtable's update
        {0x020B2148 + At.data_shift, kStMixerState + At.shift, 0},  // the vtable's set of a state
        {0x02069C54 + At.shift, At.manager + kStEffectHandle, 0},   // the effects' handle
        {kStLockChannels, 0xE92D4038},                              // stmfd sp!, {r3-r5, lr}
        {kStUnlockChannels, 0xE92D4010},                            // stmfd sp!, {r4, lr}
        {kStLoadSequenceArchive, 0xE92D4070},                       // stmfd sp!, {r4-r6, lr}
        {kStLoadBank, 0xE92D4FF8},                                  // stmfd sp!, {r3-r11, lr}
        {kStEffectRequest + At.shift, 0xE92D4070, 0},               // stmfd sp!, {r4-r6, lr}
        {0x0206DBC0 + At.shift, 0xEBFFFDD5, 0},                     // bl 0x0206D31C
        {0x0206DBC4 + At.shift, 0xE3550002, 0},                     // cmp r5, #2
        {0x0206DBD4 + At.shift, 0xE3A020B4, 0},                     // mov r2, #180
        {0x0206DBD8 + At.shift, 0xEBFE54EF - At.shift / 4, 0},      // bl NNS_SndPlayerWriteVariable
        {kStEffectStart + At.shift, 0xE92D4070, 0},                 // stmfd sp!, {r4-r6, lr}
        {0x0206D304 + At.shift, 0xE1A00540, 0},                     // asr r0, r0, #10
        {0x0206D318 + At.shift, 0x000003FF, 0},                     // the index's mask
        {0x0206D35C + At.shift, 0xEBFE5DDF - At.shift / 4, 0},      // bl NNS_SndArcPlayerStartSeqArc
    }};
};

// The Legend of Zelda: Phantom Hourglass (USA). The sound manager (0x020EEC08: the literal at 0x020D64C4 of overlay 0)
// starts music with 0x020D7044(manager, sequence, fade, volume), at volume 127 from 76 of its 81 call sites. It picks
// the handle by the record's player: 8 for player 1 (add r8, r7, #8 at 0x020D70C4), 4 for player 2 (add r8, r7, #4 at
// 0x020D70D0) and the manager's address for the others, then calls 0x020D6CD0(manager, handle, sequence, fade, volume),
// the volume on the stack. It refuses SEQ_101 (cmpne r6, #101 at 0x020D7060). Overlay 15's music object for the sea
// calls 0x020D6CD0 for SEQ_101 on the manager's handle, and sets the tracks' volumes, their mutes and the tempo by a
// mode (ntdlib's profile, AddSeaModes): through the SDK's NNS_SndPlayerSetTrackVolume (0x0201F83C, with a volume from 0
// to 127), NNS_SndPlayerSetTrackMute (0x0201F81C) and NNS_SndPlayerSetTempoRatio (0x0201F8AC).
//
// - SEQ_101 sails in mode 2, the archive's mix. _STOPPED is mode 1: 0x033A at 64, 0x04D5 at 0 and muted, the tempo's
//   ratio at 250.
// - SEQ_103 plays in the fog on the second handle, with track 7 at 0 (0x02185154 to 0x02185174) unless the Ghost Ship
//   is near: _GHOST_SHIP sets it to 127 (0x021855B8).
//
// Effects: the game's effects' start 0x020D74C4(handle, archive, index) calls the SDK's NNS_SndArcPlayerStartSeqArc
// (0x020214D4), then for entry 1165 moves the handle's volume to 127 over 120 frames while the word at 0x20 of the
// handle's structure is 0 (0x020D74E8 to 0x020D7518). The requests of archive 0 (0x020D7784) and of archive 1
// (0x020D7820) pass the manager's handle at 0x34 (the literal at 0x020D7524, among others). A set loads the entry's
// archive and its bank with the SDK's internal loaders (0x02020B9C and 0x02020BFC, as the group loader 0x02020A14 calls
// them), then calls the effects' start.
constexpr uint32_t kPhManager = 0x020EEC08;
constexpr uint32_t kPhSecondHandle = kPhManager + 4;
constexpr uint32_t kPhStart = 0x020D7044;
constexpr uint32_t kPhHandleStart = 0x020D6CD0;
constexpr uint32_t kPhTrackVolume = 0x0201F83C;
constexpr uint32_t kPhTrackMute = 0x0201F81C;
constexpr uint32_t kPhTempoRatio = 0x0201F8AC;
constexpr uint32_t kPhLoadSequenceArchive = 0x02020B9C;
constexpr uint32_t kPhLoadBank = 0x02020BFC;
constexpr uint32_t kPhEffectStart = 0x020D74C4;
constexpr uint32_t kPhEffectHandle = kPhManager + 0x34;

constexpr std::array<StubCall, 1> kPhCalls = {
    MakeCall(kPhStart, {Constant(kPhManager), Sound(), Constant(0), Constant(127)})};
constexpr std::array<std::pair<uint32_t, uint32_t>, 1> kPhSea = {{{101, 101}}};
constexpr std::array<StubCall, 1> kPhSeaCalls = {
    MakeCall(kPhHandleStart, {Constant(kPhManager), Constant(kPhManager), Sound(), Constant(0), Constant(127)})};
constexpr std::array<NumberCallsEntry, 1> kPhNumbers = {{{.ranges = kPhSea, .calls = kPhSeaCalls}}};

constexpr std::array<StubCall, 3> kPhEffectCalls = {
    SdkLoad(kPhLoadSequenceArchive, ArchiveNumber()), SdkLoad(kPhLoadBank, BankNumber()),
    MakeCall(kPhEffectStart, {Constant(kPhEffectHandle), ArchiveNumber(), Entry()})};

constexpr std::array<StubCall, 4> kPhStopped = {
    MakeCall(kPhTrackVolume, {Constant(kPhManager), Constant(0x033A), Constant(64)}),
    MakeCall(kPhTrackVolume, {Constant(kPhManager), Constant(0x04D5), Constant(0)}),
    MakeCall(kPhTrackMute, {Constant(kPhManager), Constant(0x04D5), Constant(1)}),
    MakeCall(kPhTempoRatio, {Constant(kPhManager), Constant(250)})};
constexpr std::array<StubCall, 1> kPhFog = {
    MakeCall(kPhTrackVolume, {Constant(kPhSecondHandle), Constant(0x0080), Constant(0)})};
constexpr std::array<StubCall, 1> kPhGhostShip = {
    MakeCall(kPhTrackVolume, {Constant(kPhSecondHandle), Constant(0x0080), Constant(127)})};
constexpr std::array<GameStateEntry, 3> kPhStates = {{
    {"SEQ_101", "_STOPPED", kPhStopped},
    {"SEQ_103", "", kPhFog},
    {"SEQ_103", "_GHOST_SHIP", kPhGhostShip},
}};

constexpr std::array<GameWord, 14> kPhWords = {{
    {kPhStart, 0xE92D41F8, 0},       // stmfd sp!, {r3-r8, lr}
    {kPhHandleStart, 0xE92D40F8, 0}, // stmfd sp!, {r3-r7, lr}
    {kPhTrackVolume, 0xE92D4008},    // stmfd sp!, {r3, lr}
    {kPhTrackMute, 0xE92D4008},
    {kPhTempoRatio, 0xE92D4008},
    {0x020D64C4, kPhManager, 0},          // the manager's literal
    {0x020D70C4, 0x02878008, 0},          // addeq r8, r7, #8
    {0x020D70D0, 0x02878004, 0},          // addeq r8, r7, #4
    {0x020D7060, 0x13560065, 0},          // cmpne r6, #101
    {kPhLoadSequenceArchive, 0xE92D4070}, // stmfd sp!, {r4-r6, lr}
    {kPhLoadBank, 0xE92D4FF8},            // stmfd sp!, {r3-r11, lr}
    {kPhEffectStart, 0xE92D4070, 0},      // stmfd sp!, {r4-r6, lr}
    {0x020D74D0, 0xEBFD27FF, 0},          // bl NNS_SndArcPlayerStartSeqArc
    {0x020D7524, kPhEffectHandle, 0},     // the effects' handle
}};

} // namespace twosf::rip::zelda
