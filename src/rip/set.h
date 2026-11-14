// SPDX-License-Identifier: MIT

// The parts of a set for a game on Universomo's sound driver: the stub, the library's cartridge image, the library's
// and the minis' files, and the time from the boot to a song's first tick.
//
// The library is the game's cartridge image, cut to the bytes that the set reads: the header, both binaries, the file
// system's tables and the files that the profile names. Every other file's bytes are zeros. The stub replaces the
// game's main loop, and the routine that the set doesn't require returns at once. The header and the secure area keep
// their bytes, so their checksums hold. A mini stores the song's number in the stub's word for it, and a song that
// plays once stores a loop flag of 0 in the next word.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/ntr/rom.h"
#include "xsflib/psf/psf.h"

#include "rip/profile.h"

namespace twosf::rip
{

// The stub's code and data, for the game's main loop's address, and where its two words lie from its start.
struct Stub
{
    std::vector<uint8_t> bytes;
    uint32_t song_offset = 0; // the song's number
    uint32_t loop_offset = 0; // the loop flag: 1 for a song that loops, 0 for one that plays once
};

// Assembles the stub of `profile`. The stub plays songs 0 to 999.
Stub BuildStub(const UniversomoProfile& profile);

// The library's cartridge image, with the stub in place, and the offsets in the image of the stub's two words. A mini
// writes both words in one block, so the loop flag's word follows the song's word.
struct SetImage
{
    std::vector<uint8_t> image;
    uint32_t song_word = 0;
    uint32_t loop_word = 0;
};

// Makes the library's image of `rom` for `profile`. Returns nothing and sets `error` when the stub doesn't fit in the
// main loop or in the ARM9 binary, or a part of the image lies past the cartridge's used size.
std::optional<SetImage> MakeSetImage(const ntdlib::ntr::Rom& rom, const UniversomoProfile& profile, std::string& error);

// Writes song `song`'s words into `image`, as its mini does.
void SetSong(SetImage& set, uint32_t song, bool loops);

// The library's file, and a mini's file that starts song `song` with `library`, the library's name.
std::vector<uint8_t> LibraryFile(const SetImage& set);
std::vector<uint8_t> MiniFile(const SetImage& set, uint32_t song, bool loops, const std::string& library,
                              xsflib::psf::Tags tags);

// The length and fade tags of a song of `length_ms`. A song that loops gets its length to the second and a fade of 10
// s. A song that ends gets its end to the millisecond and a fade of 1 s.
struct LengthTags
{
    std::string length;
    std::string fade;
};

LengthTags SongLengthTags(long long length_ms, bool loops);

// `title` as a name for a file, with the characters that file systems refuse replaced by '_' and no dots or spaces at
// its end.
std::string FileTitle(const std::string& title);

// The milliseconds from the boot of `set`'s image, with song `song`, to the driver's first update, on ntdlib's model of
// the DS. Throws std::runtime_error when the update doesn't come within 10 seconds, or when no song is loaded then.
double FirstTickMs(const SetImage& set, const UniversomoProfile& profile, uint32_t song, bool loops);

} // namespace twosf::rip
