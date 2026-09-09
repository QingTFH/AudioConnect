#include "pch.h"

#include "I18n.h"

#include "FnvHash.hpp"

#include <cwchar>
#include <unordered_map>

namespace
{
	std::unordered_map<uint32_t, const wchar_t*> g_hashToStrMap;
}

void LoadTranslateData(HINSTANCE hInst)
{
	auto hRes = FindResourceExW(hInst, L"YMO", MAKEINTRESOURCEW(1), GetThreadUILanguage());
	if (!hRes)
		return;

	auto hResData = LoadResource(hInst, hRes);
	if (!hResData)
		return;

	// 偏移量是相对资源数据起始处的，必须用 LockResource 返回的指针做基准。
	auto data = static_cast<const uint8_t*>(LockResource(hResData));
	if (!data)
		return;

	auto ymo = reinterpret_cast<const YMOData*>(data);
	g_hashToStrMap.reserve(ymo->len);

	for (int i = 0; i < ymo->len; ++i)
	{
		auto str = reinterpret_cast<const wchar_t*>(data + ymo->table[i].offset);
		g_hashToStrMap.emplace(ymo->table[i].hash, str);
	}
}

const wchar_t* Translate(const wchar_t* str)
{
	static std::unordered_map<const wchar_t*, const wchar_t*> ptrToStrMap;

	auto i = ptrToStrMap.find(str);
	if (i != ptrToStrMap.end())
		return i->second;

	auto translation = str;

	auto hash = fnv1a_32(str, wcslen(str) * sizeof(wchar_t));
	auto j = g_hashToStrMap.find(hash);
	if (j != g_hashToStrMap.end())
		translation = j->second;

	ptrToStrMap.emplace(str, translation);

	return translation;
}

const wchar_t* TranslateContext(const wchar_t* str, const wchar_t* ctxtStr)
{
	auto translation = Translate(ctxtStr);
	if (translation == ctxtStr)
		return str;
	return translation;
}
