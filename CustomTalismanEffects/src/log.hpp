#pragma once

// Shared logging (file next to the DLL, optional debug console). Used by both
// the worker (main.cpp) and the overlay.

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <string>

namespace cte {

extern HINSTANCE g_hinst; // set in DllMain; used to locate the DLL's folder
extern bool      g_debug; // mirror stdout to a console when true

// Path of the DLL's `.ini` (same folder + base name).
std::wstring config_path();

// Append a line to logs/<DllName>.log (truncate to start a fresh log).
void log_line(const std::string& msg, bool truncate = false);

// printf-style wrapper around log_line.
void flog(const char* fmt, ...);

} // namespace cte
