#pragma once

// 通用工具：编码转换、模块路径、系统主题探测。
// 保持无状态、无全局依赖，便于单元测试。

#include <filesystem>
#include <string>
#include <string_view>

// https://msdn.microsoft.com/en-us/magazine/mt763237
std::wstring Utf8ToUtf16(std::string_view utf8);
std::string Utf16ToUtf8(std::wstring_view utf16);

// https://docs.microsoft.com/en-us/windows/uwp/cpp-and-winrt-apis/author-coclasses#add-helper-types-and-functions
// License: see the https://github.com/MicrosoftDocs/windows-uwp/blob/docs/LICENSE-CODE file
std::filesystem::path GetModuleFsPath(HMODULE hModule);

// 读 HKCU\...\Personalize!SystemUsesLightTheme，用于托盘图标配色。
bool IsSystemLightTheme();
