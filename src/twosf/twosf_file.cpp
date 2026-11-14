// SPDX-License-Identifier: MIT

#include "twosf/twosf_file.h"

#include <algorithm>
#include <cstddef>

#include "xsflib/bytes.h"
#include "xsflib/psf/set.h"

namespace twosf
{

namespace
{

// The two words in front of a block's bytes.
constexpr std::size_t kBlockHeaderSize = 8;

// A file of a set as the loader parses it: the container and its block.
struct ParsedFile : xsflib::psf::PsfFile
{
    Block block;
};

std::optional<std::string> Parse(const std::string& path, const std::vector<uint8_t>& data, ParsedFile& file)
{
    xsflib::psf::ParseOptions options;
    options.version = kTwosfVersion;
    options.wrong_version = "not a 2SF file";
    options.decompress = xsflib::psf::InflateProgram;
    if (auto error = xsflib::psf::ParsePsf(data, file, options))
    {
        return path + ": " + *error;
    }

    if (auto error = ParseProgram(file.program, file.block))
    {
        return path + ": " + *error;
    }

    // The block holds the bytes now.
    file.program.clear();
    file.program.shrink_to_fit();

    return std::nullopt;
}

void Apply(const Block& block, std::vector<uint8_t>& image)
{
    if (block.data.empty())
    {
        return;
    }

    const std::size_t end = std::size_t{block.offset} + block.data.size();
    if (image.size() < end)
    {
        image.resize(end, 0);
    }

    std::copy(block.data.begin(), block.data.end(), image.begin() + block.offset);
}

} // namespace

std::vector<uint8_t> WriteProgram(const Block& block)
{
    std::vector<uint8_t> program;
    program.reserve(kBlockHeaderSize + block.data.size());
    xsflib::psf::Put32(program, block.offset);
    xsflib::psf::Put32(program, static_cast<uint32_t>(block.data.size()));
    program.insert(program.end(), block.data.begin(), block.data.end());

    return program;
}

std::optional<std::string> ParseProgram(std::span<const uint8_t> program, Block& out)
{
    out = {};
    if (program.empty())
    {
        return std::nullopt;
    }
    if (program.size() < kBlockHeaderSize)
    {
        return "the program ends inside its offset and size";
    }

    const uint32_t offset = xsflib::LoadLe32(program.data());
    const uint32_t size = xsflib::LoadLe32(program.data() + 4);
    if (size > program.size() - kBlockHeaderSize)
    {
        return "the program ends inside its block";
    }
    if (offset > kMaxImageSize || size > kMaxImageSize - offset)
    {
        return "the block ends past the largest cartridge image";
    }

    out.offset = offset;
    out.data.assign(program.begin() + kBlockHeaderSize, program.begin() + kBlockHeaderSize + size);

    return std::nullopt;
}

std::optional<std::string> LoadTwosf(const std::string& path, const xsflib::psf::FileReader& reader,
                                     std::string_view separators, LoadedTwosf& out)
{
    out = {};

    const auto parse = [](const std::string& file_path, const std::vector<uint8_t>& data, int, ParsedFile& file)
    {
        return Parse(file_path, data, file);
    };

    const auto add = [&out](const std::string& file_path, const std::vector<uint8_t>&, int,
                            const ParsedFile& file) -> std::optional<std::string>
    {
        Apply(file.block, out.image);
        out.sources.push_back(file_path);

        return std::nullopt;
    };

    xsflib::psf::WalkRules rules;
    rules.too_deep = "the libraries nest deeper than " + std::to_string(xsflib::psf::kMaxLibDepth) + " levels";
    rules.too_many = "the set has more than " + std::to_string(xsflib::psf::kMaxSetFiles) + " files";
    rules.count_reached = true;
    ParsedFile top;
    if (auto error = xsflib::psf::WalkSet(path, reader, separators, top, parse, add, rules))
    {
        return error;
    }

    out.tags = std::move(top.tags);

    return std::nullopt;
}

} // namespace twosf
