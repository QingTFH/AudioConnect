#include "pch.h"

#include "ConnectionManager.h"

#include "I18n.h"
#include "Log.h"

#include <cwchar>

namespace
{
	std::wstring FormatHresultError(const winrt::hresult_error& ex)
	{
		std::wstring message(64, L'\0');
		while (1)
		{
			auto result = swprintf(message.data(), message.size(), L"%s (0x%08X)", ex.message().c_str(), static_cast<uint32_t>(ex.code()));
			if (result < 0)
			{
				message.resize(message.size() * 2);
			}
			else
			{
				message.resize(result);
				break;
			}
		}
		return message;
	}

	std::wstring FormatHex(uint32_t code)
	{
		constexpr wchar_t kDigits[] = L"0123456789ABCDEF";

		std::wstring text(10, L'0');
		text[0] = L'0';
		text[1] = L'x';
		for (uint32_t i = 0; i < 8; ++i)
		{
			text[2 + i] = kDigits[(code >> (4 * (7 - i))) & 0xF];
		}
		return text;
	}

	std::wstring DescribeDevice(const DeviceInformation& device)
	{
		if (!device)
			return L"device=<null>";

		return logger::Compose(L"device=\"", std::wstring(device.Name()), L"\" id=", std::wstring(device.Id()));
	}

	const wchar_t* StateName(AudioPlaybackConnectionState state)
	{
		switch (state)
		{
		case AudioPlaybackConnectionState::Closed: return L"closed";
		case AudioPlaybackConnectionState::Opened: return L"opened";
		}
		return L"unknown";
	}

	const wchar_t* OpenStatusName(AudioPlaybackConnectionOpenResultStatus status)
	{
		switch (status)
		{
		case AudioPlaybackConnectionOpenResultStatus::Success: return L"success";
		case AudioPlaybackConnectionOpenResultStatus::RequestTimedOut: return L"request-timed-out";
		case AudioPlaybackConnectionOpenResultStatus::DeniedBySystem: return L"denied-by-system";
		case AudioPlaybackConnectionOpenResultStatus::UnknownFailure: return L"unknown-failure";
		}
		return L"unexpected";
	}
}

void ConnectionManager::SetStatusHandler(ConnectionStatusHandler handler)
{
	m_statusHandler = std::move(handler);
}

ConnectionManager::ConnectionEntry ConnectionManager::TakeOut(ConnectionMap::iterator it)
{
	ConnectionEntry entry = std::move(it->second);
	m_connections.erase(it);

	// 先撤销订阅，再交还给调用方决定是否 Close()。
	// 撤销之后，即便 Close() 同步派发 StateChanged，也不会再回到 OnStateChanged。
	RevokeQuietly(entry);
	return entry;
}

void ConnectionManager::RevokeQuietly(ConnectionEntry& entry)
{
	try
	{
		entry.connection.StateChanged(entry.stateChangedToken);
	}
	catch (winrt::hresult_error const& ex)
	{
		logger::Write(logger::Compose(L"Revoke StateChanged failed: ", FormatHresultError(ex)));
		LOG_CAUGHT_EXCEPTION();
	}
}

void ConnectionManager::CloseQuietly(AudioPlaybackConnection& connection)
{
	try
	{
		connection.Close();
	}
	catch (winrt::hresult_error const& ex)
	{
		logger::Write(logger::Compose(L"Close failed: ", FormatHresultError(ex)));
		LOG_CAUGHT_EXCEPTION();
	}
}

winrt::fire_and_forget ConnectionManager::Connect(DeviceInformation device)
{
	const std::wstring deviceId(device.Id());

	Report(device, ConnectionStatus::Connecting);
	logger::Write(logger::Compose(L"Connect: enter ", DescribeDevice(device)));

	bool success = false;
	bool inserted = false;
	std::wstring errorMessage;
	uint32_t generation = 0;

	try
	{
		auto connection = AudioPlaybackConnection::TryCreateFromId(device.Id());
		logger::Write(logger::Compose(
			L"Connect: TryCreateFromId ", connection ? L"ok " : L"returned null ", DescribeDevice(device)));

		if (connection)
		{
			// 同一设备重复连接：emplace 遇到已有键会静默失败并留下旧对象，而界面
			// 已经跳到"已连接"。这里改成先把旧条目摘出并关闭，再插入新条目。
			if (auto it = m_connections.find(deviceId); it != m_connections.end())
			{
				logger::Write(logger::Compose(L"Connect: replacing existing entry ", DescribeDevice(device)));
				auto old = TakeOut(it);
				CloseQuietly(old.connection);
			}

			generation = ++m_nextGeneration;

			auto stateChangedToken = connection.StateChanged([this, generation](const auto& sender, const auto&) {
				OnStateChanged(sender, generation);
			});

			inserted = m_connections.emplace(
				deviceId, ConnectionEntry{ device, connection, stateChangedToken, generation, true, false }).second;
			logger::Write(logger::Compose(
				L"Connect: entry inserted=", inserted ? L"true" : L"false",
				L" generation=", std::to_wstring(generation), L" ", DescribeDevice(device)));

			co_await connection.StartAsync();
			auto result = co_await connection.OpenAsync();

			logger::Write(logger::Compose(
				L"Connect: OpenAsync status=", OpenStatusName(result.Status()),
				L" extendedError=", FormatHex(static_cast<uint32_t>(result.ExtendedError())),
				L" generation=", std::to_wstring(generation)));

			switch (result.Status())
			{
			case AudioPlaybackConnectionOpenResultStatus::Success:
				success = true;
				break;
			case AudioPlaybackConnectionOpenResultStatus::RequestTimedOut:
				success = false;
				errorMessage = _(L"The request timed out");
				break;
			case AudioPlaybackConnectionOpenResultStatus::DeniedBySystem:
				success = false;
				errorMessage = _(L"The operation was denied by the system");
				break;
			case AudioPlaybackConnectionOpenResultStatus::UnknownFailure:
				success = false;
				winrt::throw_hresult(result.ExtendedError());
				break;
			}
		}
		else
		{
			success = false;
			errorMessage = _(L"Unknown error");
		}
	}
	catch (winrt::hresult_error const& ex)
	{
		success = false;
		errorMessage = FormatHresultError(ex);
		logger::Write(logger::Compose(L"Connect: exception ", errorMessage));
		LOG_CAUGHT_EXCEPTION();
	}

	// OpenAsync 期间条目可能已被替换（重复连接）或摘除（用户断开 / 远端关闭）。
	// 只有"仍属于本次调用"才允许动表，否则说明结果已经过期。
	auto it = m_connections.find(deviceId);
	const bool current = inserted && (it != m_connections.end()) && (it->second.generation == generation);
	const bool closedWhileOpening = current && it->second.closedWhileOpening;

	if (current && success && !closedWhileOpening)
	{
		it->second.opening = false;
		Report(device, ConnectionStatus::Connected);
	}
	else if (current)
	{
		auto entry = TakeOut(it);
		CloseQuietly(entry.connection);

		if (closedWhileOpening)
		{
			logger::Write(logger::Compose(L"Connect: closed while opening, report closed ", DescribeDevice(device)));
			Report(device, ConnectionStatus::Closed);
		}
		else
		{
			Report(device, ConnectionStatus::Failed, errorMessage);
		}
	}
	else if (!inserted)
	{
		// 条目根本没进表（TryCreateFromId 返回 null，或插入前就抛了异常）：
		// 界面上可能已经显示 "Connecting"，必须收尾，不能留个转圈。
		Report(device, ConnectionStatus::Failed, errorMessage);
	}
	else
	{
		logger::Write(logger::Compose(L"Connect: result discarded, entry no longer current ", DescribeDevice(device)));
	}
}

winrt::fire_and_forget ConnectionManager::ConnectById(std::wstring deviceId)
{
	logger::Write(logger::Compose(L"ConnectById: enter id=", deviceId));

	try
	{
		// 这一句必须在 try 里：ConnectById 是 fire_and_forget，协程内未捕获的异常
		// 会直接 terminate。设备已移除 / 蓝牙关闭时 CreateFromIdAsync 会抛。
		auto device = co_await DeviceInformation::CreateFromIdAsync(deviceId);
		Connect(device);
	}
	catch (winrt::hresult_error const& ex)
	{
		// 这里拿不到 DeviceInformation，无法按 Failed 上报（界面本来也没出现过这个
		// 设备，不存在需要清理的行），只落日志。
		logger::Write(logger::Compose(L"ConnectById: CreateFromIdAsync failed ", FormatHresultError(ex)));
		LOG_CAUGHT_EXCEPTION();
	}
}

bool ConnectionManager::Disconnect(std::wstring_view deviceId)
{
	auto it = m_connections.find(std::wstring(deviceId));
	if (it == m_connections.end())
	{
		logger::Write(logger::Compose(L"Disconnect: no such entry id=", deviceId));
		return false;
	}

	// 先摘出（并撤销订阅），再 Close()：关闭过程中同步派发的 StateChanged
	// 在表里已经找不到条目，不会二次 erase。
	auto entry = TakeOut(it);
	logger::Write(logger::Compose(L"Disconnect: closing ", DescribeDevice(entry.device)));
	CloseQuietly(entry.connection);
	Report(entry.device, ConnectionStatus::Closed);
	return true;
}

void ConnectionManager::CloseAll()
{
	// 整表先挪走：每条 Close() 期间即使同步派发 StateChanged，OnStateChanged 也
	// 找不到条目，不会二次摘除（E2 的根因）。
	auto entries = std::move(m_connections);
	m_connections.clear();

	for (auto& pair : entries)
	{
		auto& entry = pair.second;

		logger::Write(logger::Compose(L"CloseAll: closing ", DescribeDevice(entry.device)));
		RevokeQuietly(entry);
		CloseQuietly(entry.connection);
		Report(entry.device, ConnectionStatus::Closed);
	}
}

bool ConnectionManager::IsEmpty() const
{
	return m_connections.empty();
}

std::vector<std::wstring> ConnectionManager::DeviceIds() const
{
	std::vector<std::wstring> ids;
	ids.reserve(m_connections.size());
	for (const auto& entry : m_connections)
	{
		ids.push_back(entry.first);
	}
	return ids;
}

void ConnectionManager::OnStateChanged(const AudioPlaybackConnection& sender, uint32_t generation)
{
	const auto state = sender.State();
	const std::wstring deviceId(sender.DeviceId());

	logger::Write(logger::Compose(
		L"StateChanged: state=", StateName(state),
		L" generation=", std::to_wstring(generation),
		L" deviceId=", deviceId));

	if (state != AudioPlaybackConnectionState::Closed)
		return;

	auto it = m_connections.find(deviceId);
	if (it == m_connections.end())
	{
		logger::Write(L"StateChanged: no matching entry (already removed or superseded), ignored");
		return;
	}

	// 只认自己那一次的 generation：旧实例迟到的回调不许动新实例的条目。
	if (it->second.generation != generation)
	{
		logger::Write(logger::Compose(
			L"StateChanged: stale generation (current=", std::to_wstring(it->second.generation), L"), ignored"));
		return;
	}

	if (it->second.opening)
	{
		// 连接还没出结果，先记下来交给 Connect() 协程统一收尾，
		// 否则会出现"刚报已连接、条目却已被摘掉"的脱节。
		it->second.closedWhileOpening = true;
		logger::Write(L"StateChanged: closed while opening, deferred to Connect()");
		return;
	}

	const auto device = it->second.device;
	TakeOut(it);
	logger::Write(logger::Compose(L"StateChanged: entry removed ", DescribeDevice(device)));

	// State() == Closed 说明底层已经关闭，这里不再重复 Close()。
	// 旧实现在 erase 之后还调了一次 Close()，那正是重入的来源。
	Report(device, ConnectionStatus::Closed);
}

void ConnectionManager::Report(const DeviceInformation& device, ConnectionStatus status, const std::wstring& message)
{
	std::wstring text = logger::Compose(L"Report: status=", ConnectionStatusName(status), L" ", DescribeDevice(device));
	if (!message.empty())
	{
		text.append(L" message=\"");
		text.append(message);
		text.append(L"\"");
	}
	logger::Write(text);

	if (m_statusHandler)
	{
		m_statusHandler(device, status, message);
	}
}
