#pragma once

#include "resource.h"

using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media::Audio;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Hosting;

// 主消息窗口的窗口类名（单实例唤醒时用它 FindWindow）。
constexpr wchar_t kMainWindowClassName[] = L"AudioPlaybackConnector";

// 托盘回调消息 / 自定义消息
constexpr UINT WM_NOTIFYICON = WM_APP + 1;
constexpr UINT WM_CONNECTDEVICE = WM_APP + 2;
// 第二个实例发来：把设备列表弹到用户面前。
constexpr UINT WM_SHOWDEVICES = WM_APP + 3;
// 执行器线程 → UI 线程的封送投递（step13c 计划书 §D-1；lParam = new 出的
// std::function<void()>*，接收侧执行并 delete，投递失败发送侧 delete）。
constexpr UINT WM_MARSHAL = WM_APP + 4;
