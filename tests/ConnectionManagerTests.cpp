// 连接路径单测（step13a）：经 mock 工厂驱动 ConnectionManager 的连接/断开主路径，
// 在无蓝牙、无网络、无真实 WinRT 运行时行为的环境里覆盖计划书 §验收 的断言清单。
// 断言计数与 main.cpp 共享（g_checks / g_failures）。

#include "../pch.h"

#include "../ConnectionManager.h"

#include "MockAudioConnection.h"

#include <exception>
#include <iostream>
#include <memory>
#include <string>
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
		std::shared_ptr<SharedFactory> factory = std::make_shared<SharedFactory>();
		std::unique_ptr<ConnectionManager> mgr;

		Harness()
			: mgr(std::make_unique<ConnectionManager>(std::make_unique<ForwardFactory>(factory)))
		{
			mgr->SetStatusHandler([this](const DeviceInformation&, ConnectionStatus status, const std::wstring& message) {
				events.emplace_back(status, message);
			});
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

		h.mgr->ConnectImpl(L"dev1", NullDevice());

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

		h.mgr->ConnectImpl(L"dev1", NullDevice());

		CHECK(h.Count(ConnectionStatus::Connecting) == 1);
		CHECK(h.Count(ConnectionStatus::Connected) == 1);
		CHECK(h.Count(ConnectionStatus::Failed) == 0);
		CHECK(h.Count(ConnectionStatus::Closed) == 0);
		CHECK(!h.mgr->IsEmpty());

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

		h.mgr->ConnectImpl(L"dev1", NullDevice());

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

		h.mgr->ConnectImpl(L"dev1", NullDevice());

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
			m.openOutcome = OpenOutcome{ OpenResultKind::UnknownFailure, 0x80070005 };
		};

		h.mgr->ConnectImpl(L"dev1", NullDevice());

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
			m.openError = std::make_exception_ptr(winrt::hresult_error(winrt::hresult{ 0x80004005 }));
		};

		h.mgr->ConnectImpl(L"dev1", NullDevice());

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
			m.startError = std::make_exception_ptr(winrt::hresult_error(winrt::hresult{ 0x80070490 }));
		};

		h.mgr->ConnectImpl(L"dev1", NullDevice());

		auto mock = h.factory->created.back();
		CHECK(mock->callLog.size() == 1); // 只 Start，没 Open
		CHECK(h.Count(ConnectionStatus::Failed) == 1);
		CHECK(h.mgr->IsEmpty());
		CHECK(mock->closeCount == 1);
	}

	// —— 断言 6 / 7：Open 期间收到 Closed ⇒ 不报 Connected、收尾报 Closed 恰一次 ——
	void TestClosedWhileOpening()
	{
		Harness h;
		h.factory->onCreate = [](MockAudioConnection& m) {
			m.holdOpen = true;
		};

		h.mgr->ConnectImpl(L"dev1", NullDevice());

		auto mock = h.factory->created.back();
		CHECK(h.Count(ConnectionStatus::Connected) == 0); // 还挂着，没有任何收尾

		mock->EmitClosed();                               // opening 期间的 Closed
		mock->CompleteOpen(OpenOutcome{ OpenResultKind::Success, 0 });

		CHECK(h.Count(ConnectionStatus::Closed) == 1);
		CHECK(h.Count(ConnectionStatus::Connected) == 0);
		CHECK(h.mgr->IsEmpty());
		CHECK(mock->closeCount == 1);
	}

	// —— 断言 8：Disconnect 命中表内条目 ⇒ Report(Closed) 一次、返回 true、条目消失 ——
	void TestDisconnectTracked()
	{
		Harness h;
		h.mgr->ConnectImpl(L"dev1", NullDevice());
		CHECK(h.Count(ConnectionStatus::Connected) == 1);

		bool closed = h.mgr->DisconnectImpl(L"dev1", NullDevice());

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

		bool closed = h.mgr->DisconnectImpl(L"dev1", NullDevice());

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

		bool closed = h.mgr->DisconnectImpl(L"dev1", NullDevice());

		CHECK(!closed);
		CHECK(h.Count(ConnectionStatus::Closed) == 1); // 界面必须复位
	}

	// —— 断言 11：Close 内同步派发 Closed ⇒ Report(Closed) 恰一次（不重复）——
	void TestCloseDuringClose()
	{
		Harness h;
		h.mgr->ConnectImpl(L"dev1", NullDevice());

		auto mock = h.factory->created.back();
		mock->emitClosedOnClose = true;

		bool closed = h.mgr->DisconnectImpl(L"dev1", NullDevice());

		CHECK(closed);
		CHECK(h.Count(ConnectionStatus::Closed) == 1); // "先摘出再 Close" ⇒ 回调找不到条目
		CHECK(h.mgr->IsEmpty());
	}

	// —— 断言 12：重复连接 ⇒ 旧条目摘出并 Close、新条目生效、旧实例迟到回调被挡 ——
	void TestConnectReplacesExisting()
	{
		Harness h;
		h.mgr->ConnectImpl(L"dev1", NullDevice());
		auto first = h.factory->created[0];

		h.mgr->ConnectImpl(L"dev1", NullDevice());
		auto second = h.factory->created[1];

		CHECK(first->closeCount == 1);
		CHECK(h.mgr->DeviceIds().size() == 1);

		h.events.clear();
		first->EmitClosed(); // 旧实例迟到回调：generation 不匹配 ⇒ 忽略
		CHECK(h.Count(ConnectionStatus::Closed) == 0);
		CHECK(!h.mgr->IsEmpty());

		second->EmitClosed(); // 当前条目 ⇒ 摘出并上报
		CHECK(h.Count(ConnectionStatus::Closed) == 1);
		CHECK(h.mgr->IsEmpty());
		CHECK(second->closeCount == 0); // 远端关闭路径不重复 Close
	}

	// —— 断言 13：CloseAll 空表不崩；非空表逐条 Closed 后表空 ——
	void TestCloseAll()
	{
		{
			Harness h;
			h.mgr->CloseAll(); // 空表
			CHECK(h.events.empty());
		}
		{
			Harness h;
			h.mgr->ConnectImpl(L"dev1", NullDevice());
			h.mgr->ConnectImpl(L"dev2", NullDevice());
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
		h.mgr->ConnectImpl(L"dev1", NullDevice());
		h.mgr->ConnectImpl(L"dev2", NullDevice());

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
	TestAsyncOp();

	return g_failures;
}
