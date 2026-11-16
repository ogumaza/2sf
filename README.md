# 2sf

2sf rips the music of Nintendo DS games to 2SF, the PSF-family format for DS music. A 2SF set holds the game's
cartridge image, cut down to what its music reads, with a small stub that starts one song. Each song gets a small file,
a mini, that names the song to start. A 2SF player emulates the DS and runs the game's sound code, so the music
plays as it does in the game.

`2sfrip` is the ripper. It supports Beat City and games that play their music with the sound archive player of
Nintendo's SDK, from a sound archive (SDAT). More games and drivers will follow.

## Supported games

| Game | Release | Sound driver | What the set holds |
|---|---|---|---|
| Beat City | USA (NTR-BEYE) | Universomo's | The 66 pieces of music. `--effects` adds the 81 sound effects |
| The Legend of Zelda: Phantom Hourglass | USA (NTR-AZEE) | The SDK's sound archive player | The sequences outside the sequence archives |
| The Legend of Zelda: Spirit Tracks | USA (NTR-BKIE) and Europe (NTR-BKIP), each in revisions 0 and 1 | The SDK's sound archive player | The sequences outside the sequence archives |

`2sfrip` recognises Beat City by its game code and by its code at the addresses that the stub calls. For any other
game, it looks for a sound archive and for the SDK's sound routines in the game's code. Then it runs every mini on its
model of the DS. The command that prepares the sequence has to name the sequence's data and its bank in memory. The
mini then has to play for 5 seconds without the game changing the sequence or reading data that the set leaves out.
If a mini fails, `2sfrip` refuses the game. Other games on the SDK's player may rip as well. The table lists the games
that have been checked.

Many games play some of their music as streams: recorded audio that the sound archive contains beside its sequences.
2sfrip doesn't rip streams.

## Building

2sf requires CMake 3.23 or later and a C++20 compiler: Clang or Apple Clang on macOS, clang-cl on Windows. The
libraries that it builds on are submodules.

```
git clone --recursive <repository>
cd 2sf
cmake --preset release
cmake --build --preset release
ctest --preset release
```

## Using 2sfrip

```
2sfrip "Beat City (USA).nds"
```

writes the set into a folder next to the image, named after it with `_2sf` added: the library
`NTR-BEYE-USA.2sflib`, a `.mini2sf` for each song and a playlist, `!playlist.m3u`. `2sfrip --help` lists the options.
A game on the SDK's player gets a library named after its product code, such as `NTR-AZEE-USA.2sflib`.

- `--list` lists the songs and their kinds, then stops.
- `--only REGEX` rips the songs whose title, or whose name such as "Song 81" or "SEQ_12", matches.
- `--effects` adds the sound effects to the set. The default is the music alone.
- `--no-length` leaves out the length and fade tags.
- `--game`, `--artist`, `--year`, `--copyright` and `--by` set those tags. `--by` writes `2sfby`.

## How a set plays

The library is the game's image with every file that the music doesn't read set to zeros: it keeps the cartridge's
header, the ARM9 and ARM7 programs, the file system's tables, the song files, the sample bank and an image that the
game loads at start-up. A stub of a few dozen ARM instructions replaces the game's main loop. The game's main function
sets up the SDK, the file system and the sound as it does on the console, and a routine that the music doesn't use
returns at once. Then the stub loads the song whose number the mini stores with the game's loader, starts it, and calls
the sound driver's update once a frame, as the main loop does. A mini is a few bytes: the song's number, and for a
sound effect that plays once, a flag that stops it from looping.

A player has to answer the cartridge's read commands from the image, as it does for any DS set: the driver reads the
song's file and its samples when the song starts.

### Titles and tags

Song 105 is the title screen's music. The game gives its music no names, so the other songs are named by their number
in the game's files. Each mini has its title, the game, the year, the copyright and the length and fade. A song that
loops plays two passes of its loop and fades over 10 seconds. A sound effect that plays once ends at its end, with a
fade of 1 second. The library has no tags.

### Games on the SDK's sound archive player

The library contains the files that the game reads before its sound starts, whole, and the sound archive cut down to
the data of the set's sequences. The ARM9 program is stored decompressed, and so are the overlays that the library
keeps. The start-up decompressor therefore returns at once whenever the game calls it, and the stub replaces the rest
of its code. The first instruction of the main routine of the sound library, which the game calls once a frame,
becomes a branch to the stub. On the first call, the stub loads the sequence that the mini names into the sound heap
with the loader of the game, and starts it. Then it waits for each frame and runs the rest of the main routine, as
the game does. A mini is the sequence's number.

Songs are named as in the sound archive, or `SEQ_` and the sequence's number where the archive has no names. The
length counts from the boot, so it includes the time the game takes to set up its sound. A sequence that loops plays
two passes of its loop and fades over 10 seconds. One that ends gets a fade of 1 second. A sequence that plays no notes
gets no mini. In an archive with names, a sequence whose name and player don't mark it as music counts as a sound
effect, and `--effects` adds it. Where no sequence counts as music, every sequence outside the sequence archives does.
Sequences in sequence archives, where most sound effects are, aren't ripped.

## Credits

2SF was created by unknownfile, whose "2sf specifications, revision 7" defines the format.

## Licence

2sf is released under the MIT licence. See `LICENSE`. `THIRD-PARTY-NOTICES.md` lists the licences of its dependencies.
Rips contain the code and sound data of a game. These belong to their copyright holders. Rips are for personal use.
