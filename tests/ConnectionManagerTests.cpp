// 连接路径单测（step13a 建立，step13b 改造）：经 mock 工厂驱动 ConnectionManager
// 的连接/断开主路径，在无蓝牙、无网络、无真实 WinRT 运行时行为的环境里覆盖
// 两份计划书的断言清单。
//
// step13b 驱动方式变化：ConnectImpl/DisconnectImpl 只能在执行器线程上运行
//（生产由 Connect/Disconnect 投递），测试经 Harness::Post 投递 + drain() 等待，
// 不变式逐条保留；新增线程不变量 / 并发压测 / 快照降级用例（13b 计划书 §验收）。
// 断言计数与 main.cpp 共享（g_checks / g_failures）；CHECK 只允许在主测试线程使用。

#include "../pch.h"

#include "../ConnectionManager.h"

#include "MockAudioConnection.h"

#include <exception>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
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

	// 空投影：DescribeDevice/Report 走 "device=<null>" 分支，不需要真设备。
	DeviceInformation NullDevice()
	{
		return DeviceInformation{ nullptr };
	}

	struct SharedFactory final : public IAudioConnectionFactory
	{
		bool returnNull = false;
		std::function<void(MockAudioConnection&)> onCreate;
		std::vector<std::shared_ptr<MockAudioConnection>> created;

		std::shared_ptr<IAudioConnection> Create(std::wstring const& deviceId) override
		{
			if (returnNull)
				return nullptr;
			auto mock = std::make_shared<MockAudioConnection>(deviceId);
			if (onCreate)
				onCreate(*mock);
			created.push_back(mock);
			return mock;
		}
	};

	// ConnectionManager 持有 unique_ptr<IAudioConnectionFactory>；
	// ForwardFactory 把所有权转到测试持有的 SharedFactory 上，便于断言。
	struct ForwardFactory final : public IAudioConnectionFactory
	{
		explicit ForwardFactory(std::shared_ptr<SharedFactory> inner)
			: m_inner(std::move(inner))
		{
		}

		std::shared_ptr<IAudioConnection> Create(std::wstring const& deviceId) override
		{
			return m_inner->Create(deviceId);
		}

	private:
		std::shared_ptr<SharedFactory> m_inner;
	};

	struct Harness
	{
		std::vector<std::pair<ConnectionStatus, std::wstring>> events;
		std::vector<std::thread::id> handlerThreads; // Report 路径的被调线程（13b 断言 5）
		std::shared_ptr<SharedFactory> factory = std::make_shared<SharedFactory>();
		std::shared_ptr<SerializedExecutor> executor = std::make_shared<SerializedExecutor>(L"test-exec");
		std::unique_ptr<ConnectionManager> mgr;

		Harness()
			: mgr(std::make_unique<ConnectionManager>(executor, std::make_unique<ForwardFactory>(factory)))
		{
			mgr->SetStatusHandler([this](const DeviceInformation&, ConnectionStatus status, const std::wstring& message) {
				handlerThreads.push_back(std::this_thread::get_id());
				events.emplace_back(status, message);
			});
		}

		~Harness()
		{
			// ConnectionManager 析构会 Stop 执行器（排空 + 收线程）。
			mgr.reset();
		}

		// Post 不做 CHECK：压测里会从工作线程调用，断言计数器非原子。
		template <typename F>
		void Post(F&& fn)
		{
			executor->Post(std::function<void()>(std::forward<F>(fn)));
		}

		// 等队列排空。只允许主测试线程调用。
		void drain()
		{
			CHECK(executor->DrainFor(10s));
		}

		// 投递一次 ConnectImpl（等价生产路径 Connect 的内部投递）。
		void Connect(const wchar_t* id)
		{
			Post([this, id = std::wstring(id)] { mgr->ConnectImpl(id, NullDevice()); });
		}

		// 投递一次 DisconnectImpl；closedOut 可选回传其 bool 返回值。
		void Disconnect(const wchar_t* id, bool* closedOut = nullptr)
		{
			Post([this, id = std::wstring(id), closedOut] {
				const bool closed = mgr->DisconnectImpl(id, NullDevice());
				if (closedOut)
					*closedOut = closed;
			});
		}

		// 取执行器线程 id（probe 任务）。
		std::thread::id ExecutorThreadId()
		{
			std::thread::id id{};
			Post([&id] { id = std::this_thread::get_id(); });
			drain();
			return id;
		}

		int Count(ConnectionStatus status) const
		{
			int n = 0;
			for (auto const& e : events)
				if (e.first == status)
					++n;
			return n;
		}

		const std::wstring* LastMessage(ConnectionStatus status) const
		{
			const std::wstring* found = nullptr;
			for (auto const& e : events)
				if (e.first == status)
					found = &e.second;
			return found;
		}
	};

	// —— 断言 1：工厂返回 null ⇒ 不入表、Report(Failed) 恰一次、无转圈残留 ——
	void TestConnectFactoryReturnsNull()
	{
		Harness h;
		h.factory->returnNull = true;

		h.Connect(L"dev1");
		h.drain();

		CHECK(h.events.size() == 2);
		CHECK(h.Count(ConnectionStatus::Connecting) == 1);
		CHECK(h.Count(ConnectionStatus::Failed) == 1);
		auto msg = h.LastMessage(ConnectionStatus::Failed);
		CHECK(msg && *msg == L"Unknown error");
		CHECK(h.mgr->IsEmpty());
	}

	// —— 断言 2 / 7 / 15：Open 成功 ⇒ Report(Connected) 恰一次、条目留表、
	//      Close 不被调、Start 先于 Open ——
	void TestConnectSuccess()
	{
		Harness h;

		h.Connect(L"dev1");
		h.drain();

		CHECK(h.Count(ConnectionStatus::Connecting) == 1);
		CHECK(h.Count(ConnectionStatus::Connected) == 1);
		CHECK(h.Count(ConnectionStatus::Failed) == 0);
		CHECK(h.Count(ConnectionStatus::Closed) == 0);
		CHECK(h.mgr->IsEmpty() == false);

		auto mock = h.factory->created.back();
		CHECK(mock->closeCount == 0);
		CHECK(mock->callLog.size() == 2);
		CHECK(mock->callLog[0] == L"Start");
		CHECK(mock->callLog[1] == L"Open");
	}

	// —— 断言 3：TimedOut ⇒ Report(Failed, 超时文案)、条目摘出、Close 恰一次 ——
	void TestConnectTimedOut()
	{
		Harness h;
		h.factory->onCreate = [](MockAudioConnection& m) {
			m.openOutcome = OpenOutcome{ OpenResultKind::TimedOut, 0 };
		};

		h.Connect(L"dev1");
		h.drain();

		CHECK(h.Count(ConnectionStatus::Failed) == 1);
		auto msg = h.LastMessage(ConnectionStatus::Failed);
		CHECK(msg && *msg == L"The request timed out");
		CHECK(h.Count(ConnectionStatus::Connected) == 0);
		CHECK(h.mgr->IsEmpty());
		CHECK(h.factory->created.back()->closeCount == 1);
	}

	// —— 断言 4：Denied ⇒ Report(Failed, 拒绝文案) ——
	void TestConnectDenied()
	{
		Harness h;
		h.factory->onCreate = [](MockAudioConnection& m) {
			m.openOutcome = OpenOutcome{ OpenResultKind::Denied, 0 };
		};

		h.Connect(L"dev1");
		h.drain();

		CHECK(h.Count(ConnectionStatus::Failed) == 1);
		auto msg = h.LastMessage(ConnectionStatus::Failed);
		CHECK(msg && *msg == L"The operation was denied by the system");
		CHECK(h.mgr->IsEmpty());
		CHECK(h.factory->created.back()->closeCount == 1);
	}

	// —— 断言 5a：UnknownFailure 走返回值 ⇒ L2 抛 hresult_error ⇒
	//      Report(Failed, 含 0x 码的描述) ——
	void TestConnectUnknownFailureOutcome()
	{
		Harness h;
		h.factory->onCreate = [](MockAudioConnection& m) {
			m.openOutcome = OpenOutcome{ OpenResultKind::UnknownFailure, 0x80070005u };
		};

		h.Connect(L"dev1");
		h.drain();

		CHECK(h.Count(ConnectionStatus::Failed) == 1);
		auto msg = h.LastMessage(ConnectionStatus::Failed);
		CHECK(msg && msg->find(L"0x80070005") != std::wstring::npos);
		CHECK(h.mgr->IsEmpty());
		CHECK(h.factory->created.back()->closeCount == 1);
	}

	// —— 断言 5b：Open 经 AsyncOp 错误通道抛异常 ⇒ 同样落到 Report(Failed) ——
	void TestConnectOpenException()
	{
		Harness h;
		h.factory->onCreate = [](MockAudioConnection& m) {
			m.openError = std::make_exception_ptr(
				winrt::hresult_error(winrt::hresult{ static_cast<int32_t>(0x80004005) }));
		};

		h.Connect(L"dev1");
		h.drain();

		CHECK(h.Count(ConnectionStatus::Failed) == 1);
		auto msg = h.LastMessage(ConnectionStatus::Failed);
		CHECK(msg && msg->find(L"0x80004005") != std::wstring::npos);
		CHECK(h.mgr->IsEmpty());
	}

	// —— 补充：Start 抛异常 ⇒ 不再调 Open、Report(Failed)、条目被摘出并 Close ——
	void TestConnectStartException()
	{
		Harness h;
		h.factory->onCreate = [](MockAudioConnection& m) {
			m.startError = std::make_exception_ptr(
				winrt::hresult_error(winrt::hresult{ static_cast<int32_t>(0x80070490) }));
		};

		h.Connect(L"dev1");
		h.drain();

		auto mock = h.factory->created.back();
		// callLog = ["Start", "Close"]：收尾摘出条目并 Close；关键是不出现 "Open"。
		CHECK(mock->callLog.size() == 2);
		CHECK(mock->callLog[0] == L"Start");
		CHECK(std::find(mock->callLog.begin(), mock->callLog.end(), L"Open") == mock->callLog.end());
		CHECK(h.Count(ConnectionStatus::Failed) == 1);
		CHECK(h.mgr->IsEmpty());
		CHECK(mock->closeCount == 1);
	}

	// —— 断言 6 / 7：Open 期间收到 Closed ⇒ 不报 Connected、收尾报 Closed 恰一次 ——
	//      （CompleteOpen 在测试线程恢复协程 ⇒ 走真实的 YieldTo hop，13b 断言 2 的 hop 路径）
	void TestClosedWhileOpening()
	{
		Harness h;
		h.factory->onCreate = [](MockAudioConnection& m) {
			m.holdOpen = true;
		};

		h.Connect(L"dev1");
		h.drain();

		auto mock = h.factory->created.back();
		CHECK(h.Count(ConnectionStatus::Connected) == 0); // 还挂着，没有任何收尾

		mock->EmitClosed();                               // opening 期间的 Closed（投递）
		mock->CompleteOpen(OpenOutcome{ OpenResultKind::Success, 0 }); // 协程在测试线程恢复 → hop

		h.drain();

		CHECK(h.Count(ConnectionStatus::Closed) == 1);
		CHECK(h.Count(ConnectionStatus::Connected) == 0);
		CHECK(h.mgr->IsEmpty());
		CHECK(mock->closeCount == 1);
	}

	// —— 断言 8：Disconnect 命中表内条目 ⇒ Report(Closed) 一次、返回 true、条目消失 ——
	void TestDisconnectTracked()
	{
		Harness h;
		h.Connect(L"dev1");
		h.drain();
		CHECK(h.Count(ConnectionStatus::Connected) == 1);

		bool closed = false;
		h.Disconnect(L"dev1", &closed);
		h.drain();

		CHECK(closed);
		CHECK(h.Count(ConnectionStatus::Closed) == 1);
		CHECK(h.mgr->IsEmpty());
		CHECK(h.factory->created.back()->closeCount == 1);
	}

	// —— 断言 9：表内无条目（F5）⇒ 仍 Report(Closed)、返回 true、且调用了 Create ——
	void TestDisconnectFreshInstance()
	{
		Harness h;
		CHECK(h.mgr->IsEmpty());

		bool closed = false;
		h.Disconnect(L"dev1", &closed);
		h.drain();

		CHECK(closed);
		CHECK(h.factory->created.size() == 1); // 现造了一个连接对象
		CHECK(h.Count(ConnectionStatus::Closed) == 1);
		CHECK(h.factory->created[0]->closeCount == 1);
	}

	// —— 断言 10：F5 且工厂返回 null ⇒ Report(Closed) 仍发生、返回 false ——
	void TestDisconnectFreshInstanceNull()
	{
		Harness h;
		h.factory->returnNull = true;

		bool closed = true;
		h.Disconnect(L"dev1", &closed);
		h.drain();

		CHECK(!closed);
		CHECK(h.Count(ConnectionStatus::Closed) == 1); // 界面必须复位
	}

	// —— 断言 11：Close 内同步派发 Closed ⇒ Report(Closed) 恰一次（不重复）——
	void TestCloseDuringClose()
	{
		Harness h;
		h.Connect(L"dev1");
		h.drain();

		auto mock = h.factory->created.back();
		mock->emitClosedOnClose = true;

		bool closed = false;
		h.Disconnect(L"dev1", &closed);
		h.drain();

		CHECK(closed);
		CHECK(h.Count(ConnectionStatus::Closed) == 1); // "先摘出再 Close" ⇒ 回调找不到条目
		CHECK(h.mgr->IsEmpty());
	}

	// —— 断言 12：重复连接 ⇒ 旧条目摘出并 Close、新条目生效、旧实例迟到回调被挡 ——
	void TestConnectReplacesExisting()
	{
		Harness h;
		h.Connect(L"dev1");
		h.drain();
		auto first = h.factory->created[0];

		h.Connect(L"dev1");
		h.drain();
		auto second = h.factory->created[1];

		CHECK(first->closeCount == 1);
		CHECK(h.mgr->DeviceIds().size() == 1);

		h.events.clear();
		h.handlerThreads.clear();
		first->EmitClosed(); // 旧实例迟到回调：generation 不匹配 ⇒ 忽略
		h.drain();
		CHECK(h.Count(ConnectionStatus::Closed) == 0);
		CHECK(h.mgr->IsEmpty() == false);

		second->EmitClosed(); // 当前条目 ⇒ 摘出并上报
		h.drain();
		CHECK(h.Count(ConnectionStatus::Closed) == 1);
		CHECK(h.mgr->IsEmpty());
		CHECK(second->closeCount == 0); // 远端关闭路径不重复 Close
	}

	// —— 断言 13：CloseAll 空表不崩；非空表逐条 Closed 后表空 ——
	void TestCloseAll()
	{
		{
			Harness h;
			h.mgr->CloseAll(); // 空表（阻塞语义，直接返回）
			CHECK(h.events.empty());
		}
		{
			Harness h;
			h.Connect(L"dev1");
			h.Connect(L"dev2");
			h.drain();
			CHECK(h.mgr->DeviceIds().size() == 2);

			h.mgr->CloseAll();

			CHECK(h.mgr->IsEmpty());
			CHECK(h.Count(ConnectionStatus::Closed) == 2);
			CHECK(h.factory->created[0]->closeCount == 1);
			CHECK(h.factory->created[1]->closeCount == 1);

			h.mgr->CloseAll(); // 已空，不崩、不重复上报
			CHECK(h.Count(ConnectionStatus::Closed) == 2);
		}
	}

	// —— 断言 14：DeviceIds() 与表内容一致 ——
	void TestDeviceIds()
	{
		Harness h;
		h.Connect(L"dev1");
		h.Connect(L"dev2");
		h.drain();

		auto ids = h.mgr->DeviceIds();
		CHECK(ids.size() == 2);
		bool has1 = false, has2 = false;
		for (auto const& id : ids)
		{
			has1 = has1 || id == L"dev1";
			has2 = has2 || id == L"dev2";
		}
		CHECK(has1);
		CHECK(has2);
	}

	// —— 13b 断言 3 / 5：表操作与 handler 只在执行器线程 ——
	void TestThreadInvariants()
	{
		Harness h;
		auto execId = h.ExecutorThreadId();

		h.Connect(L"dev1");
		h.drain();
		auto mock = h.factory->created.back();
		mock->EmitClosed(); // 平台回调在测试线程触发（模拟 WinRT 事件线程）→ L2 入队
		h.drain();

		// L2 对连接对象的调用（Start/Open/Close）全部发生在执行器线程
		CHECK(mock->callThreads.size() == mock->callLog.size());
		for (auto tid : mock->callThreads)
			CHECK(tid == execId);
		// ConnectionStatusHandler（Report 路径）全部在执行器线程
		for (auto tid : h.handlerThreads)
			CHECK(tid == execId);
	}

	// —— 13b 断言 4：并发压测 ——
	// 多线程交错投递 Connect/Disconnect 同一设备；同时一个线程锤只读快照。
	// 串行化下的确定性不变量：终态表空（每轮以 Disconnect 收尾）、
	// 无泄漏（每个被创建的连接恰 Close 一次）、handler 线程恒为执行器线程。
	void TestConcurrentStress()
	{
		Harness h;
		auto execId = h.ExecutorThreadId();

		constexpr int kThreads = 4;
		constexpr int kRounds = 25;
		std::atomic<int> snapshotMismatches{ 0 };

		{
			std::vector<std::thread> workers;
			for (int t = 0; t < kThreads; ++t)
			{
				workers.emplace_back([&h] {
					for (int i = 0; i < kRounds; ++i)
					{
						h.Connect(L"stress");
						h.Disconnect(L"stress");
					}
				});
			}
			std::thread snapshotter([&h, &snapshotMismatches] {
				for (int i = 0; i < 50; ++i)
				{
					const bool empty = h.mgr->IsEmpty();
					auto ids = h.mgr->DeviceIds();
					if (empty != ids.empty())
						++snapshotMismatches;
				}
			});

			for (auto& w : workers)
				w.join();
			snapshotter.join();
		}

		h.drain();

		CHECK(snapshotMismatches == 0);
		CHECK(h.mgr->IsEmpty()); // 每轮 [Connect, Disconnect] ⇒ 终态必空
		CHECK(h.mgr->DeviceIds().empty());

		int closes = 0;
		for (auto const& m : h.factory->created)
		{
			closes += m->closeCount;
			for (auto tid : m->callThreads)
				CHECK(tid == execId);
		}
		CHECK(closes == static_cast<int>(h.factory->created.size())); // 无泄漏条目

		for (auto tid : h.handlerThreads)
			CHECK(tid == execId);
	}

	// —— 13b 断言 6：快照限时等待的超时降级路径 ——
	void TestSnapshotTimeoutDegradation()
	{
		Harness h;
		// 占住执行器 2.5s > 快照超时 2s ⇒ IsEmpty/DeviceIds 走降级
		h.Post([] { std::this_thread::sleep_for(2500ms); });

		CHECK(h.mgr->IsEmpty() == false); // 保守取向：按"非空"降级（守住退出确认）
		auto ids = h.mgr->DeviceIds();
		CHECK(ids.empty());               // 快照降级为空

		h.drain(); // 等长任务与堆积的快照任务处理完
	}

	// —— 13b 断言 7：连接进行中 CloseAll ⇒ 条目摘出、协程结果作废（discard 路径）——
	void TestCloseAllDuringOpening()
	{
		Harness h;
		h.factory->onCreate = [](MockAudioConnection& m) {
			m.holdOpen = true;
		};

		h.Connect(L"dev1");
		h.drain();
		CHECK(h.Count(ConnectionStatus::Connected) == 0);

		h.mgr->CloseAll(); // 阻塞：条目被摘出并 Close，Report(Closed)
		CHECK(h.Count(ConnectionStatus::Closed) == 1);
		CHECK(h.mgr->IsEmpty());

		auto mock = h.factory->created.back();
		mock->CompleteOpen(OpenOutcome{ OpenResultKind::Success, 0 }); // 协程恢复 → hop → 收尾
		h.drain();

		// 条目已不在表 ⇒ "result discarded" 分支：不再产生新事件、不重复 Close
		CHECK(h.events.size() == 2); // Connecting + Closed
		CHECK(mock->closeCount == 1);
	}

	// —— 断言 16：AsyncOp 自身（FromValue 同步 / 挂起后 Complete /
	//      CompleteError rethrow / 二次 Complete 幂等 / 未完成 await 抛 logic_error）——
	int g_asyncOut = 0;
	std::exception_ptr g_asyncErr = nullptr;

	winrt::fire_and_forget ConsumeAsync(AsyncOp<int> op)
	{
		try
		{
			g_asyncOut = co_await op;
		}
		catch (...)
		{
			g_asyncErr = std::current_exception();
		}
	}

	bool g_voidDone = false;

	winrt::fire_and_forget ConsumeVoidAsync(AsyncOp<void> op)
	{
		try
		{
			co_await op;
			g_voidDone = true;
		}
		catch (...)
		{
		}
	}

	void TestAsyncOp()
	{
		// FromValue：同步完成，协程不挂起
		{
			g_asyncOut = 0;
			g_asyncErr = nullptr;
			ConsumeAsync(AsyncOp<int>::FromValue(42));
			CHECK(g_asyncOut == 42);
			CHECK(!g_asyncErr);
		}
		// 未完成 ⇒ 挂起；Complete ⇒ 恢复
		{
			auto op = AsyncOp<int>{};
			g_asyncOut = 0;
			g_asyncErr = nullptr;
			ConsumeAsync(op);
			CHECK(g_asyncOut == 0);
			CHECK(!op.await_ready());

			op.state()->Complete(7);
			CHECK(g_asyncOut == 7);
			CHECK(!g_asyncErr);
		}
		// CompleteError ⇒ await_resume 处 rethrow
		{
			auto op = AsyncOp<int>{};
			g_asyncOut = 0;
			g_asyncErr = nullptr;
			ConsumeAsync(op);

			op.state()->CompleteError(std::make_exception_ptr(std::logic_error("boom")));
			CHECK(g_asyncOut == 0);
			CHECK(g_asyncErr != nullptr);
			bool caught = false;
			try
			{
				std::rethrow_exception(g_asyncErr);
			}
			catch (std::logic_error const& e)
			{
				caught = std::string(e.what()) == "boom";
			}
			CHECK(caught);
		}
		// 二次 Complete 被忽略（幂等）
		{
			auto op = AsyncOp<int>{};
			g_asyncOut = 0;
			g_asyncErr = nullptr;
			ConsumeAsync(op);

			op.state()->Complete(1);
			op.state()->Complete(2);
			CHECK(g_asyncOut == 1);
		}
		// 未完成时直接 await_resume ⇒ logic_error
		{
			auto op = AsyncOp<int>{};
			bool caught = false;
			try
			{
				op.await_resume();
			}
			catch (std::logic_error const&)
			{
				caught = true;
			}
			CHECK(caught);
		}
		// void 特化：FromValue 同步、Complete 后恢复
		{
			g_voidDone = false;
			ConsumeVoidAsync(AsyncOp<void>::FromValue());
			CHECK(g_voidDone);

			auto op = AsyncOp<void>{};
			g_voidDone = false;
			ConsumeVoidAsync(op);
			CHECK(!g_voidDone);
			op.state()->Complete();
			CHECK(g_voidDone);
		}
	}
}

int RunConnectionManagerTests()
{
	TestConnectFactoryReturnsNull();
	TestConnectSuccess();
	TestConnectTimedOut();
	TestConnectDenied();
	TestConnectUnknownFailureOutcome();
	TestConnectOpenException();
	TestConnectStartException();
	TestClosedWhileOpening();
	TestDisconnectTracked();
	TestDisconnectFreshInstance();
	TestDisconnectFreshInstanceNull();
	TestCloseDuringClose();
	TestConnectReplacesExisting();
	TestCloseAll();
	TestDeviceIds();
	TestThreadInvariants();
	TestConcurrentStress();
	TestSnapshotTimeoutDegradation();
	TestCloseAllDuringOpening();
	TestAsyncOp();

	return g_failures;
}
