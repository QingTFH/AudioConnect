#pragma once

// L1：平台连接接口（step13a 平台接口化）。
// 这是 ConnectionManager（L2）唯一能看到的"连接"抽象；
// WinRT 具体实现见 WinrtAudioConnection.h/.cpp（全仓唯一允许出现
// winrt::Windows::Media::Audio 的地方）。
//
// 契约要点（docs/step13-架构解耦-设计.md §3.1 + step13a 计划书 §契约 2）：
//   - Start 必须先于 Open（官方语义：Start 置系统为"接受"态，Open 才是
//     PC 主动发起）。实现方与调用方都要保证这个顺序。
//   - 回调报告状态变化（stateName = "opened" / "closed" / ...，与日志
//     文本一致）；【不撤销】订阅 —— 在回调里撤销订阅会破坏 C++/WinRT
//     的 handler 容器（实测 0xC0000374 堆损坏），本接口因此刻意不给
//     任何撤销手段。
//   - Close() noexcept：不允许把异常抛给调用方。

#include "AsyncOp.hpp"

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

// 刻意不暴露 WinRT 的 AudioPlaybackConnectionOpenResultStatus。
enum class OpenResultKind
{
	Success,
	TimedOut,
	Denied,
	UnknownFailure
};

struct OpenOutcome
{
	OpenResultKind kind = OpenResultKind::UnknownFailure;
	uint32_t extendedError = 0;
};

// 结果枚举 → 日志文本。映射与接口化之前逐字一致（零可观察变化）。
inline const wchar_t* OpenStatusName(OpenResultKind kind)
{
	switch (kind)
	{
	case OpenResultKind::Success: return L"success";
	case OpenResultKind::TimedOut: return L"request-timed-out";
	case OpenResultKind::Denied: return L"denied-by-system";
	case OpenResultKind::UnknownFailure: return L"unknown-failure";
	}
	return L"unexpected";
}

class IAudioConnection
{
public:
	virtual ~IAudioConnection() = default;

	virtual std::wstring DeviceId() const = 0;

	// Start 必须先于 Open。
	virtual AsyncOp<void> Start() = 0;

	// Open 的结果经 OpenOutcome 返回。UnknownFailure 同样走返回值
	// （extendedError 带码），由调用方决定是否抛 —— 抛的位置在 L2，
	// 与接口化之前一致，保证 "OpenAsync status=..." 日志先于异常打印。
	// 平台调用本身抛出的异常（如设备消失）经 AsyncOp 的错误通道穿透。
	virtual AsyncOp<OpenOutcome> Open() = 0;

	// 每次状态变化回调一次；stateName 与 StateName() 的日志文本一致。
	// 返回值刻意是 void：本项目【不撤销】订阅（规约 §4.4）。
	virtual void RegisterStateChanged(std::function<void(std::wstring_view stateName)> onStateChanged) = 0;

	virtual void Close() noexcept = 0;
};

class IAudioConnectionFactory
{
public:
	virtual ~IAudioConnectionFactory() = default;

	// 失败返回 nullptr（对应 TryCreateFromId 返回 null），不吞异常 ——
	// 平台抛出的异常向上传播，由调用方的 catch 生成错误文案。
	virtual std::shared_ptr<IAudioConnection> Create(std::wstring const& deviceId) = 0;
};
