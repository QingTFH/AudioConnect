// SerializedExecutor 自测（step13b 计划书 §验收 断言 2）：
// Post 先进先出、跨线程投递、DrainFor 排空、Stop 后丢弃、IsOnExecutorThread、
// 任务异常不杀线程、YieldTo 跳跃（异步完成路径）。
// 断言计数与 main.cpp 共享（g_checks / g_failures）。

#include "../pch.h"

#include "../SerializedExecutor.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern int g_checks;
extern int g_failures;

#define WIDEN2(x) L##x
#define WIDEN(x) WIDEN2(x)
#define CHECK(cond)                                                                                                    \
	do                                                                                                                 \
	{                                                                                                                  \
		++g_checks;                                                                                                    \
		if (!(cond))                                                                                                   \
		{                                                                                                              \
			++g_failures;                                                                                              \
			std::wcerr << L"FAIL line " << __LINE__ << L": " WIDEN(#cond) << std::endl;                                 \
		}                                                                                                              \
	} while (0)

namespace
{
	using namespace std::chrono_literals;

	winrt::fire_and_forget HopFrom(SerializedExecutor& executor, std::thread::id* landedOn, bool* done)
	{
		co_await executor.YieldTo();
		*landedOn = std::this_thread::get_id();
		*done = true;
	}
}

int RunSerializedExecutorTests()
{
	constexpr auto kTimeout = 10s;

	// —— Post 先进先出 + IsOnExecutorThread ——
	{
		SerializedExecutor executor(L"test-fifo");
		CHECK(!executor.IsOnExecutorThread()); // 测试线程不是执行器线程

		std::vector<int> order;
		for (int i = 0; i < 5; ++i)
		{
			CHECK(executor.Post([&order, i] { order.push_back(i); }));
		}
		CHECK(executor.DrainFor(kTimeout));

		CHECK(order.size() == 5);
		bool inOrder = true;
		for (int i = 0; i < 5; ++i)
			inOrder = inOrder && order[i] == i;
		CHECK(inOrder);

		bool onExecutor = false;
		executor.Post([&executor, &onExecutor] { onExecutor = executor.IsOnExecutorThread(); });
		CHECK(executor.DrainFor(kTimeout));
		CHECK(onExecutor);

		executor.Stop();
	}

	// —— 跨线程 Post ——
	{
		SerializedExecutor executor(L"test-cross");
		int executed = 0;

		std::thread poster([&executor, &executed] {
			for (int i = 0; i < 10; ++i)
				executor.Post([&executed] { ++executed; });
		});
		poster.join();

		CHECK(executor.DrainFor(kTimeout));
		CHECK(executed == 10);

		executor.Stop();
	}

	// —— DrainFor 在有任务在执行时等待其完成 ——
	{
		SerializedExecutor executor(L"test-drain");

		std::atomic<bool> started{ false };
		executor.Post([&started] {
			started = true;
			std::this_thread::sleep_for(50ms);
		});
		// 等 started 置位，确保 DrainFor 调用时任务正在执行
		while (!started)
			std::this_thread::sleep_for(1ms);

		CHECK(executor.DrainFor(kTimeout)); // 必须等任务跑完才返回 true
		executor.Stop();
	}

	// —— 任务异常不杀线程，后续任务照常执行 ——
	{
		SerializedExecutor executor(L"test-throw");
		bool reached = false;

		executor.Post([] { throw std::logic_error("boom"); });
		executor.Post([&reached] { reached = true; });

		CHECK(executor.DrainFor(kTimeout));
		CHECK(reached);

		executor.Stop();
	}

	// —— Stop：排空后收线程；之后 Post 返回 false ——
	{
		SerializedExecutor executor(L"test-stop");
		int executed = 0;

		executor.Post([&executed] { ++executed; });
		executor.Stop(); // 排空：已入队的任务仍会执行
		CHECK(executed == 1);

		CHECK(!executor.Post([] {})); // Stop 后丢弃
		executor.Stop();              // 幂等
		CHECK(executor.DrainFor(kTimeout));
	}

	// —— YieldTo：非执行器线程上 co_await ⇒ 跳到执行器线程 ——
	{
		SerializedExecutor executor(L"test-hop");
		std::thread::id landedOn{};
		bool done = false;

		HopFrom(executor, &landedOn, &done); // 在测试线程启动，挂起在 YieldTo
		CHECK(!done);                        // 尚未恢复

		CHECK(executor.DrainFor(kTimeout));
		CHECK(done);
		CHECK(landedOn == executor.m_thread.get_id());

		executor.Stop();
	}

	return g_failures;
}
