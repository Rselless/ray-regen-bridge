#pragma once

#include <filesystem>
#include <string>

std::wstring Utf8ToWide(const std::string& text);
std::wstring Lower(std::wstring text);

// ProductName from the file's version resource, empty when there is none
std::wstring ProductName(const std::filesystem::path& file);
std::wstring FileVersion(const std::filesystem::path& file);

// Folder of the launcher exe
std::filesystem::path LauncherDir();
