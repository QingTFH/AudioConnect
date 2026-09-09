#pragma once

// 托盘图标：同一份 SVG 资源渲成黑白两套，跟随系统主题切换。
// SVG 渲染实现在 Direct2DSvg.hpp（Direct2D ID2D1SvgDocument）。

struct TrayIcons
{
	HICON light = nullptr;
	HICON dark = nullptr;
};

// 从 hInst 的 "SVG" 类型资源（id 1）加载并渲染；失败时 FAIL_FAST。
TrayIcons LoadTrayIcons(HINSTANCE hInst, int width, int height);
