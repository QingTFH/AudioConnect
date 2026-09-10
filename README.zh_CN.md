# AudioPlaybackConnector
[English](README.md) | **简体中文**

Windows 10 2004+ 蓝牙音频接收 (A2DP Sink) 连接工具。

微软在 Windows 10 2004 加入了蓝牙 A2DP Sink 支持。但是需要第三方软件来管理连接。\
已经有了一个可以实现此功能的 app。但是它不可以隐藏到通知区域，而且不开源。\
本项目提供一个简单的，现代且开源的替代品。

> **本仓库是 [ysc3839/AudioPlaybackConnector](https://github.com/ysc3839/AudioPlaybackConnector) 的维护分支**（上游 v1.3.1，MIT 许可）。
> 界面保持上游原样，改动仅限**修 bug**、**构建 / CI 现代化**与**架构解耦** —— 详见[本分支改动](#本分支改动)。

# 预览
![预览](AudioPlaybackConnector.gif)

# 使用方法
* 从 [releases](https://github.com/QingTFH/AudioConnect/releases) 下载并运行 AudioPlaybackConnector。若暂无 Release，可到 [Actions](https://github.com/QingTFH/AudioConnect/actions) 取最新一次成功构建的 artifact。
* 在系统蓝牙设置中添加蓝牙设备。你可以右键点击通知区域的 AudioPlaybackConnector 图标然后选择“蓝牙设置”。
* 点击 AudioPlaybackConnector 图标然后选择想要连接的设备。
* 尽情享受吧！

# 本分支改动

上游原有的功能一律保持原样：设备选择器、托盘菜单、退出确认、开机重连、多语言（英 / 简中 / 繁中）、主题跟随的托盘图标。

**修正的上游 bug**
* `fnv1a_32` 缺少 `inline` —— 代码拆成多个编译单元后必然 LNK2005。
* `LoadTranslateData()` 从未被调用 —— **翻译此前从未生效**，界面一直是英文原文。
* YMO 字符串偏移的基准指针用错（`HGLOBAL` 应为 `LockResource` 指针）。

**新增**
* **单实例保护** —— 第二次启动改为唤起已有实例的设备选择器并以正常退出码结束，而不是再起一个托盘图标。

**工程化**
* 上游 451 行的单文件拆成模块 —— `Util`、`I18n`、`Settings`、`SvgIcon`、`TrayIcon`、`ConnectionManager`、`Singleton` 加一个瘦身后的主文件。模块不读全局变量，依赖显式传参。
* GitHub Actions 每次推送构建 x86 / x64 / ARM64。工具链更新到 C++/WinRT 2.0.250303.1、WIL 1.0.260126.7 与 `stdcpp20`。
* 零依赖测试工程（`tests/`）覆盖 FNV-1a、UTF-8 / UTF-16 往返、YMO 解析与配置序列化，并在 CI 中运行。

# 构建

* Visual Studio 2022 或更新版本，需 C++ 桌面开发工作负载（工具集 `v143` 或更新）。
* Python 3 —— `translate/gen_rc.sh` 用它把 `.po` 文件编译成 `.ymo` 资源。

用 Visual Studio 打开 `AudioPlaybackConnector.sln`，或在开发者命令提示符中执行：

```
nuget restore AudioPlaybackConnector.sln
msbuild AudioPlaybackConnector.sln -p:Configuration=Release -p:Platform=x64
```

`.github/workflows/build.yaml` 会构建三个平台并运行单元测试。
