#pragma once

// L1：IAudioConnection / IAudioConnectionFactory 的 WinRT 实现。
// 本文件（.h + .cpp）是全仓唯一允许出现 winrt::Windows::Media::Audio 与
// AudioPlaybackConnection 的地方（step13a 完成线第 1 条）。
//
// 依赖 pch.h 先行包含（项目既有惯例，同 ConnectionManager.h）。

#include "AudioConnection.h"

class WinrtAudioConnection final : public IAudioConnection
{
public:
	explicit WinrtAudioConnection(winrt::Windows::Media::Audio::AudioPlaybackConnection connection);

	std::wstring DeviceId() const override;

	AsyncOp<void> Start() override;
	AsyncOp<OpenOutcome> Open() override;

	void RegisterStateChanged(std::function<void(std::wstring_view stateName)> onStateChanged) override;

	// 契约 noexcept：底层 Close 的异常吞掉并落日志（原 L2 CloseQuietly
	// 的 catch 文案随之下沉到这里，保证日志一致）。
	void Close() noexcept override;

private:
	winrt::Windows::Media::Audio::AudioPlaybackConnection m_connection;
};

class WinrtConnectionFactory final : public IAudioConnectionFactory
{
public:
	std::shared_ptr<IAudioConnection> Create(std::wstring const& deviceId) override;
};
