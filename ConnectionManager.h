#pragma once

// 蓝牙 A2DP Sink 连接的生命周期管理（L2）。
// 与 UI 完全解耦：状态变化通过 SetStatusHandler 回调上报，
// 由调用方决定怎么呈现（现在是 XAML DevicePicker，将来可以是任何东西）。
//
// step13b 串行执行器后，本类的线程模型（「事件线程承诺」的 L2 侧）：
//   - 连接表 m_connections 只在执行器线程上被增删改查 ⇒ 无需加锁；
//   - 平台回调（StateChanged / Start/Open 完成）只入队，不在回调栈内操作表；
//   - ConnectionStatusHandler 始终在执行器线程上被调用；
//     把 UI 更新封送到 UI 线程是 L3 的责任（13c）。
//
// 表里的每个条目都带一个单调递增的 generation。StateChanged 回调只认自己那一次的
// generation，于是：
//   - 被替换掉的旧实例迟到的 Closed 回调，不会误删新实例的条目；
//   - 关闭过程中的同步回调，不会二次摘除同一条目；
//   - 连接还没出结果时的 Closed 事件，转 Disconnecting、交给 Connect 协程统一收尾。

#include "AudioConnection.h"
#include "SerializedExecutor.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace winrt::Windows::Devices::Enumeration;

enum class ConnectionStatus
{
	Connecting,   // 正在 Start/Open
	Connected,    // Open 成功
	Failed,       // 失败，message 为可展示的错误描述
	Closed        // 被用户断开或远端关闭
};

// 日志用的标签。纯函数，便于单测。
inline const wchar_t* ConnectionStatusName(ConnectionStatus status)
{
	switch (status)
	{
	case ConnectionStatus::Connecting: return L"connecting";
	case ConnectionStatus::Connected: return L"connected";
	case ConnectionStatus::Failed: return L"failed";
	case ConnectionStatus::Closed: return L"closed";
	}
	return L"unknown";
}

// (设备, 状态, 描述文案)；仅 Failed 时描述非空。
using ConnectionStatusHandler = std::function<void(const DeviceInformation&, ConnectionStatus, const std::wstring&)>;

class ConnectionManager
{
public:
	// 执行器与工厂都强制注入：生产路径传 SerializedExecutor(+init_apartment) 与
	// WinrtConnectionFactory，测试传真执行器与 mock 工厂。
	ConnectionManager(std::shared_ptr<SerializedExecutor> executor,
		std::unique_ptr<IAudioConnectionFactory> factory);

	// 停掉执行器（排空剩余任务并收线程）：确保进程 teardown 时
	// 没有任务还在引用本对象（计划书 §易错点 4）。
	~ConnectionManager();

	void SetStatusHandler(ConnectionStatusHandler handler);

	// 按值接收 device：协程会挂起，不能持有调用方临时对象的引用。
	// 内部把实现投递到执行器线程。
	winrt::fire_and_forget Connect(DeviceInformation device);
	winrt::fire_and_forget ConnectById(std::wstring deviceId);

	// 主动断开。
	//
	// 设备在表中（正常路径）：摘出条目并 Close()。
	// 设备不在表中也要处理：最典型的场景是**应用重启过**（表随之清空），而 Windows 侧
	// 仍持有这条 A2DP sink 连接。此时若直接返回，界面上的"断开连接"按钮点下去毫无
	// 反应，用户从本程序侧没有任何办法断开它。这里改用调用方手上的 DeviceInformation
	// 现造一个连接对象关掉。
	//
	// 语义投递到执行器线程执行，故无返回值（step13b 计划书 §D-2；
	// 旧 bool 返回值全仓无消费方）。
	void Disconnect(const DeviceInformation& device);

	// 关闭并上报全部连接，随后清空。阻塞至执行器完成关闭（限时）。
	void CloseAll();

	// 进程退出兜底（step13c 计划书 §D-2b）：对表内【残余】条目泄漏底层 ABI 引用
	//（DetachAbi）后清表，不让连接对象参与进程 teardown 析构。正常路径 CloseAll
	// 已清表，本方法是幂等空操作；只在 CloseAll 超时降级（执行器停摆）时兜底。
	// 投递执行器 + 限时等待；投不进去时静默放弃（进程即将结束，无可再保）。
	void DetachForProcessExit() noexcept;

	// 只读快照。阻塞至执行器给出结果（限时）；超时降级并落日志。
	// IsEmpty 降级取向刻意取 false（"非空"）：它守着退出确认浮层
	//（AudioPlaybackConnector.cpp 的 exitItem.Click）——宁可让用户多确认一次，
	// 也不能静默跳过确认。
	bool IsEmpty() const;
	std::vector<std::wstring> DeviceIds() const;

	// —— 以下为测试可见的实现核心 ——

	// Connect/Disconnect 的实现主体，**只能在执行器线程上运行**
	//（生产由 Connect/Disconnect 投递，测试经 Harness 的 Post 投递）。
	// 拆出来是为了让单测注入 deviceId 字符串而不必构造有效的 DeviceInformation
	//（空投影取 Id 会抛）。行为与拆分前逐字一致。
	winrt::fire_and_forget ConnectImpl(std::wstring deviceId, DeviceInformation device);
	bool DisconnectImpl(std::wstring deviceId, const DeviceInformation& device);

private:
	// 条目的显式生命周期（step13b 计划书 §D-4；替代 13a 的 opening/closedWhileOpening
	// 布尔组合）。不在表里 = 不处于任何状态（表的存在本身就是状态），故无 Idle 值；
	// Failed 从不存表（失败条目立即摘出），亦无枚举值。
	enum class LifecycleState
	{
		Connecting,    // Start/Open 进行中
		Connected,     // Open 成功
		Disconnecting  // Connecting 期间收到 Closed，等 Connect 协程收尾
	};

	struct ConnectionEntry
	{
		DeviceInformation device;
		std::shared_ptr<IAudioConnection> connection;
		uint32_t generation = 0;
		LifecycleState state = LifecycleState::Connecting;
	};

	using ConnectionMap = std::unordered_map<std::wstring, ConnectionEntry>;

	void OnStateChanged(std::wstring const& deviceId, std::wstring_view stateName, uint32_t generation);
	void Report(const DeviceInformation& device, ConnectionStatus status, const std::wstring& message = {});

	// CloseAll 的实现主体（执行器线程上运行）。
	void CloseAllOnExecutor();

	// DetachForProcessExit 的实现主体（执行器线程上运行）。
	void DetachForProcessExitOnExecutor() noexcept;

	// 从表中摘出条目，由调用方决定是否 Close()。
	// 必须先摘出再 Close：Close 可能同步派发 StateChanged，此时表里已无该条目，
	// OnStateChanged 自然找不到，不会二次 erase。
	// 刻意【不】撤销事件订阅 —— 接口本身不提供撤销手段（回调内撤销会堆损坏）。
	ConnectionEntry TakeOut(ConnectionMap::iterator it);

	// Close 不允许把异常抛到调用方（fire_and_forget 里就是 terminate）。
	// 接口契约里 Close() 已是 noexcept，这里再兜一层防实现方违规。
	static void CloseQuietly(std::shared_ptr<IAudioConnection> const& connection);

	std::shared_ptr<SerializedExecutor> m_executor;
	std::unique_ptr<IAudioConnectionFactory> m_factory;
	ConnectionMap m_connections;
	ConnectionStatusHandler m_statusHandler;
	uint32_t m_nextGeneration = 0;
};
