#pragma once

// 托盘图标的 Win32 封装（NOTIFYICONDATAW + NOTIFYICONIDENTIFIER）。
// 原先是 main 里两个裸全局结构体 + 一个 UpdateNotifyIcon()，
// 状态散在 wWinMain / WndProc / SetupMenu 三处。

class TrayIcon
{
public:
	TrayIcon();

	// 只登记窗口与回调消息，图标在 Refresh 时才设置。
	void Initialize(HWND hWnd, UINT callbackMessage, const wchar_t* tip);

	// NIM_MODIFY 失败（图标尚未注册）时自动退回 NIM_ADD + NIM_SETVERSION。
	void Refresh(HICON icon);
	void Remove();

	// 取托盘图标屏幕矩形，用于把设备列表弹窗定位到图标附近。
	HRESULT GetRect(RECT& rect);

private:
	NOTIFYICONDATAW m_nid;
	NOTIFYICONIDENTIFIER m_niid;
};
