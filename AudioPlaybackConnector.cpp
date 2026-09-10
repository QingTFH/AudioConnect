#include "pch.h"

#include "AudioPlaybackConnector.h"

#include "ConnectionManager.h"
#include "DeviceListWindow.h"
#include "ExitConfirm.h"
#include "I18n.h"
#include "Settings.h"
#include "SvgIcon.h"
#include "TrayIcon.h"
#include "TrayMenu.h"
#include "Util.h"

namespace
{
	HINSTANCE g_hInst;
	HWND g_hWnd;
	UINT g_wmTaskbarCreated = 0;

	ConnectionManager g_connections;
	DeviceListWindow g_deviceList;
	TrayIcon g_trayIcon;
	TrayIcons g_trayIcons;
	SettingsData g_settings;

	bool IsOsSupported();
	void UpdateNotifyIcon();
	void ApplyConnectionStatus(const DeviceInformation& device, ConnectionStatus status, const std::wstring& message);
	void ToggleDeviceList(POINT fallbackPoint);
	void OnTrayContextMenu(POINT point);
	void RequestExit();
	void OpenBluetoothSettings();
	LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR    lpCmdLine,
	_In_ int       nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);
	UNREFERENCED_PARAMETER(nCmdShow);

	g_hInst = hInstance;

	winrt::init_apartment();

	// 必须早于任何 _() 调用，否则界面永远是英文原文。
	LoadTranslateData(g_hInst);

	if (!IsOsSupported())
	{
		TaskDialog(nullptr, nullptr, _(L"Unsupported Operating System"), nullptr, _(L"AudioPlaybackConnector is not supported on this operating system version."), TDCBF_OK_BUTTON, TD_ERROR_ICON, nullptr);
		return EXIT_FAILURE;
	}

	// ListView 属于 comctl32，必须先初始化。
	INITCOMMONCONTROLSEX commonControls = { sizeof(commonControls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
	FAIL_FAST_IF_WIN32_BOOL_FALSE(InitCommonControlsEx(&commonControls));

	g_settings = LoadSettings(GetSettingsPath(g_hInst));

	WNDCLASSEXW wcex = {
		.cbSize = sizeof(wcex),
		.lpfnWndProc = WndProc,
		.hInstance = hInstance,
		.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_AUDIOPLAYBACKCONNECTOR)),
		.hCursor = LoadCursorW(nullptr, IDC_ARROW),
		.lpszClassName = L"AudioPlaybackConnector",
		.hIconSm = wcex.hIcon
	};

	RegisterClassExW(&wcex);

	// 纯消息窗口：只用来收托盘回调，从不 ShowWindow。
	g_hWnd = CreateWindowExW(0, L"AudioPlaybackConnector", nullptr, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
	FAIL_FAST_LAST_ERROR_IF_NULL(g_hWnd);

	if (!g_deviceList.Register(hInstance))
	{
		LOG_LAST_ERROR();
		return EXIT_FAILURE;
	}
	g_deviceList.SetConnectionManager(&g_connections);

	g_trayIcons = LoadTrayIcons(g_hInst, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));

	g_connections.SetStatusHandler(ApplyConnectionStatus);

	g_trayIcon.Initialize(g_hWnd, WM_NOTIFYICON, _(L"AudioPlaybackConnector"));
	UpdateNotifyIcon();

	g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
	LOG_LAST_ERROR_IF(g_wmTaskbarCreated == 0);

	PostMessageW(g_hWnd, WM_CONNECTDEVICE, 0, 0);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	return static_cast<int>(msg.wParam);
}

namespace
{
	bool IsOsSupported()
	{
		using namespace winrt::Windows::Foundation::Metadata;

		try
		{
			// 不再依赖 XAML Islands，因此只探测 A2DP Sink 本身是否可用。
			return ApiInformation::IsTypePresent(winrt::name_of<AudioPlaybackConnection>());
		}
		catch (winrt::hresult_error const&)
		{
			LOG_CAUGHT_EXCEPTION();
			return false;
		}
	}

	void ApplyConnectionStatus(const DeviceInformation& device, ConnectionStatus status, const std::wstring& message)
	{
		g_deviceList.OnConnectionStatus(device, status, message);
	}

	void ToggleDeviceList(POINT fallbackPoint)
	{
		// 托盘图标矩形拿不到时（罕见），退化用鼠标位置当锚点。
		RECT iconRect = { fallbackPoint.x, fallbackPoint.y, fallbackPoint.x + 1, fallbackPoint.y + 1 };
		RECT actualRect;
		if (SUCCEEDED(g_trayIcon.GetRect(actualRect)))
			iconRect = actualRect;

		g_deviceList.ToggleFromTray(iconRect);
	}

	void OnTrayContextMenu(POINT point)
	{
		g_deviceList.Hide();

		switch (ShowTrayMenu(g_hWnd, point))
		{
		case TrayMenuCommand::BluetoothSettings:
			OpenBluetoothSettings();
			break;
		case TrayMenuCommand::Exit:
			RequestExit();
			break;
		case TrayMenuCommand::None:
			break;
		}
	}

	void RequestExit()
	{
		if (g_connections.IsEmpty())
		{
			PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
			return;
		}

		bool reconnect = g_settings.reconnect;
		if (ConfirmExit(g_hWnd, reconnect))
		{
			g_settings.reconnect = reconnect;
			PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
		}
	}

	void OpenBluetoothSettings()
	{
		const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"ms-settings:bluetooth", nullptr, nullptr, SW_SHOWNORMAL));
		if (result <= 32)
		{
			LOG_LAST_ERROR();
		}
	}

	void UpdateNotifyIcon()
	{
		g_trayIcon.Refresh(IsSystemLightTheme() ? g_trayIcons.light : g_trayIcons.dark);
	}

	LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
	{
		switch (message)
		{
		case WM_DESTROY:
		{
			// 勾选了"下次启动重连"才把当前设备写进配置，否则落盘空列表。
			g_settings.lastDevices = g_settings.reconnect ? g_connections.DeviceIds() : std::vector<std::wstring>{};
			SaveSettings(GetSettingsPath(g_hInst), g_settings);

			g_connections.CloseAll();
			g_trayIcon.Remove();
			PostQuitMessage(0);
			break;
		}
		case WM_SETTINGCHANGE:
			if (lParam && CompareStringOrdinal(reinterpret_cast<LPCWCH>(lParam), -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL)
			{
				UpdateNotifyIcon();
			}
			break;
		case WM_NOTIFYICON:
		{
			// NOTIFYICON_VERSION_4：wParam 低位是鼠标坐标，lParam 低位是事件。
			const POINT point = { GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam) };
			switch (LOWORD(lParam))
			{
			case NIN_SELECT:
			case NIN_KEYSELECT:
				ToggleDeviceList(point);
				break;
			case WM_CONTEXTMENU:
				OnTrayContextMenu(point);
				break;
			}
			break;
		}
		case WM_CONNECTDEVICE:
			if (g_settings.reconnect)
			{
				for (const auto& id : g_settings.lastDevices)
				{
					g_connections.ConnectById(id);
				}
				g_settings.lastDevices.clear();
			}
			break;
		default:
			if (g_wmTaskbarCreated && message == g_wmTaskbarCreated)
			{
				UpdateNotifyIcon();
			}
			return DefWindowProcW(hWnd, message, wParam, lParam);
		}
		return 0;
	}
}
