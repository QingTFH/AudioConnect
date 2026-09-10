#pragma once

#include "resource.h"

using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media::Audio;

// 托盘回调消息 / 自定义消息
constexpr UINT WM_NOTIFYICON = WM_APP + 1;
constexpr UINT WM_CONNECTDEVICE = WM_APP + 2;
