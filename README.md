# 2sf

2sf rips the music of Nintendo DS games to 2SF, the PSF-family format for DS music. A 2SF set contains the cartridge
image of the game, cut down to what its music reads, with a small stub that starts one song. Each song gets a small
file, a mini, that names the song to start. A 2SF player emulates the DS and runs the sound code of the game, and the
music plays as it does in the game.

`2sfrip` is the ripper. It supports Beat City, the games that play their music with the sound archive player of
Nintendo's SDK from a sound archive (SDAT), seventeen games on Procyon Studio's Digital Sound Elements (DSE), and Super
Princess Peach on Nintendo SPD's sound driver. The games on DSE are the three Pokémon Mystery Dungeon: Explorers games,
four Inazuma Eleven games, three Professor Layton games, three Shiren games, the three Luminous Arc games and Soma
Bringer. More games and drivers will follow.

`2sfplay` plays the sets and renders them to WAV or FLAC files. The foobar2000 component, `foo_input_2sfplay`, and the
XMPlay plugin, `xmp-2sfplay`, play them in those players. All three use the same player: a model of the DS with both
CPUs and the sound unit that runs the code of the game.

## Supported games

| Game | Release | Sound driver | What the set contains |
|---|---|---|---|
| Beat City | USA (NTR-BEYE) | Universomo's | The 66 pieces of music. `--effects` adds the 81 sound effects |
| The Legend of Zelda: Phantom Hourglass | USA (NTR-AZEE) | The SDK's sound archive player, through the game's sound manager | The sequences outside the sequence archives: 112 minis, with two states of the sea's music, and 18 streams. `--effects` adds the 1,449 sound effects that play notes |
| The Legend of Zelda: Spirit Tracks | USA (NTR-BKIE) and Europe (NTR-BKIP), each in revisions 0 and 1 | The SDK's sound archive player, through the game's sound manager | The sequences outside the sequence archives: 228 minis, with 28 states of the pieces, and 21 streams. `--effects` adds the 2,331 sound effects that play notes |
| The Legend of Zelda: Four Swords Anniversary Edition | USA (TWL-KQ9E) and Europe (TWL-KQ9V), from images with the secure area decrypted | The SDK's sound archive player, and the game's player of its streams | The sequences outside the sequence archives, and the 8 streams of the areas |
| Pokémon Mystery Dungeon: Blue Rescue Team | USA (NTR-APHE) | The SDK's sound archive player | The 77 pieces of music that play notes and the 19 short pieces (MEs) |
| Fire Emblem: Shadow Dragon | USA (NTR-YFEE) | The SDK's sound archive player | The sequences outside the sequence archives, with the six fanfares (FF_), and 3 streams |
| Fire Emblem: Shin Monshou no Nazo: Hikari to Kage no Eiyuu | Japan (TWL-VI2J), revision 1 | The SDK's sound archive player | The sequences outside the sequence archives, with the six fanfares (FF_), and 4 streams |
| Pokémon Mystery Dungeon: Explorers of Sky | USA (NTR-C2SE) | DSE | The music: 195 pieces, one of them unused, three of them at two levels each, and 14 short pieces |
| Pokémon Mystery Dungeon: Explorers of Time | USA (NTR-YFTE) | DSE | The music: 127 pieces, one of them unused, one of them at two levels, and 14 short pieces |
| Pokémon Mystery Dungeon: Explorers of Darkness | USA (NTR-YFYE) | DSE | The music: 127 pieces, one of them unused, one of them at two levels, and 14 short pieces |
| Inazuma Eleven | Japan (NTR-YEEJ), revision 1 | DSE | The music: 24 pieces, and 14 streams: the title's and the ending's music and 12 jingles |
| Inazuma Eleven 2: Kyoui no Shinryakusha: Fire | Japan (NTR-BEEJ), revision 1 | DSE | The music: 46 pieces, and 15 streams: the title's and the ending's music and 13 jingles |
| Inazuma Eleven 3: Sekai e no Chousen!! Bomber | Japan (NTR-BEZJ) | DSE | The music: 59 pieces, and 16 streams: the title's and the ending's music and 14 jingles |
| Inazuma Eleven 3: Sekai e no Chousen!! The Ogre | Japan (NTR-BOEJ) | DSE | The music: 77 pieces, and 16 streams: the title's and the ending's music and 14 jingles |
| Professor Layton and the Diabolical Box | USA (NTR-YLTE) | DSE | The music: 15 pieces, and 24 streams: 5 pieces of music and 19 short pieces, among them a few sound effects |
| Professor Layton and the Unwound Future | USA (NTR-C3JE) | DSE | The music: 28 pieces, and 31 streams: 3 pieces of music and 28 short pieces |
| Professor Layton and the Last Specter | USA (NTR-BLFE) | DSE | The music: 23 pieces of the main game, 9 of London Life, and 83 streams: 3 pieces of music and 78 short pieces of the main game, among them a few sound effects, and 2 pieces of London Life |
| Fushigi no Dungeon: Fuurai no Shiren DS 2: Sabaku no Majou | Japan (NTR-YWFJ) | DSE | The music: 41 pieces, three of them at two levels each, and 7 short pieces |
| Fushigi no Dungeon: Fuurai no Shiren 4: Kami no Hitomi to Akuma no Heso | Japan (NTR-B4FJ) | DSE | The music: 72 pieces, three of them sounds of a jungle, and 12 short pieces |
| Fushigi no Dungeon: Fuurai no Shiren 5: Fortune Tower to Unmei no Dice | Japan (NTR-B5FJ) | DSE | The music: 75 pieces, four of them sounds of a jungle and of wind, and 16 short pieces |
| Luminous Arc | USA (NTR-ANIE) | DSE | The music: 38 pieces |
| Luminous Arc 2 | USA (NTR-YL2E) | DSE | The music: 40 pieces |
| Luminous Arc 3: Eyes | Japan (NTR-BLUJ) | DSE | The music: 43 pieces |
| Soma Bringer | Japan (NTR-YBSJ) | DSE | The music: 52 pieces |
| Super Princess Peach | USA (NTR-ASPE) | Nintendo SPD's | The music: 96 sounds of the music player and two variants |
| Pokémon Black | USA (TWL-IRBO) | The SDK's sound archive player, through the game's music manager | The music: 240 minis, with a mini for each state of a field piece's tracks that the game sets, and the two pieces of its download-play program with the minigame's last 20 s |
| Pokémon White | USA (TWL-IRAO) | The SDK's sound archive player, through the game's music manager | The music: 240 minis, with a mini for each state of a field piece's tracks that the game sets, and the two pieces of its download-play program with the minigame's last 20 s |
| Pokémon Black 2 | USA (TWL-IREO) | The SDK's sound archive player, through the game's music manager | The music: 350 minis of sequences, with a mini for each state of a field piece's tracks and the two pieces of its download-play program with the minigame's last 20 s, and the title screen's stream |
| Pokémon White 2 | USA (TWL-IRDO) | The SDK's sound archive player, through the game's music manager | The music: 350 minis of sequences, with a mini for each state of a field piece's tracks and the two pieces of its download-play program with the minigame's last 20 s, and the title screen's stream |
| Pokémon HeartGold | USA (NTR-IPKE), revision 10 | The SDK's sound archive player, through the game's music manager | The music, the field music and the short pieces (MEs): 357 minis |
| Pokémon SoulSilver | USA (NTR-IPGE) | The SDK's sound archive player, through the game's music manager | The music, the field music and the short pieces (MEs): 357 minis |
| Pokémon Diamond | USA (NTR-ADAE), revision 5 | The SDK's sound archive player, through the game's music manager | The music, the field music and the fanfares (MEs): 188 minis, with the TV's music in a room |
| Pokémon Pearl | USA (NTR-APAE), revision 5 | The SDK's sound archive player, through the game's music manager | The music, the field music and the fanfares (MEs): 188 minis, with the TV's music in a room |
| Pokémon Platinum | USA (NTR-CPUE), revision 1 | The SDK's sound archive player, through the game's music manager | The music, the field music and the short pieces (MEs): 223 minis, with the TV's music in a room and the Wi-Fi game's music at its fast tempo |
| Mario Kart DS | USA (NTR-AMCE) | The SDK's sound archive player, through the game's music manager | The music: 104 minis, with each course's music on the final lap |

`2sfrip` recognises Beat City by its game code and by the instructions at the addresses that the stub calls. For a game
that it doesn't recognise, it looks for a sound archive and for the SDK's sound routines in the code of the game. Then
it runs every mini on its model of the DS. The command that prepares the sequence has to name the data of the sequence
and its bank in memory. The mini then has to play for 5 seconds without the game changing the sequence or reading data
that the set leaves out. If a mini fails, `2sfrip` refuses the game. Other games on the SDK's player may rip as well.
The table lists the games that have been checked.

Pokémon Black and Black 2 send a second DS a download-play program, Poké Transfer. It plays two pieces that the game's
sound archive doesn't contain: `SEQ_BGM_PALPARK_BOX` and `SEQ_BGM_PALPARK_GAME`. White and White 2 send the same
programs. `2sfrip` rips the program as a set of its own and writes it beside the set of the game. The set has a library
named after the library of the game with `_child_r_eng` added, such as `NTR-IRBO_child_r_eng.2sflib`, and a mini for
each piece, numbered after the game's minis. The program has no file system. Its library contains the sound archive of
the minigame in place of four archives in the program's code that the set doesn't use. The library also leaves out the
box screen's start of its music, and each mini starts its piece with the routines of the program. In the last 20 s of a
round, the minigame plays its piece 11 % faster and 1.33 semitones higher. `SEQ_BGM_PALPARK_GAME_HURRY` plays it that
way.

Shin Monshou no Nazo starts its sound again with one larger sound heap before it plays its ending piece,
BGM_SYS_ENDING4. Before that piece, the stub calls the sound start-up of the game with the same setting. A copy of the
game whose start-up differs from the one in the table of `2sfrip` gets a set without that piece, and `--list` marks it
as not ripped.

Blue Rescue Team starts its music with routines of its own. `2sfrip` recognises the game by its game code and checks the
instructions at the addresses that the stub calls. Like the game, the stub starts each piece with the routine for music
or the routine for a short piece. That routine gives each track of the piece the channels that the game gives it in
play: on most pieces, tracks 8 to 15 share three channels. The game calls the sound update from a thread that the
vertical blank wakes, and the stub waits in that thread. The main thread of the game sleeps from the end of the sound
start-up on.

Pokémon Diamond, Pearl, Platinum, HeartGold, SoulSilver, Black, White, Black 2 and White 2 and Mario Kart DS start their
music through a music manager too, and `2sfrip` recognises them the same way. Like the game, the stub starts each piece
with the routines of the game. The manager picks the player and the bank of the piece, and sets what the game sets at
the start: the channels of the tracks, the volume, and the reverb for the title and the credits of Diamond, Pearl and
Platinum. Where the game plays a piece in another state, the piece gets a mini for each state, named after the piece
with the state added. The field units of Black, White, Black 2 and White 2 change the volumes of some tracks, or the
tempo and the pitch. The TV of Diamond, Pearl and Platinum plays quieter in a room (`SEQ_TV_HOUSOU_ROOM`). Mario Kart DS
plays the music of each course faster and a semitone higher on the final lap (`SEQ_15_FINAL_LAP`). `2sfrip` compares
what each mini sends at its start with the profile of the game and warns of any difference.

Phantom Hourglass and Spirit Tracks start their music through a sound manager as well. `2sfrip` recognises them the same
way, Spirit Tracks in revisions 0 and 1. Spirit Tracks gives each piece a state of its mixer. The state sets the volume,
the tempo, and the volume and the pitch of each track. Where a scene plays a piece in another state, the piece gets a
mini for each state, such as `SEQ_101_STOPPED`, a slower mix of the sea's music in Phantom Hourglass. Some states change
as the piece plays. In the `_INTRO` minis of Spirit Tracks, the intro plays with every track at full volume and then
fades to the state of the mixer. Such a mini calls the routines of the game in the frames in which the game calls them.
Some cutscenes of Spirit Tracks play a stream beside the music, and the game keeps two channels free for it. The minis
of those pieces keep the same two channels free, from the same frames, but don't play the stream. Numbers whose minis
would start the same sound share one mini, and `2sfrip` names the others in its output. The sound effects of both games
are in sequence archives, and `--effects` adds them. The game's routine for effects starts each one. Its mini is titled
`Effect` with the number of the archive and the index of the effect, such as `Effect 0-2`.

The sets of Black, White, Black 2, White 2 and Mario Kart DS contain patched game code. Without a save, the four Pokémon
games stop at an error screen that never returns. The set leaves out the call of that screen, and the game then takes
the path of a new save. Mario Kart DS queues its title music while it boots. The set leaves out that call, and the piece
that the mini names plays alone. A set changes the code of a game only where the game can't reach its music otherwise.

Black 2 and White 2 set up the SDK's stream player only on their title screens. Like the title screen, the mini of each
title stream calls the stream manager of the game to set up the player and start the stream.

The music managers of the Pokémon games from Diamond to SoulSilver don't start `SEQ_BGM_END`, a record that names no
sound. In a game that starts its music through its manager, `2sfrip` leaves out a sequence that the code of the game
doesn't start and that plays no notes, and names it in its output with the reason. Any other failing mini makes `2sfrip`
refuse the game.

`--effects` doesn't rip the Pokémon games from Diamond to SoulSilver yet. Their music managers start only the players of
the music, and the sound effects don't start in a set.

`2sfrip` recognises the Explorers, Inazuma Eleven, Professor Layton, Shiren and Luminous Arc games and Soma Bringer by
their game codes and checks the instructions at the addresses that the stub calls. It runs every mini on its model of
the DS too. The game has to load the song and its bank and start the song, then let it play for 5 seconds, or until the
end of a short piece that ends sooner. Other games on DSE don't rip yet.

The sets of the games on DSE contain the music that the games sequence, and the streamed music of the Professor Layton
and Inazuma Eleven games (see below). The sound effects aren't ripped yet. The one exception is a few effects that the
Professor Layton games start as streams, the same way as their short pieces of music. A song whose files are a copy of
the files of an earlier song, and which the game starts at the same volume, plays the same and gets no mini. Two short
pieces of Shiren 5 and one piece of Inazuma Eleven 3 Bomber are such copies. `2sfrip` names each copy in its output, and
`--list` marks a song whose files repeat an earlier one.

The three Explorers games start some of their music below full level. The stub starts each piece with the game's routine
that takes a level, with no fade, at the level that most of the scenes in the scripts of the game start it at. Eight
pieces of Sky and six of Time and Darkness play below full level. A piece that several scenes start at a second level
gets a second mini at it, named for the level out of 256: `bgm0086_LEVEL256`, `bgm0125_LEVEL190` and `bgm0181_LEVEL190`
in Sky, and `bgm0125_LEVEL190` in Time and Darkness. Every other piece plays at full level. The Sky Jukebox plays all of
them at full level.

Many games play some of their music as streams: recorded audio that the sound archive contains beside its sequences. A
set of a game on the SDK's player contains its streams too, and the code of the game plays them as it plays the
sequences. Each stream file goes in a library of its own, and the mini of the stream names that library as `_lib2`. A
mini of a sequence loads none of them. A stream file that the game reads while it boots stays whole in the library of
the set and gets no library of its own. So does every stream file when `2sfrip` can't cut the sound archive and keeps it
whole.

`2sfrip` finds the SDK's routine that starts a stream in the code of the game, and checks the mini of every stream on
its model of the DS. The game has to set up a channel for each channel of the stream with the start of the first block
in memory, start the channels, and then read more of the stream file. If the mini of a stream fails, `2sfrip` refuses
the game. If the search doesn't find the routine, `2sfrip` leaves the streams out with a warning. A stream that makes no
sound is left out. Streams whose records give the same file, volume, priority, player, flags and reserved bytes play the
same. As with sequences, only the first gets a mini.

The mini of a stream starts it as that routine does: it plays the stream once, at the volume that the sound archive
gives it. The code of some games loops a stream that ends, or lowers the volume of a stream, and their minis do neither.
With `--stream-files`, `2sfrip` writes the stream files of the game (`.strm`) beside the set instead, for sites that
want the original files. The playlist doesn't list them, and a rip of streams alone writes no library. A `.strm` file
doesn't contain the volume that the sound archive gives its stream, and a player plays it at the full level of the file.
`2sfrip` doesn't rip streams in other formats yet, apart from those of Four Swords Anniversary Edition and of the games
on DSE (see "Games on DSE"). It doesn't rip songs that chain several streams either.

Four Swords Anniversary Edition plays the music of its areas from 8 files of its own (`.wave`), with a player in the
code of the game. A table in the sound data of the game gives the rate, the format (8-bit PCM or IMA-ADPCM) and the loop
of each file. `2sfrip` recognises the game by its game code and checks the instructions at the addresses that the stub
calls. Like the game, the mini of a stream calls the play routine of the game with the number of the stream, from 490
on. Then it calls the routine of the game for each frame, and that routine updates the stream players and the sound
library. The game loads the whole file and plays it on one channel. `2sfrip` checks that the game sets up a channel at
the rate of the stream with the data of the file, and starts it. As for the streams of a sound archive, it also checks
that the game leaves the channel alone after the start. The mini is named after the file, such as `zelda_tit`. Its
length is two passes of a loop at the rate that the channel plays, or the last sound of a file that ends.

## Players

Every set that 2sf makes plays in 2sf's player: `2sfplay`, the foobar2000 component and the XMPlay plugin. They read
sets from other tools too. The component runs in 64-bit and 32-bit foobar2000 on Windows and in foobar2000 for Mac.

Beat City's sets also play in vio2sf 0.26.2, the usual 2SF component for foobar2000. These sets don't play in it:

- The sets of Phantom Hourglass and Spirit Tracks stay silent. vio2sf can't run the start-up of these games, and 2sf
  doesn't change the start-up of a game to suit one player. These sets play only in 2sf's player. The same is true of
  any other game whose start-up vio2sf can't run.
- Four Swords Anniversary Edition runs only on a DSi. vio2sf has no DSi mode, and the sets of games that run only on the
  DSi play only in 2sf's player.
- The stream minis of the Professor Layton and Inazuma Eleven games, such as the title music of Inazuma Eleven, end at
  once with no sound in vio2sf 0.26.2 (32-bit foobar2000 on Windows).

Soma Bringer's sets play in 2sf's player. They haven't been tried in vio2sf 0.26.2.

vio2sf and 2sf's component both open `.mini2sf` files. If both are installed, foobar2000 uses one of them for these
files, and it may use vio2sf. To play the sets of Phantom Hourglass, Spirit Tracks and Four Swords Anniversary Edition
and the stream minis of the Professor Layton and Inazuma Eleven games in foobar2000, remove vio2sf (`foo_input_vio2sf`)
in Preferences › Components and install 2sf's component. Do the same for any other set that stays silent in vio2sf.
2sf's component plays Beat City's sets as well.

## Building

2sf requires CMake 3.23 or later and a C++20 compiler: Clang or Apple Clang on macOS, clang-cl on Windows. The libraries
that it builds on are submodules.

```
git clone --recursive <repository>
cd 2sf
cmake --preset release
cmake --build --preset release
ctest --preset release
```

The foobar2000 component builds when `FOOBAR2000_SDK_DIR` names the foobar2000 SDK, and the XMPlay plugin when
`XMPLAY_SDK_DIR` names the XMPlay plugin SDK. `plugins/foobar2000/README.md` and `plugins/xmplay/README.md` explain how.

## Using 2sfrip

```
2sfrip "Beat City (USA).nds"
```

writes the set into a folder next to the image, named after it with `_2sf` added: the library `NTR-BEYE-USA.2sflib`, a
`.mini2sf` for each song and a playlist, `!playlist.m3u`. `2sfrip --help` lists the options. A library is named after
the product code of the game, such as `NTR-AZEE-USA.2sflib` for Phantom Hourglass. Some games get `NTR-` and the game
code instead, such as `NTR-YLTE.2sflib` for the Diabolical Box.

- `-o DIR` puts the rips in DIR.
- `--list` lists the songs and their kinds, then stops.
- `--only REGEX` rips the songs whose title, or whose name such as "Song 81" or "SEQ_12", matches.
- `--effects` adds the sound effects to the set. The default is the music alone.
- `--stream-files` writes the stream files of a game on the SDK's sound archive player beside the set, in place of
  stream minis.
- `--no-length` leaves out the length and fade tags.
- `--jobs N` sets how many songs run at once. The default is the number of CPU threads.
- `--game`, `--artist`, `--year`, `--copyright` and `--by` set those tags. `--by` writes `2sfby`. Without `--game`, the
  game tag is the name of the image file.

## Using 2sfplay

```
2sfplay "01 Song 81.mini2sf"
```

renders the song to `01 Song 81.wav` next to it, for the length and the fade that its tags give. `2sfplay --help` lists
the options.

A track plays as a soundtrack album plays it: from its first sound, after 0.5 s of silence, with 1 s of silence after
it. A loop plays the passes in its tags, or another number of passes with `--loops`.

- `-o FILE` writes FILE instead. Its extension selects WAV or FLAC.
- `--length` and `--fade` replace the times in the tags. `--default` sets the length of a file without a length tag.
- `--fade-exponent P` sets the curve of the fade, from 0.5 to 4. 1 fades linearly. The default, 2, fades with the square
  of the time left in the fade. Most soundtrack albums fade that way.
- `--keep-opening`, `--start-silence`, `--end-silence`, `--loops`, `--loop-fade`, `--one-pass-over`, `--ignore-tags` and
  `--keep-ending` change how a track plays. `--raw` plays the file as it is, with no skip and no added silence.
- `--info` prints the tags and the files that each input loads, without rendering.

The output is 16-bit stereo at 32,728 Hz, the rate of the DS sound unit.

## How a set plays

A set boots the game from its cartridge image. Where the sound of the game runs, a stub that `2sfrip` writes takes over
and starts the song that the mini names with the routines of the game. As for any DS set, a player has to answer the
cartridge's read commands from the image: the driver reads the song's file and its samples when the song starts.

### Lengths and tags

Each mini has its title, the game, the length and the fade, and the artist, the year and the copyright when the options
give them. The library has no tags. A song that loops plays two passes of its loop and fades over 7 seconds. A piece
that ends stops at its last sound, with a fade of 1 second.

The length counts from the first sound: the first sample that isn't 0 of what the player renders. Players that skip the
silence at the start of a track start it there. `2sfrip` takes that sample from a render of each mini by 2sf's player.
The end of a piece that ends is the last sample over -72 dB of that render. A loop also gets `loop_start` and
`loop_length`, the start of its loop and the length of one pass. 2sf's player reads them to play another number of
passes.

### Beat City

The library is the image of the game with every file that the music doesn't read set to zeros. It keeps the header of
the cartridge, the ARM9 and ARM7 programs, the tables of the file system, the song files, the sample bank and an image
that the game loads at start-up. A stub of a few dozen ARM instructions replaces the main loop of the game. The main
function sets up the SDK, the file system and the sound as it does on the console, and a routine that the music doesn't
use returns at once. Then the stub loads the song whose number the mini stores with the loader of the game and starts
it. Like the main loop, it then calls the update of the sound driver once a frame. A mini is a few bytes: the number of
the song, and for a sound effect that plays once, a flag that stops it from looping.

Each song is named by its number in the files of the game, as "Song 81", and each sound effect as "Effect 12". The set
lists the music by number, then the effects.

### Games on the SDK's sound archive player

The library contains the files that the game reads before its sound starts, whole, and the sound archive cut down to the
data of the sequences of the set. The ARM9 program is stored decompressed, and so are the overlays that the library
keeps. The start-up decompressor therefore returns at once whenever the game calls it, and the stub replaces the rest of
its code. The first instruction of the main routine of the sound library, which the game calls once a frame, becomes a
branch to the stub. On the first call, the stub loads the sequence that the mini names into the sound heap with the
loader of the game, and starts it. Then, as the game does, it waits for each frame and runs the rest of the main
routine. A mini is the number of the sequence.

Some archives give several numbers the same record: the same sequence, bank, volume, priorities and player. A set starts
a sequence with the SDK's start routine, and that routine reads only the record. Such numbers therefore play the same in
a set. Only the first gets a mini, and `2sfrip` names the others in its output and in `--list`.

Songs are named as in the sound archive, or `SEQ_` and the number of the sequence where the archive has no names. A
sequence that plays no notes gets no mini. In an archive with names, a sequence whose name and player don't mark it as
music counts as a sound effect, and `--effects` adds it. Where no sequence counts as music, every sequence outside the
sequence archives does. Sequences in sequence archives, where most sound effects are, aren't ripped. The exception is
the sound effects of Phantom Hourglass and Spirit Tracks with `--effects`.

### Games on DSE

The sequencer of DSE runs on the ARM9, in a thread that the sound driver of the ARM7 wakes once a tick: every 10 ms, or
once a video frame in the main game of Professor Layton and the Last Specter.

In the Explorers games, the set runs the start-up of the game until the sound, the thread that counts the frames of the
game and the handler of the vertical blank are running. There the stub takes over the main thread of the game. It starts
the song that the mini names with the game's routine for music, or for a short piece of music (an ME), then sleeps. The
sound thread of the game then loads the song and its bank through the file system of the game and starts it. The thread
of the tasks of the game never starts. The screens of the game never run, and nothing stops the song. The thread that
counts the frames skips the work of the game for each frame and keeps the work of the sound. The skipped work reads the
buttons, the touch screen and the lid, and steps the random numbers of the game. The audio is the same with and without
it, and the set plays many times faster. A mini is the number of the song, followed by the word 1 for a short piece, and
by the level for a piece at a second level.

The library contains the files that the game reads at start-up, among them the main bank of samples, whole, and the
files of the songs of the set. Songs are named after their files, such as `bgm0001` and `me0000`. Each game has a piece
of music, `bgm0000`, that none of its routines starts: the sound code of the game refuses number 0. The set changes that
one instruction, and the code of the game then plays `bgm0000` like any other piece. Its mini comes last. A song that
plays no notes gets no mini. `--effects` adds nothing yet: the sound effects of these games aren't ripped.

The Inazuma Eleven games start their music by the name of the song, from the main loop of the game. There the stub
starts the song with the game's routine for music, with no fade in or out. It then runs the loop of the game for the
sound: it waits for each frame and runs the work of the sound for the frame, and that work loads the song and its bank
and starts it. A mini is the name of the song. Inazuma Eleven keeps each song in a file of its own. The other three keep
their songs and banks in one sound pack. The set keeps the pack at its size, with the songs that the set plays and what
the game reads at start-up, and zeros elsewhere. Songs are named after the name in their headers, such as `P04`. A few
of these differ from the name of the song in the files of the game: the game names `SR_000_D` `SR_001`, for example.

The Professor Layton games start their music from the scripts of their scenes, by the number of the song. Their engine
and DSE are in overlays of the ARM9 code, and the hook is in one of them. The library contains each of those overlays
decompressed, and the stub is in the overlay with the hook. The stub makes the same calls as the start of music in a
script, with no fade in or out. In the Diabolical Box, the call loads the song and its bank and starts it, and the stub
then sleeps. In the Unwound Future and the main game of the Last Specter, the calls only ask for the song. The stub then
waits for each frame and runs the work of the game for the frame that loads the song and its bank and starts it. A mini
is the number of the song. Songs are named after their files, such as `BG_004`. The Unwound Future keeps each song with
its bank in a compressed file of its own, and the Last Specter keeps its songs in one large archive. The library keeps
what the game reads of those files, at their sizes, with zeros elsewhere.

Professor Layton and the Last Specter contains a second game, London Life. It starts DSE again, with a tick of 10 ms,
and plays pieces that the main game doesn't. `2sfrip` rips it as a second set beside the set of the game: a library
named after the library of the game with `_ll` added, `NTR-BLFE_ll.2sflib`, and a mini for each piece, numbered after
the minis of the game. The library changes the one instruction with which the start-up picks the game to run, and the
set always starts London Life. The hook is at the end of the first step of the scenes of London Life, after its sound
has started. There the stub makes the calls of London Life for a piece of music, with no fade in. They load the piece
and its bank and start it, and the stub then sleeps. The names of the London Life pieces end in `_ll`, such as
`BG_004_ll`. Several of them have the names of pieces of the main game.

The Professor Layton and Inazuma Eleven games play some of their music, many short pieces and jingles, and in the
Professor Layton games a few sound effects as streams beside DSE. The streams are files in Procyon Studio's sadl format.
The stream player of the game decodes them on the ARM9 into a buffer for each channel, and the sound hardware plays the
buffers at 32,728 Hz. A set of these games contains the streams too, with a mini for each one after the minis of the
songs. Each stream file goes in a library of its own, and the mini of the stream names that library as `_lib2`. The
library of the set keeps the place of the file, with zeros. A mini of a stream makes the calls with which the scripts of
the game start a stream, with no fade in. The Professor Layton games and London Life start a stream by its number. The
Inazuma Eleven games start it by the name of the file: their start of music plays a stream for a name that ends in
`.SAD`. `2sfrip` runs the mini of every stream on its model of the DS too. The game has to set up a channel for each
channel of the stream at the rate of the stream and start them. Then it has to fill their buffers with the first samples
of the stream and read more of the stream file. The games have two builds of the stream player. They round the samples
differently, and `2sfrip` decodes the streams of each game as its player does. A stream is named after its file, such as
`BG_001` or `J01`, and the names of the London Life streams end in `_ll`. Its length is two passes of the loop that its
header gives, or the last sound of a stream that ends. The streams of the voices of the characters and of the movies are
left out, and so is a stream that makes no sound.

The Shiren games start their music by the number of the song, with the game's request for music. The stub makes the same
request with no fade. In Shiren DS 2 the request loads the song and its bank and starts the song, and the stub then
sleeps. The game's table of songs gives each number its song and bank. The scripts of the game start some pieces below
full level, and the stub passes the level of the mini's state. A piece that the scripts start at a second level gets a
second mini at it, such as `SND_BGM_M_BOOM2_LEVEL220`. Songs are named after their files. Shiren 4 and 5 run Thumb code.
Their stub makes the request of music or of a short piece, then waits for each frame and runs the sound work for the
frame, and that work loads and starts a short piece. The two games keep each song with its bank in one of two packs, one
for music and one for short pieces, and a mini is the number of the song in its pack. The library keeps each pack at its
size, with the entries of the songs of the set and zeros elsewhere. The two games start each piece at a volume that the
game sets: a level for the piece times the level of the game's options. Shiren 5 lowers three pieces in the frame in
which it starts them, and their minis make the same change. Songs are named after the name in their headers, cut to 15
characters, such as `BGM_DUN_BOSS_JA`, with `_2` and up for a name that comes again. A few have Shiren DS 2's names,
such as `SND_BGM_M_SHOP`. The games stop a short piece once it ends, and so do the sets.

The Luminous Arc games and Soma Bringer start their music by the number of the song, with the game's request for music.
The stub makes the same request with no fade. In the Luminous Arc games the request loads the song and its bank and
starts the song, and the stub then sleeps. Luminous Arc 2 and 3 keep each song in a file of its own, and the game's
table of songs gives each number the file's name. Songs are named after their files, such as `ADV01` and `BGM01`.
Luminous Arc keeps its songs and banks in one archive, and a table in the code of the game gives each number its
members. The library keeps the archive at its size, with its table of members and the songs of the set, and zeros
elsewhere. Its songs are named after the names in their headers, such as `02LUMINOUS_ARC_` and `16BALLTE7`. The game
starts its music at a volume of 100 out of 127, and so do the sets.

Soma Bringer keeps its songs in a cartridge image inside the files of the game, with a file for each song. The game
gives each song a number by the name of the file. Its stub first sets up the file loader of the game and the object that
plays its music. The game sets them up the same way on its way from the title into the game. Then it makes the request.
It then waits for each frame and runs the work of the game for the music and for the loader. That work reads the file of
the song, loads the song and its bank and starts the song. The library keeps the files of the songs of the set whole,
and zeros elsewhere. Songs are named after their files, such as `F01` and `B01`.

### Games on Nintendo SPD's driver

Super Princess Peach plays its sound with a driver of Nintendo SPD's on the ARM7. The sound data is one file, made of
blocks. The first block contains the programs, the samples and the sounds that every scene uses (set A), and the game
loads one other block at a time beside it (set B). The set runs the start-up of the game until the sound driver has
started. There the stub takes over the main function of the game. It loads the block that contains the mini's sound as
set B with the game's routine, and starts the sound with no fade. Then, as the scenes of the game do, it waits for each
vertical blank and sends the game's messages to the driver. The screens of the game never run. A mini is the number of
the sound in its block, from 1, the slot of the block and a flag. With the flag at 1, the stub sets the branch flag of
the driver right after the start. The code of the game does the same.

The library contains the files that the game reads at start-up, whole, and the sound data. Sounds are named by the slot
of the block and the place of the sound in the block, from 0, such as `01_000`. The set contains the sounds of the music
player, player 0. `--effects` adds nothing yet. A sound that makes no sound gets no mini: 12 of the 122 sounds of the
game on player 0 make none. Neither does a sound that only starts another sound in its place. The log names such a sound
as an alias of the other. The game can set a flag that sends two pieces, 02_007 and 03_002, on to sections that they
don't reach otherwise. Those pieces get a second mini with the flag set, `02_007_FLAG1` and `03_002_FLAG1`, and their
six sections get no mini of their own. `2sfrip` recognises the game by its game code and checks the instructions at the
addresses that the stub calls. It runs every mini on its model of the DS: the stub has to start the sound of the mini,
and the sound has to play. The length comes from a model of the driver: two passes of a loop, or the last note of the
sound. For a sound that ends, a render by 2sf's player then sets the end. A sound that holds a note without end fades
out from where it stops changing.

### Games that run only on the DSi

Four Swords Anniversary Edition runs only on a DSi. Its sets run in the DSi mode of 2sf's player. The player picks that
mode from the unit code in the header of the image. The mode has the DSi's 16 MiB of main memory, its registers, its
shared work memory, its DMA and the calls of its BIOS. The game starts from the card, with the state that the DSi menu
leaves in memory. The game checks each sector of 0x400 bytes that it reads from the card against the hash tables in the
image. The library therefore keeps the layout of the image and its hash tables. The files that the music doesn't read
contain zeros, apart from their bytes in a sector of a file that the music reads. `2sfrip` writes `_dsi=1` in the minis.
The sets play only in 2sf's player.

A game that runs on a DS and also has DSi features, such as Pokémon Black, rips as a DS runs it.

### The backup memory

A cartridge has a backup memory for the game's saves, and a 2SF set can't say which kind. Some games read other files at
start-up when they find one. A Naruto game from Japan (NTR-AREJ) shows an error screen without a backup memory, and its
logo screen with one. A player that gives the game a backup memory, as a DS and melonDS do, therefore requires files
that a player without one never reads. `2sfrip` starts each game with no backup memory and with a blank one of each
size, and the library keeps the files that any of those start-ups reads. A set can start up more slowly than the game
and read more before its music starts. The library therefore also keeps what the set reads when every file of the game
is in it. The checks then start the library on each backup memory, and refuse it if it reads other files than that set
does. They run every mini on each start-up. The lengths come from the start-up without a backup memory.

## Credits

2SF was created by unknownfile, whose "2sf specifications, revision 7" defines the format.

## Licence

2sf is released under the MIT licence. See `LICENSE`. `THIRD-PARTY-NOTICES.md` lists the licences of its dependencies.
Rips contain the code and sound data of a game. These belong to their copyright holders. Rips are for personal use.
