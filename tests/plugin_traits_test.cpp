// SPDX-License-Identifier: MIT

// The traits that the foobar2000 component and the XMPlay plugin share (plugins/plugin_traits.h), on hand-built sets:
// the loader through the traits, in a folder and inside an archive; the length, fade and volume of the mini's tags in
// xsflib's playback; and the tags that the plugins hide.

#include "plugin_traits.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/player/playback.h"
#include "xsflib/psf/plugin_tags.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/sets.h"

#include "hand_built_set.h"

namespace
{

using Traits = twosf::PluginTraits;
using Playback = xsflib::player::Playback<Traits::Player>;
using twosf::testing::Files;
using twosf::testing::HaltingImage;
using twosf::testing::SetOf;

// The templates pass these separators for a file in a folder and for a file inside an archive.
constexpr const char* kFolderSeparators = "/\\";
constexpr const char* kArchiveSeparators = "/\\|";

// A mini in a folder loads its library from the same folder. The set's tags are the mini's.
void LoadsASetInAFolder()
{
    const Files files = SetOf(HaltingImage(), {{"title", "Title"}}, "music/");
    Traits::Set set;

    const auto error =
        Traits::LoadSet("music/test.mini2sf", xsflib::testing::ExactReader(files), set, kFolderSeparators);

    XSFLIB_CHECK(!error);
    XSFLIB_CHECK((set.sources == std::vector<std::string>{"music/test.2sflib", "music/test.mini2sf"}));
    XSFLIB_CHECK(set.tags.at("title") == "Title");
    XSFLIB_CHECK(!set.image.empty());
}

// Inside an archive, foobar2000 names a file after the archive, a bar and the path in the archive. A mini at the top of
// the archive finds its library there, after the bar.
void LoadsASetInAnArchive()
{
    const Files files = SetOf(HaltingImage(), {}, "rips.zip|");
    Traits::Set set;

    const auto error =
        Traits::LoadSet("rips.zip|test.mini2sf", xsflib::testing::ExactReader(files), set, kArchiveSeparators);

    XSFLIB_CHECK(!error);
    XSFLIB_CHECK(set.sources.size() == 2 && set.sources[0] == "rips.zip|test.2sflib");
}

// The mini's length, fade and volume reach the playback. The track ends after the length and the fade.
void PlaysTheTaggedLength()
{
    const Files files = SetOf(HaltingImage(), {{"length", "0:01"}, {"fade", "0.5"}, {"volume", "0.5"}});
    Traits::Set set;
    XSFLIB_REQUIRE(!Traits::LoadSet("test.mini2sf", xsflib::testing::ExactReader(files), set, kFolderSeparators));

    Playback playback;
    XSFLIB_REQUIRE(playback.Open(std::move(set), {}));
    XSFLIB_CHECK(playback.HasLengthTag());
    XSFLIB_CHECK(playback.PlayFrames() == uint64_t{Traits::Player::kSampleRate});
    XSFLIB_CHECK(playback.FadeFrames() == uint64_t{Traits::Player::kSampleRate} / 2);
    XSFLIB_CHECK(playback.GetPlayer().Volume() == 0.5);
    XSFLIB_REQUIRE(playback.Start());

    std::vector<int16_t> out(2 * 4096);
    uint64_t total = 0;
    while (const std::size_t got = playback.Render(out.data(), 4096))
    {
        total += got;
    }

    XSFLIB_CHECK(playback.Error().empty());
    XSFLIB_CHECK(total == playback.LengthFrames());
}

// The tags that set how a set plays are hidden from the metadata. A song's other tags aren't.
void HidesThePlaybackTags()
{
    for (const char* tag : Traits::kPlaybackTags)
    {
        XSFLIB_CHECK(xsflib::psf::IsHiddenTag(tag, Traits::kPlaybackTags));
    }

    for (const char* tag : {"title", "game", "artist", "year", "copyright", "2sfby"})
    {
        XSFLIB_CHECK(!xsflib::psf::IsHiddenTag(tag, Traits::kPlaybackTags));
    }
}

} // namespace

int main()
{
    LoadsASetInAFolder();
    LoadsASetInAnArchive();
    PlaysTheTaggedLength();
    HidesThePlaybackTags();

    return xsflib::testing::Summary("2SF plugin traits");
}
