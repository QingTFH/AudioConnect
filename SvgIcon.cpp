#include "pch.h"

#include "SvgIcon.h"

#include "Direct2DSvg.hpp"

TrayIcons LoadTrayIcons(HINSTANCE hInst, int width, int height)
{
	auto hRes = FindResourceW(hInst, MAKEINTRESOURCEW(1), L"SVG");
	FAIL_FAST_LAST_ERROR_IF_NULL(hRes);

	auto size = SizeofResource(hInst, hRes);
	FAIL_FAST_IF(size == 0);

	auto hResData = LoadResource(hInst, hRes);
	FAIL_FAST_LAST_ERROR_IF_NULL(hResData);

	auto svgData = reinterpret_cast<const char*>(LockResource(hResData));
	FAIL_FAST_IF_NULL_ALLOC(svgData);

	const std::string_view svg(svgData, size);

	TrayIcons icons;
	icons.light = SvgTohIcon(svg, width, height, { 0, 0, 0, 1 });
	icons.dark = SvgTohIcon(svg, width, height, { 1, 1, 1, 1 });
	return icons;
}
