// SPDX-License-Identifier: MIT

// The memory of the tests that run a stub on ntdlib's ARM interpreter: 64 KiB at the start of main memory, and nothing
// else on the bus.

#pragma once

#include <cstdint>
#include <vector>

#include "ntdlib/arm/cpu.h"

namespace twosf::testing
{

// The memory's start and size, and the top of the stack that the tests give a stub.
constexpr uint32_t kBase = 0x02000000;
constexpr uint32_t kMemorySize = 0x10000;
constexpr uint32_t kStack = kBase + 0xF000;

// 64 KiB of memory at kBase. Every other address reads as 0 and ignores writes.
class Memory : public ntdlib::arm::Bus
{
public:
    Memory() : bytes_(kMemorySize, 0)
    {
    }

    uint8_t* Page(uint32_t page) override
    {
        return page >= kBase && page - kBase < kMemorySize ? bytes_.data() + (page - kBase) : nullptr;
    }

    uint32_t Read(uint32_t, int) override
    {
        return 0;
    }

    void Write(uint32_t, int, uint32_t) override
    {
    }

    uint8_t* At(uint32_t address)
    {
        return bytes_.data() + (address - kBase);
    }

private:
    std::vector<uint8_t> bytes_;
};

} // namespace twosf::testing
