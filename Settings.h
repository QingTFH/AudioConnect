#pragma once

// 持久化配置。原先 SettingsUtil.hpp 直接读写全局变量 g_reconnect /
// g_audioPlaybackConnections，无法单测也无法换存储位置。这里改成纯数据
// 结构体 + 显式路径的读写函数，调用方决定存在哪。

#include <filesystem>
#include <string>
#include <vector>

struct SettingsData
{
	bool reconnect = false;
	std::vector<std::wstring> lastDevices;
};

// 默认路径：exe 同目录下的 AudioPlaybackConnector.json
std::filesystem::path GetSettingsPath(HINSTANCE hInst);

// 读失败（文件不存在/JSON 损坏）时记录日志并返回默认值。
SettingsData LoadSettings(const std::filesystem::path& path);
void SaveSettings(const std::filesystem::path& path, const SettingsData& data);
