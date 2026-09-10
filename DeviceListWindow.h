#pragma once

// 设备列表弹窗：左键点托盘图标时贴着图标弹出，列出可用于 A2DP Sink 的蓝牙设备，
// 双击某一行建立或断开连接。替代上游的 UWP DevicePicker（XAML Islands）。
//
// 设备枚举用 FindAllAsync + 定时刷新，而不是 DeviceWatcher：后者的 Added/Removed
// 事件在线程池线程上触发，跨线程更新控件还要额外做线程封送；列表只在弹窗可见时
// 才有意义，3 秒一次的轮询开销可以忽略。

#include "ConnectionManager.h"

#include <string>
#include <vector>

class DeviceListWindow
{
public:
	// 注册窗口类。类已存在（同进程多实例）也算成功。
	bool Register(HINSTANCE hInst);

	// 非拥有依赖：弹窗只借用它来发起连接/断开。
	void SetConnectionManager(ConnectionManager* connections);

	// 以 anchorRect（托盘图标的屏幕矩形）为锚弹出。
	void Show(const RECT& anchorRect);
	void Hide();
	bool IsVisible() const;

	// 托盘左键语义：可见则关闭；刚刚因失活自动隐藏（就是这次点击造成的）则不再重开。
	void ToggleFromTray(const RECT& anchorRect);

	// ConnectionManager 的状态回调入口（在 UI 线程被调用）。
	void OnConnectionStatus(const DeviceInformation& device, ConnectionStatus status, const std::wstring& message);

private:
	// 注意：DeviceInformation 没有默认构造函数（C++/WinRT 投影类型只用
	// nullptr 构造或从 ABI 指针构造），所以 DeviceEntry 不能靠"先默认构造
	// 再赋值"，一律走 MakeEntry。
	struct DeviceEntry
	{
		DeviceInformation device;
		std::wstring id;
		std::wstring name;
		ConnectionStatus status;
		std::wstring message;
	};

	static DeviceEntry MakeEntry(const DeviceInformation& device);
	static std::wstring StatusText(const DeviceEntry& entry);

	static LRESULT CALLBACK StaticWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
	LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);

	void CreateChildren();
	void UpdateLayout();
	void UpdateColumnWidths();
	SIZE ComputeSize() const;
	void PositionWindow(int width, int height);
	void RebuildList();
	void ActivateItem(int index);
	winrt::fire_and_forget RefreshDevices();
	DeviceEntry* FindEntry(std::wstring_view id);
	UINT Dpi() const;

	HINSTANCE m_hInst = nullptr;
	HWND m_hWnd = nullptr;
	HWND m_hList = nullptr;
	ConnectionManager* m_connections = nullptr;
	std::vector<DeviceEntry> m_devices;

	bool m_refreshing = false;
	bool m_activated = false;      // 是否真正拿到过激活（SetForegroundWindow 可能失败）
	int m_columnDpi = 0;           // 列宽上次生效的 DPI，只在 DPI 变化时重设
	RECT m_anchor = {};
	ULONGLONG m_lastAutoHideTick = 0;
};
