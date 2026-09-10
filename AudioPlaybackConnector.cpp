#include "pch.h"

#include "AudioPlaybackConnector.h"

#include "ConnectionManager.h"
#include "I18n.h"
#include "Log.h"
#include "Settings.h"
#include "Singleton.h"
#include "SvgIcon.h"
#include "TrayIcon.h"
#include "Util.h"
#include "WinrtAudioConnection.h"

namespace
{
	HINSTANCE g_hInst;
	HWND g_hWnd;
	HWND g_hWndXaml;
	Canvas g_xamlCanvas = nullptr;
	Flyout g_xamlFlyout = nullptr;
	MenuFlyout g_xamlMenu = nullptr;
	FocusState g_menuFocusState = FocusState::Unfocused;
	DevicePicker g_devicePicker = nullptr;
	UINT g_wmTaskbarCreated = 0;

	// 工厂注入（step13a）：静态初始化期只构造对象、不调 WinRT，安全。
	ConnectionManager g_connections{ std::make_unique<WinrtConnectionFactory>() };
	TrayIcon g_trayIcon;
	TrayIcons g_trayIcons;
	SettingsData g_settings;

	bool IsOsSupported();
	void SetupFlyout();
	void SetupMenu();
	void SetupDevicePicker();
	void UpdateNotifyIcon();
	void ApplyConnectionStatus(const DeviceInformation& device, ConnectionStatus status, const std::wstring& message);
	void ShowDevicePicker();
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

	// 日志要在任何可能出问题的步骤之前就绪，包括单实例判定。
	logger::SetLogFile(GetSettingsPath(g_hInst).parent_path() / std::filesystem::path(logger::kLogFileName));
	logger::Write(L"=== AudioPlaybackConnector start ===");

	// 必须放在最前面：第二个实例在这里就退出了，不碰任何全局资源。
	SingleInstanceGuard instanceGuard;
	if (!instanceGuard.TryAcquire())
	{
		if (!instanceGuard.NotifyExistingInstance())
		{
			LOG_LAST_ERROR();
		}
		return EXIT_SUCCESS;
	}

	winrt::init_apartment();

	// 必须早于任何 _() 调用，否则界面永远是英文原文。
	LoadTranslateData(g_hInst);

	if (!IsOsSupported())
	{
		TaskDialog(nullptr, nullptr, _(L"Unsupported Operating System"), nullptr, _(L"AudioPlaybackConnector is not supported on this operating system version."), TDCBF_OK_BUTTON, TD_ERROR_ICON, nullptr);
		return EXIT_FAILURE;
	}

	g_settings = LoadSettings(GetSettingsPath(g_hInst));

	WNDCLASSEXW wcex = {
		.cbSize = sizeof(wcex),
		.lpfnWndProc = WndProc,
		.hInstance = hInstance,
		.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_AUDIOPLAYBACKCONNECTOR)),
		.hCursor = LoadCursorW(nullptr, IDC_ARROW),
		.lpszClassName = kMainWindowClassName,
		.hIconSm = wcex.hIcon
	};

	RegisterClassExW(&wcex);

	// When parent window size is 0x0 or invisible, the dpi scale of menu is incorrect. Here we set window size to 1x1 and use WS_EX_LAYERED to make window looks like invisible.
	g_hWnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TOPMOST, kMainWindowClassName, nullptr, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
	FAIL_FAST_LAST_ERROR_IF_NULL(g_hWnd);
	FAIL_FAST_IF_WIN32_BOOL_FALSE(SetLayeredWindowAttributes(g_hWnd, 0, 0, LWA_ALPHA));

	DesktopWindowXamlSource desktopSource;
	auto desktopSourceNative2 = desktopSource.as<IDesktopWindowXamlSourceNative2>();
	winrt::check_hresult(desktopSourceNative2->AttachToWindow(g_hWnd));
	winrt::check_hresult(desktopSourceNative2->get_WindowHandle(&g_hWndXaml));

	g_xamlCanvas = Canvas();
	desktopSource.Content(g_xamlCanvas);

	g_trayIcons = LoadTrayIcons(g_hInst, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));

	SetupFlyout();
	SetupMenu();
	SetupDevicePicker();

	g_connections.SetStatusHandler(ApplyConnectionStatus);

	g_trayIcon.Initialize(g_hWnd, WM_NOTIFYICON, _(L"AudioPlaybackConnector"));
	UpdateNotifyIcon();

	g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
	LOG_LAST_ERROR_IF(g_wmTaskbarCreated == 0);

	PostMessageW(g_hWnd, WM_CONNECTDEVICE, 0, 0);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0))
	{
		BOOL processed = FALSE;
		winrt::check_hresult(desktopSourceNative2->PreTranslateMessage(&msg, &processed));
		if (!processed)
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	logger::Write(L"=== AudioPlaybackConnector stop ===");
	logger::Shutdown();

	return static_cast<int>(msg.wParam);
}

namespace
{
	bool IsOsSupported()
	{
		using namespace winrt::Windows::Foundation::Metadata;

		try
		{
			return ApiInformation::IsTypePresent(winrt::name_of<DesktopWindowXamlSource>()) &&
				ApiInformation::IsTypePresent(winrt::name_of<AudioPlaybackConnection>());
		}
		catch (winrt::hresult_error const&)
		{
			LOG_CAUGHT_EXCEPTION();
			return false;
		}
	}

	void ApplyConnectionStatus(const DeviceInformation& device, ConnectionStatus status, const std::wstring& message)
	{
		switch (status)
		{
		case ConnectionStatus::Connecting:
			g_devicePicker.SetDisplayStatus(device, _(L"Connecting"), DevicePickerDisplayStatusOptions::ShowProgress | DevicePickerDisplayStatusOptions::ShowDisconnectButton);
			break;
		case ConnectionStatus::Connected:
			g_devicePicker.SetDisplayStatus(device, _(L"Connected"), DevicePickerDisplayStatusOptions::ShowDisconnectButton);
			break;
		case ConnectionStatus::Failed:
			g_devicePicker.SetDisplayStatus(device, message, DevicePickerDisplayStatusOptions::ShowRetryButton);
			break;
		case ConnectionStatus::Closed:
			g_devicePicker.SetDisplayStatus(device, {}, DevicePickerDisplayStatusOptions::None);
			break;
		}
	}

	LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
	{
		switch (message)
		{
		case WM_DESTROY:
		{
			// 勾选了"下次启动重连"才把当前设备写进配置，否则落盘空列表。
			g_settings.lastDevices = g_settings.reconnect ? g_connections.DeviceIds() : std::vector<std::wstring>{};
			logger::Write(logger::Compose(
				L"WM_DESTROY: exiting, reconnect=", g_settings.reconnect ? L"true" : L"false",
				L" lastDevices=", std::to_wstring(g_settings.lastDevices.size())));
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
			switch (LOWORD(lParam))
			{
			case NIN_SELECT:
			case NIN_KEYSELECT:
				ShowDevicePicker();
				break;
			case WM_RBUTTONUP: // Menu activated by mouse click
				g_menuFocusState = FocusState::Pointer;
				break;
			case WM_CONTEXTMENU:
			{
				if (g_menuFocusState == FocusState::Unfocused)
					g_menuFocusState = FocusState::Keyboard;

				auto dpi = GetDpiForWindow(hWnd);
				Point point = {
					static_cast<float>(GET_X_LPARAM(wParam) * USER_DEFAULT_SCREEN_DPI / dpi),
					static_cast<float>(GET_Y_LPARAM(wParam) * USER_DEFAULT_SCREEN_DPI / dpi)
				};

				SetWindowPos(g_hWndXaml, 0, 0, 0, 0, 0, SWP_NOZORDER | SWP_SHOWWINDOW);
				SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, 1, 1, SWP_SHOWWINDOW);
				SetForegroundWindow(hWnd);

				g_xamlMenu.ShowAt(g_xamlCanvas, point);
			}
			break;
			}
			break;
		case WM_SHOWDEVICES:
			// 第二个实例被启动：把设备列表弹到用户面前。
			// 这里是"确保可见"，而不是像点托盘图标那样做开关切换——否则连点两次启动，
			// 反而会把已经打开的列表关掉。
			ShowDevicePicker();
			break;
		case WM_CONNECTDEVICE:
			if (g_settings.reconnect)
			{
				logger::Write(logger::Compose(
					L"WM_CONNECTDEVICE: reconnect count=", std::to_wstring(g_settings.lastDevices.size())));

				for (const auto& id : g_settings.lastDevices)
				{
					logger::Write(logger::Compose(L"WM_CONNECTDEVICE: reconnect id=", id));
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

	void ShowDevicePicker()
	{
		using namespace winrt::Windows::UI::Popups;

		RECT iconRect;
		auto hr = g_trayIcon.GetRect(iconRect);
		if (FAILED(hr))
		{
			LOG_HR(hr);
			return;
		}

		auto dpi = GetDpiForWindow(g_hWnd);
		Rect rect = {
			static_cast<float>(iconRect.left * USER_DEFAULT_SCREEN_DPI / dpi),
			static_cast<float>(iconRect.top * USER_DEFAULT_SCREEN_DPI / dpi),
			static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi),
			static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi)
		};

		SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_HIDEWINDOW);
		SetForegroundWindow(g_hWnd);
		g_devicePicker.Show(rect, Placement::Above);
	}

	void SetupFlyout()
	{
		TextBlock textBlock;
		textBlock.Text(_(L"All connections will be closed.\nExit anyway?"));
		textBlock.Margin({ 0, 0, 0, 12 });

		static CheckBox checkbox;
		checkbox.IsChecked(g_settings.reconnect);
		checkbox.Content(winrt::box_value(_(L"Reconnect on next start")));

		Button button;
		button.Content(winrt::box_value(_(L"Exit")));
		button.HorizontalAlignment(HorizontalAlignment::Right);
		button.Click([](const auto&, const auto&) {
			g_settings.reconnect = checkbox.IsChecked().Value();
			PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
		});

		StackPanel stackPanel;
		stackPanel.Children().Append(textBlock);
		stackPanel.Children().Append(checkbox);
		stackPanel.Children().Append(button);

		Flyout flyout;
		flyout.ShouldConstrainToRootBounds(false);
		flyout.Content(stackPanel);

		g_xamlFlyout = flyout;
	}

	void SetupMenu()
	{
		// https://docs.microsoft.com/en-us/windows/uwp/design/style/segoe-ui-symbol-font
		FontIcon settingsIcon;
		settingsIcon.Glyph(L"\xE713");

		MenuFlyoutItem settingsItem;
		settingsItem.Text(_(L"Bluetooth Settings"));
		settingsItem.Icon(settingsIcon);
		settingsItem.Click([](const auto&, const auto&) {
			winrt::Windows::System::Launcher::LaunchUriAsync(Uri(L"ms-settings:bluetooth"));
		});

		FontIcon closeIcon;
		closeIcon.Glyph(L"\xE8BB");

		MenuFlyoutItem exitItem;
		exitItem.Text(_(L"Exit"));
		exitItem.Icon(closeIcon);
		exitItem.Click([](const auto&, const auto&) {
			if (g_connections.IsEmpty())
			{
				PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
				return;
			}

			RECT iconRect;
			auto hr = g_trayIcon.GetRect(iconRect);
			if (FAILED(hr))
			{
				LOG_HR(hr);
				return;
			}

			auto dpi = GetDpiForWindow(g_hWnd);

			SetWindowPos(g_hWnd, HWND_TOPMOST, iconRect.left, iconRect.top, 0, 0, SWP_HIDEWINDOW);
			g_xamlCanvas.Width(static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi));
			g_xamlCanvas.Height(static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi));

			g_xamlFlyout.ShowAt(g_xamlCanvas);
		});

		MenuFlyout menu;
		menu.Items().Append(settingsItem);
		menu.Items().Append(exitItem);
		menu.Opened([](const auto& sender, const auto&) {
			auto menuItems = sender.template as<MenuFlyout>().Items();
			auto itemsCount = menuItems.Size();
			if (itemsCount > 0)
			{
				menuItems.GetAt(itemsCount - 1).Focus(g_menuFocusState);
			}
			g_menuFocusState = FocusState::Unfocused;
		});
		menu.Closed([](const auto&, const auto&) {
			ShowWindow(g_hWnd, SW_HIDE);
		});

		g_xamlMenu = menu;
	}

	void SetupDevicePicker()
	{
		g_devicePicker = DevicePicker();
		winrt::check_hresult(g_devicePicker.as<IInitializeWithWindow>()->Initialize(g_hWnd));

		g_devicePicker.Filter().SupportedDeviceSelectors().Append(AudioPlaybackConnection::GetDeviceSelector());
		g_devicePicker.DevicePickerDismissed([](const auto&, const auto&) {
			SetWindowPos(g_hWnd, nullptr, 0, 0, 0, 0, SWP_NOZORDER | SWP_HIDEWINDOW);
		});
		g_devicePicker.DeviceSelected([](const auto&, const auto& args) {
			g_connections.Connect(args.SelectedDevice());
		});
		g_devicePicker.DisconnectButtonClicked([](const auto&, const auto& args) {
			// 传整个 DeviceInformation 而不只是 Id：设备不在表里时（典型是应用重启后
			// 表已清空、而 Windows 侧仍连着），管理器要靠它现造对象才能把连接关掉。
			g_connections.Disconnect(args.Device());
		});
	}

	void UpdateNotifyIcon()
	{
		g_trayIcon.Refresh(IsSystemLightTheme() ? g_trayIcons.light : g_trayIcons.dark);
	}
}
