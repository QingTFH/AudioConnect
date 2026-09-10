#include "pch.h"

#include "WinrtAudioConnection.h"

#include "Log.h"

#include <cwchar>
#include <utility>

using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Media::Audio;

namespace
{
	// 从 ConnectionManager.cpp 原样迁来（日志文本零变化）。
	const wchar_t* StateName(AudioPlaybackConnectionState state)
	{
		switch (state)
		{
		case AudioPlaybackConnectionState::Closed: return L"closed";
		case AudioPlaybackConnectionState::Opened: return L"opened";
		}
		return L"unknown";
	}

	// 与 ConnectionManager.cpp 的 FormatHresultError 逐字一致：
	// Close 的 catch 文案下沉到 L1 后，错误描述格式必须保持不变。
	std::wstring FormatHresultError(const winrt::hresult_error& ex)
	{
		std::wstring message(64, L'\0');
		while (1)
		{
			auto result = swprintf(message.data(), message.size(), L"%s (0x%08X)", ex.message().c_str(), static_cast<uint32_t>(ex.code()));
			if (result < 0)
			{
				message.resize(message.size() * 2);
			}
			else
			{
				message.resize(result);
				break;
			}
		}
		return message;
	}
}

WinrtAudioConnection::WinrtAudioConnection(AudioPlaybackConnection connection)
	: m_connection(std::move(connection))
{
}

std::wstring WinrtAudioConnection::DeviceId() const
{
	return std::wstring(m_connection.DeviceId());
}

AsyncOp<void> WinrtAudioConnection::Start()
{
	auto op = AsyncOp<void>{};
	auto state = op.state();
	auto connection = m_connection;

	[state = std::move(state), connection = std::move(connection)]() -> winrt::fire_and_forget {
		try
		{
			co_await connection.StartAsync();
			state->Complete();
		}
		catch (...)
		{
			state->CompleteError(std::current_exception());
		}
	}();

	return op;
}

AsyncOp<OpenOutcome> WinrtAudioConnection::Open()
{
	auto op = AsyncOp<OpenOutcome>{};
	auto state = op.state();
	auto connection = m_connection;

	[state = std::move(state), connection = std::move(connection)]() -> winrt::fire_and_forget {
		try
		{
			auto result = co_await connection.OpenAsync();

			OpenOutcome outcome{};
			switch (result.Status())
			{
			case AudioPlaybackConnectionOpenResultStatus::Success:
				outcome.kind = OpenResultKind::Success;
				break;
			case AudioPlaybackConnectionOpenResultStatus::RequestTimedOut:
				outcome.kind = OpenResultKind::TimedOut;
				break;
			case AudioPlaybackConnectionOpenResultStatus::DeniedBySystem:
				outcome.kind = OpenResultKind::Denied;
				break;
			case AudioPlaybackConnectionOpenResultStatus::UnknownFailure:
				outcome.kind = OpenResultKind::UnknownFailure;
				break;
			}
			outcome.extendedError = static_cast<uint32_t>(result.ExtendedError());

			state->Complete(outcome);
		}
		catch (...)
		{
			state->CompleteError(std::current_exception());
		}
	}();

	return op;
}

void WinrtAudioConnection::RegisterStateChanged(std::function<void(std::wstring_view stateName)> onStateChanged)
{
	// 刻意【不】保存 token、不提供撤销：条目离开 L2 连接表后，迟到回调由
	// find 失败 / generation 校验挡掉；本对象析构时订阅自然解除。
	// （在回调里撤销订阅会破坏 C++/WinRT 的 handler 容器 —— 实测
	// 0xC0000374 堆损坏。见 docs/项目规约.md §4.4。）
	m_connection.StateChanged([onStateChanged = std::move(onStateChanged)](const auto& sender, const auto&) {
		onStateChanged(StateName(sender.State()));
	});
}

void WinrtAudioConnection::Close() noexcept
{
	try
	{
		m_connection.Close();
	}
	catch (winrt::hresult_error const& ex)
	{
		// 原 L2 CloseQuietly 的 catch 分支原样下沉（日志文本零变化）。
		logger::Write(logger::Compose(L"Close failed: ", FormatHresultError(ex)));
		LOG_CAUGHT_EXCEPTION();
	}
	catch (...)
	{
		LOG_CAUGHT_EXCEPTION();
	}
}

std::shared_ptr<IAudioConnection> WinrtConnectionFactory::Create(std::wstring const& deviceId)
{
	// TryCreateFromId 失败返回 null（不抛）；设备无效等异常向上传播，
	// 由调用方的 catch 生成错误文案（与接口化之前一致）。
	auto connection = AudioPlaybackConnection::TryCreateFromId(deviceId);
	if (!connection)
		return nullptr;

	return std::make_shared<WinrtAudioConnection>(std::move(connection));
}
