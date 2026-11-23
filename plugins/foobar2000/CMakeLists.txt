# SPDX-License-Identifier: MIT

# foo_input_2sfplay: the foobar2000 component. It's a DLL built with clang-cl on Windows. On macOS it's a .component
# bundle. The top-level CMakeLists.txt adds it when FOOBAR2000_SDK_DIR is set (see README.md). xsflib's
# xsflib_add_foobar2000_component compiles the parts of the SDK that a component requires.

# On macOS, the SDK's shared part includes Objective-C++ files. CMake enables a language only at file scope.
if (APPLE)
    enable_language(OBJCXX)
endif()

xsflib_add_foobar2000_component(foo_input_2sfplay
    SDK ${FOOBAR2000_SDK_DIR}
    SOURCES foo_input_2sfplay.cpp
    LIBRARIES twosf_player
    VERSION ${twosf_VERSION}
    BUNDLE_ID com.foobar2000.foo-input-2sfplay
    OPTIONS ${TWOSF_WARNINGS})
xsflib_add_version(foo_input_2sfplay VERSION ${twosf_VERSION} DEFINE TWOSF_VERSION
                   DESCRIPTION "2SF decoder for foobar2000" PRODUCT 2sf
                   COPYRIGHT "Copyright (c) 2026 the 2sf authors. MIT licence.")
