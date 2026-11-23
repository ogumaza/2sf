// SPDX-License-Identifier: MIT

// The parts of a set for a game on Universomo's sound driver: the stub, the library's cartridge image, the minis'
// blocks, and the time from the boot to a song's first tick.
//
// The library is the game's cartridge image, cut to the bytes that the set reads: the header, both binaries, the file
// system's tables, the files that the profile names and the files that the set's boots (rip/boots.h) read before the
// main loop. Every other file's bytes are zeros. The stub replaces the game's main loop, and the routine that the set
// doesn't require returns at once. The header and the secure area keep their bytes. Their checksums therefore hold. A
// mini stores the song's number in the stub's word for it, and a song that plays once stores a loop flag of 0 in the
// next word.

#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/ntr/machine.h"
#include "ntdlib/ntr/rom.h"

#include "rip/profile.h"
#include "twosf/twosf_file.h"

namespace twosf::rip
{

// The stub's code and data, for the address of the game's main loop, and where its two words are from its start.
struct Stub
{
    std::vector<uint8_t> bytes;
    uint32_t song_offset = 0; // the song's number
    uint32_t loop_offset = 0; // the loop flag: 1 for a song that loops, 0 for one that plays once
};

// Assembles the stub of `profile`. The stub plays songs 0 to 999.
Stub BuildStub(const UniversomoProfile& profile);

// The library's cartridge image, with the stub in place, and the offsets in the image of the stub's two words. A mini
// writes both words in one block. The loop flag's word therefore follows the song's word.
struct SetImage
{
    std::vector<uint8_t> image;
    uint32_t song_word = 0;
    uint32_t loop_word = 0;
};

// A boot to the main loop: whether it reached it, the machine's clock there and the files that the card's reads
// reached.
struct MainLoopBoot
{
    bool reached = false;
    uint64_t clock = 0;
    std::set<uint32_t> files;
};

// Boots `rom` on ntdlib's DS with `backup` in its card until the ARM9 reaches the main loop of `profile`, for at most
// 10 seconds.
MainLoopBoot BootToMainLoop(const ntdlib::ntr::Rom& rom, const UniversomoProfile& profile,
                            const ntdlib::ntr::BackupMemory& backup);

// Makes the library's image of `rom` for `profile`, with the files `boot_files` too. Returns nothing and sets `error`
// when the stub doesn't fit in the main loop or in the ARM9 binary, or a part of the image is past the cartridge's used
// size.
std::optional<SetImage> MakeSetImage(const ntdlib::ntr::Rom& rom, const UniversomoProfile& profile,
                                     const std::set<uint32_t>& boot_files, std::string& error);

// Writes the words of song `song` into `set.image`, as its mini does.
void SetSong(SetImage& set, uint32_t song, bool loops);

// The block of the mini that starts song `song`: the song's number at the stub's word, and a loop flag of 0 after it
// for a song that plays once. A song that loops keeps the library's loop flag of 1.
Block MiniBlock(const SetImage& set, uint32_t song, bool loops);

// `title` as a name for a file (seqlib::SafeFileName). The characters that file systems refuse and the bytes of invalid
// UTF-8 become '_'. A Windows device name before the first dot gets a '_' after it: CON becomes CON_. The dots and
// spaces at the end go. A rip names the minis, the second libraries and the game's files with it.
std::string FileTitle(const std::string& title);

// The milliseconds from the boot of `set`'s image, with song `song` and with `backup` in its card, to the driver's
// first update, on ntdlib's model of the DS. Throws std::runtime_error when the update doesn't come within 10 seconds,
// or when no song is loaded then.
double FirstTickMs(const SetImage& set, const UniversomoProfile& profile, uint32_t song, bool loops,
                   const ntdlib::ntr::BackupMemory& backup = {});

} // namespace twosf::rip
