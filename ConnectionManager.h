#pragma once

// 蓝牙 A2DP Sink 连接的生命周期管理。
// 与 UI 完全解耦：状态变化通过 SetStatusHandler 回调上报，
// 由调用方决定怎么呈现（现在是 XAML DevicePicker，将来可以是任何东西）。
//
// 表里的每个条目都带一个单调递增的 generation。StateChanged 回调只认自己那一次的
// generation，于是：
//   - 被替换掉的旧实例迟到的 Closed 回调，不会误删新实例的条目；
//   - 关闭过程中的同步回调，不会二次摘除同一条目；
//   - 连接还没出结果时的 Closed 事件，只记录、交给 Connect() 协程统一收尾。

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Media::Audio;

enum class ConnectionStatus
{
	Connecting,   // 正在 Start/Open
	Connected,    // OpenAsync 成功
	Failed,       // 失败，message 为可展示的错误描述
	Closed        // 被用户断开或远端关闭
};

// 日志用的标签。纯函数，便于单测。
inline const wchar_t* ConnectionStatusName(ConnectionStatus status)
{
	switch (status)
	{
	case ConnectionStatus::Connecting: return L"connecting";
	case ConnectionStatus::Connected: return L"connected";
	case ConnectionStatus::Failed: return L"failed";
	case ConnectionStatus::Closed: return L"closed";
	}
	return L"unknown";
}

// (设备, 状态, 描述文案)；仅 Failed 时描述非空。
using ConnectionStatusHandler = std::function<void(const DeviceInformation&, ConnectionStatus, const std::wstring&)>;

class ConnectionManager
{
public:
	void SetStatusHandler(ConnectionStatusHandler handler);

	// 按值接收 device：协程会挂起，不能持有调用方临时对象的引用。
	winrt::fire_and_forget Connect(DeviceInformation device);
	winrt::fire_and_forget ConnectById(std::wstring deviceId);

	// 主动断开；设备不在表中返回 false。
	bool Disconnect(std::wstring_view deviceId);

	// 关闭并上报全部连接，随后清空。
	void CloseAll();

	bool IsEmpty() const;
	std::vector<std::wstring> DeviceIds() const;

private:
	struct ConnectionEntry
	{
		DeviceInformation device;
		AudioPlaybackConnection connection;
		// 订阅令牌仅作记录，刻意不用于撤销：在 StateChanged 回调里撤销订阅会
		// 破坏 C++/WinRT 的 handler 容器（实测 0xC0000374）。详见 .cpp。
		winrt::event_token stateChangedToken{};
		uint32_t generation = 0;
		bool opening = false;           // Connect() 协程还没拿到 OpenAsync 结果
		bool closedWhileOpening = false; // 在 opening 期间收到了 Closed
	};

	using ConnectionMap = std::unordered_map<std::wstring, ConnectionEntry>;

	void OnStateChanged(const AudioPlaybackConnection& sender, uint32_t generation);
	void Report(const DeviceInformation& device, ConnectionStatus status, const std::wstring& message = {});

	// 从表中摘出条目，由调用方决定是否 Close()。
	// 必须先摘出再 Close：Close 可能同步派发 StateChanged，此时表里已无该条目，
	// OnStateChanged 自然找不到，不会二次 erase。
	// 刻意【不】撤销事件订阅 —— 见 .cpp 中 TakeOut 的说明（回调中撤销会堆损坏）。
	ConnectionEntry TakeOut(ConnectionMap::iterator it);

	// Close 不允许把异常抛到调用方（fire_and_forget 里就是 terminate）。
	static void CloseQuietly(AudioPlaybackConnection& connection);

	ConnectionMap m_connections;
	ConnectionStatusHandler m_statusHandler;
	uint32_t m_nextGeneration = 0;
};
