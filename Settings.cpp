#include "pch.h"

#include "Settings.h"

#include "Util.h"

using namespace winrt::Windows::Data::Json;

constexpr auto CONFIG_NAME = L"AudioPlaybackConnector.json";
constexpr size_t BUFFER_SIZE = 4096;

std::filesystem::path GetSettingsPath(HINSTANCE hInst)
{
	return GetModuleFsPath(hInst).remove_filename() / CONFIG_NAME;
}

SettingsData LoadSettings(const std::filesystem::path& path)
{
	SettingsData data;

	try
	{
		wil::unique_hfile hFile(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
		THROW_LAST_ERROR_IF(!hFile);

		std::string string;
		while (1)
		{
			size_t size = string.size();
			string.resize(size + BUFFER_SIZE);
			DWORD read = 0;
			THROW_IF_WIN32_BOOL_FALSE(ReadFile(hFile.get(), string.data() + size, static_cast<DWORD>(BUFFER_SIZE), &read, nullptr));
			string.resize(size + read);
			if (read == 0)
				break;
		}

		std::wstring utf16 = Utf8ToUtf16(string);
		auto jsonObj = JsonObject::Parse(utf16);
		data.reconnect = jsonObj.Lookup(L"reconnect").GetBoolean();

		auto lastDevices = jsonObj.Lookup(L"lastDevices").GetArray();
		data.lastDevices.reserve(lastDevices.Size());
		for (const auto& i : lastDevices)
		{
			if (i.ValueType() == JsonValueType::String)
				data.lastDevices.push_back(std::wstring(i.GetString()));
		}
	}
	CATCH_LOG();

	return data;
}

void SaveSettings(const std::filesystem::path& path, const SettingsData& data)
{
	try
	{
		JsonObject jsonObj;
		jsonObj.Insert(L"reconnect", JsonValue::CreateBooleanValue(data.reconnect));

		JsonArray lastDevices;
		for (const auto& i : data.lastDevices)
		{
			lastDevices.Append(JsonValue::CreateStringValue(i));
		}
		jsonObj.Insert(L"lastDevices", lastDevices);

		wil::unique_hfile hFile(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
		THROW_LAST_ERROR_IF(!hFile);

		std::string utf8 = Utf16ToUtf8(jsonObj.Stringify());
		DWORD written = 0;
		THROW_IF_WIN32_BOOL_FALSE(WriteFile(hFile.get(), utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr));
		THROW_HR_IF(E_FAIL, written != utf8.size());
	}
	CATCH_LOG();
}
