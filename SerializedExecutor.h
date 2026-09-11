#pragma once

// L0：单线程任务队列（step13b，见 docs/step13b-串行执行器-计划书.md §新增）。
// 所有经 Post 的任务在同一线程上依序执行，连接表等共享状态因此无需加锁
// （step13 设计 §2.2 规则 2：表只在一个线程上被碰）。
//
// 线程约定（「事件线程承诺」的物理载体，规约 §4.4）：
//   - Post / Stop / DrainFor / IsOnExecutorThread 可从任意线程调用；
//   - 任务与 YieldTo 的续体只在执行器线程上运行；
//   - 【不要】在任务内部调用 Stop / DrainFor —— 会等待自身 ⇒ 死锁；
//   - Stop 之后 Post 返回 false，任务被丢弃（不抛）。
//
// 本头文件刻意【零 WinRT 依赖】：tests 工程不链接 WinRT 运行时行为，
// 也在这里实例化执行器。需要 apartment 的场景由构造方经 threadInit 注入。

#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

class SerializedExecutor
{
public:
	// threadInit 在专属线程启动后、处理任何任务前执行一次（可为空）。
	// 生产路径传 winrt::init_apartment（执行器线程要调 WinRT）；tests 传空。
	explicit SerializedExecutor(std::wstring threadName, std::function<void()> threadInit = {});
	~SerializedExecutor();

	SerializedExecutor(const SerializedExecutor&) = delete;
	SerializedExecutor& operator=(const SerializedExecutor&) = delete;

	// 若已 Stop，任务被丢弃并返回 false（不抛）。
	bool Post(std::function<void()> task);

	// 排空已入队任务后退出线程。幂等。可在析构外的任意时刻调用。
	void Stop();

	// 断言辅助："当前线程就是执行器线程？"
	bool IsOnExecutorThread() const;

	// 等待队列排空（含正在执行的任务完成）。超时返回 false。
	// 为可测试性刻意加的：没有它，测试只能 sleep 等异步效果（step13 设计 §3.2）。
	bool DrainFor(std::chrono::milliseconds timeout);

	// co_await executor->YieldTo()：
	//   - 已在执行器线程 ⇒ await_ready = true，同步继续（空跳，零开销）；
	//   - 否则挂起，把续体投递到执行器。
	// 协程在非执行器线程上只允许执行这一跳，不碰任何共享状态
	// （step13b 计划书 §D-3：AsyncOp 完成仍 inline 恢复，恢复后立刻跳回）。
	auto YieldTo() noexcept
	{
		struct Awaiter
		{
			SerializedExecutor* self;

			bool await_ready() const noexcept
			{
				return self->IsOnExecutorThread();
			}

			void await_suspend(std::coroutine_handle<> continuation) const noexcept
			{
				// Post 失败（已 Stop）⇒ 续体不再恢复，协程帧随之泄漏。
				// 仅发生在进程退出期：此时正确性已无意义，不 resume 反而安全
				//（避免在 teardown 中的错误线程上继续跑收尾逻辑）。
				self->Post([continuation] {
					continuation.resume();
				});
			}

			void await_resume() const noexcept {}
		};
		return Awaiter{ this };
	}

private:
	void Run();

	std::wstring m_threadName;
	std::function<void()> m_threadInit;
	std::thread m_thread;
	mutable std::mutex m_mutex;
	std::condition_variable m_cv;
	std::deque<std::function<void()>> m_queue;
	bool m_executing = false; // 正在执行某个任务（DrainFor 的谓词用）
	bool m_stopped = false;
};
