// Paths + logging plumbing (same pattern as the other mods in this repo).
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <string>

namespace omni {

extern HINSTANCE g_hinst;
extern bool      g_debug;

std::wstring config_path(); // <dll dir>\OmniCaster.ini
std::wstring log_path();    // <dll dir>\logs\OmniCaster.log

void log_line(const std::string& msg, bool truncate = false);
void flog(const char* fmt, ...);

} // namespace omni
