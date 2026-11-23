// SPDX-License-Identifier: MIT

#include "rip/sadl.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/silence_level.h"

namespace twosf::rip
{

namespace
{

constexpr uint32_t kFrameBytes = 16;
constexpr uint32_t kFrameSamples = 30;
constexpr uint8_t kCodingFull = 0xB4;
constexpr uint8_t kCodingHalf = 0xB2;
constexpr uint32_t kDefaultCutoff = 3000;

// The filters' coefficients for the previous sample and the one before it, in 64ths.
constexpr std::array<std::array<int32_t, 2>, 5> kCoefficients = {{{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}}};

// `value` / 2^`bits`, rounded toward zero. The decoder divides this way: it adds the sign's bits, then shifts
// arithmetically.
int32_t DivideTowardZero(int32_t value, int bits)
{
    const int32_t adjust = value < 0 ? (1 << bits) - 1 : 0;

    return (value + adjust) >> bits;
}

// The decoder's state of one channel: the last two samples, with 6 bits more than it writes, and for a stream at half
// the rate the last sample that it wrote and the filter's last output.
struct ChannelState
{
    int32_t history1 = 0;
    int32_t history2 = 0;
    int32_t previous = 0;
    int32_t filtered = 0;
};

// The filter's coefficients at `cutoff` Hz, in 256ths: the input's and the last output's (0x020069A8). The player works
// them out in single precision and drops the fractions.
std::array<int32_t, 2> FilterCoefficients(uint32_t cutoff)
{
    const float share = static_cast<float>(2.0 * 3.141592653589793 * cutoff / 32728.0);

    return {static_cast<int32_t>(256.0f * share), static_cast<int32_t>(256.0f * (1.0f - share))};
}

int16_t Clamp16(int32_t value)
{
    return static_cast<int16_t>(std::clamp(value, -0x8000, 0x7FFF));
}

// The sample that the earlier build writes: clamped, with its low 6 bits clear.
int16_t ClearLowBits(int32_t value)
{
    return static_cast<int16_t>(DivideTowardZero(value, 6) * 64);
}

// Decodes the frame at `frame` into `out` as `build` does: 30 samples, or 60 at half the rate (`half`). `filter` gives
// the earlier build's filter at half the rate.
void DecodeFrame(std::span<const uint8_t> frame, ChannelState& state, SadlBuild build, bool half,
                 const std::array<int32_t, 2>& filter, std::vector<int16_t>& out)
{
    const bool earlier = build == SadlBuild::kEarlier;
    const int32_t rounding = earlier ? 32 : 0;
    const uint32_t control = frame[15] ^ 0x80u;
    const uint32_t index = (control >> 4) > 4 ? 0 : control >> 4;
    const int shift = 12 - static_cast<int>(control & 0xF);
    const int32_t c1 = kCoefficients[index][0];
    const int32_t c2 = kCoefficients[index][1];
    for (uint32_t i = 0; i < kFrameSamples; i++)
    {
        const uint32_t byte = frame[i / 2] ^ 0x80u;
        const uint32_t nibble = (i % 2 == 0 ? byte : byte >> 4) & 0xF;
        const auto scaled = static_cast<int32_t>(static_cast<int16_t>(nibble << 12)) >> shift;
        const int32_t sample = DivideTowardZero(state.history1 * c1 + state.history2 * c2 + rounding, 6) + scaled * 64;
        state.history2 = state.history1;
        state.history1 = sample;
        const int32_t value = DivideTowardZero(sample + rounding, 6);
        if (!half)
        {
            out.push_back(earlier ? ClearLowBits(Clamp16(value)) : Clamp16(value + 32));
            continue;
        }

        if (!earlier)
        {
            // The later build: the mean of the last sample and this one, then this one, from the samples before the
            // clamp.
            out.push_back(Clamp16(DivideTowardZero(state.previous + value, 1)));
            out.push_back(Clamp16(value));
            state.previous = value;
            continue;
        }

        // The earlier build: the sample between the last one and this one, then this one, each through the filter.
        const int16_t written = Clamp16(value);
        const auto middle = static_cast<int16_t>(DivideTowardZero(written + state.previous, 1));
        const auto first = static_cast<int16_t>(DivideTowardZero(filter[0] * middle + filter[1] * state.filtered, 8));
        const auto second = static_cast<int16_t>(DivideTowardZero(filter[0] * written + filter[1] * first, 8));
        out.push_back(ClearLowBits(first));
        out.push_back(ClearLowBits(second));
        state.previous = written;
        state.filtered = second;
    }
}

} // namespace

std::optional<SadlHeader> ReadSadlHeader(std::span<const uint8_t> file)
{
    constexpr std::size_t kHeaderBytes = 0x6C;
    if (file.size() < kHeaderBytes || file[0] != 's' || file[1] != 'a' || file[2] != 'd' || file[3] != 'l')
    {
        return std::nullopt;
    }

    SadlHeader header;
    header.loop = file[0x31] != 0;
    header.channels = file[0x32];
    header.coding = file[0x33];
    header.data_end = xsflib::LoadLe32(&file[0x40]);
    header.data_start = xsflib::LoadLe32(&file[0x48]);
    header.loop_start = xsflib::LoadLe32(&file[0x54]);
    header.loop_end = xsflib::LoadLe32(&file[0x58]);
    header.cutoff = xsflib::LoadLe32(&file[0x68]);
    const uint32_t frame_bytes = xsflib::LoadLe16(&file[0x3A]);
    const bool coding = header.coding == kCodingFull || header.coding == kCodingHalf;
    const bool channels = header.channels == 1 || header.channels == 2;
    const bool data = header.data_start < header.data_end && header.data_end <= file.size();
    const bool loop = !header.loop || (header.data_start <= header.loop_start && header.loop_start < header.loop_end &&
                                       header.loop_end <= header.data_end);
    if (!coding || !channels || frame_bytes != kFrameBytes || !data || !loop)
    {
        return std::nullopt;
    }

    return header;
}

uint32_t SadlUpsampling(const SadlHeader& header)
{
    return header.coding == kCodingHalf ? 2 : 1;
}

uint32_t SadlSamples(const SadlHeader& header)
{
    const uint32_t end = header.loop ? header.loop_end : header.data_end;
    const uint32_t frames = (end - header.data_start) / (kFrameBytes * header.channels);

    return frames * kFrameSamples * SadlUpsampling(header);
}

uint32_t SadlLoopStart(const SadlHeader& header)
{
    const uint32_t frames = (header.loop_start - header.data_start) / (kFrameBytes * header.channels);

    return frames * kFrameSamples * SadlUpsampling(header);
}

std::vector<std::vector<int16_t>> DecodeSadl(std::span<const uint8_t> file, const SadlHeader& header, uint32_t frames,
                                             SadlBuild build)
{
    const std::array<int32_t, 2> filter = FilterCoefficients(header.cutoff == 0 ? kDefaultCutoff : header.cutoff);
    const bool half = header.coding == kCodingHalf;
    const uint32_t available = (header.data_end - header.data_start) / (kFrameBytes * header.channels);
    std::vector<std::vector<int16_t>> channels(header.channels);
    std::vector<ChannelState> states(header.channels);
    for (uint32_t frame = 0; frame < std::min(frames, available); frame++)
    {
        for (uint32_t channel = 0; channel < header.channels; channel++)
        {
            const std::size_t offset =
                header.data_start + (std::size_t{frame} * header.channels + channel) * kFrameBytes;
            DecodeFrame(file.subspan(offset, kFrameBytes), states[channel], build, half, filter, channels[channel]);
        }
    }

    return channels;
}

bool SadlAudible(std::span<const uint8_t> file, const SadlHeader& header, SadlBuild build)
{
    const uint32_t frames = SadlSamples(header) / (kFrameSamples * SadlUpsampling(header));
    const std::vector<std::vector<int16_t>> channels = DecodeSadl(file, header, frames, build);

    // The later build adds 32 to each sample at the full rate: silence decodes to 32.
    const int32_t rest = build == SadlBuild::kLater && header.coding == kCodingFull ? 32 : 0;
    const auto sounds = [rest](int16_t sample)
    {
        return std::abs(sample - rest) > xsflib::kSilenceLevel;
    };

    return std::ranges::any_of(
        channels, [&](const std::vector<int16_t>& samples) { return std::ranges::any_of(samples, sounds); });
}

} // namespace twosf::rip
