# xmp-2sfplay

XMPlay input plugin for 2SF rips (`.mini2sf`, and the rarely used `.2sf`). It's built on the same player as `2sfplay`.
The player boots the set's cartridge image on a model of the DS with both CPUs and the sound unit. The game's code makes
the sound.

XMPlay is a 32-bit program for Windows. The plugin is therefore a 32-bit DLL, `xmp-2sfplay.dll`, built with clang-cl. It
requires XMPlay 3.8.2 or later.

## Installing

The Releases page has `xmp-2sfplay-X.Y.Z.zip`, with the DLL and the licences. Put `xmp-2sfplay.dll` in XMPlay's folder,
or in a folder inside it, and restart XMPlay.

## Features

- Tags become XMPlay's tags: `game` as album and `year` as date. The Message info window lists every tag except `_lib`,
  `length`, `fade`, `loop_start`, `loop_length`, `volume` and `utf8`, and the tags that set how a set plays in other
  players, such as `_arm9clk` and `_arm7clk`.
- Length and fade come from the `length` and `fade` tags. Files without a length tag play for the default length and
  fade. `volume` scales the output.
- Each track plays as a soundtrack album plays it: from the first sound, after 0.5 s of silence, with 1 s of silence
  after it. A loop plays the passes in its tags, or another number of passes with its `loop_start` and `loop_length`
  tags.
- The plugin's options page, among the decoders in XMPlay's options: default length and fade, the fade's curve, endless
  playback, rendering ahead, and the settings of an album's track: skip silence at the start, the silences before and
  after a track, the loops, one pass for long loops, the fade length for loops, ignore length and fade tags, and skip
  silence at the end. XMPlay stores the settings.
- During playback, the plugin renders ahead on a separate thread as fast as the CPU allows. It buffers up to 10 minutes
  and 10 seconds of audio (about 76 MiB). That's enough for a whole track of up to 10 minutes and its fade. `2sfrip`
  gives a longer length only to music whose two passes take longer. Seeking within the buffer is instant. Seeking
  further ahead waits for rendering to reach the requested position. Rendering ahead keeps one CPU core busy until it
  reaches the end of the track or the buffer limit. The player saves no states. A seek back before the buffered audio
  renders the track again from its start. That happens only in a longer track or in endless playback.
- Output is 16-bit stereo at 32,728 Hz, the rate of the DS's sound unit. The output isn't resampled.

## Building

**On GitHub (no Windows machine needed):** the `XMPlay plugin` workflow (`.github/workflows/xmplay.yml`) builds the DLL
and uploads it with the licences. Start it from the repository's Actions tab (Run workflow). GitHub lists a manual
workflow only once it's on the default branch (`main`). The workflow requires GitHub Actions to be enabled for the
repository. It downloads the XMPlay plugin SDK from un4seen.com (`externals/xsflib/ci/xmplay-sdk`).

**On Windows:** you need Visual Studio 2022 or later with the C++ workload and the "C++ Clang tools for Windows"
component, plus CMake 3.23 or later. Download the XMPlay plugin SDK (<https://www.un4seen.com/files/xmp-sdk.zip>) and
unpack it. Use the folder that contains `xmpin.h` as the SDK path below.

```
cmake -S . -B build-xmplay -A Win32 -T ClangCL -DXMPLAY_SDK_DIR=C:/path/to/xmp-sdk
cmake --build build-xmplay --config Release --target xmp-2sfplay
```

This produces `build-xmplay/plugins/xmplay/Release/xmp-2sfplay.dll`. Everything is built with the static C runtime
(`/MT`). The plugin therefore requires no runtime DLLs.

## Performance

Playback emulates a whole DS on one CPU core.

## Licence

Like the rest of 2sf, the plugin is MIT-licensed. It uses xsflib (MIT), ntdlib (MIT) and zlib. Ship `LICENSE` and
`THIRD-PARTY-NOTICES.md` (or their text) with a build. The XMPlay plugin SDK states no licence. The plugin uses only the
SDK's declarations of XMPlay's interface. No code from the SDK goes into it.
