// SPDX-License-Identifier: MIT

// The routines that the tests of a set for a game on DSE share: a game's code in the test's memory that starts a song
// by number, and one that starts a song by two calls.

#pragma once

#include "rip/dse_set.h"
#include "stub_memory.h"

namespace twosf::testing
{

// Routines in the test's memory, each of which returns at once, and the hook with room for the stub.
inline rip::DseCode TestCode()
{
    rip::DseCode code;
    code.hook = kBase + 0x400;
    code.hook_end = kBase + 0x600;
    code.play_bgm = kBase + 0x100;
    code.play_me = kBase + 0x110;
    code.sleep = kBase + 0x120;
    code.bgm_format = "SOUND/BGM/bgm%04u.smd";
    code.me_format = "SOUND/ME/me%04u.smd";

    return code;
}

// Routines in the test's memory for a game that starts a song by two calls: the second takes the song's number and a
// float of 1.0. With `frames`, the stub then runs the frames: the wait and two calls of the game's work, the first with
// an argument.
inline rip::DseCode CallsTestCode(bool frames)
{
    rip::DseCode code = TestCode();
    code.start = rip::DseStartKind::kCalls;
    code.calls[0] = {.routine = kBase + 0x160, .arguments = {7, 0, 0, 0}};
    code.calls[1] = {.routine = kBase + 0x170, .arguments = {kBase + 0x8000, rip::kDseSongNumber, 0x3F800000, 0}};
    if (frames)
    {
        code.frame_wait = kBase + 0x140;
        code.frame_calls[0] = {.routine = kBase + 0x180, .arguments = {kBase + 0x8100, 0, 0, 0}};
        code.frame_calls[1] = {.routine = kBase + 0x190};
    }

    return code;
}

} // namespace twosf::testing
