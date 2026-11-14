// SPDX-License-Identifier: MIT

#include "twosf/player.h"

#include "ntdlib/ntr/arm7.h"

namespace twosf
{

std::optional<std::string> Player::LoadFile(const std::string& path, const xsflib::psf::FileReader& reader,
                                            LoadedTwosf& set)
{
    return LoadTwosf(path, reader, "/\\", set);
}

Player::Player() = default;

Player::~Player() = default;

void Player::Unload()
{
    machine_.reset();
    rom_.reset();
    unit_output_.clear();
    samples_.clear();
}

bool Player::Playable() const
{
    return !set_.image.empty();
}

bool Player::StartSource()
{
    machine_.reset();
    samples_.clear();

    // The parsed image points into the set's bytes. The set stays as it is until the next Load.
    rom_ = ntdlib::ntr::Rom::Parse(set_.image);
    machine_ = std::make_unique<ntdlib::ntr::Machine>(*rom_);

    return true;
}

bool Player::RunStep()
{
    // A step is one frame of the display: 263 lines.
    unit_output_.clear();
    machine_->Run(machine_->Clock() + ntdlib::ntr::kFrameCycles, unit_output_);
    ntdlib::ntr::AppendPcm16(unit_output_, samples_);

    return true;
}

bool Player::Finished() const
{
    // The game's code never tells the player that a track is over. The length tag ends it.
    return false;
}

std::vector<int16_t>& Player::Output()
{
    return samples_;
}

} // namespace twosf
