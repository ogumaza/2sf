// SPDX-License-Identifier: MIT

// The times of a mini in the render of 2sf's player. A mini's length tags count from the first sample that isn't 0 of
// what the player renders, and a piece that ends ends at its last sample over -72 dB. The length analyses run models of
// the drivers, and the player runs the game's code from the boot. The ripper therefore takes these times from a render
// of the mini by the player itself (twosf::Player), and the rest of the timing from the analysis.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/rip/length.h"
#include "xsflib/rip/length_tags.h"

namespace twosf::rip
{

// A piece that ends ends after this long with no sample over -72 dB: the fade of a piece that ends. A player plays the
// fade after the last sound.
constexpr double kEndQuietMs = static_cast<double>(xsflib::rip::kEndFadeMs);

// The search for the last sound of a piece that ends stops this long after the analysis's end.
constexpr double kEndSearchMs = 10000.0;

// The times of a mini's render, in milliseconds from the boot at the player's rate (twosf::Player::kSampleRate).
struct RenderTimes
{
    // The first sample that isn't 0 in either channel. A player searches for it up to xsflib::player::kMaxLeadMs from
    // the boot and counts from there when it finds none. `first_sample_found` says whether the render has one.
    double first_sample_ms = 0;
    bool first_sample_found = false;

    // The first sample over -72 dB in either channel, if the search found one.
    std::optional<double> first_sound_ms;

    // For a piece that ends: the last sample over -72 dB, if any, and whether a sample over -72 dB came within
    // kEndQuietMs of the end of the search (a sound that holds past kEndSearchMs).
    std::optional<double> last_sound_ms;
    bool sounds_on = false;
};

// Renders `image`, a set's image with a mini in place, with 2sf's player from the boot, as a player of the set renders
// it. The render goes on until it has found the first sample that isn't 0 and the first sample over -72 dB, and stops
// searching for each at xsflib::player::kMaxLeadMs after the first sample (after the boot for the first sample). With
// `end_ms`, the analysis's end of a piece that ends, counted from the boot, it also goes on past `end_ms` until
// kEndQuietMs passes with no sample over -72 dB, or until kEndSearchMs after `end_ms`. Throws std::runtime_error when
// the player fails.
RenderTimes RenderMini(std::vector<uint8_t> image, std::optional<double> end_ms);

// Whether `timing` is a piece that ends: it makes a sound, has no fade and isn't at the limit.
bool Ends(const xsflib::rip::Timing& timing);

// What a ripper prints about the render of a mini: warnings, and notes that aren't warnings.
struct RenderReport
{
    std::vector<std::string> warnings;
    std::vector<std::string> notes;
};

// The analysis and the render may differ by this many milliseconds at the first sample or at the end of a piece that
// ends without a warning.
constexpr long long kRenderModelMs = 100;

// Sets the first sounds of `timing`, the analysis of a sound that starts `start_ms` after the boot, from `render`, as
// times from that start. A piece that ends gets the render's last sound as its end, earlier or later than the
// analysis's, unless the render still sounds at the end of its search: then it keeps the analysis's end. Reports:
// - a warning for a render with no sample that isn't 0 within its search;
// - a warning for a piece that ends and still sounds at the end of the search;
// - a warning when the analysis has a first sample (a model of the samples, such as a sequence's) and the render's
//   comes more than kRenderModelMs from it, or the render's end of a piece that ends does;
// - a note when the render's end of a piece that ends comes more than kRenderModelMs from the end of an analysis
//   without a first sample, such as the end of a stream's file.
RenderReport TimeFromRender(xsflib::rip::Timing& timing, double start_ms, const RenderTimes& render);

// Times `timing`, the analysis of a sound that starts `start_ms` after the boot of `image`, from the render of `image`
// by 2sf's player (RenderMini, then TimeFromRender). A silent timing stays as it is. Throws std::runtime_error, with
// `label` in its message, when the player fails.
RenderReport TimeFromPlayer(std::vector<uint8_t> image, double start_ms, const std::string& label,
                            xsflib::rip::Timing& timing);

// Prints `report` about the mini `label` to stderr: each warning as "warning: LABEL: ...", then each note as
// "LABEL: ...".
void PrintRenderReport(const std::string& label, const RenderReport& report);

} // namespace twosf::rip
