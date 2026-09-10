#pragma once

// 蓝牙 A2DP Sink 连接的生命周期管理（L2）。
// 与 UI 完全解耦：状态变化通过 SetStatusHandler 回调上报，
// 由调用方决定怎么呈现（现在是 XAML DevicePicker，将来可以是任何东西）。
//
// step13a 平台接口化后，本类只依赖 IAudioConnection / IAudioConnectionFactory
// （L1 接口），不再直接触碰 WinRT 连接类型；DeviceInformation 仍留在本层
// （它是 L3 的选择器给进来的值，换掉它会扩散到上报路径与 F5 语义）。
//
// 表里的每个条目都带一个单调递增的 generation。StateChanged 回调只认自己那一次的
// generation，于是：
//   - 被替换掉的旧实例迟到的 Closed 回调，不会误删新实例的条目；
//   - 关闭过程中的同步回调，不会二次摘除同一条目；
//   - 连接还没出结果时的 Closed 事件，只记录、交给 Connect() 协程统一收尾。

#include "AudioConnection.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace winrt::Windows::Devices::Enumeration;

enum class ConnectionStatus
{
	Connecting,   // 正在 Start/Open
	Connected,    // Open 成功
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
	// 工厂强制注入：生产路径传 WinrtConnectionFactory，测试传 mock。
	explicit ConnectionManager(std::unique_ptr<IAudioConnectionFactory> factory);

	void SetStatusHandler(ConnectionStatusHandler handler);

	// 按值接收 device：协程会挂起，不能持有调用方临时对象的引用。
	winrt::fire_and_forget Connect(DeviceInformation device);
	winrt::fire_and_forget ConnectById(std::wstring deviceId);

	// 主动断开。
	//
	// 设备在表中（正常路径）：摘出条目并 Close()。
	// 设备不在表中也要处理：最典型的场景是**应用重启过**（表随之清空），而 Windows 侧
	// 仍持有这条 A2DP sink 连接。此时若直接返回，界面上的"断开连接"按钮点下去毫无
	// 反应，用户从本程序侧没有任何办法断开它。这里改用调用方手上的 DeviceInformation
	// 现造一个连接对象关掉。
	//
	// 返回是否真的对底层做了关闭动作（界面复位与它无关，两条路径都会复位）。
	bool Disconnect(const DeviceInformation& device);

	// 关闭并上报全部连接，随后清空。
	void CloseAll();

	bool IsEmpty() const;
	std::vector<std::wstring> DeviceIds() const;

	// —— 以下为测试可见的实现核心 ——

	// Connect/Disconnect 的实现主体。拆出来是为了让单测注入 deviceId 字符串
	// 而不必构造有效的 DeviceInformation（空投影取 Id 会抛）。行为与拆分前
	// 逐字一致。
	winrt::fire_and_forget ConnectImpl(std::wstring deviceId, DeviceInformation device);
	bool DisconnectImpl(std::wstring deviceId, const DeviceInformation& device);

private:
	struct ConnectionEntry
	{
		DeviceInformation device;
		std::shared_ptr<IAudioConnection> connection;
		uint32_t generation = 0;
		bool opening = false;           // Connect() 协程还没拿到 Open 结果
		bool closedWhileOpening = false; // 在 opening 期间收到了 Closed
	};

	using ConnectionMap = std::unordered_map<std::wstring, ConnectionEntry>;

	void OnStateChanged(std::wstring const& deviceId, std::wstring_view stateName, uint32_t generation);
	void Report(const DeviceInformation& device, ConnectionStatus status, const std::wstring& message = {});

	// 从表中摘出条目，由调用方决定是否 Close()。
	// 必须先摘出再 Close：Close 可能同步派发 StateChanged，此时表里已无该条目，
	// OnStateChanged 自然找不到，不会二次 erase。
	// 刻意【不】撤销事件订阅 —— 接口本身不提供撤销手段（回调内撤销会堆损坏）。
	ConnectionEntry TakeOut(ConnectionMap::iterator it);

	// Close 不允许把异常抛到调用方（fire_and_forget 里就是 terminate）。
	// 接口契约里 Close() 已是 noexcept，这里再兜一层防实现方违规。
	static void CloseQuietly(std::shared_ptr<IAudioConnection> const& connection);

	std::unique_ptr<IAudioConnectionFactory> m_factory;
	ConnectionMap m_connections;
	ConnectionStatusHandler m_statusHandler;
	uint32_t m_nextGeneration = 0;
};
