#pragma once

// L0：一次性异步结果 AsyncOp<T>（可 co_await 的最小基础设施）。
// 见 docs/step13a-平台接口化-计划书.md §契约 1（技术选型裁决 (B)）。
//
// 约定：
//   - 实现方对同一个 AsyncOp 恰好调用一次 Complete / CompleteError
//     （可同步、可异步）；二次调用被忽略（幂等）。
//   - 协程侧 co_await：已完成 ⇒ 同步继续；未完成 ⇒ 挂起，
//     由调用 Complete 的线程恢复。
//   - 状态由 shared_ptr<State> 持有 ⇒ co_await 产生的临时 AsyncOp
//     在挂起点被销毁也安全。
//   - 错误通道刻意保留：平台异常（winrt::hresult_error 等）经
//     CompleteError 穿透到 co_await 处 rethrow，保证 L2 的
//     catch + FormatHresultError 错误文案路径与接口化之前一致。

#include <coroutine>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

template <typename T>
class AsyncOp
{
public:
	// 完成源。实现方（平台适配层 / mock）经由 AsyncOp::state() 拿到它，
	// 在平台回调里调用 Complete / CompleteError。
	struct State
	{
		void Complete(T value)
		{
			std::coroutine_handle<> continuation{};
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				if (m_completed)
					return; // 幂等：二次完成被忽略
				m_value = std::move(value);
				m_completed = true;
				continuation = m_continuation;
			}
			if (continuation)
				continuation.resume();
		}

		void CompleteError(std::exception_ptr error)
		{
			std::coroutine_handle<> continuation{};
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				if (m_completed)
					return;
				m_error = std::move(error);
				m_completed = true;
				continuation = m_continuation;
			}
			if (continuation)
				continuation.resume();
		}

		bool IsCompleted() const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_completed;
		}

	private:
		friend class AsyncOp<T>;

		bool SetContinuation(std::coroutine_handle<> continuation)
		{
			// 返回 false 表示已完成（await_suspend 不挂起，立即继续）。
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_completed)
				return false;
			m_continuation = continuation;
			return true;
		}

		T TakeResult()
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (!m_completed)
				throw std::logic_error("AsyncOp awaited before completion");
			T value = std::move(m_value);
			std::exception_ptr error = std::move(m_error);
			m_error = nullptr;
			if (error)
				std::rethrow_exception(error);
			return value;
		}

		mutable std::mutex m_mutex;
		T m_value{};
		std::exception_ptr m_error;
		std::coroutine_handle<> m_continuation{};
		bool m_completed = false;
	};

	AsyncOp()
		: m_state(std::make_shared<State>())
	{
	}

	static AsyncOp FromValue(T value)
	{
		AsyncOp op;
		op.m_state->Complete(std::move(value));
		return op;
	}

	static AsyncOp FromError(std::exception_ptr error)
	{
		AsyncOp op;
		op.m_state->CompleteError(std::move(error));
		return op;
	}

	std::shared_ptr<State> state() const
	{
		return m_state;
	}

	// --- awaitable 协议 ---

	bool await_ready() const
	{
		return m_state->IsCompleted();
	}

	bool await_suspend(std::coroutine_handle<> continuation)
	{
		// 已完成则不挂起（防丢唤醒竞态）。
		return m_state->SetContinuation(continuation);
	}

	T await_resume()
	{
		return m_state->TakeResult();
	}

private:
	std::shared_ptr<State> m_state;
};

// void 特化：没有值，只有"完成与否 / 以什么异常完成"。
template <>
class AsyncOp<void>
{
public:
	struct State
	{
		void Complete()
		{
			std::coroutine_handle<> continuation{};
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				if (m_completed)
					return;
				m_completed = true;
				continuation = m_continuation;
			}
			if (continuation)
				continuation.resume();
		}

		void CompleteError(std::exception_ptr error)
		{
			std::coroutine_handle<> continuation{};
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				if (m_completed)
					return;
				m_error = std::move(error);
				m_completed = true;
				continuation = m_continuation;
			}
			if (continuation)
				continuation.resume();
		}

		bool IsCompleted() const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_completed;
		}

	private:
		friend class AsyncOp<void>;

		bool SetContinuation(std::coroutine_handle<> continuation)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_completed)
				return false;
			m_continuation = continuation;
			return true;
		}

		void TakeResult()
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (!m_completed)
				throw std::logic_error("AsyncOp awaited before completion");
			std::exception_ptr error = std::move(m_error);
			m_error = nullptr;
			if (error)
				std::rethrow_exception(error);
		}

		mutable std::mutex m_mutex;
		std::exception_ptr m_error;
		std::coroutine_handle<> m_continuation{};
		bool m_completed = false;
	};

	AsyncOp()
		: m_state(std::make_shared<State>())
	{
	}

	static AsyncOp FromValue()
	{
		AsyncOp op;
		op.m_state->Complete();
		return op;
	}

	static AsyncOp FromError(std::exception_ptr error)
	{
		AsyncOp op;
		op.m_state->CompleteError(std::move(error));
		return op;
	}

	std::shared_ptr<State> state() const
	{
		return m_state;
	}

	bool await_ready() const
	{
		return m_state->IsCompleted();
	}

	bool await_suspend(std::coroutine_handle<> continuation)
	{
		return m_state->SetContinuation(continuation);
	}

	void await_resume()
	{
		m_state->TakeResult();
	}

private:
	std::shared_ptr<State> m_state;
};
