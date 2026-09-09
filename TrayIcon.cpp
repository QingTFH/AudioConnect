#include "pch.h"

#include "TrayIcon.h"

#include <cwchar>

TrayIcon::TrayIcon()
{
	m_nid = {};
	m_nid.cbSize = sizeof(m_nid);
	m_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
	m_nid.uVersion = NOTIFYICON_VERSION_4;

	m_niid = {};
	m_niid.cbSize = sizeof(m_niid);
}

void TrayIcon::Initialize(HWND hWnd, UINT callbackMessage, const wchar_t* tip)
{
	m_nid.hWnd = hWnd;
	m_nid.uCallbackMessage = callbackMessage;
	wcscpy_s(m_nid.szTip, tip);

	m_niid.hWnd = hWnd;
}

void TrayIcon::Refresh(HICON icon)
{
	m_nid.hIcon = icon;

	if (!Shell_NotifyIconW(NIM_MODIFY, &m_nid))
	{
		if (Shell_NotifyIconW(NIM_ADD, &m_nid))
		{
			FAIL_FAST_IF_WIN32_BOOL_FALSE(Shell_NotifyIconW(NIM_SETVERSION, &m_nid));
		}
		else
		{
			LOG_LAST_ERROR();
		}
	}
}

void TrayIcon::Remove()
{
	Shell_NotifyIconW(NIM_DELETE, &m_nid);
}

HRESULT TrayIcon::GetRect(RECT& rect)
{
	return Shell_NotifyIconGetRect(&m_niid, &rect);
}
