// SPDX-License-Identifier: MIT

// Hand-built cartridge images whose boot tells a backup memory from none, for the tests of the boots with a backup
// memory (rip/boots.h). Each test writes the ARM9 code that runs after the check. No game data.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/arm/assembler.h"
#include "ntdlib/ntr/machine.h"
#include "ntdlib/testing/nds_files.h"

#include "hand_built_set.h"

namespace twosf::testing
{

// Where the hand-built ARM9 binary loads and starts.
constexpr uint32_t kArm9Base = 0x02000000;

// The backup memory of the tests: a blank EEPROM of 512 bytes.
inline ntdlib::ntr::BackupMemory TestBackup()
{
    return {.address_bytes = 1, .size = 512};
}

// Emits code that sends the backup memory's write enable (06h) and then reads its status (05h). A backup memory sets
// its write-enable latch, bit 1 of the status. Without one, the status reads as 0. Branches to `no_backup` when the
// latch is clear. Uses r0 and r1.
inline void EmitBackupCheck(ntdlib::arm::ArmAssembler& a, int no_backup)
{
    constexpr uint32_t kSpiControl = 0x040001A0; // AUXSPICNT, with AUXSPIDATA 2 bytes on
    constexpr uint32_t kSpiOn = 0xA000;          // the bus on, in the backup memory's mode
    constexpr uint32_t kSpiHold = 0xA040;        // the same, with the memory held selected after the byte
    constexpr int kSpiData = 2;
    constexpr uint32_t kWriteEnable = 0x06;
    constexpr uint32_t kReadStatus = 0x05;
    constexpr uint32_t kWriteEnableLatch = 0x02;

    a.LoadConstant(1, kSpiControl);
    a.LoadConstant(0, kSpiOn);
    a.Strh(0, 1);
    a.Mov(0, kWriteEnable);
    a.Strb(0, 1, kSpiData);
    a.LoadConstant(0, kSpiHold);
    a.Strh(0, 1);
    a.Mov(0, kReadStatus);
    a.Strb(0, 1, kSpiData);
    a.LoadConstant(0, kSpiOn);
    a.Strh(0, 1);
    a.Mov(0, 0);
    a.Strb(0, 1, kSpiData);
    a.Ldrh(0, 1, kSpiData);
    a.Tst(0, kWriteEnableLatch);
    a.B(no_backup, ntdlib::arm::Condition::kEq);
}

// Emits a halt for good at `label`: the BIOS's Halt, then a branch back to it.
inline void EmitHaltForever(ntdlib::arm::ArmAssembler& a, int label)
{
    a.Bind(label);
    a.Swi(0x06);
    a.B(label);
}

// Pads the binary of `a` with zeros up to `address`.
inline void PadTo(ntdlib::arm::ArmAssembler& a, uint32_t address)
{
    while (a.Here() < address)
    {
        a.Word(0);
    }
}

// Ends the code of `a`: puts the literal pool, then pads with zeros up to `address`. The tests put their data from
// there on.
inline void EndCodeAt(ntdlib::arm::ArmAssembler& a, uint32_t address)
{
    a.Pool();
    PadTo(a, address);
}

// Where the hand-built ARM7 binary loads and starts.
constexpr uint32_t kArm7Base = 0x02380000;

// Emits code that turns the sound unit on at full volume. Uses r0 and r1.
inline void EmitSoundOn(ntdlib::arm::ArmAssembler& a)
{
    constexpr uint32_t kSoundControl = 0x04000500;
    constexpr uint32_t kSoundOn = 0x807F; // on, at full volume

    a.LoadConstant(1, kSoundControl);
    a.LoadConstant(0, kSoundOn);
    a.Strh(0, 1);
}

// An ARM7 binary that turns the sound unit on, starts channel 6 with the control word `control` on `buffer`, words of
// 8-bit samples, and halts for good.
inline std::vector<uint8_t> Channel6Arm7(std::span<const uint32_t> buffer, uint32_t control)
{
    constexpr uint32_t kBuffer = kArm7Base + 0x100;
    constexpr uint32_t kChannel6Control = 0x04000460;
    constexpr uint32_t kChannel6Source = 0x04000464;
    constexpr uint32_t kChannel6Timer = 0x04000468;
    constexpr uint32_t kChannel6Length = 0x0400046C;
    constexpr uint32_t kTimer = 0xF000;

    ntdlib::arm::ArmAssembler a(kArm7Base);
    const int halt = a.NewLabel();
    EmitSoundOn(a);
    a.LoadConstant(1, kChannel6Source);
    a.LoadConstant(0, kBuffer);
    a.Str(0, 1);
    a.LoadConstant(1, kChannel6Timer);
    a.LoadConstant(0, kTimer);
    a.Strh(0, 1);
    a.LoadConstant(1, kChannel6Length);
    a.LoadConstant(0, static_cast<uint32_t>(buffer.size()));
    a.Str(0, 1);
    a.LoadConstant(1, kChannel6Control);
    a.LoadConstant(0, control);
    a.Str(0, 1);
    EmitHaltForever(a, halt);
    EndCodeAt(a, kBuffer);
    for (const uint32_t word : buffer)
    {
        a.Word(word);
    }

    return a.Finish();
}

// An ARM7 binary that turns the sound unit on, starts a square wave on channel 8 and halts for good. The sound unit's
// output changes from the boot on.
inline std::vector<uint8_t> ToneArm7()
{
    constexpr uint32_t kChannel8Control = 0x04000480;
    constexpr uint32_t kChannel8Timer = 0x04000488;
    constexpr uint32_t kTimer = 0xF000;
    constexpr uint32_t kSquareWave = 0xE340007F; // started, a square wave of duty 4/8, at the centre, at full volume

    ntdlib::arm::ArmAssembler a(kArm7Base);
    const int halt = a.NewLabel();
    EmitSoundOn(a);
    a.LoadConstant(1, kChannel8Timer);
    a.LoadConstant(0, kTimer);
    a.Strh(0, 1);
    a.LoadConstant(1, kChannel8Control);
    a.LoadConstant(0, kSquareWave);
    a.Str(0, 1);
    EmitHaltForever(a, halt);
    a.Pool();

    return a.Finish();
}

// Emits code that enables the IPC's FIFO and sends each list of the sound driver's commands at the addresses `lists` to
// the ARM7, in order. Uses r0 and r1.
inline void EmitSendLists(ntdlib::arm::ArmAssembler& a, const std::vector<uint32_t>& lists)
{
    constexpr uint32_t kFifoControl = 0x04000184;
    constexpr uint32_t kFifoSend = 0x04000188;
    constexpr uint32_t kFifoEnable = 0x8000;
    constexpr uint32_t kSoundTag = 7;
    constexpr int kListShift = 6;

    a.LoadConstant(1, kFifoControl);
    a.LoadConstant(0, kFifoEnable);
    a.Strh(0, 1);
    a.LoadConstant(1, kFifoSend);
    for (const uint32_t list : lists)
    {
        a.LoadConstant(0, list << kListShift | kSoundTag);
        a.Str(0, 1);
    }
}

// Emits the words of `commands`, a list of the sound driver's commands at `address`: each command links to the next,
// then gives its id and four arguments.
inline void EmitList(ntdlib::arm::ArmAssembler& a, uint32_t address, const std::vector<std::vector<uint32_t>>& commands)
{
    constexpr uint32_t kCommandBytes = 24;
    for (std::size_t i = 0; i < commands.size(); i++)
    {
        const bool last = i + 1 == commands.size();
        a.Word(last ? 0 : address + kCommandBytes * static_cast<uint32_t>(i + 1));
        for (const uint32_t word : commands[i])
        {
            a.Word(word);
        }
    }
}

// A whole image with `arm9` as its ARM9 binary at kArm9Base, `files` in its file system and `arm7` as its ARM7 binary.
// The default ARM7 halts.
inline std::vector<uint8_t> ImageWithArm9(std::vector<uint8_t> arm9,
                                          std::vector<std::pair<std::string, std::vector<uint8_t>>> files = {},
                                          std::vector<uint8_t> arm7 = Halts())
{
    ntdlib::testing::nds_files::Image spec;
    spec.arm9 = std::move(arm9);
    spec.arm7 = std::move(arm7);
    spec.files = std::move(files);

    return ntdlib::testing::nds_files::BuildImage(spec).first;
}

} // namespace twosf::testing
