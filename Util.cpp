#include "pch.h"

#include "Util.h"

std::wstring Utf8ToUtf16(std::string_view utf8)
{
	if (utf8.empty())
	{
		return {};
	}

	constexpr DWORD kFlags = MB_ERR_INVALID_CHARS;
	const int utf8Length = static_cast<int>(utf8.length());
	const int utf16Length = MultiByteToWideChar(
		CP_UTF8,
		kFlags,
		utf8.data(),
		utf8Length,
		nullptr,
		0
	);
	THROW_LAST_ERROR_IF(utf16Length == 0);

	std::wstring utf16(utf16Length, L'\0');
	const int result = MultiByteToWideChar(
		CP_UTF8,
		kFlags,
		utf8.data(),
		utf8Length,
		utf16.data(),
		utf16Length
	);
	THROW_LAST_ERROR_IF(result == 0);

	return utf16;
}

std::string Utf16ToUtf8(std::wstring_view utf16)
{
	if (utf16.empty())
	{
		return {};
	}

	constexpr DWORD kFlags = WC_ERR_INVALID_CHARS;
	const int utf16Length = static_cast<int>(utf16.length());
	const int utf8Length = WideCharToMultiByte(
		CP_UTF8,
		kFlags,
		utf16.data(),
		utf16Length,
		nullptr,
		0,
		nullptr, nullptr
	);
	THROW_LAST_ERROR_IF(utf8Length == 0);

	std::string utf8(utf8Length, '\0');
	const int result = WideCharToMultiByte(
		CP_UTF8,
		kFlags,
		utf16.data(),
		utf16Length,
		utf8.data(),
		utf8Length,
		nullptr, nullptr
	);
	THROW_LAST_ERROR_IF(result == 0);

	return utf8;
}

std::filesystem::path GetModuleFsPath(HMODULE hModule)
{
	std::wstring path(MAX_PATH, L'\0');
	DWORD actualSize;

	while (1)
	{
		actualSize = GetModuleFileNameW(hModule, path.data(), static_cast<DWORD>(path.size()));

		if (static_cast<size_t>(actualSize) + 1 > path.size())
			path.resize(path.size() * 2);
		else
			break;
	}

	path.resize(actualSize);
	return std::filesystem::path(path);
}

bool IsSystemLightTheme()
{
	DWORD value = 0, cbValue = sizeof(value);
	LOG_IF_WIN32_ERROR(RegGetValueW(HKEY_CURRENT_USER, LR"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)", L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &cbValue));
	return value != 0;
}
