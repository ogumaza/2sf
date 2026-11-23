# SPDX-License-Identifier: MIT

# xmp-2sfplay: the XMPlay input plugin, a 32-bit DLL built with clang-cl. XMPlay is a 32-bit program. Added by the
# top-level CMakeLists.txt when XMPLAY_SDK_DIR is set. See README.md. xsflib builds it against the SDK's headers
# (xsflib_add_xmplay_plugin).

xsflib_add_xmplay_plugin(xmp-2sfplay
    SDK ${XMPLAY_SDK_DIR}
    SOURCES xmp_2sfplay.cpp
    LIBRARIES twosf_player
    OPTIONS ${TWOSF_WARNINGS})
xsflib_add_version(xmp-2sfplay VERSION ${twosf_VERSION} DEFINE TWOSF_VERSION DESCRIPTION "2SF plugin for XMPlay"
                   PRODUCT 2sf COPYRIGHT "Copyright (c) 2026 the 2sf authors. MIT licence.")
