// SPDX-License-Identifier: MIT

#include "tools/messages.h"

#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

namespace twosf::tools
{

int Refuse(const std::string& input, const std::string& why)
{
    std::fprintf(stderr, "error: %s: %s\n", input.c_str(), why.c_str());

    return 1;
}

std::string CountOf(std::size_t count, std::string_view one, std::string_view many)
{
    return std::to_string(count) + " " + std::string(count == 1 ? one : many);
}

} // namespace twosf::tools
