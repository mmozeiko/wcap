#pragma once

#include "wcap.h"

typedef struct {
	BOOL ShowNotifications;
	BOOL OpenVideoFolder;
	BOOL MouseCursor;
	BOOL OnlyClientArea;
	BOOL FragmentedOutput;
	BOOL HardwareEncoder;
	DWORD MaxVideoWidth;
	DWORD MaxVideoHeight;
	DWORD MaxVideoFramerate;
	DWORD VideoBitrate;
	WCHAR OutputFolder[MAX_PATH];
} Config;

void Config_Load(Config* Config, LPCWSTR FileName);
void Config_Save(Config* Config, LPCWSTR FileName);
BOOL Config_ShowDialog(Config* Config, HWND Window);
