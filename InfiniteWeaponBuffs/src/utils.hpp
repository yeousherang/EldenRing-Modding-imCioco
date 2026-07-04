#pragma once

#include <string>

namespace iwb {

// ---- paths -------------------------------------------------
// config.ini sits next to the DLL; the log goes in a logs/ subfolder
// alongside the DLL (the convention other mods use).
std::wstring module_path();
std::wstring dir_of(const std::wstring& p);
std::wstring stem_of(const std::wstring& p);
std::wstring config_path();
std::wstring log_path();

// ---- logging: ALWAYS writes logs/<DllName>.log near the DLL --
void log_line(const std::string& msg, bool truncate = false);
void flog(const char* fmt, ...);

} // namespace iwb
