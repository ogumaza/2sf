// SPDX-License-Identifier: MIT

#include "rip/mini_render.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/player/playback.h"
#include "xsflib/rip/length.h"
#include "xsflib/silence_level.h"

#include "twosf/player.h"
#include "twosf/twosf_file.h"

namespace twosf::rip
{

namespace
{

constexpr int kRate = Player::kSampleRate;

// The frames that each call of the player renders.
constexpr std::size_t kBlockFrames = 4096;

double MsOf(uint64_t frame)
{
    return static_cast<double>(frame) * 1000.0 / kRate;
}

bool IsSound(int16_t sample)
{
    return sample > xsflib::kSilenceLevel || sample < -xsflib::kSilenceLevel;
}

} // namespace

RenderTimes RenderMini(std::vector<uint8_t> image, std::optional<double> end_ms)
{
    Player player;
    LoadedTwosf set;
    set.image = std::move(image);
    if (!player.Load(std::move(set)) || !player.Start())
    {
        throw std::runtime_error("2sf's player can't start the mini: " + player.Error());
    }

    // A player counts from the first sample that isn't 0 within its search, or from the end of the search.
    const uint64_t lead_limit = xsflib::player::MsToFrames(xsflib::player::kMaxLeadMs, kRate);
    std::optional<uint64_t> first_sample;
    std::optional<uint64_t> first_sound;
    std::optional<uint64_t> last_sound;
    const auto searching = [&](uint64_t frame)
    {
        const uint64_t counted = first_sample.value_or(lead_limit);
        const bool firsts = (!first_sample && frame < lead_limit) || (!first_sound && frame < counted + lead_limit);
        if (firsts || !end_ms)
        {
            return firsts;
        }

        const double now = MsOf(frame);
        const double quiet_since = last_sound ? MsOf(*last_sound + 1) : 0.0;
        return (now < *end_ms || now - quiet_since < kEndQuietMs) && now < *end_ms + kEndSearchMs;
    };

    std::vector<int16_t> block(2 * kBlockFrames);
    uint64_t frame = 0;
    while (searching(frame))
    {
        if (player.Render(block.data(), kBlockFrames) != kBlockFrames)
        {
            throw std::runtime_error("2sf's player fails in the mini: " + player.Error());
        }

        for (std::size_t i = 0; i < kBlockFrames; i++)
        {
            const int16_t left = block[2 * i];
            const int16_t right = block[2 * i + 1];
            const uint64_t at = frame + i;
            if (!first_sample && at < lead_limit && (left != 0 || right != 0))
            {
                first_sample = at;
            }

            if (IsSound(left) || IsSound(right))
            {
                first_sound = first_sound.value_or(at);
                last_sound = at;
            }
        }
        frame += kBlockFrames;
    }

    RenderTimes times;
    times.first_sample_ms = MsOf(first_sample.value_or(lead_limit));
    times.first_sample_found = first_sample.has_value();
    if (first_sound && *first_sound < first_sample.value_or(lead_limit) + lead_limit)
    {
        times.first_sound_ms = MsOf(*first_sound);
    }

    if (end_ms)
    {
        times.last_sound_ms = last_sound ? std::optional(MsOf(*last_sound)) : std::nullopt;
        const double quiet_since = last_sound ? MsOf(*last_sound + 1) : 0.0;
        times.sounds_on = MsOf(frame) - quiet_since < kEndQuietMs;
    }

    return times;
}

bool Ends(const xsflib::rip::Timing& timing)
{
    return !timing.silent && !timing.at_limit && timing.fade_ms == 0;
}

RenderReport TimeFromRender(xsflib::rip::Timing& timing, double start_ms, const RenderTimes& render)
{
    const auto from_start = [start_ms](double ms)
    {
        return std::llround(ms - start_ms);
    };

    // The analysis's times, for the comparison with the render. A model of the samples gives the first sample.
    const std::optional<long long> model_first = timing.first_sample_ms;
    const long long model_end = timing.length_ms;
    timing.first_sample_ms = from_start(render.first_sample_ms);
    timing.first_sound_ms = render.first_sound_ms ? std::optional(from_start(*render.first_sound_ms)) : std::nullopt;

    // The length tag counts from the first sample: it is the render's time between the two, rounded once.
    const bool ends = Ends(timing);
    if (ends && render.last_sound_ms && !render.sounds_on)
    {
        timing.length_ms = *timing.first_sample_ms + std::llround(*render.last_sound_ms - render.first_sample_ms);
    }

    RenderReport report;
    if (!render.first_sample_found)
    {
        report.warnings.push_back("2sf's player renders no sample that isn't 0 within " +
                                  std::to_string(xsflib::player::kMaxLeadMs / 1000) + " s of the boot");
    }

    if (ends && render.sounds_on)
    {
        report.warnings.push_back("the piece ends, and 2sf's player still sounds " +
                                  std::to_string(std::lround(kEndSearchMs / 1000)) +
                                  " s after its end; the length tag keeps the analysis's end");
    }

    if (model_first && std::abs(*model_first - *timing.first_sample_ms) > kRenderModelMs)
    {
        report.warnings.push_back("the analysis and 2sf's player differ by " +
                                  std::to_string(*timing.first_sample_ms - *model_first) +
                                  " ms at the first sample; the tags follow the player");
    }

    const long long end_difference = timing.length_ms - model_end;
    if (ends && std::abs(end_difference) > kRenderModelMs)
    {
        // A model of the samples is a sequence's analysis. A stream's end and a walk's end come from their data.
        const std::string text = "2sf's player ends " + std::to_string(end_difference) +
                                 " ms from the analysis's end; the length tag follows the player";
        (model_first ? report.warnings : report.notes).push_back(text);
    }

    return report;
}

RenderReport TimeFromPlayer(std::vector<uint8_t> image, double start_ms, const std::string& label,
                            xsflib::rip::Timing& timing)
{
    if (timing.silent)
    {
        return {};
    }

    const std::optional<double> end_ms =
        Ends(timing) ? std::optional(start_ms + static_cast<double>(timing.length_ms)) : std::nullopt;
    try
    {
        return TimeFromRender(timing, start_ms, RenderMini(std::move(image), end_ms));
    }
    catch (const std::runtime_error& e)
    {
        throw std::runtime_error(label + ": " + e.what());
    }
}

void PrintRenderReport(const std::string& label, const RenderReport& report)
{
    for (const std::string& warning : report.warnings)
    {
        std::fprintf(stderr, "warning: %s: %s\n", label.c_str(), warning.c_str());
    }

    for (const std::string& note : report.notes)
    {
        std::fprintf(stderr, "%s: %s\n", label.c_str(), note.c_str());
    }
}

} // namespace twosf::rip
