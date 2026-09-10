#pragma once

// 落地日志：把运行痕迹追加写到 exe 同目录的 AudioPlaybackConnector.log。
//
// 为什么需要它：WIL 的 LOG_* 只走 OutputDebugString，发布版拿不到任何痕迹，
// 出问题时只能靠外部脚本去猜。这里提供一个最小、无外部依赖、绝不抛异常的实现。
//
// 硬性约束：
//   - 任何失败（打不开文件、写盘失败、编码失败）都必须静默降级，不得影响主流程；
//   - 不依赖 WinRT / UI，可被单元测试单独编译。

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace logger
{
	// 默认文件名，放在 exe 同目录（与 AudioPlaybackConnector.json 并列）。
	inline constexpr wchar_t kLogFileName[] = L"AudioPlaybackConnector.log";

	// 拆成字段的时间戳，便于单测直接构造。
	struct Timestamp
	{
		uint16_t year = 0;
		uint16_t month = 0;
		uint16_t day = 0;
		uint16_t hour = 0;
		uint16_t minute = 0;
		uint16_t second = 0;
		uint16_t millisecond = 0;
	};

	// 纯函数：拼一行（不含换行）。格式为 `YYYY-MM-DD HH:MM:SS.mmm [tid] message`
	std::wstring FormatLogLine(const Timestamp& ts, uint32_t threadId, std::wstring_view message);

	// 把若干片段拼成一段文本，调用点免去到处写 + / append。
	// 片段可以是 const wchar_t* / std::wstring / std::wstring_view。
	namespace detail
	{
		inline void AppendParts(std::wstring&)
		{
		}

		template <typename First, typename... Rest>
		void AppendParts(std::wstring& text, const First& first, const Rest&... rest)
		{
			text.append(std::wstring_view(first));
			AppendParts(text, rest...);
		}
	}

	template <typename... Parts>
	std::wstring Compose(const Parts&... parts)
	{
		std::wstring text;
		detail::AppendParts(text, parts...);
		return text;
	}

	// 指定日志文件并打开（已有内容则追加）。可重复调用，以最后一次为准。
	void SetLogFile(const std::filesystem::path& path);

	// 当前日志文件路径；未设置时为空。
	const std::filesystem::path& LogFilePath();

	// 记录一行（内部补时间戳、线程 id、换行）。未设置文件时静默丢弃。
	void Write(std::wstring_view message);

	// 关闭句柄。不调用也无妨（随进程回收），仅供干净退出。
	void Shutdown();
}
