// SPDX-License-Identifier: MIT

// The player: a 2SF track with its libraries, booted as a cartridge image on a model of the DS with both CPUs
// (ntdlib's ntr::Machine). The game's code runs and the sound unit's output is the track.
//
//     twosf::Player player;
//     if (!player.Load(path, xsflib::psf::ReadWholeFile) || !player.Start())
//     {
//         return player.Error();
//     }
//     const size_t got = player.Render(frames, count);
//
// Player is a PlayerBase of xsflib, so it has Load, Start, Render, GetState, Error, GetTags, LengthMs, FadeMs and
// Volume from there. The caller applies the length, the fade and the volume. xsflib's Playback does that.
//
// The DS's sound unit gives a sample every 1024 cycles of its clock of 33,513,982 Hz: 32,728.498 samples a second.
// kSampleRate is that rate as a whole number. The frames are the unit's, one for one.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/player/player.h"
#include "xsflib/psf/psf.h"

#include "ntdlib/ntr/machine.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/ntr/sound.h"

#include "twosf/twosf_file.h"

namespace twosf
{

class Player : public xsflib::player::PlayerBase<Player, LoadedTwosf>
{
public:
    using Base = xsflib::player::PlayerBase<Player, LoadedTwosf>;

    static constexpr int kSampleRate = 32728;

    static constexpr const char* kNotPlayable = "the set holds no cartridge image";
    static constexpr const char* kStartFailure = "the image failed to boot: ";
    static constexpr const char* kNoCode = "the image has no ARM9 or ARM7 code";
    static constexpr const char* kStepFailure = "the emulation failed: ";

    // Loads the track at `path` with its libraries, as LoadTwosf does. Bad sets are rejected here. The error tells why.
    static std::optional<std::string> LoadFile(const std::string& path, const xsflib::psf::FileReader& reader,
                                               LoadedTwosf& set);

    Player();
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

private:
    friend Base;

    // The hooks of PlayerBase.
    void Unload();
    bool Playable() const;
    bool StartSource();
    bool RunStep();
    bool Finished() const;
    std::vector<int16_t>& Output();

    std::optional<ntdlib::ntr::Rom> rom_;
    std::unique_ptr<ntdlib::ntr::Machine> machine_;
    std::vector<ntdlib::ntr::SoundOutput> unit_output_;
    std::vector<int16_t> samples_;
};

} // namespace twosf
