#include "pch.h"

#include "Singleton.h"

#include "AudioPlaybackConnector.h"

namespace
{
	constexpr wchar_t kMutexName[] = L"Local\\AudioPlaybackConnector";
}

bool SingleInstanceGuard::TryAcquire()
{
	const HANDLE handle = CreateMutexW(nullptr, FALSE, kMutexName);
	// 立刻取错误码：后面任何一次 CloseHandle(nullptr) 都可能把它冲掉。
	const DWORD error = GetLastError();

	if (!handle)
	{
		LOG_LAST_ERROR();
		return true;
	}

	m_mutex.reset(handle);
	return error != ERROR_ALREADY_EXISTS;
}

bool SingleInstanceGuard::NotifyExistingInstance() const
{
	const HWND hWnd = FindWindowW(kMainWindowClassName, nullptr);
	if (!hWnd)
	{
		LOG_LAST_ERROR();
		return false;
	}

	// 第二个实例此刻是前台进程，把"允许设置前台窗口"的权利借给第一个实例，
	// 否则它的 SetForegroundWindow 会被前台窗口抢占限制挡掉。
	AllowSetForegroundWindow(ASFW_ANY);

	return PostMessageW(hWnd, WM_SHOWDEVICES, 0, 0) != FALSE;
}

SingleInstanceGuard::~SingleInstanceGuard() = default;
