#pragma once

// 蓝牙 A2DP Sink 连接的生命周期管理。
// 与 UI 完全解耦：状态变化通过 SetStatusHandler 回调上报，
// 由调用方决定怎么呈现（现在是自建的设备列表弹窗）。

#include <functional>
#include <string>
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
	void OnStateChanged(const AudioPlaybackConnection& sender);
	void Report(const DeviceInformation& device, ConnectionStatus status, const std::wstring& message = {});

	std::unordered_map<std::wstring, std::pair<DeviceInformation, AudioPlaybackConnection>> m_connections;
	ConnectionStatusHandler m_statusHandler;
};
