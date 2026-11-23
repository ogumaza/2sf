# Third-party notices

2sf is under the MIT licence (`LICENSE`). Its dependencies have the following licences.

| Component | Licence | Use in 2sf |
|---|---|---|
| xsflib, Copyright (c) 2026 the xsflib authors | MIT (`externals/xsflib/LICENSE`) | In `externals/xsflib`, a submodule, compiled into every 2sf program: byte order, files, the PSF container, the frame of the rip tool, and for `2sfplay` the playback framework and the command-line frame. |
| ntdlib, Copyright (c) 2026 the ntdlib authors | MIT (`externals/ntdlib/LICENSE`) | In `externals/ntdlib`, a submodule, compiled into every 2sf program: the DS's cartridge images, the model of the DS that runs a game's code, the ARM interpreter and the ARM assembler of the stub, and in `2sfrip` the search of the SDK's sound routines, the readers of the sound data and the length analyses of the drivers. |
| seqlib, Copyright (c) 2026 the seqlib authors | MIT (`externals/xsflib/externals/seqlib/LICENSE`) | In `externals/xsflib/externals/seqlib`, a submodule of xsflib. `2sfrip` names the minis, the second libraries and the game's files with its safe file names. The checks of the tests come from it, through `xsflib::testing`. |
| [zlib](https://zlib.net), Copyright (C) 1995-2026 Jean-loup Gailly and Mark Adler | zlib | The compression of the PSF container in every 2sf program, through xsflib, and the CRC-32 with which `2sfrip` finds a song in a hashed sound pack. The installed copy is used if there is one. Otherwise v1.4 is downloaded when configuring. |
| [libFLAC](https://xiph.org/flac/), Copyright the Xiph.Org Foundation | BSD-3-Clause | FLAC output in `2sfplay`. The installed copy is used if there is one. Otherwise 1.5.0 is downloaded when configuring. |
| [foobar2000 SDK](https://www.foobar2000.org/SDK), Copyright Peter Pawlowski | BSD-style (the SDK's `sdk-license.txt`, and `pfc-license.txt` for its pfc library) | Only for `plugins/foobar2000`. Not included here. The SDK is supplied when building the component. |
| [XMPlay plugin SDK](https://www.un4seen.com/xmplay.html), from Un4seen Developments | None stated | Only for `plugins/xmplay`. Not included here. The SDK is supplied when building the plugin. CI downloads it from un4seen.com. The plugin uses only the SDK's declarations of XMPlay's interface. |

Binary distributions must include the MIT notices of 2sf, xsflib and ntdlib. Every program contains xsflib and ntdlib.
`2sfrip` also contains seqlib and requires its MIT notice. `2sfplay` and the plugins don't contain seqlib. The libFLAC
notice is required when libFLAC is linked in: `2sfplay` links it, and `2sfrip` and the plugins don't. Shipping
`LICENSE` and this file meets these requirements. The licence texts are at the end of this file. The licences of zlib,
the foobar2000 SDK and its pfc library require no notice in binaries. No code from the XMPlay plugin SDK goes into the
XMPlay plugin.

## Licence texts

### xsflib

```
MIT License

Copyright (c) 2026 the xsflib authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### ntdlib

```
MIT License

Copyright (c) 2026 the ntdlib authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### seqlib

```
MIT License

Copyright (c) 2026 the seqlib authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### zlib

```
Copyright notice:

 (C) 1995-2026 Jean-loup Gailly and Mark Adler

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Jean-loup Gailly        Mark Adler
  jloup@gzip.org          madler@alumni.caltech.edu
```

### libFLAC

```
Copyright (C) 2000-2009  Josh Coalson
Copyright (C) 2011-2025  Xiph.Org Foundation

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

- Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.

- Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.

- Neither the name of the Xiph.Org Foundation nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
