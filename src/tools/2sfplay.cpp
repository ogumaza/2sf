// SPDX-License-Identifier: MIT

// 2sfplay: renders .mini2sf and .2sf files to WAV or FLAC.
//
// usage: 2sfplay [options] <file> [<file> ...]
//   -o, --output FILE  the output file, WAV or FLAC by its extension (default: the input's name with .wav, in the same
//                      folder). Only valid with a single input.
//   --length T         override the length tag (seconds, m:s or h:m:s)
//   --fade T           override the fade tag
//   --default T        the length when there's no length tag (default and fade: PlaybackOptions)
//   --info             print the tags and loaded files without rendering
//   --version          print the version
//
// The files are rendered one after another. A file that fails doesn't stop the rest. A .2sflib among the inputs is
// skipped. It's a library that the other files load. Files dropped on the program become inputs.
//
// The tool runs on xsflib's play-tool frame (xsflib/cli/play.h). This file holds its names, help and --info summary.

#include <string>

#include "xsflib/cli/play.h"

#include "twosf/player.h"

namespace
{

using Playback = xsflib::player::Playback<twosf::Player>;

// The help's description of what the tool does.
constexpr char kDescription[] =
    "Renders each .mini2sf or .2sf file to a WAV file next to it. The game's code runs on a model\n"
    "of the DS. The length and fade come from the file's tags. Libraries (.2sflib) are skipped\n"
    "because the .mini2sf files load them.\n"
    "You can also drop files on the program.\n";

// What --info prints about a set: its files and its cartridge image.
std::string DescribeSet(Playback& playback)
{
    const twosf::LoadedTwosf& set = playback.GetPlayer().GetLoadedSet();

    return std::to_string(set.sources.size()) + " files, a cartridge image of " + std::to_string(set.image.size()) +
           " bytes";
}

} // namespace

int main(int argc, char** argv)
{
    xsflib::cli::PlayTool<twosf::Player> tool;
    tool.name = "2sfplay";
    tool.version = TWOSF_VERSION;
    tool.description = kDescription;
    tool.output_option = "write FILE instead. Its extension selects WAV or FLAC (one input only)";
    tool.info_option = "print the tags and the files that each input loads, without rendering";
    tool.library_extension = ".2sflib";
    tool.mini_extension = ".mini2sf";
    tool.a_file = "a 2SF file";
    tool.set_up = "start-up";
    tool.describe_set = DescribeSet;

    return xsflib::cli::RunPlayTool(tool, argc, argv);
}
