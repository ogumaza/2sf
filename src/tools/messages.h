// SPDX-License-Identifier: MIT

// The messages that 2sfrip's rips of every driver print: a refusal, a count with its noun, and the minis that fail
// their runs.

#pragma once

#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace twosf::tools
{

// Prints the error "error: <input>: <why>". Returns 1, the exit code of a rip that fails.
int Refuse(const std::string& input, const std::string& why);

// `count` and the noun for it, for a message: `one` for a count of 1 and `many` for any other, such as "1 file" or
// "3 files".
std::string CountOf(std::size_t count, std::string_view one, std::string_view many);

// Prints the error of each of `jobs` that has one, after the job's label. A job has a `label` and an optional `error`.
// Returns the number of jobs with an error.
template <typename Job>
std::size_t ReportFailures(const std::vector<Job>& jobs)
{
    std::size_t failed = 0;
    for (const Job& job : jobs)
    {
        if (job.error)
        {
            std::fprintf(stderr, "%s: %s\n", job.label.c_str(), job.error->c_str());
            failed++;
        }
    }

    return failed;
}

} // namespace twosf::tools
