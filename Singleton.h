#pragma once

// 单实例保护：命名互斥体保证同一登录会话只跑一个实例；
// 第二个实例唤醒第一个实例的设备列表后自行退出。
//
// 用 Local\ 命名空间（会话作用域）而不是 Global\：托盘程序本来就该按会话隔离，
// 多用户登录 / 快速切换用户时各自的实例互不干扰。

class SingleInstanceGuard
{
public:
	// 抢锁。true = 本实例是第一个；false = 已有实例正在运行。
	// CreateMutexW 本身失败时返回 true（宁可允许多开，也不要因为建不出互斥体而拒绝启动）。
	bool TryAcquire();

	// 唤醒已有实例弹出设备列表。找不到它的窗口时返回 false。
	bool NotifyExistingInstance() const;

	~SingleInstanceGuard();

private:
	wil::unique_handle m_mutex;
};
