# AudioPlaybackConnector
**English** | [简体中文](README.zh_CN.md)

Bluetooth audio playback (A2DP Sink) connector for Windows 10 2004+.

Microsoft added Bluetooth A2DP Sink to Windows 10 2004. However, a third-party app is required to manage connection.\
There is already an app can do this job. However it can't hide to notification area and it's not open-source.\
This project provides a simple, modern and open-source alternative.

> **This repository is a maintenance fork** of [ysc3839/AudioPlaybackConnector](https://github.com/ysc3839/AudioPlaybackConnector) (upstream v1.3.1, MIT licensed).
> The original UI is kept unchanged. Changes are limited to **bug fixes**, **build / CI modernization** and **architecture decoupling** — see [Changes in this fork](#changes-in-this-fork).

# Preview
![Preview](AudioPlaybackConnector.gif)

# Usage
* Download and run AudioPlaybackConnector from [releases](https://github.com/QingTFH/AudioConnect/releases). If no release is available yet, take the artifacts of the latest green run from [Actions](https://github.com/QingTFH/AudioConnect/actions).
* Add a bluetooth device in system bluetooth settings. You can right click AudioPlaybackConnector icon in notification area and select "Bluetooth Settings".
* Click AudioPlaybackConnector icon and select the device you want to connect.
* Enjoy!

# Changes in this fork

Everything the upstream version does is kept as-is: the device picker, the tray menu, the exit confirmation, reconnect-on-start, multi-language support (English / Simplified Chinese / Traditional Chinese) and the theme-aware tray icon.

**Upstream bugs fixed**
* `fnv1a_32` was missing `inline`, which breaks the link (LNK2005) as soon as the code is split into multiple translation units.
* `LoadTranslateData()` was never called — **translations had never taken effect**, the UI always showed the original English strings.
* YMO string offsets were computed from the wrong base pointer (`HGLOBAL` instead of the `LockResource` pointer).

**Added**
* **Single instance protection** — launching a second copy wakes the device picker of the running instance and exits with a success code, instead of starting another tray icon.

**Engineering**
* The upstream 451-line single file is split into modules — `Util`, `I18n`, `Settings`, `SvgIcon`, `TrayIcon`, `ConnectionManager`, `Singleton` plus a slim main file. Modules take their dependencies as explicit parameters instead of reading globals.
* GitHub Actions builds x86 / x64 / ARM64 on every push. Toolchain updated to C++/WinRT 2.0.250303.1, WIL 1.0.260126.7 and `stdcpp20`.
* A zero-dependency test project (`tests/`) covers FNV-1a, UTF-8 / UTF-16 round-trips, YMO parsing and settings serialization, and runs in CI.

# Build

* Visual Studio 2022 or newer, with the C++ desktop workload (toolset `v143` or later).
* Python 3 — used by `translate/gen_rc.sh` to compile the `.po` files into `.ymo` resources.

Open `AudioPlaybackConnector.sln`, or from a developer command prompt:

```
nuget restore AudioPlaybackConnector.sln
msbuild AudioPlaybackConnector.sln -p:Configuration=Release -p:Platform=x64
```

`.github/workflows/build.yaml` builds all three platforms and runs the unit tests.
