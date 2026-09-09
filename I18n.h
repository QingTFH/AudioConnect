#pragma once

// 极简 i18n：编译期把源码里的英文原文算 FNV-1a 哈希，运行时在自定义
// YMO 资源里查表替换。资源由 translate/po2ymo.py 从 .po 生成。

// 从 hInst 的 YMO 资源加载当前线程 UI 语言对应的翻译表。
// 找不到对应语言资源时静默返回（界面保持英文原文）。
void LoadTranslateData(HINSTANCE hInst);

const wchar_t* Translate(const wchar_t* str);
const wchar_t* TranslateContext(const wchar_t* str, const wchar_t* ctxtStr);

#define _(str) Translate(str)
#define C_(ctxt, str) TranslateContext(str, ctxt L"\004" str)

// YMO 二进制布局（小端），与 po2ymo.py 的输出保持一致：
//   uint16 条目数
//   { uint32 fnv1a32 哈希, uint16 偏移 } * 条目数     偏移相对资源数据起始处
//   payload: 每条 UTF-16LE 字符串，以 2 字节 NUL 结尾
#pragma pack(push, 1)
struct YMOData
{
	uint16_t len;
	struct
	{
		uint32_t hash;
		uint16_t offset;
	} table[1];
};
#pragma pack(pop)
