// SPDX-License-Identifier: MIT

// The sets of the download-play programs that some games send to a second DS, such as Poké Transfer's program in
// Pokémon Black and Black 2. Such a program is a whole DS image, a file of the game, and it plays music that the game's
// set doesn't contain.
//
// A program's set is a set of the program's image, as for an SDAT game (rip/sdat_set.h). The program's code starts each
// piece with the calls of the table of programs. A piece's mini makes those calls as its start block. The program has
// no file system: its sound archives are in memory. Some are archives (NARCs) in its ARM9 binary. The game sends it
// another NARC. The library contains that NARC in place of embedded archives that no boot and no mini reads. That NARC
// keeps only the piece's sound archive, cut to the piece's data (CutArchive). The library also patches out the boot's
// start of a piece, as Mario Kart DS's set patches out its title's start.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/ntr/rom.h"

#include "rip/sdat_set.h"

namespace twosf::rip
{

// The bytes of member `member` of `narc`, a NARC. Throws xsflib::FormatError when `narc` isn't a NARC or has no such
// member.
std::span<const uint8_t> NarcMember(std::span<const uint8_t> narc, uint32_t member);

// `narc` with member `member` replaced by `bytes` and every other member empty, at the end of `bytes`. The header, the
// count of members and the names stay. Throws xsflib::FormatError as NarcMember does.
std::vector<uint8_t> CutNarc(std::span<const uint8_t> narc, uint32_t member, std::span<const uint8_t> bytes);

// A piece of music that a program plays: its label, the sound archive that its mini plays it from, as the set's memory
// holds it, and the sequence. `game_code` and `profile_file` give the piece's profile in ntdlib's profiles
// (ntdlib::ntr::GameProfileOf): the game that sends the program, and its file of the archive.
struct ProgramPiece
{
    std::string label;
    std::vector<uint8_t> archive;
    ntdlib::ntr::SequenceRef sequence;
    std::string game_code;
    std::string profile_file;
};

// A download-play program of a game, ready to rip: its name (the file's name without its extension), its image as a DS
// runs it with the ARM9 binary decompressed (`image`, the full image's base), the same with the sent NARC cut over the
// free archives (`library_image`, the library's base), its routines and its pieces. The table's calls of each piece are
// the state of its label with no suffix (StatesOf). Each state of the label with a suffix gets a mini of its own.
struct Program
{
    std::string name;
    std::vector<uint8_t> image;
    std::vector<uint8_t> library_image;
    SdatCode code;
    std::vector<ProgramPiece> pieces;
};

// The state of `label` with no suffix in `code` (StatesOf): the state that contains the calls of a program's piece.
// Null unless the label has exactly one such state.
const GameState* PieceState(const SdatCode& code, std::string_view label);

// The download-play programs of the game `rom` in the table of programs. Pokémon Black and White (USA) send one
// program, and Black 2 and White 2 (USA) another. Returns nothing, with `error` set, when a program in the table is
// missing from the game, when its code doesn't contain an instruction or a word that the table expects (FindTableCode),
// when the sent file isn't a NARC whose member is a sound archive with the piece's sequence, or when the sent NARC's
// cut doesn't fit over the free archives. Empty for a game outside the table.
std::optional<std::vector<Program>> FindPrograms(const ntdlib::ntr::Rom& rom, std::string& error);

} // namespace twosf::rip
