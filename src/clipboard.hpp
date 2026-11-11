#pragma once

#include <filesystem>
#include <string>
#include <vector>

// what a paste can attach: the files copied in the file manager, or the picture on the
// clipboard saved as a png in tempDir. empty when the clipboard holds neither (text is
// imgui's business). pasteboard: a named one, for tests; null = the system's.
// shortcut: macos only (clipboard_mac.mm), empty elsewhere; belongs in imshell (its #58)
std::vector<std::string> clipboardFiles(const std::filesystem::path& tempDir, const char* pasteboard = nullptr);
