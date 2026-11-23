# foo_input_2sfplay

foobar2000 input component for 2SF rips (`.mini2sf`, and the rarely used `.2sf`). The component is built on the same
player as `2sfplay`. The player boots the set's cartridge image on a model of the DS with both CPUs and the sound unit.
The game's code makes the sound.

vio2sf (`foo_input_vio2sf`) opens the same files. If both are installed, foobar2000 uses one of them for these files.
Remove the other to be sure which one plays. Some of 2sf's sets play silent in vio2sf 0.26.2. The "Players" section of
2sf's README lists them.

**Status:** the component builds with clang-cl for 32-bit and 64-bit foobar2000 on Windows. On macOS, Apple Clang builds
it for Apple Silicon. Releases include a package for 64-bit and 32-bit foobar2000 on Windows
(`.github/workflows/release.yml`). `ctest` covers the player. xsflib's tests cover rendering ahead.

## Features

- Tags are shown as metadata in the same way as in kode54's PSF-family components: `game` as album, `year` as date, and
  `replaygain_*` as ReplayGain. Tags can be edited. Editing rewrites the tag area and keeps `_lib`, `length`, `fade`,
  `loop_start`, `loop_length` and `volume`, and the tags that set how a set plays in other players, such as `_arm9clk`
  and `_arm7clk`. The component doesn't show those as metadata.
- Length and fade come from the `length` and `fade` tags. Files without a length tag play for the default length and
  fade. `volume` scales the output.
- Each track plays as a soundtrack album plays it: from the first sound, after 0.5 s of silence, with 1 s of silence
  after it. A loop plays the passes in its tags, or another number of passes with its `loop_start` and `loop_length`
  tags.
- Advanced preferences › Decoding › 2SF decoder (2sfplay): default length and fade, the fade's curve, endless playback,
  rendering ahead, and the settings of an album's track: skip silence at the start, the silences before and after a
  track, the loops, one pass for long loops, the fade length for loops, ignore length and fade tags, and skip silence at
  the end. Endless playback and rendering ahead apply to playback only. They never apply to conversion or scanning.
- During playback, the component renders ahead on a separate thread as fast as the CPU allows. The component buffers up
  to 10 minutes and 10 seconds of audio (about 76 MiB). That's enough for a whole track of up to 10 minutes and its
  fade. `2sfrip` gives a longer length only to music whose two passes take longer. Seeking within the buffer is instant.
  Seeking further ahead waits for rendering to reach the requested position. Rendering ahead keeps one CPU core busy
  until it reaches the end of the track or the buffer limit. The player saves no states. A seek back before the buffered
  audio renders the track again from its start. That happens only in a longer track or in endless playback.
- Output is 16-bit stereo at 32,728 Hz, the rate of the DS's sound unit. The output isn't resampled.
- The player ignores `_arm9clk` and `_arm7clk`. Its CPUs run the game's code as fast as the code requires.

## Building

**On GitHub (no Windows machine or Mac needed):** the `foobar2000 component` workflow
(`.github/workflows/foobar2000.yml`) builds the 32-bit and 64-bit DLLs and the Mac bundle. The workflow uploads one
`.fb2k-component` with all three. Start it from the repository's Actions tab (Run workflow). GitHub lists a manual
workflow only once it's on the default branch (`main`). The workflow requires GitHub Actions to be enabled for the
repository. The workflow downloads the SDK version that xsflib's action, `externals/xsflib/ci/foobar2000-sdk`, names
from foobar2000.org.

To build it yourself, you need the foobar2000 SDK, from <https://www.foobar2000.org/SDK>. Unpack it and use the folder
containing `pfc` and `foobar2000` as the SDK path below. A newer SDK than the one xsflib's action names may require
changes to xsflib's `cmake/XsflibApps.cmake`.

**On macOS:** you need the Xcode command line tools (`xcode-select --install`), CMake 3.23 or later, and the SDK
unpacked somewhere (macOS's `tar -xf SDK-<date>.7z` unpacks it).

```
cmake -S . -B build-fb2k -DCMAKE_BUILD_TYPE=Release -DFOOBAR2000_SDK_DIR=/path/to/SDK \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build build-fb2k --target foo_input_2sfplay
```

This produces `build-fb2k/plugins/foobar2000/foo_input_2sfplay.component`. It's a bundle for Apple Silicon Macs that
runs on macOS 11 or later. That's the same minimum as in the projects of the SDK. Without the two `CMAKE_OSX_` options,
the bundle is for the Mac that built it only. For a `.fb2k-component`, put the bundle in a `mac` folder inside a zip.
The bundle is signed ad hoc, not with a Developer ID. It isn't notarized.

**On Windows:** you need Visual Studio 2022 or later with the C++ workload and the "C++ Clang tools for Windows"
component, plus CMake 3.23 or later.

```
cmake -S . -B build-fb2k -A x64 -T ClangCL -DFOOBAR2000_SDK_DIR=C:/path/to/SDK
cmake --build build-fb2k --config Release --target foo_input_2sfplay
```

This produces `build-fb2k/plugins/foobar2000/Release/foo_input_2sfplay.dll`. For a `.fb2k-component`, zip the DLL. For
64-bit foobar2000 v2, put the DLL in an `x64` folder inside the zip. For 32-bit foobar2000, configure with `-A Win32 -T
ClangCL` and put that DLL at the top level of the zip. With the 64-bit DLL in `pkg\x64`, Windows' `tar` makes the zip:
`tar -a -c -f foo_input_2sfplay.zip -C pkg x64`. For a 32-bit DLL in `pkg` itself, add `foo_input_2sfplay.dll` after
`x64`. Then rename the zip to `foo_input_2sfplay.fb2k-component`. Windows PowerShell's `Compress-Archive` writes the
folder's name with a backslash. The zip format doesn't allow that, and foobar2000 doesn't accept a package made that
way. Omitting `-T ClangCL` selects MSVC. MSVC isn't supported.

The build compiles the SDK's `pfc`, `foobar2000/SDK` and `foobar2000/foobar2000_component_client` sources directly. On
Windows, the build links `foobar2000/shared/shared-<arch>.lib` (or `shared.lib`). On macOS, the build compiles the parts
of `foobar2000/shared` and the Objective-C++ files that the Xcode projects of the SDK build. xsflib's
`xsflib_add_foobar2000_component` (`cmake/XsflibApps.cmake`) does this.

On Windows, everything is built with the static C runtime (`/MT`). The component therefore requires no runtime DLLs. On
macOS, the component uses only the system's libraries.

## Performance

Playback emulates a whole DS on one CPU core. Use 64-bit foobar2000 and the 64-bit component if your other components
support them.

## Licence

Like the rest of 2sf, the component is MIT-licensed. It uses xsflib (MIT), ntdlib (MIT) and zlib. Ship `LICENSE` and
`THIRD-PARTY-NOTICES.md` (or their text) with a build. Nothing in the component is GPL. It can be distributed as a
foobar2000 component.
