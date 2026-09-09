#include "pch.h"

#include "ConnectionManager.h"

#include "I18n.h"

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
}

void ConnectionManager::SetStatusHandler(ConnectionStatusHandler handler)
{
	m_statusHandler = std::move(handler);
}

winrt::fire_and_forget ConnectionManager::Connect(DeviceInformation device)
{
	Report(device, ConnectionStatus::Connecting);

	bool success = false;
	std::wstring errorMessage;

	try
	{
		auto connection = AudioPlaybackConnection::TryCreateFromId(device.Id());
		if (connection)
		{
			m_connections.emplace(std::wstring(device.Id()), std::pair(device, connection));

			connection.StateChanged([this](const auto& sender, const auto&) {
				OnStateChanged(sender);
			});

			co_await connection.StartAsync();
			auto result = co_await connection.OpenAsync();

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
		LOG_CAUGHT_EXCEPTION();
	}

	if (success)
	{
		Report(device, ConnectionStatus::Connected);
	}
	else
	{
		auto it = m_connections.find(std::wstring(device.Id()));
		if (it != m_connections.end())
		{
			it->second.second.Close();
			m_connections.erase(it);
		}
		Report(device, ConnectionStatus::Failed, errorMessage);
	}
}

winrt::fire_and_forget ConnectionManager::ConnectById(std::wstring deviceId)
{
	auto device = co_await DeviceInformation::CreateFromIdAsync(deviceId);
	Connect(device);
}

bool ConnectionManager::Disconnect(std::wstring_view deviceId)
{
	auto it = m_connections.find(std::wstring(deviceId));
	if (it == m_connections.end())
		return false;

	auto device = it->second.first;
	it->second.second.Close();
	m_connections.erase(it);

	Report(device, ConnectionStatus::Closed);
	return true;
}

void ConnectionManager::CloseAll()
{
	for (const auto& entry : m_connections)
	{
		entry.second.second.Close();
		Report(entry.second.first, ConnectionStatus::Closed);
	}
	m_connections.clear();
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

void ConnectionManager::OnStateChanged(const AudioPlaybackConnection& sender)
{
	if (sender.State() != AudioPlaybackConnectionState::Closed)
		return;

	auto it = m_connections.find(std::wstring(sender.DeviceId()));
	if (it != m_connections.end())
	{
		Report(it->second.first, ConnectionStatus::Closed);
		m_connections.erase(it);
	}
	sender.Close();
}

void ConnectionManager::Report(const DeviceInformation& device, ConnectionStatus status, const std::wstring& message)
{
	if (m_statusHandler)
	{
		m_statusHandler(device, status, message);
	}
}
