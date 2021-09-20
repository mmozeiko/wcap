#include "wcap.h"
#include "wcap_config.h"
#include "wcap_capture.h"
#include "wcap_encoder.h"

#include <d3d11.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>

#pragma comment (lib, "kernel32.lib")
#pragma comment (lib, "user32.lib")
#pragma comment (lib, "d3d11.lib")
#pragma comment (lib, "dwmapi.lib")
#pragma comment (lib, "shell32.lib")
#pragma comment (lib, "shlwapi.lib")
#pragma comment (lib, "mfplat.lib")
#pragma comment (lib, "mfuuid.lib")
#pragma comment (lib, "mfreadwrite.lib")
#pragma comment (lib, "evr.lib")
#pragma comment (lib, "strmiids.lib")
#pragma comment (lib, "ole32.lib")

#define WM_WCAP_ALREADY_RUNNING (WM_USER+1)
#define WM_WCAP_STOP_CAPTURE    (WM_USER+2)
#define WM_WCAP_TRAY_TITLE      (WM_USER+3)
#define WM_WCAP_COMMAND         (WM_USER+4)

#define CMD_WCAP     1
#define CMD_QUIT     2
#define CMD_SETTINGS 3

#define HOT_RECORD_WINDOW  1
#define HOT_RECORD_MONITOR 2

// constants
static WCHAR gConfigPath[MAX_PATH];
static LARGE_INTEGER gTickFreq;
static HICON gIcon1;
static HICON gIcon2;
static UINT WM_TASKBARCREATED;

// recording state
static BOOL gRecording;
static DWORD gRecordingLimitFramerate;
static DWORD gRecordingDroppedFrames;
static UINT64 gRecordingNextEncode;
static UINT64 gRecordingNextTooltip;
static EXECUTION_STATE gRecordingState;
static WCHAR gRecordingPath[MAX_PATH];

// globals
static HWND gWindow;
static Config gConfig;
static Capture gCapture;
static Encoder gEncoder;

static void ShowNotification(LPCWSTR Message, LPCWSTR Title, DWORD Flags)
{
	NOTIFYICONDATAW Data =
	{
		.cbSize = sizeof(Data),
		.hWnd = gWindow,
		.uFlags = NIF_INFO | NIF_TIP,
		.dwInfoFlags = Flags, // NIIF_INFO, NIIF_WARNING, NIIF_ERROR
	};
	StrCpyNW(Data.szTip, WCAP_TITLE, _countof(Data.szTip));
	StrCpyNW(Data.szInfo, Message, _countof(Data.szInfo));
	StrCpyNW(Data.szInfoTitle, Title ? Title : WCAP_TITLE, _countof(Data.szInfoTitle));
	Shell_NotifyIconW(NIM_MODIFY, &Data);
}

static void UpdateTrayTitle(LPCWSTR Title)
{
	NOTIFYICONDATAW Data =
	{
		.cbSize = sizeof(Data),
		.hWnd = gWindow,
		.uFlags = NIF_TIP,
	};
	StrCpyNW(Data.szTip, Title, _countof(Data.szTip));
	Shell_NotifyIconW(NIM_MODIFY, &Data);
}

static void UpdateTrayIcon(HICON Icon)
{
	NOTIFYICONDATAW Data =
	{
		.cbSize = sizeof(Data),
		.hWnd = gWindow,
		.uFlags = NIF_ICON,
		.hIcon = Icon,
	};
	Shell_NotifyIconW(NIM_MODIFY, &Data);
}

static void AddTrayIcon(HWND Window)
{
	NOTIFYICONDATAW Data =
	{
		.cbSize = sizeof(Data),
		.hWnd = Window,
		.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP,
		.uCallbackMessage = WM_WCAP_COMMAND,
		.hIcon = gIcon1,
	};
	StrCpyNW(Data.szTip, WCAP_TITLE, _countof(Data.szTip));
	Shell_NotifyIconW(NIM_ADD, &Data);
}

static void RemoveTrayIcon(HWND Window)
{
	NOTIFYICONDATAW Data =
	{
		.cbSize = sizeof(Data),
		.hWnd = Window,
	};
	Shell_NotifyIconW(NIM_DELETE, &Data);
}

static void ShowFileInFolder(LPCWSTR Filename)
{
	SFGAOF Flags;
	PIDLIST_ABSOLUTE List;
	if (Filename[0] && SUCCEEDED(SHParseDisplayName(Filename, NULL, &List, 0, &Flags)))
	{
		HR(SHOpenFolderAndSelectItems(List, 0, NULL, 0));
		CoTaskMemFree((LPVOID)List);
	}
}

static void StartRecording(LPCWSTR Caption)
{
	SYSTEMTIME Time;
	GetLocalTime(&Time);

	WCHAR Filename[256];
	wsprintfW(Filename, L"%04u%02u%02u_%02u%02u%02u.mp4", Time.wYear, Time.wMonth, Time.wDay, Time.wHour, Time.wMinute, Time.wSecond);

	StrCpyW(gRecordingPath, gConfig.OutputFolder);
	PathAppendW(gRecordingPath, Filename);

	DWM_TIMING_INFO Info = { .cbSize = sizeof(Info) };
	HR(DwmGetCompositionTimingInfo(NULL, &Info));

	DWORD FramerateNum = Info.rateCompose.uiNumerator;
	DWORD FramerateDen = Info.rateCompose.uiDenominator;
	if (gConfig.MaxVideoFramerate > 0 && gConfig.MaxVideoFramerate * FramerateDen < FramerateNum)
	{
		// limit rate only if max framerate is specified and it is lower than compositor framerate
		gRecordingLimitFramerate = gConfig.MaxVideoFramerate;
		FramerateNum = gConfig.MaxVideoFramerate;
		FramerateDen = 1;
	}
	else
	{
		gRecordingLimitFramerate = 0;
	}

	EncoderConfig EncConfig =
	{
		.FragmentedOutput = gConfig.FragmentedOutput,
		.HardwareEncoder = gConfig.HardwareEncoder,
		.Width = gCapture.CurrentSize.cx,
		.Height = gCapture.CurrentSize.cy,
		.MaxWidth = gConfig.MaxVideoWidth,
		.MaxHeight = gConfig.MaxVideoHeight,
		.FramerateNum = FramerateNum,
		.FramerateDen = FramerateDen,
		.VideoBitrate = gConfig.VideoBitrate,
	};
	if (!Encoder_Start(&gEncoder, gRecordingPath, &EncConfig))
	{
		Capture_Stop(&gCapture);
		return;
	}

	gRecordingNextTooltip = 0;
	gRecordingNextEncode = 0;
	gRecordingDroppedFrames = 0;
	Capture_Start(&gCapture, gConfig.MouseCursor);

	if (gConfig.ShowNotifications)
	{
		ShowNotification(Caption, L"Recording Started", NIIF_INFO);
	}
	UpdateTrayIcon(gIcon2);
	gRecordingState = SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED);
	gRecording = TRUE;
}

static void StopRecording(void)
{
	gRecording = FALSE;
	SetThreadExecutionState(gRecordingState);

	Capture_Stop(&gCapture);
	Encoder_Stop(&gEncoder);
	if (gConfig.OpenVideoFolder)
	{
		ShowFileInFolder(gRecordingPath);
	}

	UpdateTrayIcon(gIcon1);
	if (gConfig.ShowNotifications)
	{
		ShowNotification(PathFindFileNameW(gRecordingPath), L"Recording Finished", NIIF_INFO);
	}
	else
	{
		UpdateTrayTitle(WCAP_TITLE);
	}
}

static void CaptureWindow(void)
{
	HWND Window = GetForegroundWindow();
	if (Window == NULL)
	{
		ShowNotification(L"No window is selected!", L"Cannot Start Recording", NIIF_WARNING);
		return;
	}

	DWORD Affinity;
	BOOL Success = GetWindowDisplayAffinity(Window, &Affinity);
	Assert(Success);

	if (Affinity != WDA_NONE)
	{
		ShowNotification(L"Window is excluded from capture!", L"Cannot Start Recording", NIIF_WARNING);
		return;
	}

	if (!Capture_CreateWindow(&gCapture, Window, gConfig.OnlyClientArea))
	{
		ShowNotification(L"Cannot record selected window!", L"Error", NIIF_WARNING);
		return;
	}

	WCHAR WindowTitle[1024];
	GetWindowTextW(Window, WindowTitle, _countof(WindowTitle));

	StartRecording(WindowTitle);
}

static void CaptureMonitor(void)
{
	POINT Mouse;
	GetCursorPos(&Mouse);

	HMONITOR Monitor = MonitorFromPoint(Mouse, MONITOR_DEFAULTTONULL);
	if (Monitor == NULL)
	{
		ShowNotification(L"Unknown monitor!", L"Cannot Start Recording", NIIF_WARNING);
		return;
	}

	if (!Capture_CreateMonitor(&gCapture, Monitor))
	{
		ShowNotification(L"Cannot record selected monitor!", L"Error", NIIF_WARNING);
		return;
	}

	MONITORINFOEXW Info = { .cbSize = sizeof(Info) };
	GetMonitorInfoW(Monitor, (LPMONITORINFO)&Info);

	WCHAR MonitorName[128];
	MonitorName[0] = 0;

	DISPLAYCONFIG_PATH_INFO DisplayPaths[32];
	DISPLAYCONFIG_MODE_INFO DisplayModes[32];
	UINT32 PathCount = _countof(DisplayPaths);
	UINT32 ModeCount = _countof(DisplayModes);

	// try to get actual monitor name
	if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &PathCount, DisplayPaths, &ModeCount, DisplayModes, NULL) == ERROR_SUCCESS)
	{
		for (UINT32 i = 0; i < PathCount; i++)
		{
			const DISPLAYCONFIG_PATH_INFO* Path = &DisplayPaths[i];

			DISPLAYCONFIG_SOURCE_DEVICE_NAME SourceName =
			{
				.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,
				.header.size = sizeof(SourceName),
				.header.adapterId = Path->sourceInfo.adapterId,
				.header.id = Path->sourceInfo.id,
			};

			if (DisplayConfigGetDeviceInfo(&SourceName.header) == ERROR_SUCCESS && StrCmpW(Info.szDevice, SourceName.viewGdiDeviceName) == 0)
			{
				DISPLAYCONFIG_TARGET_DEVICE_NAME TargetName =
				{
					.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME,
					.header.size = sizeof(TargetName),
					.header.adapterId = Path->sourceInfo.adapterId,
					.header.id = Path->targetInfo.id,
				};

				if (DisplayConfigGetDeviceInfo(&TargetName.header) == ERROR_SUCCESS)
				{
					wsprintfW(MonitorName, L"%s - %s", Info.szDevice, TargetName.monitorFriendlyDeviceName);
					break;
				}
			}
		}
	}

	// if cannot get monitor name, just use device name
	StartRecording(MonitorName[0] ? MonitorName : Info.szDevice);
}

static LRESULT CALLBACK WindowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
	if (Message == WM_CREATE)
	{
		BOOL Success1 = RegisterHotKey(Window, HOT_RECORD_WINDOW, MOD_CONTROL | MOD_WIN, VK_SNAPSHOT);
		BOOL Success2 = RegisterHotKey(Window, HOT_RECORD_MONITOR, MOD_CONTROL, VK_SNAPSHOT);
		if (!Success1 || !Success2)
		{
			MessageBoxW(NULL, L"Cannot register wcap keyboard shortcuts!", WCAP_TITLE, MB_ICONERROR);
			return -1;
		}
		AddTrayIcon(Window);
		return 0;
	}
	else if (Message == WM_DESTROY)
	{
		if (gRecording)
		{
			StopRecording();
		}
		RemoveTrayIcon(Window);
		PostQuitMessage(0);
		return 0;
	}
	else if (Message == WM_POWERBROADCAST)
	{
		if (WParam == PBT_APMQUERYSUSPEND)
		{
			if (gRecording)
			{
				if (LParam & 1)
				{
					// reject request to suspend when recording
					return BROADCAST_QUERY_DENY;
				}
				else
				{
					// if cannot prevent suspend, need to stop recording
					StopRecording();
				}
			}
			else
			{
				// allow to suspend when not recording
			}
		}
		return TRUE;
	}
	else if (Message == WM_WCAP_COMMAND)
	{
		if (LOWORD(LParam) == WM_RBUTTONUP)
		{
			HMENU Menu = CreatePopupMenu();
			Assert(Menu);

			AppendMenuW(Menu, MF_STRING, CMD_WCAP, WCAP_TITLE);
			AppendMenuW(Menu, MF_SEPARATOR, 0, NULL);
			AppendMenuW(Menu, MF_STRING | (gRecording ? MF_DISABLED : 0), CMD_SETTINGS, L"Settings");
			AppendMenuW(Menu, MF_STRING, CMD_QUIT, L"Exit");

			POINT Mouse;
			GetCursorPos(&Mouse);

			SetForegroundWindow(Window);
			int Command = TrackPopupMenu(Menu, TPM_RETURNCMD | TPM_NONOTIFY, Mouse.x, Mouse.y, 0, Window, NULL);
			if (Command == CMD_WCAP)
			{
				ShellExecuteW(NULL, L"open", WCAP_URL, NULL, NULL, SW_SHOWNORMAL);
			}
			else if (Command == CMD_QUIT)
			{
				DestroyWindow(Window);
			}
			else if (Command == CMD_SETTINGS)
			{
				if (Config_ShowDialog(&gConfig, Window))
				{
					Config_Save(&gConfig, gConfigPath);
				}
			}

			DestroyMenu(Menu);
		}
		else if (LOWORD(LParam) == WM_LBUTTONDBLCLK)
		{
			if (!gRecording)
			{
				if (Config_ShowDialog(&gConfig, Window))
				{
					Config_Save(&gConfig, gConfigPath);
				}
			}
		}
		else if (LOWORD(LParam) == NIN_BALLOONUSERCLICK)
		{
			// TODO: no idea how to prevent this happening for right-click on tray icon...
			ShowFileInFolder(gRecordingPath);
		}
		return 0;
	}
	else if (Message == WM_HOTKEY)
	{
		if (gRecording)
		{
			StopRecording();
		}
		else
		{
			if (WParam == HOT_RECORD_WINDOW)
			{
				CaptureWindow();
			}
			else if (WParam == HOT_RECORD_MONITOR)
			{
				CaptureMonitor();
			}
		}
		return 0;
	}
	else if (Message == WM_WCAP_TRAY_TITLE)
	{
		if (gRecording)
		{
			UINT64 FileSize;
			DWORD Bitrate, LengthMsec;
			Encoder_GetStats(&gEncoder, &Bitrate, &LengthMsec, &FileSize);

			// wsprintfW cannot do floats, so we multiply by 100 to have 2 digits after decimal point
			DWORD Framerate = MUL_DIV_ROUND_UP(100, gEncoder.FramerateNum, gEncoder.FramerateDen);

			WCHAR LengthText[128];
			StrFromTimeIntervalW(LengthText, _countof(LengthText), LengthMsec, 6);

			WCHAR SizeText[128];
			StrFormatByteSizeW(FileSize, SizeText, _countof(SizeText));

			WCHAR Text[1024];
			wsprintfW(Text, L"Recording: %dx%d @ %d.%02d\nLength: %s\nBitrate: %u kbit/s\nSize: %s\nFramedrop: %u",
				gEncoder.OutputWidth, gEncoder.OutputHeight, Framerate / 100, Framerate % 100,
				LengthText,
				Bitrate,
				SizeText,
				gRecordingDroppedFrames);

			UpdateTrayTitle(Text);
		}
		return 0;
	}
	else if (Message == WM_WCAP_STOP_CAPTURE)
	{
		if (gRecording)
		{
			StopRecording();
		}
		return 0;
	}
	else if (Message == WM_WCAP_ALREADY_RUNNING)
	{
		ShowNotification(L"wcap is already running!", NULL, NIIF_INFO);
		return 0;
	}
	else if (Message == WM_TASKBARCREATED)
	{
		// in case taskbar was re-created (explorer.exe crashed) add our icon back
		AddTrayIcon(Window);
		return 0;
	}

	return DefWindowProcW(Window, Message, WParam, LParam);
}

static void OnCaptureClose(void)
{
	PostMessageW(gWindow, WM_WCAP_STOP_CAPTURE, 0, 0);
}

static void OnCaptureFrame(ID3D11Texture2D* Texture, RECT Rect, UINT64 Time)
{
	BOOL DoEncode = TRUE;
	DWORD LimitFramerate = gRecordingLimitFramerate;
	if (LimitFramerate != 0)
	{
		if (Time * LimitFramerate < gRecordingNextEncode)
		{
			DoEncode = FALSE;
		}
		else
		{
			if (gRecordingNextEncode == 0)
			{
				gRecordingNextEncode = Time * LimitFramerate;
			}
			gRecordingNextEncode += gTickFreq.QuadPart;
		}
	}

	if (DoEncode)
	{
		if (!Encoder_NewFrame(&gEncoder, Texture, Rect, Time, gTickFreq.QuadPart))
		{
			// TODO: maybe highlight tray icon when droppped frames are increasing too much?
			gRecordingDroppedFrames++;
		}
	}

	// update tray title with stats once every second
	if (gRecordingNextTooltip == 0)
	{
		gRecordingNextTooltip = Time + gTickFreq.QuadPart;
	}
	else if (Time >= gRecordingNextTooltip)
	{
		gRecordingNextTooltip += gTickFreq.QuadPart;

		// do the update, but not from frame callback to minimize time when texture is used
		PostMessageW(gWindow, WM_WCAP_TRAY_TITLE, 0, 0);
	}
}

void WinMainCRTStartup()
{
	WNDCLASSEXW WindowClass =
	{
		.cbSize = sizeof(WindowClass),
		.lpfnWndProc = WindowProc,
		.hInstance = GetModuleHandleW(NULL),
		.lpszClassName = L"wcap_window_class",
	};

	HWND Existing = FindWindowW(WindowClass.lpszClassName, NULL);
	if (Existing)
	{
		PostMessageW(Existing, WM_WCAP_ALREADY_RUNNING, 0, 0);
		ExitProcess(0);
	}

	if (!Capture_IsSupported())
	{
		MessageBoxW(NULL, L"Windows 10 Version 1903, May 2019 Update (19H1) or newer is required!", WCAP_TITLE, MB_ICONEXCLAMATION);
		ExitProcess(0);
	}

	ID3D11Device* Device;
	ID3D11DeviceContext* Context;

	UINT flags = 0;
#ifdef _DEBUG
	flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
	if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags, NULL, 0, D3D11_SDK_VERSION, &Device, NULL, &Context)))
	{
		MessageBoxW(NULL, L"Failed to create D3D11 device!", WCAP_TITLE, MB_ICONEXCLAMATION);
		ExitProcess(0);
	}

	GetModuleFileNameW(NULL, gConfigPath, _countof(gConfigPath));
	PathRenameExtensionW(gConfigPath, L".ini");

	Config_Load(&gConfig, gConfigPath);
	Capture_Init(&gCapture, Device, &OnCaptureClose, &OnCaptureFrame);
	Encoder_Init(&gEncoder, Device, Context);

	QueryPerformanceFrequency(&gTickFreq);

	gIcon1 = LoadIconW(WindowClass.hInstance, MAKEINTRESOURCEW(1));
	gIcon2 = LoadIconW(WindowClass.hInstance, MAKEINTRESOURCEW(2));
	Assert(gIcon1 && gIcon2);

	WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");
	Assert(WM_TASKBARCREATED);

	ATOM Atom = RegisterClassExW(&WindowClass);
	Assert(Atom);

	gWindow = CreateWindowExW(0, WindowClass.lpszClassName, WCAP_TITLE,
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
		CW_USEDEFAULT, NULL, NULL, WindowClass.hInstance, NULL);
	if (!gWindow)
	{
		ExitProcess(0);
	}

	for (;;)
	{
		MSG Message;
		BOOL Result = GetMessageW(&Message, NULL, 0, 0);
		if (Result == 0)
		{
			ExitProcess(0);
		}
		Assert(Result > 0);

		TranslateMessage(&Message);
		DispatchMessageW(&Message);
	}
 }
