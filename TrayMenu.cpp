#include "pch.h"

#include "TrayMenu.h"

#include "I18n.h"

namespace
{
	constexpr UINT kCommandBluetoothSettings = 1;
	constexpr UINT kCommandExit = 2;
}

TrayMenuCommand ShowTrayMenu(HWND owner, POINT screenPoint)
{
	HMENU menu = CreatePopupMenu();
	if (!menu)
	{
		LOG_LAST_ERROR();
		return TrayMenuCommand::None;
	}

	FAIL_FAST_IF_WIN32_BOOL_FALSE(AppendMenuW(menu, MF_STRING, kCommandBluetoothSettings, _(L"Bluetooth Settings")));
	FAIL_FAST_IF_WIN32_BOOL_FALSE(AppendMenuW(menu, MF_SEPARATOR, 0, nullptr));
	FAIL_FAST_IF_WIN32_BOOL_FALSE(AppendMenuW(menu, MF_STRING, kCommandExit, _(L"Exit")));

	SetForegroundWindow(owner);
	const int command = TrackPopupMenu(menu,
		TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
		screenPoint.x, screenPoint.y, 0, owner, nullptr);

	// 官方推荐的收尾动作：让菜单正确关闭，否则它会一直挂在屏幕上。
	PostMessageW(owner, WM_NULL, 0, 0);
	DestroyMenu(menu);

	switch (command)
	{
	case kCommandBluetoothSettings:
		return TrayMenuCommand::BluetoothSettings;
	case kCommandExit:
		return TrayMenuCommand::Exit;
	default:
		return TrayMenuCommand::None;
	}
}
