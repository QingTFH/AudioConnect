#include "pch.h"

#include "DeviceListWindow.h"

#include "I18n.h"

#include <algorithm>

namespace
{
	constexpr wchar_t kWindowClassName[] = L"AudioPlaybackConnectorDeviceList";
	constexpr UINT_PTR kRefreshTimerId = 1;
	constexpr UINT kListControlId = 1001;
	constexpr int kRefreshIntervalMs = 3000;

	// 逻辑像素（96 DPI 基准），实际尺寸按窗口 DPI 缩放。
	constexpr int kWindowWidth = 360;
	constexpr int kPadding = 8;
	constexpr int kRowHeight = 22;
	constexpr int kNameColumnWidth = 200;
	constexpr int kStatusColumnWidth = 132;
	constexpr size_t kMaxVisibleRows = 8;

	// 因失活而自动隐藏后，多久之内收到的托盘点击视为"这次点击就是来关它的"。
	constexpr ULONGLONG kTrayToggleGraceMs = 300;
}

bool DeviceListWindow::Register(HINSTANCE hInst)
{
	m_hInst = hInst;

	WNDCLASSEXW wc = {
		.cbSize = sizeof(wc),
		.style = CS_HREDRAW | CS_VREDRAW,
		.lpfnWndProc = StaticWndProc,
		.hInstance = hInst,
		.hCursor = LoadCursorW(nullptr, IDC_ARROW),
		.lpszClassName = kWindowClassName,
	};

	if (RegisterClassExW(&wc))
		return true;

	return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

void DeviceListWindow::SetConnectionManager(ConnectionManager* connections)
{
	m_connections = connections;
}

UINT DeviceListWindow::Dpi() const
{
	if (m_hWnd)
	{
		if (const UINT dpi = GetDpiForWindow(m_hWnd))
			return dpi;
	}
	return USER_DEFAULT_SCREEN_DPI;
}

bool DeviceListWindow::IsVisible() const
{
	return m_hWnd != nullptr && IsWindowVisible(m_hWnd) != FALSE;
}

void DeviceListWindow::Show(const RECT& anchorRect)
{
	if (!m_hWnd)
	{
		m_hWnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kWindowClassName, L"AudioPlaybackConnector",
			WS_POPUP | WS_BORDER, 0, 0, 0, 0, nullptr, nullptr, m_hInst, this);
		FAIL_FAST_LAST_ERROR_IF_NULL(m_hWnd);
		CreateChildren();
	}

	m_anchor = anchorRect;
	m_activated = false;

	const SIZE size = ComputeSize();
	UpdateColumnWidths();
	PositionWindow(size.cx, size.cy);

	ShowWindow(m_hWnd, SW_SHOW);
	SetForegroundWindow(m_hWnd);
	SetFocus(m_hList);

	SetTimer(m_hWnd, kRefreshTimerId, kRefreshIntervalMs, nullptr);
	RefreshDevices();
}

void DeviceListWindow::Hide()
{
	if (!m_hWnd)
		return;

	KillTimer(m_hWnd, kRefreshTimerId);
	ShowWindow(m_hWnd, SW_HIDE);
}

void DeviceListWindow::ToggleFromTray(const RECT& anchorRect)
{
	if (IsVisible())
	{
		Hide();
		return;
	}

	// 点托盘图标会先让弹窗失活（于是自动隐藏），再投递 WM_NOTIFYICON。
	// 若那次隐藏就是这次点击造成的，就不要再弹出来 —— 等价于"再点一下关掉"。
	if (GetTickCount64() - m_lastAutoHideTick < kTrayToggleGraceMs)
	{
		m_lastAutoHideTick = 0;
		return;
	}

	Show(anchorRect);
}

void DeviceListWindow::CreateChildren()
{
	m_hList = CreateWindowExW(0, WC_LISTVIEWW, nullptr,
		WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
		0, 0, 0, 0, m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListControlId)), m_hInst, nullptr);
	FAIL_FAST_LAST_ERROR_IF_NULL(m_hList);

	ListView_SetExtendedListViewStyle(m_hList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
	SendMessageW(m_hList, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), FALSE);

	LVCOLUMNW column = {};
	column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
	column.iSubItem = 0;
	column.pszText = const_cast<LPWSTR>(_(L"Device"));
	ListView_InsertColumn(m_hList, 0, &column);

	column.iSubItem = 1;
	column.pszText = const_cast<LPWSTR>(_(L"Status"));
	ListView_InsertColumn(m_hList, 1, &column);
}

SIZE DeviceListWindow::ComputeSize() const
{
	const int dpi = static_cast<int>(Dpi());
	const int padding = MulDiv(kPadding, dpi, USER_DEFAULT_SCREEN_DPI);
	const int rowHeight = MulDiv(kRowHeight, dpi, USER_DEFAULT_SCREEN_DPI);
	const size_t rows = std::clamp<size_t>(m_devices.size(), 1, kMaxVisibleRows);

	SIZE size = {};
	size.cx = MulDiv(kWindowWidth, dpi, USER_DEFAULT_SCREEN_DPI);
	// 表头算一行。
	size.cy = padding * 2 + rowHeight * (static_cast<int>(rows) + 1);
	return size;
}

void DeviceListWindow::UpdateColumnWidths()
{
	const int dpi = static_cast<int>(Dpi());
	if (dpi == m_columnDpi)
		return;
	m_columnDpi = dpi;

	ListView_SetColumnWidth(m_hList, 0, MulDiv(kNameColumnWidth, dpi, USER_DEFAULT_SCREEN_DPI));
	ListView_SetColumnWidth(m_hList, 1, MulDiv(kStatusColumnWidth, dpi, USER_DEFAULT_SCREEN_DPI));
}

void DeviceListWindow::UpdateLayout()
{
	if (!m_hWnd || !m_hList)
		return;

	const SIZE size = ComputeSize();

	if (IsVisible())
		PositionWindow(size.cx, size.cy);
	else
		SetWindowPos(m_hWnd, nullptr, 0, 0, size.cx, size.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void DeviceListWindow::PositionWindow(int width, int height)
{
	MONITORINFO monitor = { sizeof(monitor) };
	const HMONITOR hMonitor = MonitorFromRect(&m_anchor, MONITOR_DEFAULTTONEAREST);
	if (!GetMonitorInfoW(hMonitor, &monitor))
		return;

	const int margin = MulDiv(kPadding, static_cast<int>(Dpi()), USER_DEFAULT_SCREEN_DPI);
	const RECT& work = monitor.rcWork;

	// 默认出现在图标上方，放不下就翻到下方。
	int x = m_anchor.left + (m_anchor.right - m_anchor.left) / 2 - width / 2;
	int y = m_anchor.top - height - margin;
	if (y < work.top)
		y = m_anchor.bottom + margin;

	const int minX = static_cast<int>(work.left) + margin;
	const int minY = static_cast<int>(work.top) + margin;
	int maxX = static_cast<int>(work.right) - margin - width;
	int maxY = static_cast<int>(work.bottom) - margin - height;
	if (maxX < minX)
		maxX = minX;
	if (maxY < minY)
		maxY = minY;

	x = std::clamp(x, minX, maxX);
	y = std::clamp(y, minY, maxY);

	SetWindowPos(m_hWnd, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
}

void DeviceListWindow::RebuildList()
{
	if (!m_hList)
		return;

	SendMessageW(m_hList, WM_SETREDRAW, FALSE, 0);
	ListView_DeleteAllItems(m_hList);

	if (m_devices.empty())
	{
		// 占位行的行号是 0，而 m_devices 为空，ActivateItem 的范围检查会挡掉它。
		LVITEMW placeholder = {};
		placeholder.mask = LVIF_TEXT;
		placeholder.iItem = 0;
		placeholder.pszText = const_cast<LPWSTR>(_(L"No devices found"));
		ListView_InsertItem(m_hList, &placeholder);
	}
	else
	{
		for (size_t i = 0; i < m_devices.size(); ++i)
		{
			const DeviceEntry& entry = m_devices[i];

			LVITEMW item = {};
			item.mask = LVIF_TEXT;
			item.iItem = static_cast<int>(i);
			item.pszText = const_cast<LPWSTR>(entry.name.c_str());
			ListView_InsertItem(m_hList, &item);

			const std::wstring status = StatusText(entry);
			ListView_SetItemText(m_hList, static_cast<int>(i), 1, const_cast<LPWSTR>(status.c_str()));
		}
	}

	SendMessageW(m_hList, WM_SETREDRAW, TRUE, 0);
	InvalidateRect(m_hList, nullptr, TRUE);
}

void DeviceListWindow::ActivateItem(int index)
{
	if (!m_connections || index < 0 || static_cast<size_t>(index) >= m_devices.size())
		return;

	const DeviceEntry& entry = m_devices[static_cast<size_t>(index)];
	switch (entry.status)
	{
	case ConnectionStatus::Connected:
		m_connections->Disconnect(entry.id);
		break;
	case ConnectionStatus::Connecting:
		break;   // 已在连接中，忽略重复请求
	case ConnectionStatus::Closed:
	case ConnectionStatus::Failed:
		if (entry.device)
			m_connections->Connect(entry.device);
		break;
	}
}

winrt::fire_and_forget DeviceListWindow::RefreshDevices()
{
	if (m_refreshing)
		co_return;

	m_refreshing = true;
	try
	{
		auto devices = co_await DeviceInformation::FindAllAsync(AudioPlaybackConnection::GetDeviceSelector());

		std::vector<DeviceEntry> merged;
		merged.reserve(devices.Size());
		for (const DeviceInformation& device : devices)
		{
			DeviceEntry entry = MakeEntry(device);
			if (const DeviceEntry* previous = FindEntry(entry.id))
			{
				entry.status = previous->status;
				entry.message = previous->message;
			}
			merged.push_back(std::move(entry));
		}

		// 枚举结果里没有、但正在连接或已连接的设备（例如开机自动重连）保留下来。
		for (const DeviceEntry& previous : m_devices)
		{
			if (previous.status == ConnectionStatus::Closed)
				continue;
			const bool exists = std::any_of(merged.begin(), merged.end(),
				[&previous](const DeviceEntry& entry) { return entry.id == previous.id; });
			if (!exists)
				merged.push_back(previous);
		}

		m_devices = std::move(merged);
		RebuildList();
		UpdateLayout();
	}
	catch (winrt::hresult_error const&)
	{
		LOG_CAUGHT_EXCEPTION();
	}

	m_refreshing = false;
}

DeviceListWindow::DeviceEntry* DeviceListWindow::FindEntry(std::wstring_view id)
{
	for (DeviceEntry& entry : m_devices)
	{
		if (std::wstring_view(entry.id) == id)
			return &entry;
	}
	return nullptr;
}

DeviceListWindow::DeviceEntry DeviceListWindow::MakeEntry(const DeviceInformation& device)
{
	std::wstring id(device.Id());
	std::wstring name(device.Name());
	if (name.empty())
		name = id;

	return DeviceEntry{ device, std::move(id), std::move(name), ConnectionStatus::Closed, {} };
}

std::wstring DeviceListWindow::StatusText(const DeviceEntry& entry)
{
	switch (entry.status)
	{
	case ConnectionStatus::Connecting:
		return _(L"Connecting");
	case ConnectionStatus::Connected:
		return _(L"Connected");
	case ConnectionStatus::Failed:
		return entry.message.empty() ? std::wstring(_(L"Failed")) : entry.message;
	case ConnectionStatus::Closed:
		break;
	}
	return _(L"Not connected");
}

void DeviceListWindow::OnConnectionStatus(const DeviceInformation& device, ConnectionStatus status, const std::wstring& message)
{
	const std::wstring id(device.Id());

	DeviceEntry* entry = FindEntry(id);
	if (!entry)
	{
		m_devices.push_back(MakeEntry(device));
		entry = &m_devices.back();
	}

	entry->status = status;
	entry->message = message;

	if (IsVisible())
	{
		RebuildList();
		UpdateLayout();
	}
}

LRESULT CALLBACK DeviceListWindow::StaticWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	DeviceListWindow* self = reinterpret_cast<DeviceListWindow*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

	if (message == WM_NCCREATE)
	{
		const auto create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		self = static_cast<DeviceListWindow*>(create->lpCreateParams);
		self->m_hWnd = hWnd;
		SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
	}

	if (!self)
		return DefWindowProcW(hWnd, message, wParam, lParam);

	return self->WndProc(message, wParam, lParam);
}

LRESULT DeviceListWindow::WndProc(UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_ACTIVATE:
		if (LOWORD(wParam) == WA_INACTIVE)
		{
			// 只有真的被激活过才谈得上"失活"：SetForegroundWindow 失败时
			// 弹窗会立刻收到 WA_INACTIVE，那种情况不能把它吃掉。
			if (m_activated)
			{
				m_lastAutoHideTick = GetTickCount64();
				Hide();
			}
		}
		else
		{
			m_activated = true;
		}
		return 0;

	case WM_TIMER:
		if (wParam == kRefreshTimerId)
			RefreshDevices();
		return 0;

	case WM_SIZE:
		if (m_hList)
		{
			const int padding = MulDiv(kPadding, static_cast<int>(Dpi()), USER_DEFAULT_SCREEN_DPI);
			SetWindowPos(m_hList, nullptr, padding, padding,
				LOWORD(lParam) - padding * 2, HIWORD(lParam) - padding * 2, SWP_NOZORDER);
		}
		return 0;

	case WM_NOTIFY:
	{
		const auto header = reinterpret_cast<const NMHDR*>(lParam);
		if (header->idFrom == kListControlId)
		{
			if (header->code == LVN_ITEMACTIVATE)
			{
				// 行号就是 m_devices 下标（RebuildList 按同序插入）。
				const auto activated = reinterpret_cast<const NMLISTVIEW*>(lParam);
				ActivateItem(activated->iItem);
			}
			else if (header->code == LVN_KEYDOWN)
			{
				const auto key = reinterpret_cast<const NMLVKEYDOWN*>(lParam);
				if (key->wVKey == VK_ESCAPE)
					Hide();
			}
		}
		break;
	}

	case WM_CLOSE:
		Hide();
		return 0;
	}

	return DefWindowProcW(m_hWnd, message, wParam, lParam);
}
