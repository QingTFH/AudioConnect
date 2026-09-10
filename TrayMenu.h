#pragma once

// 托盘右键菜单：原生 Win32 弹出菜单，替代上游的 XAML MenuFlyout。
// 只负责"弹出来 + 告诉调用方选了什么"，不执行业务动作。

enum class TrayMenuCommand
{
	None,               // 取消或选择失败
	BluetoothSettings,  // 打开系统蓝牙设置
	Exit                // 退出程序（是否需要二次确认由调用方决定）
};

// 在屏幕坐标 screenPoint 处弹出。owner 用于前台化：隐藏窗口必须先成为前台窗口，
// 否则点菜单外部时菜单不会消失。
TrayMenuCommand ShowTrayMenu(HWND owner, POINT screenPoint);
