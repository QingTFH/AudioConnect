#pragma once

// IAudioConnection 的 mock：只实现接口契约（Start / Open / RegisterStateChanged /
// Close），不模拟 WinRT 语义 —— 真实语义由真机四步覆盖（step13a 计划书 §风险）。
// 完成时机完全确定：默认同步完成，holdOpen 模式下由测试手动收尾。

#include "../AudioConnection.h"

#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct MockAudioConnection final : public IAudioConnection
{
	std::wstring id;

	// —— 可配置行为 ——
	OpenOutcome openOutcome{ OpenResultKind::Success, 0 };
	std::exception_ptr startError; // 非 null ⇒ Start 以异常完成
	std::exception_ptr openError;  // 非 null ⇒ Open 以异常完成
	bool holdOpen = false;         // true ⇒ Open 返回未完成 op，测试经 CompleteOpen 收尾
	bool emitClosedOnClose = false; // Close 时同步派发一次 closed（钉住"先摘出再 Close"）

	// —— 观测 ——
	std::vector<std::wstring> callLog; // 依序记录 "Start" / "Open" / "Close"
	int closeCount = 0;

	explicit MockAudioConnection(std::wstring deviceId)
		: id(std::move(deviceId))
	{
	}

	std::wstring DeviceId() const override
	{
		return id;
	}

	AsyncOp<void> Start() override
	{
		callLog.push_back(L"Start");
		callThreads.push_back(std::this_thread::get_id());
		if (startError)
			return AsyncOp<void>::FromError(startError);
		return AsyncOp<void>::FromValue();
	}

	AsyncOp<OpenOutcome> Open() override
	{
		callLog.push_back(L"Open");
		callThreads.push_back(std::this_thread::get_id());
		if (openError)
			return AsyncOp<OpenOutcome>::FromError(openError);
		if (holdOpen)
		{
			auto op = AsyncOp<OpenOutcome>{};
			m_pendingOpenState = op.state();
			return op;
		}
		return AsyncOp<OpenOutcome>::FromValue(openOutcome);
	}

	void RegisterStateChanged(std::function<void(std::wstring_view stateName)> onStateChanged) override
	{
		m_onStateChanged = std::move(onStateChanged);
	}

	void Close() noexcept override
	{
		++closeCount;
		callLog.push_back(L"Close");
		if (emitClosedOnClose)
			EmitClosed();
	}

	// —— 测试驱动接口 ——

	void EmitStateChanged(std::wstring_view stateName)
	{
		if (m_onStateChanged)
			m_onStateChanged(stateName);
	}

	void EmitClosed()
	{
		EmitStateChanged(L"closed");
	}

	void CompleteOpen(OpenOutcome outcome)
	{
		if (m_pendingOpenState)
			m_pendingOpenState->Complete(std::move(outcome));
	}

private:
	std::function<void(std::wstring_view)> m_onStateChanged;
	std::shared_ptr<AsyncOp<OpenOutcome>::State> m_pendingOpenState;
};
