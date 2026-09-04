#pragma once

// Bump on release. Kept in a header rather than a compile definition so the
// string literal never has to survive CMake and MSVC command-line quoting.
#define MFG_VERSION_STRING L"0.5.1"
