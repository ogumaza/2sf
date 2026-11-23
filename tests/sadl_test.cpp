// SPDX-License-Identifier: MIT

// Tests the streams of Procyon Studio's stream player (rip/sadl.h): the header and the files that it refuses, the
// samples and the loop, the samples that each build of the decoder writes at both rates, and whether a stream has
// sound.

#include "rip/sadl.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

namespace
{

using twosf::rip::SadlBuild;
using twosf::rip::SadlHeader;
using xsflib::testing::Check;

constexpr uint32_t kDataStart = 0x100;

// A stream of `channels` channels in `coding` whose data contains `frames` frames of each channel: the first frame of
// each channel is `frame`, and the others are silent. With `loop`, the loop runs from the second frame to the end.
std::vector<uint8_t> Stream(uint8_t coding, uint8_t channels, uint32_t frames, const std::vector<uint8_t>& frame,
                            bool loop)
{
    const uint32_t end = kDataStart + 16 * channels * frames;
    std::vector<uint8_t> file(end, 0x80);
    std::fill(file.begin(), file.begin() + kDataStart, uint8_t{0});
    file[0] = 's';
    file[1] = 'a';
    file[2] = 'd';
    file[3] = 'l';
    file[0x31] = loop ? 1 : 0;
    file[0x32] = channels;
    file[0x33] = coding;
    xsflib::StoreLe16(&file[0x3A], 16);
    xsflib::StoreLe32(&file[0x40], end);
    xsflib::StoreLe32(&file[0x48], kDataStart);
    xsflib::StoreLe32(&file[0x54], kDataStart + 16 * channels);
    xsflib::StoreLe32(&file[0x58], end);
    for (uint8_t channel = 0; channel < channels; channel++)
    {
        std::copy(frame.begin(), frame.end(), file.begin() + kDataStart + 16 * channel);
    }

    return file;
}

// A frame with the filter 1 (60/64 of the previous sample) and a shift of 0: the values 1, 0, -1, then zeros. Each
// byte is stored XOR 0x80: byte 0 contains 1 and 0, byte 1 contains -1 and 0, and byte 15 contains the filter and 12.
std::vector<uint8_t> TestFrame()
{
    std::vector<uint8_t> frame(16, 0x80);
    frame[0] = 0x80 ^ 0x01;
    frame[1] = 0x80 ^ 0x0F;
    frame[15] = 0x80 ^ 0x1C;

    return frame;
}

// The header and the files that the player can't play.
void TestHeader()
{
    const std::vector<uint8_t> stream = Stream(0xB4, 2, 4, TestFrame(), true);
    const std::optional<SadlHeader> header = twosf::rip::ReadSadlHeader(stream);
    Check(header && header->channels == 2 && header->coding == 0xB4 && header->loop &&
              header->data_start == kDataStart && header->data_end == kDataStart + 128 &&
              header->loop_start == kDataStart + 32 && header->loop_end == kDataStart + 128 && header->cutoff == 0,
          "the header's fields");

    const auto refused = [&stream](std::size_t offset, uint8_t value)
    {
        std::vector<uint8_t> file = stream;
        file[offset] = value;
        return !twosf::rip::ReadSadlHeader(file);
    };
    Check(refused(0, 'S'), "another magic word");
    Check(refused(0x33, 0x74), "IMA-ADPCM");
    Check(refused(0x32, 3), "three channels");
    Check(refused(0x3A, 32), "frames of 32 bytes");
    Check(refused(0x41, 0x10), "data past the file's end");
    Check(refused(0x55, 0x10), "a loop that starts past its end");
    std::vector<uint8_t> no_loop = stream;
    no_loop[0x31] = 0;
    xsflib::StoreLe32(&no_loop[0x58], 0);
    Check(twosf::rip::ReadSadlHeader(no_loop).has_value(), "a stream without a loop ignores the loop's range");
    Check(!twosf::rip::ReadSadlHeader(std::vector<uint8_t>(stream.begin(), stream.begin() + 0x60)),
          "a file shorter than the header");
}

// The samples and the loop's start at the rate that the player plays: 30 samples a frame, twice as many for 0xB2. A
// stream with a loop plays to the loop's end.
void TestSamples()
{
    const SadlHeader full = twosf::rip::ReadSadlHeader(Stream(0xB4, 2, 4, TestFrame(), true)).value();
    const SadlHeader half = twosf::rip::ReadSadlHeader(Stream(0xB2, 1, 5, TestFrame(), false)).value();
    Check(twosf::rip::SadlUpsampling(full) == 1 && twosf::rip::SadlUpsampling(half) == 2, "the upsampling");
    Check(twosf::rip::SadlSamples(full) == 120 && twosf::rip::SadlLoopStart(full) == 30,
          "a loop from the second of four frames");
    Check(twosf::rip::SadlSamples(half) == 300, "five frames at half the rate, to the end");
}

// The samples that each build writes for TestFrame at the full rate. The earlier build rounds the prediction and the
// sample to the nearest and clears the low 6 bits; the later truncates and adds 32. With the history h1 and h2, in
// 64ths of a sample:
// 1. 1 << 12 << 6 = 262144: 4096 in both, and 4096 + 32 in the later build.
// 2. 60 * 262144 / 64 = 245760: 3840, and 3872 in the later build.
// 3. 60 * 245760 / 64 = 230400, less 262144: -31744. The earlier build: (-31744 + 32) / 64 = -495, cleared to -448.
//    The later build: -31744 / 64 = -496, and 32 more: -464.
// 4. The earlier build: (60 * -31744 + 32) / 64 = -29759, (-29759 + 32) / 64 = -464, cleared to -448. The later build:
//    60 * -31744 / 64 = -29760, -29760 / 64 = -465, and 32 more: -433.
void TestFullRate()
{
    const std::vector<uint8_t> stream = Stream(0xB4, 1, 2, TestFrame(), false);
    const SadlHeader header = twosf::rip::ReadSadlHeader(stream).value();
    const std::vector<std::vector<int16_t>> earlier = twosf::rip::DecodeSadl(stream, header, 1, SadlBuild::kEarlier);
    const std::vector<std::vector<int16_t>> later = twosf::rip::DecodeSadl(stream, header, 1, SadlBuild::kLater);
    Check(earlier.size() == 1 && earlier[0].size() == 30, "one frame of one channel gives 30 samples");
    Check(earlier[0][0] == 4096 && earlier[0][1] == 3840 && earlier[0][2] == -448 && earlier[0][3] == -448,
          "the earlier build's samples");
    Check(later[0][0] == 4128 && later[0][1] == 3872 && later[0][2] == -464 && later[0][3] == -433,
          "the later build's samples");
    Check(twosf::rip::DecodeSadl(stream, header, 10, SadlBuild::kEarlier)[0].size() == 60,
          "the decoder stops at the data's end");
}

// The samples at half the rate for TestFrame's first sample, 4096. The earlier build's filter at 3,000 Hz takes 147/256
// of the input and 108/256 of its last output: the sample between, 2048, gives 147 * 2048 / 256 = 1176, cleared to
// 1152, and 4096 then gives (147 * 4096 + 108 * 1176) / 256 = 2848, cleared to 2816. The later build writes the mean,
// 2048, then 4096 with no 32 added, then the mean of 4096 and 3840 and 3840.
void TestHalfRate()
{
    const std::vector<uint8_t> stream = Stream(0xB2, 1, 1, TestFrame(), false);
    const SadlHeader header = twosf::rip::ReadSadlHeader(stream).value();
    const std::vector<int16_t> earlier = twosf::rip::DecodeSadl(stream, header, 1, SadlBuild::kEarlier)[0];
    const std::vector<int16_t> later = twosf::rip::DecodeSadl(stream, header, 1, SadlBuild::kLater)[0];
    Check(earlier.size() == 60 && earlier[0] == 1152 && earlier[1] == 2816, "the earlier build's filter at 3,000 Hz");
    Check(later.size() == 60 && later[0] == 2048 && later[1] == 4096 && later[2] == 3968 && later[3] == 3840,
          "the later build's means");

    std::vector<uint8_t> cutoff = stream;
    xsflib::StoreLe32(&cutoff[0x68], 6000);
    const SadlHeader at_6000 = twosf::rip::ReadSadlHeader(cutoff).value();
    Check(twosf::rip::DecodeSadl(cutoff, at_6000, 1, SadlBuild::kEarlier)[0][0] != earlier[0],
          "the header's cutoff changes the earlier build's filter");
}

// A stream has sound where a sample up to its end has. A silent frame decodes to silence in the earlier build and to
// 32 in the later. Neither is sound.
void TestAudible()
{
    const std::vector<uint8_t> loud = Stream(0xB4, 2, 3, TestFrame(), false);
    const std::vector<uint8_t> silent = Stream(0xB4, 2, 3, std::vector<uint8_t>(16, 0x80), false);
    for (const SadlBuild build : {SadlBuild::kEarlier, SadlBuild::kLater})
    {
        Check(twosf::rip::SadlAudible(loud, twosf::rip::ReadSadlHeader(loud).value(), build), "a stream with sound");
        Check(!twosf::rip::SadlAudible(silent, twosf::rip::ReadSadlHeader(silent).value(), build), "a silent stream");
    }
}

} // namespace

int main()
{
    TestHeader();
    TestSamples();
    TestFullRate();
    TestHalfRate();
    TestAudible();

    return xsflib::testing::Summary("sadl");
}
