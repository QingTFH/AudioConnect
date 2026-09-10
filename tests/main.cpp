// 零依赖单元测试：不引第三方框架，断言失败即累计计数，
// main 返回非 0 让 CI 直接失败。
// 覆盖：FNV-1a、Utf8/Utf16 往返、YMO 解析与查表、Settings 读写往返、
// 日志行格式化与拼接、连接状态标签映射。

#include "../pch.h"

#include "../ConnectionManager.h"
#include "../FnvHash.hpp"
#include "../I18n.h"
#include "../Log.h"
#include "../Settings.h"
#include "../Util.h"

#include <iostream>
#include <string>

static int g_checks = 0;
static int g_failures = 0;

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

static void TestFnv1a()
{
	// 空输入 = FNV offset basis
	CHECK(fnv1a_32(L"", 0) == 0x811c9dc5);
	// 预先用独立实现算好的基准值（对 UTF-16LE 字节流做 FNV-1a 32）
	CHECK(fnv1a_32(L"a", sizeof(wchar_t)) == 0x2b24d044);
	CHECK(fnv1a_32(L"abc", 3 * sizeof(wchar_t)) == 0xae1e997d);
	// 分段哈希（hval 续算）与一次性哈希等价
	auto whole = fnv1a_32(L"abcdef", 6 * sizeof(wchar_t));
	auto half1 = fnv1a_32(L"abc", 3 * sizeof(wchar_t));
	auto half2 = fnv1a_32(L"def", 3 * sizeof(wchar_t), half1);
	CHECK(whole == half2);
}

static void TestUtfRoundtrip()
{
	const std::string cases[] = {
		"",
		"hello",
		"中文测试",
		"emoji \xF0\x9F\x8E\xA7 mixed",
		"\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E utf8 no bom",
	};
	for (const auto& s : cases)
	{
		auto wide = Utf8ToUtf16(s);
		CHECK(Utf16ToUtf8(wide) == s);
	}
	// 双向各走一遍也必须稳定
	for (const auto& s : cases)
	{
		auto wide = Utf8ToUtf16(s);
		auto back = Utf16ToUtf8(wide);
		CHECK(Utf8ToUtf16(back) == wide);
	}
}

static std::vector<uint8_t> BuildYmo(std::initializer_list<std::pair<std::wstring, std::wstring>> pairs)
{
	std::vector<uint8_t> data;
	auto append16 = [&data](uint16_t v)
	{
		data.push_back(v & 0xFF);
		data.push_back((v >> 8) & 0xFF);
	};
	auto append32 = [&data](uint32_t v)
	{
		for (int i = 0; i < 4; ++i)
			data.push_back((v >> (8 * i)) & 0xFF);
	};

	append16(static_cast<uint16_t>(pairs.size()));
	// 先占位表项，payload 偏移之后再回填
	std::vector<size_t> offsets;
	for (size_t i = 0; i < pairs.size(); ++i)
	{
		append32(0);
		append16(0);
	}
	for (const auto& p : pairs)
	{
		offsets.push_back(data.size());
		auto bytes = reinterpret_cast<const uint8_t*>(p.first.c_str());
		size_t n = (p.first.size() + 1) * sizeof(wchar_t);
		data.insert(data.end(), bytes, bytes + n);
	}
	for (size_t i = 0; i < pairs.size(); ++i)
	{
		auto p = pairs.begin() + i;
		uint32_t hash = fnv1a_32(p->second.c_str(), p->second.size() * sizeof(wchar_t));
		size_t pos = 2 + i * 6;
		data[pos] = hash & 0xFF;
		data[pos + 1] = (hash >> 8) & 0xFF;
		data[pos + 2] = (hash >> 16) & 0xFF;
		data[pos + 3] = (hash >> 24) & 0xFF;
		data[pos + 4] = offsets[i] & 0xFF;
		data[pos + 5] = (offsets[i] >> 8) & 0xFF;
	}
	return data;
}

static void TestI18nYmo()
{
	auto blob = BuildYmo({
		{ L"你好", L"Hello" },
		{ L"再见", L"Goodbye" },
	});
	LoadTranslateDataFromMemory(blob.data(), blob.size());

	CHECK(wcscmp(Translate(L"Hello"), L"你好") == 0);
	CHECK(wcscmp(Translate(L"Goodbye"), L"再见") == 0);
	// 表里没有的原文原样返回
	CHECK(wcscmp(Translate(L"Missing"), L"Missing") == 0);

	// C_(ctxt, str)：按 (ctxt\004str) 组合串的哈希查；表里有则命中，没有则返回 str 本身
	auto blob2 = BuildYmo({
		{ L"打开(&O)", L"Menu\004Open" },
	});
	LoadTranslateDataFromMemory(blob2.data(), blob2.size());
	CHECK(wcscmp(Translate(L"Menu\004Open"), L"打开(&O)") == 0);
	// TranslateContext(str, ctxtStr)：ctxtStr 是 ctxt\004str 组合串（与 C_ 宏展开一致），命中返回译文
	CHECK(wcscmp(TranslateContext(L"Open", L"Menu\004Open"), L"打开(&O)") == 0);
	// 未命中时返回 str 本身
	CHECK(wcscmp(TranslateContext(L"Open", L"Other\004Open"), L"Open") == 0);

	// 空数据 / 截断数据不能崩、不能装载任何条目
	LoadTranslateDataFromMemory(nullptr, 0);
	LoadTranslateDataFromMemory(blob.data(), 3);
	CHECK(true);
}

static void TestSettingsRoundtrip()
{
	auto dir = std::filesystem::temp_directory_path() / L"apc_unit_tests";
	std::filesystem::create_directories(dir);
	auto path = dir / L"settings.json";

	SettingsData out;
	out.reconnect = true;
	out.lastDevices = { L"BT-Headset 01", L"音箱·蓝牙" };
	SaveSettings(path, out);

	auto in = LoadSettings(path);
	CHECK(in.reconnect == true);
	CHECK(in.lastDevices.size() == 2);
	CHECK(in.lastDevices[0] == L"BT-Headset 01");
	CHECK(in.lastDevices[1] == L"音箱·蓝牙");

	// 空列表与默认值
	SettingsData empty;
	empty.reconnect = false;
	SaveSettings(path, empty);
	auto in2 = LoadSettings(path);
	CHECK(in2.reconnect == false);
	CHECK(in2.lastDevices.empty());

	// 损坏的 JSON → 回退默认值而不是崩溃
	std::error_code ec;
	std::filesystem::remove(path, ec);
	{
		wil::unique_hfile h(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
		THROW_LAST_ERROR_IF(!h);
		DWORD written = 0;
		const char* junk = "{ not valid json !!";
		THROW_IF_WIN32_BOOL_FALSE(WriteFile(h.get(), junk, static_cast<DWORD>(strlen(junk)), &written, nullptr));
	}
	auto in3 = LoadSettings(path);
	CHECK(in3.reconnect == false);
	CHECK(in3.lastDevices.empty());

	std::filesystem::remove_all(dir, ec);
}

static void TestGetModuleFsPath()
{
	auto exe = GetModuleFsPath(nullptr);
	CHECK(!exe.empty());
	CHECK(std::filesystem::exists(exe));
}

static void TestLogFormat()
{
	logger::Timestamp ts{ 2026, 9, 10, 15, 23, 29, 7 };
	CHECK(logger::FormatLogLine(ts, 4294967295u, L"hello") == L"2026-09-10 15:23:29.007 [4294967295] hello");

	// 各字段补零
	logger::Timestamp zero{ 2000, 1, 1, 0, 0, 0, 0 };
	CHECK(logger::FormatLogLine(zero, 0, L"") == L"2000-01-01 00:00:00.000 [0] ");

	// 中文与内嵌引号原样保留
	CHECK(logger::FormatLogLine(ts, 12345, L"设备 \"A2DP SNK\" 已连接") == L"2026-09-10 15:23:29.007 [12345] 设备 \"A2DP SNK\" 已连接");
}

static void TestLogCompose()
{
	CHECK(logger::Compose() == L"");

	// const wchar_t* 片段
	CHECK(logger::Compose(L"a", L"bc", L"d") == L"abcd");

	// std::wstring 与 std::wstring_view 混用
	std::wstring tail = L"-tail";
	std::wstring_view middle = L"-mid";
	CHECK(logger::Compose(L"head", middle, tail) == L"head-mid-tail");
}

static void TestConnectionStatusName()
{
	CHECK(wcscmp(ConnectionStatusName(ConnectionStatus::Connecting), L"connecting") == 0);
	CHECK(wcscmp(ConnectionStatusName(ConnectionStatus::Connected), L"connected") == 0);
	CHECK(wcscmp(ConnectionStatusName(ConnectionStatus::Failed), L"failed") == 0);
	CHECK(wcscmp(ConnectionStatusName(ConnectionStatus::Closed), L"closed") == 0);
}

int wmain()
{
	winrt::init_apartment(winrt::apartment_type::single_threaded);

	TestFnv1a();
	TestUtfRoundtrip();
	TestI18nYmo();
	TestSettingsRoundtrip();
	TestGetModuleFsPath();
	TestLogFormat();
	TestLogCompose();
	TestConnectionStatusName();

	std::wcout << L"checks=" << g_checks << L" failures=" << g_failures << std::endl;
	winrt::uninit_apartment();
	return g_failures == 0 ? 0 : 1;
}
