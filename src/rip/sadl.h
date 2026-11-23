// SPDX-License-Identifier: MIT

// The streams of Procyon Studio's stream player for the DS (sadl files): the header that the player reads, and the
// samples that its decoder writes to the buffers of the stream's hardware channels. The player decodes the stream on
// the ARM9 into a ring buffer of 16-bit PCM for each channel, and the SDK's sound driver plays the buffers at 32,728
// Hz. The addresses below are those of Professor Layton and the Diabolical Box (North America, YLTE), unless they name
// another game. The ARM9 binary of that game contains the player.
//
// The header (the stream's open, 0x020065A4): the magic word "sadl" at 0, the loop flag at 0x31, the channels at 0x32,
// the coding at 0x33, the bytes of each channel's frame at 0x3A, the end of the data at 0x40, the start of the data at
// 0x48, the start and the end of the loop at 0x54 and 0x58, the volume at 0x60 and the cutoff of the filter of a stream
// at half the rate, in Hz, at 0x68. The player starts at the start of the data. A stream whose loop flag is set goes
// back to the loop's start once it reaches the loop's end, and any other stream stops at the end of the data
// (0x020068FC). Each channel's frames of 16 bytes follow each other in turn: frame 0 of each channel, then frame 1.
//
// The coding 0xB4 stores the samples at 32,728 Hz, and 0xB2 at 16,364 Hz. The player writes two samples for each sample
// of 0xB2: a sample between it and the one before, then the sample. The codings 0x72 and 0x74, IMA-ADPCM, aren't read
// here.
//
// A frame holds 30 samples: 4 bits each in the low halves first, the bytes 0 to 14, each byte XOR 0x80. Byte 15 XOR
// 0x80 gives the filter in its high half (0 to 4, and 0 for a higher value) and 12 less the shift in its low half. Each
// sample adds the 4-bit value, shifted, to a prediction from the two samples before it with the filter's coefficients
// (the table at 0x0203E3B0, each pair stored inverted: 0 and 0, 60 and 0, 115 and -52, 98 and -55, 122 and -60, in
// 64ths). The decoder keeps 6 more bits than it writes. Two builds of the player round differently (SadlBuild).

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace twosf::rip
{

// The rate at which the stream player plays every stream: the timer 512 of the sound unit.
constexpr double kSadlPlayRate = 33513982.0 / 2.0 / 512.0;

// The builds of the stream player. They round the decoder's samples differently.
enum class SadlBuild
{
    // Layton 2's decoders (0x02007B60 for 0xB4, 0x02007D5C for 0xB2) round the prediction and each sample to the
    // nearest, and write each sample with its low 6 bits clear. For 0xB2, both samples go through a filter of one pole
    // at the header's cutoff (0x020069A8): 3,000 Hz where the header gives 0 (0x02006748).
    kEarlier,

    // Professor Layton and the Last Specter's decoders (North America, BLFE, overlay 0: 0x0204FD20 for 0xB4, 0x0204FEEC
    // for 0xB2) round the prediction and each sample toward zero, and write all 16 bits. For 0xB4, each sample gets 32
    // more. For 0xB2, the sample between is the mean of the two, with no filter.
    kLater,
};

// The header of a stream, as the stream player reads it. Offsets count from the file's start.
struct SadlHeader
{
    uint32_t channels = 0;
    uint8_t coding = 0;
    bool loop = false;
    uint32_t data_start = 0;
    uint32_t data_end = 0;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;
    uint32_t cutoff = 0; // in Hz, 0 for the player's 3,000 Hz
};

// The header of `file`. Nothing for a file that isn't a stream of the codings 0xB2 or 0xB4, with 1 or 2 channels and
// frames of 16 bytes, or whose data or loop is outside the file.
std::optional<SadlHeader> ReadSadlHeader(std::span<const uint8_t> file);

// The samples that the player writes for each sample that a stream stores: 2 for the coding 0xB2, 1 for 0xB4.
uint32_t SadlUpsampling(const SadlHeader& header);

// The samples of each channel that the player plays from the start of the data, at kSadlPlayRate: up to the loop's end
// for a stream with a loop, or to the end of the data.
uint32_t SadlSamples(const SadlHeader& header);

// The sample of each channel, at kSadlPlayRate, where the loop starts.
uint32_t SadlLoopStart(const SadlHeader& header);

// The samples that the player of `build` writes to the buffer of each channel of `file` for its first `frames` frames,
// as the decoder writes them, from the start of the data. Fewer where the data ends first.
std::vector<std::vector<int16_t>> DecodeSadl(std::span<const uint8_t> file, const SadlHeader& header, uint32_t frames,
                                             SadlBuild build);

// Whether any sample of any channel of `file`, up to SadlSamples, as the player of `build` decodes it, differs from the
// decoder's output for silence by more than xsflib::kSilenceLevel.
bool SadlAudible(std::span<const uint8_t> file, const SadlHeader& header, SadlBuild build);

} // namespace twosf::rip
