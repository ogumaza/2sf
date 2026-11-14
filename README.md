# 2sf

2sf rips the music of Nintendo DS games to 2SF, the PSF-family format for DS music. A 2SF set holds the game's
cartridge image, cut down to what its music reads, with a small stub that starts one song. Each song gets a small file,
a mini, that names the song to start. A 2SF player emulates the DS and runs the game's sound code, so the music
plays as it does in the game.

`2sfrip` is the ripper. This release supports one game, Beat City. More games and drivers will follow.

## Supported games

| Game | Release | Sound driver | What the set holds |
|---|---|---|---|
| Beat City | USA (NTR-BEYE) | Universomo's | The 66 pieces of music. `--effects` adds the 81 sound effects |

`2sfrip` recognises a supported game by its game code and by its code at the addresses that the stub calls. It refuses
any other image.

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

- `--list` lists the songs and their kinds, then stops.
- `--only REGEX` rips the songs whose title, or whose name such as "Song 81", matches.
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

## Credits

2SF was created by unknownfile, whose "2sf specifications, revision 7" defines the format.

## Licence

2sf is released under the MIT licence. See `LICENSE`. `THIRD-PARTY-NOTICES.md` lists the licences of its dependencies.
Rips contain the code and sound data of a game. These belong to their copyright holders. Rips are for personal use.
