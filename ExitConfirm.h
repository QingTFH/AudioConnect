#pragma once

// 退出确认：用 TaskDialogIndirect 替代上游的 XAML Flyout。
// 除了二次确认，还承载"下次启动时重新连接"这个勾选项。

// 返回 true 表示用户确认退出，并通过 reconnect 传出勾选状态（入参为初始勾选态）。
bool ConfirmExit(HWND owner, bool& reconnect);
