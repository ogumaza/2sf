// SPDX-License-Identifier: MIT

// Tests the times of a mini in the render of 2sf's player (rip/mini_render.h): the first sample that isn't 0, the
// first sample over -72 dB and the last sound of a piece that ends, against a scan of the same player's render, sample
// by sample, with both channels and with the right channel alone; the search's limits, and a rest before the
// analysis's end; and the timing that the times give, against the arithmetic of the length tags, with the reports of a
// render that differs from the analysis.

#include "rip/mini_render.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/arm/assembler.h"
#include "xsflib/player/playback.h"
#include "xsflib/rip/length.h"
#include "xsflib/rip/length_tags.h"
#include "xsflib/silence_level.h"
#include "xsflib/testing/check.h"

#include "backup_image.h"
#include "twosf/player.h"
#include "twosf/twosf_file.h"

namespace
{

using xsflib::testing::Check;

constexpr int kRate = twosf::Player::kSampleRate;

double MsOf(uint64_t frame)
{
    return static_cast<double>(frame) * 1000.0 / kRate;
}

// A part of a buffer of 8-bit samples: `words` words of zeros, or of a square wave.
struct Part
{
    uint32_t words = 0;
    bool tone = false;
};

constexpr uint32_t kCentre = 0x40;
constexpr uint32_t kRight = 0x7F;

// An ARM7 binary that turns the sound unit on and plays a buffer of 8-bit samples once on channel 6 at `pan`, then
// halts for good. The buffer is `parts` in order. The channel then stops.
std::vector<uint8_t> OneShotArm7(const std::vector<Part>& parts, uint32_t pan)
{
    constexpr uint32_t kOneShotPcm8 = 0x9000007F; // started, once, 8-bit samples, at full volume
    constexpr int kPanShift = 16;

    std::vector<uint32_t> buffer;
    for (const Part& part : parts)
    {
        for (uint32_t i = 0; i < part.words; i++)
        {
            buffer.push_back(!part.tone ? 0 : i % 2 == 0 ? 0x7F7F7F7Fu : 0x80808080u);
        }
    }

    return twosf::testing::Channel6Arm7(buffer, kOneShotPcm8 | pan << kPanShift);
}

std::vector<uint8_t> OneShotImage(const std::vector<Part>& parts, uint32_t pan = kCentre)
{
    return twosf::testing::ImageWithArm9(twosf::testing::Halts(), {}, OneShotArm7(parts, pan));
}

// The times of `image` in a render of 2sf's player over `frames`, by a scan of each frame, and whether the left
// channel has a sample that isn't 0.
struct Scan
{
    std::optional<uint64_t> first_sample;
    std::optional<uint64_t> first_sound;
    std::optional<uint64_t> last_sound;
    bool left = false;
};

Scan ScanRender(std::vector<uint8_t> image, std::size_t frames)
{
    twosf::Player player;
    twosf::LoadedTwosf set;
    set.image = std::move(image);
    std::vector<int16_t> out(2 * frames);
    const bool rendered = player.Load(std::move(set)) && player.Start() && player.Render(out.data(), frames) == frames;
    Check(rendered, "the scan's render");

    const auto sound = [](int16_t sample)
    {
        return std::abs(sample) > xsflib::kSilenceLevel;
    };

    Scan scan;
    for (std::size_t frame = 0; frame < frames; frame++)
    {
        const int16_t left = out[2 * frame];
        const int16_t right = out[2 * frame + 1];
        scan.left = scan.left || left != 0;
        if (!scan.first_sample && (left != 0 || right != 0))
        {
            scan.first_sample = frame;
        }

        if (sound(left) || sound(right))
        {
            scan.first_sound = scan.first_sound.value_or(frame);
            scan.last_sound = frame;
        }
    }

    return scan;
}

// Checks the times of a render against `scan`, under `what`.
void CheckTimes(const twosf::rip::RenderTimes& times, const Scan& scan, const std::string& what)
{
    Check(times.first_sample_found && times.first_sample_ms == MsOf(scan.first_sample.value_or(0)),
          what + ": the first sample that isn't 0");
    Check(times.first_sound_ms == MsOf(scan.first_sound.value_or(0)), what + ": the first sample over -72 dB");
    Check(times.last_sound_ms == MsOf(scan.last_sound.value_or(0)), what + ": the last sample over -72 dB");
    Check(!times.sounds_on, what + ": the sound ends within the search");
}

// A tone that starts after a stretch of zeros and stops: the render's times are the scan's, and the search for the
// last sound ends after kEndQuietMs of silence.
void TestOneShot()
{
    const std::vector<uint8_t> image = OneShotImage({{256, false}, {512, true}}); // 0.25 s of zeros, a tone of 0.5 s

    const Scan scan = ScanRender(image, 2 * kRate);
    Check(scan.first_sample && scan.first_sound && scan.last_sound, "the scan finds the tone");
    Check(scan.first_sample && MsOf(*scan.first_sample) > 200.0 && MsOf(*scan.first_sample) < 300.0,
          "the tone starts after the zeros");
    Check(scan.last_sound && MsOf(*scan.last_sound) > 700.0 && MsOf(*scan.last_sound) < 800.0,
          "the tone stops after half a second");

    // The analysis ends 0.2 s before the render's last sound.
    const twosf::rip::RenderTimes times = twosf::rip::RenderMini(image, MsOf(scan.last_sound.value_or(0)) - 200.0);
    CheckTimes(times, scan, "a tone");

    const twosf::rip::RenderTimes no_end = twosf::rip::RenderMini(image, std::nullopt);
    Check(no_end.first_sample_ms == times.first_sample_ms && !no_end.last_sound_ms && !no_end.sounds_on,
          "without an end, no last sound");
}

// A rest of 1.5 s inside the piece, before the analysis's end: the search goes on past it to the second tone.
void TestRestBeforeEnd()
{
    const std::vector<uint8_t> image = OneShotImage({{256, false}, {512, true}, {1536, false}, {512, true}});

    const Scan scan = ScanRender(image, 4 * kRate);
    Check(scan.last_sound && MsOf(*scan.last_sound) > 2500.0, "the second tone ends after the rest");

    const twosf::rip::RenderTimes times = twosf::rip::RenderMini(image, MsOf(scan.last_sound.value_or(0)));
    CheckTimes(times, scan, "a rest");
}

// A tone on the right channel alone: the search reads both channels.
void TestRightChannel()
{
    const std::vector<uint8_t> image = OneShotImage({{256, false}, {512, true}}, kRight);

    const Scan scan = ScanRender(image, 2 * kRate);
    Check(!scan.left && scan.first_sample && scan.last_sound, "the tone plays on the right channel alone");

    const twosf::rip::RenderTimes times = twosf::rip::RenderMini(image, MsOf(scan.last_sound.value_or(0)));
    CheckTimes(times, scan, "the right channel");
}

// A tone that goes on: the search for the last sound stops kEndSearchMs after the analysis's end and reports it.
void TestHeldTone()
{
    const std::vector<uint8_t> image =
        twosf::testing::ImageWithArm9(twosf::testing::Halts(), {}, twosf::testing::ToneArm7());

    const twosf::rip::RenderTimes times = twosf::rip::RenderMini(image, 100.0);

    Check(times.first_sample_found && times.first_sample_ms < 10.0, "the tone sounds from the boot");
    Check(times.sounds_on, "the tone still sounds at the end of the search");
    Check(times.last_sound_ms && *times.last_sound_ms >= 100.0 + twosf::rip::kEndSearchMs - twosf::rip::kEndQuietMs,
          "the last sound comes in the search's last second");
}

// A silent image: the first sample is where a player's search for it ends.
void TestSilent()
{
    const std::vector<uint8_t> image = twosf::testing::HaltingImage();

    const twosf::rip::RenderTimes times = twosf::rip::RenderMini(image, std::nullopt);

    const uint64_t limit = xsflib::player::MsToFrames(xsflib::player::kMaxLeadMs, kRate);
    Check(!times.first_sample_found && times.first_sample_ms == MsOf(limit), "the end of the player's search");
    Check(!times.first_sound_ms, "no sound");
}

std::optional<xsflib::rip::LengthTags> TagsOf(const xsflib::rip::Timing& timing)
{
    return xsflib::rip::TagsOfTiming(timing, xsflib::rip::LengthFrom::kFirstSound, 0.0);
}

// The timing from a render, and the tags that it gives: a loop keeps its analysis and counts from the render's first
// sample. A piece that ends ends at the render's last sound, earlier or later than the analysis's end.
void TestTimeFromRender()
{
    twosf::rip::RenderTimes render;
    render.first_sample_ms = 800.4;
    render.first_sample_found = true;
    render.first_sound_ms = 900.0;

    xsflib::rip::Timing loop{.length_ms = 10000, .fade_ms = 7000, .loop = xsflib::rip::LoopPass{2000, 4000}};
    const twosf::rip::RenderReport loop_report = twosf::rip::TimeFromRender(loop, 500.0, render);
    const std::optional<xsflib::rip::LengthTags> loop_tags = TagsOf(loop);

    Check(loop_report.warnings.empty() && loop_report.notes.empty() && loop.first_sample_ms == 300 &&
              loop.first_sound_ms == 400 && loop.length_ms == 10000,
          "a loop's first sounds from the sound's start");
    Check(loop_tags && loop_tags->length == "0:09.700" && loop_tags->fade == "7" &&
              loop_tags->loop_start == "0:01.700" && loop_tags->loop_length == "0:04.000",
          "a loop's tags count from the render's first sample");

    // A stream's end, 401 ms before the render's: 3,100.6 ms from the first sample to the last sound, rounded once.
    render.last_sound_ms = 3901.0;
    xsflib::rip::Timing later{.length_ms = 3000};
    const twosf::rip::RenderReport later_report = twosf::rip::TimeFromRender(later, 500.0, render);
    const std::optional<xsflib::rip::LengthTags> later_tags = TagsOf(later);

    Check(later.length_ms == 3401, "an ending ends at the render's last sound, after the analysis's end");
    Check(later_tags && later_tags->length == "0:03.101" && later_tags->fade == "1",
          "an ending's tags: its last sound from its first sample, and a fade of 1 s");
    Check(later_report.warnings.empty() && later_report.notes.size() == 1,
          "a note for an end 401 ms from an analysis without a first sample");

    // A sequence's analysis, with a first sample 10 ms after the render's and an end 40 ms after it.
    render.last_sound_ms = 3460.4;
    xsflib::rip::Timing earlier{.length_ms = 3000, .first_sample_ms = 310};
    const twosf::rip::RenderReport earlier_report = twosf::rip::TimeFromRender(earlier, 500.0, render);
    const std::optional<xsflib::rip::LengthTags> earlier_tags = TagsOf(earlier);

    Check(earlier.length_ms == 2960 && earlier_tags && earlier_tags->length == "0:02.660",
          "an ending ends at the render's last sound, before the analysis's end");
    Check(earlier_report.warnings.empty() && earlier_report.notes.empty(),
          "no report for differences of up to kRenderModelMs");

    // The same, with the analysis 150 ms after the render at the first sample and 200 ms after it at the end.
    render.last_sound_ms = 3300.4;
    xsflib::rip::Timing apart{.length_ms = 3000, .first_sample_ms = 450};
    const twosf::rip::RenderReport apart_report = twosf::rip::TimeFromRender(apart, 500.0, render);
    Check(apart.length_ms == 2800 && apart_report.warnings.size() == 2 && apart_report.notes.empty(),
          "a sequence's analysis more than kRenderModelMs from the render warns at the first sample and at the end");

    render.last_sound_ms = 3901.0;
    render.sounds_on = true;
    xsflib::rip::Timing held{.length_ms = 3000};
    const twosf::rip::RenderReport held_report = twosf::rip::TimeFromRender(held, 500.0, render);
    Check(held_report.warnings.size() == 1 && held.length_ms == 3000,
          "a piece that still sounds keeps its analysis's end, with a warning");

    render.sounds_on = false;
    render.first_sample_found = false;
    xsflib::rip::Timing unfound{.length_ms = 3000, .fade_ms = 7000};
    Check(twosf::rip::TimeFromRender(unfound, 500.0, render).warnings.size() == 1,
          "a render with no first sample warns");
}

} // namespace

int main()
{
    TestOneShot();
    TestRestBeforeEnd();
    TestRightChannel();
    TestHeldTone();
    TestSilent();
    TestTimeFromRender();

    return xsflib::testing::Summary("mini_render");
}
