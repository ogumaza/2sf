// SPDX-License-Identifier: MIT

// xmp-2sfplay: XMPlay input plugin for 2SF (.mini2sf and .2sf), from xsflib's plugin template (xsflib/xmplay/input.h).
// The template's comment says what the plugin does. plugin_traits.h defines 2SF's traits.
//
// The player (src/twosf) boots the set's cartridge image on ntdlib's model of the DS. The game's code makes the sound.
//
// Build: see plugins/xmplay/README.md (clang-cl for 32-bit Windows, with the XMPlay plugin SDK).

#include <windows.h>
#include <xmpin.h>

#include "xsflib/xmplay/input.h"

#include "../plugin_traits.h"

// XMPlay looks the plugin up by this name (the SDK's xmpin.def exports it).
XMPIN* WINAPI XMPIN_GetInterface(DWORD face, InterfaceProc faceproc)
{
    return xsflib::xmplay::Plugin<twosf::PluginTraits>::GetInterface(face, faceproc);
}
