// SPDX-License-Identifier: MIT

#include "twosf/twosf_file.h"

namespace twosf
{

std::vector<uint8_t> WriteTwosf(const Block& block, const xsflib::psf::Tags& tags)
{
    return xsflib::psf::WritePsf(kTwosfVersion, {}, WriteProgram(block), tags);
}

} // namespace twosf
