#include "pch.h"

#include "Log.h"

#include <mutex>

namespace logger
{
	namespace
	{
		// 单文件上限 1 MB，超出后把 .log 轮转为 .log.1（旧的 .log.1 直接覆盖）。
		constexpr uint64_t kMaxFileBytes = 1024 * 1024;

		std::mutex g_mutex;
		wil::unique_hfile g_file;
		std::filesystem::path g_path;
		uint64_t g_bytesWritten = 0;
		bool g_rotationDisabled = false;

		// 非抛出版本的 UTF-16 → UTF-8。坏字符由系统替换，绝不失败。
		// 故意不复用 Util.h 的 Utf16ToUtf8：那条路径会 THROW，而日志必须保证不抛。
		std::string ToUtf8NoThrow(std::wstring_view text)
		{
			if (text.empty())
				return {};

			const int length = static_cast<int>(text.size());
			const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), length, nullptr, 0, nullptr, nullptr);
			if (size <= 0)
				return {};

			std::string utf8(static_cast<size_t>(size), '\0');
			const int written = WideCharToMultiByte(CP_UTF8, 0, text.data(), length, utf8.data(), size, nullptr, nullptr);
			if (written <= 0)
				return {};

			utf8.resize(static_cast<size_t>(written));
			return utf8;
		}

		Timestamp Now()
		{
			SYSTEMTIME st{};
			GetLocalTime(&st);
			return Timestamp{ st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds };
		}

		// 下面三个 *Locked 都必须持有 g_mutex。

		void CloseLocked()
		{
			g_file.reset();
			g_bytesWritten = 0;
		}

		bool OpenLocked()
		{
			if (g_path.empty())
				return false;

			g_file.reset(CreateFileW(
				g_path.c_str(),
				FILE_APPEND_DATA,
				FILE_SHARE_READ,
				nullptr,
				OPEN_ALWAYS,
				FILE_ATTRIBUTE_NORMAL,
				nullptr));
			if (!g_file)
				return false;

			g_bytesWritten = 0;
			LARGE_INTEGER size{};
			if (GetFileSizeEx(g_file.get(), &size))
				g_bytesWritten = static_cast<uint64_t>(size.QuadPart);
			return true;
		}

		// 写满就轮转：log → log.1。轮转失败则彻底放弃轮转，继续往原文件追加。
		void RotateLocked()
		{
			CloseLocked();

			std::filesystem::path rotated = g_path;
			rotated += L".1";

			if (!MoveFileExW(g_path.c_str(), rotated.c_str(), MOVEFILE_REPLACE_EXISTING))
			{
				g_rotationDisabled = true;
				OpenLocked();
				return;
			}

			OpenLocked();
		}
	}

	std::wstring FormatLogLine(const Timestamp& ts, uint32_t threadId, std::wstring_view message)
	{
		// 手工补零，不依赖 CRT 的格式化函数：日志层要尽可能少的运行时依赖。
		std::wstring line;
		line.reserve(48 + message.size());

		auto appendPadded = [&line](uint32_t value, size_t width)
		{
			wchar_t digits[16] = {};
			size_t count = 0;
			do
			{
				digits[count++] = static_cast<wchar_t>(L'0' + value % 10);
				value /= 10;
			} while (value != 0 && count < 16);

			while (count < width)
				digits[count++] = L'0';

			while (count > 0)
				line.push_back(digits[--count]);
		};

		appendPadded(ts.year, 4);
		line.push_back(L'-');
		appendPadded(ts.month, 2);
		line.push_back(L'-');
		appendPadded(ts.day, 2);
		line.push_back(L' ');
		appendPadded(ts.hour, 2);
		line.push_back(L':');
		appendPadded(ts.minute, 2);
		line.push_back(L':');
		appendPadded(ts.second, 2);
		line.push_back(L'.');
		appendPadded(ts.millisecond, 3);
		line.push_back(L' ');
		line.push_back(L'[');
		appendPadded(threadId, 1);
		line.push_back(L']');
		line.push_back(L' ');

		line.append(message);
		return line;
	}

	void SetLogFile(const std::filesystem::path& path)
	{
		std::lock_guard<std::mutex> lock(g_mutex);

		CloseLocked();
		g_path = path;
		g_bytesWritten = 0;
		g_rotationDisabled = false;
		OpenLocked();
	}

	const std::filesystem::path& LogFilePath()
	{
		return g_path;
	}

	void Write(std::wstring_view message)
	{
		std::lock_guard<std::mutex> lock(g_mutex);

		if (g_path.empty())
			return;
		if (!g_file && !OpenLocked())
			return;

		std::wstring line = FormatLogLine(Now(), GetCurrentThreadId(), message);
		line.append(L"\r\n");

		const std::string bytes = ToUtf8NoThrow(line);
		if (bytes.empty())
			return;

		const DWORD size = static_cast<DWORD>(bytes.size());

		if (!g_rotationDisabled && g_bytesWritten > 0 && g_bytesWritten + size > kMaxFileBytes)
		{
			RotateLocked();
			if (!g_file)
				return;
		}

		DWORD written = 0;
		if (!WriteFile(g_file.get(), bytes.data(), size, &written, nullptr) || written != size)
		{
			// 写失败就关掉句柄，等下一次 Write 重开；期间的所有日志静默丢弃。
			CloseLocked();
			return;
		}

		g_bytesWritten += written;
	}

	void Shutdown()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		CloseLocked();
	}
}
