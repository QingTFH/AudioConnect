#include "pch.h"

#include "ExitConfirm.h"

#include "I18n.h"

bool ConfirmExit(HWND owner, bool& reconnect)
{
	constexpr int kButtonExit = 100;
	constexpr int kButtonCancel = 101;

	const TASKDIALOG_BUTTON buttons[] = {
		{ kButtonExit, _(L"Exit") },
		{ kButtonCancel, _(L"Cancel") },
	};

	TASKDIALOGCONFIG config = {};
	config.cbSize = sizeof(config);
	config.hwndParent = owner;
	config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
	config.pszWindowTitle = _(L"AudioPlaybackConnector");
	config.pszMainIcon = TD_WARNING_ICON;
	config.pszMainInstruction = _(L"All connections will be closed.");
	config.pszContent = _(L"Exit anyway?");
	config.cButtons = ARRAYSIZE(buttons);
	config.pButtons = buttons;
	config.nDefaultButton = kButtonCancel;
	config.pszVerificationText = _(L"Reconnect on next start");
	if (reconnect)
		config.dwFlags |= TDF_VERIFICATION_FLAG_CHECKED;

	BOOL checked = FALSE;
	int pressed = 0;
	const HRESULT hr = TaskDialogIndirect(&config, &pressed, nullptr, &checked);
	if (FAILED(hr))
	{
		LOG_HR(hr);
		return false;
	}

	if (pressed != kButtonExit)
		return false;

	reconnect = checked != FALSE;
	return true;
}
