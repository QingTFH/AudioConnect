#include "pch.h"

#include "SerializedExecutor.h"

#include "Log.h"

#include <system_error>

SerializedExecutor::SerializedExecutor(std::wstring threadName, std::function<void()> threadInit)
	: m_threadName(std::move(threadName))
	, m_threadInit(std::move(threadInit))
	, m_thread([this] { Run(); })
{
}

SerializedExecutor::~SerializedExecutor()
{
	Stop();
}

bool SerializedExecutor::Post(std::function<void()> task)
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_stopped)
			return false;
		m_queue.push_back(std::move(task));
	}
	m_cv.notify_one();
	return true;
}

void SerializedExecutor::Stop()
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_stopped)
			return;
		m_stopped = true;
	}
	m_cv.notify_all();

	// join 会先排空剩余任务（Run 的谓词是 stopped || !queue.empty()），
	// 队列清空后线程才退出。
	if (m_thread.joinable())
		m_thread.join();
}

bool SerializedExecutor::IsOnExecutorThread() const
{
	return m_thread.get_id() == std::this_thread::get_id();
}

bool SerializedExecutor::DrainFor(std::chrono::milliseconds timeout)
{
	std::unique_lock<std::mutex> lock(m_mutex);
	return m_cv.wait_for(lock, timeout, [this] {
		return m_stopped || (m_queue.empty() && !m_executing);
	});
}

void SerializedExecutor::Run()
{
	// 给崩溃转储与 Process Explorer 一个可辨认的名字（失败忽略，纯诊断用）。
	if (!m_threadName.empty())
	{
		HRESULT hr = SetThreadDescription(GetCurrentThread(), m_threadName.c_str());
		LOG_LAST_ERROR_IF(FAILED(hr));
	}

	if (m_threadInit)
	{
		try
		{
			m_threadInit();
		}
		catch (...)
		{
			// init_apartment 失败属于致命环境问题；吞掉让后续 WinRT 调用
			// 在 L2 的 catch 里以 hresult_error 形式暴露（有日志可查），
			// 而不是在执行器线程上直接 terminate。
			LOG_CAUGHT_EXCEPTION();
		}
	}

	for (;;)
	{
		std::function<void()> task;
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_cv.wait(lock, [this] {
				return m_stopped || !m_queue.empty();
			});
			if (m_queue.empty())
			{
				// stopped 且已排空 ⇒ 收工。
				return;
			}
			task = std::move(m_queue.front());
			m_queue.pop_front();
			m_executing = true;
		}

		// 任务异常不允许杀死执行器线程 —— 后续任务还要跑。
		try
		{
			task();
		}
		catch (...)
		{
			LOG_CAUGHT_EXCEPTION();
		}

		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_executing = false;
		}
		// 唤醒 DrainFor 等待者（每次任务完成都通知，简单且足够）。
		m_cv.notify_all();
	}
}
