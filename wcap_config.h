#pragma once

#include "wcap.h"

//
// interface
//

#define CONFIG_VIDEO_H264 0
#define CONFIG_VIDEO_H265 1

#define CONFIG_VIDEO_BASE    0
#define CONFIG_VIDEO_MAIN    1
#define CONFIG_VIDEO_HIGH    2
#define CONFIG_VIDEO_MAIN_10 3

#define CONFIG_AUDIO_AAC  0
#define CONFIG_AUDIO_FLAC 1

typedef struct
{
	// capture
	BOOL MouseCursor;
	BOOL OnlyClientArea;
	BOOL ShowRecordingBorder;
	BOOL KeepRoundedWindowCorners;
	BOOL HardwareEncoder;
	BOOL HardwarePreferIntegrated;
	// output
	WCHAR OutputFolder[MAX_PATH];
	BOOL OpenFolder;
	BOOL FragmentedOutput;
	BOOL EnableLimitLength;
	BOOL EnableLimitSize;
	DWORD LimitLength;
	DWORD LimitSize;
	// video
	BOOL GammaCorrectResize;
	BOOL ImprovedColorConversion;
	DWORD VideoCodec;
	DWORD VideoProfile;
	DWORD VideoMaxWidth;
	DWORD VideoMaxHeight;
	DWORD VideoMaxFramerate;
	DWORD VideoBitrate;
	// audio
	BOOL CaptureAudio;
	BOOL ApplicationLocalAudio;
	DWORD AudioCodec;
	DWORD AudioChannels;
	DWORD AudioSamplerate;
	DWORD AudioBitrate;
	// shortcuts
	DWORD ShortcutMonitor;
	DWORD ShortcutWindow;
	DWORD ShortcutRegion;
}
Config;

#define HOT_KEY(Key, Mod) ((Key) | ((Mod) << 24))
#define HOT_GET_KEY(KeyMod) ((KeyMod) & 0xffffff)
#define HOT_GET_MOD(KeyMod) (((KeyMod) >> 24) & 0xff)

static void Config_Defaults(Config* C);
static void Config_Load(Config* C, LPCWSTR FileName);
static void Config_Save(Config* C, LPCWSTR FileName);
static BOOL Config_ShowDialog(Config* C);

//
// implementation
//

#include "wcap_screen_capture.h"
#include "wcap_audio_capture.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <knownfolders.h>
#include <windowsx.h>

#define INI_SECTION L"wcap"

// control id's
#define ID_OK                      IDOK     // 1
#define ID_CANCEL                  IDCANCEL // 2
#define ID_DEFAULTS                3

#define ID_MOUSE_CURSOR            20
#define ID_ONLY_CLIENT_AREA        30
#define ID_SHOW_RECORDING_BORDER   40
#define ID_ROUNDED_CORNERS         50
#define ID_GPU_ENCODER             60

#define ID_OUTPUT_FOLDER           100
#define ID_OPEN_FOLDER             110
#define ID_FRAGMENTED_MP4          120
#define ID_LIMIT_LENGTH            130
#define ID_LIMIT_SIZE              140

#define ID_VIDEO_GAMMA_RESIZE      200
#define ID_VIDEO_IMPROVED_CONVERT  210
#define ID_VIDEO_CODEC             220
#define ID_VIDEO_PROFILE           230
#define ID_VIDEO_MAX_WIDTH         240
#define ID_VIDEO_MAX_HEIGHT        250
#define ID_VIDEO_MAX_FRAMERATE     260
#define ID_VIDEO_BITRATE           270

#define ID_AUDIO_CAPTURE           300
#define ID_AUDIO_APPLICATION_LOCAL 310
#define ID_AUDIO_CODEC             320
#define ID_AUDIO_CHANNELS          330
#define ID_AUDIO_SAMPLERATE        340
#define ID_AUDIO_BITRATE           350

#define ID_SHORTCUT_MONITOR        400
#define ID_SHORTCUT_WINDOW         410
#define ID_SHORTCUT_REGION         420

// control types
#define ITEM_CHECKBOX (1<<0)
#define ITEM_NUMBER   (1<<1)
#define ITEM_COMBOBOX (1<<2)
#define ITEM_FOLDER   (1<<3)
#define ITEM_BUTTON   (1<<4)
#define ITEM_HOTKEY   (1<<5)

// win32 control styles
#define CONTROL_BUTTON    0x0080
#define CONTROL_EDIT      0x0081
#define CONTROL_STATIC    0x0082
#define CONTROL_LISTBOX   0x0083
#define CONTROL_SCROLLBAR 0x0084
#define CONTROL_COMBOBOX  0x0085

// group box width/height
#define COL00W 120
#define COL01W 154
#define COL10W 144
#define COL11W 130
#define ROW0H 84
#define ROW1H 124
#define ROW2H 56

#define PADDING 4             // padding for dialog and group boxes
#define BUTTON_WIDTH 50       // normal button width
#define BUTTON_SMALL_WIDTH 14 // small button width
#define ITEM_HEIGHT 14        // control item height

// MFT encoder supported AAC bitrates & samplerates
static const DWORD gAudioBitrates[] = { 96, 128, 160, 192, 0 };
static const DWORD gAudioSamplerates[] = { 44100, 48000, 0 };

static const LPCWSTR gVideoCodecs[] = { L"H264", L"H265", NULL};
static const LPCWSTR gVideoProfiles[] = { L"Base", L"Main", L"High", L"Main10", NULL };
static const LPCWSTR gAudioCodecs[] = { L"AAC", L"FLAC", NULL };

static const int gValidVideoProfiles[][4] =
{
	{ CONFIG_VIDEO_BASE, CONFIG_VIDEO_MAIN, CONFIG_VIDEO_HIGH, -1 },
	{ CONFIG_VIDEO_MAIN, CONFIG_VIDEO_MAIN_10, -1 },
};

// currently open dialog window
static HWND gDialogWindow;

// current control to set shortcut
struct
{
	WNDPROC WindowProc;
	Config* Config;
	int Control;
}
static gConfigShortcut;

static void Config__UpdateVideoProfiles(HWND Window, DWORD Codec)
{
	HWND Control = GetDlgItem(Window, ID_VIDEO_PROFILE);
	ComboBox_ResetContent(Control);

	if (Codec == CONFIG_VIDEO_H264)
	{
		ComboBox_AddString(Control, L"Base");
		ComboBox_AddString(Control, L"Main");
		ComboBox_AddString(Control, L"High");

		ComboBox_SetItemData(Control, 0, CONFIG_VIDEO_BASE);
		ComboBox_SetItemData(Control, 1, CONFIG_VIDEO_MAIN);
		ComboBox_SetItemData(Control, 2, CONFIG_VIDEO_HIGH);
		ComboBox_SetCurSel(Control, 2);
	}
	else if (Codec == CONFIG_VIDEO_H265)
	{
		ComboBox_AddString(Control, L"Main (8-bit)");
		ComboBox_AddString(Control, L"Main10 (10-bit)");

		ComboBox_SetItemData(Control, 0, CONFIG_VIDEO_MAIN);
		ComboBox_SetItemData(Control, 1, CONFIG_VIDEO_MAIN_10);
		ComboBox_SetCurSel(Control, 1);
	}
}

static void Config__SelectVideoProfile(HWND Window, DWORD Codec, DWORD Profile)
{
	HWND Control = GetDlgItem(Window, ID_VIDEO_PROFILE);
	DWORD Count = (DWORD)ComboBox_GetCount(Control);
	for (DWORD i=0; i<Count; i++)
	{
		DWORD Item = (DWORD)ComboBox_GetItemData(Control, i);
		if (Item == Profile)
		{
			ComboBox_SetCurSel(Control, i);
			return;
		}
	}
	ComboBox_SetCurSel(Control, Count - 1);
}

static DWORD Config__GetSelectedVideoProfile(HWND Window)
{
	HWND Control = GetDlgItem(Window, ID_VIDEO_PROFILE);
	return (DWORD)ComboBox_GetItemData(Control, ComboBox_GetCurSel(Control));
}

static void Config__UpdateAudioBitrate(HWND Window, DWORD Codec, DWORD AudioBitrate)
{
	HWND Control = GetDlgItem(Window, ID_AUDIO_BITRATE);
	ComboBox_ResetContent(Control);

	if (Codec == CONFIG_AUDIO_AAC)
	{
		for (const DWORD* Bitrate = gAudioBitrates; *Bitrate; Bitrate++)
		{
			WCHAR Text[64];
			StrFormat(Text, L"%u", *Bitrate);
			ComboBox_AddString(Control, Text);
		}

		WCHAR Text[64];
		StrFormat(Text, L"%u", AudioBitrate);
		ComboBox_SelectString(Control, -1, Text);

	}
	else if (Codec == CONFIG_AUDIO_FLAC)
	{
		ComboBox_AddString(Control, L"auto");
		ComboBox_SetCurSel(Control, 0);
	}
}

static void Config__FormatKey(DWORD KeyMod, WCHAR* Text)
{
	if (KeyMod == 0)
	{
		StrCpyW(Text, L"[none]");
		return;
	}

	DWORD Mod = HOT_GET_MOD(KeyMod);

	Text[0] = 0;
	if (Mod & MOD_CONTROL) StrCatW(Text, L"Ctrl + ");
	if (Mod & MOD_WIN)     StrCatW(Text, L"Win + ");
	if (Mod & MOD_ALT)     StrCatW(Text, L"Alt + ");
	if (Mod & MOD_SHIFT)   StrCatW(Text, L"Shift + ");

	struct
	{
		DWORD Key;
		LPWSTR Text;
	}
	static const Overrides[] =
	{
		{ VK_PAUSE,    L"Pause"    },
		{ VK_SNAPSHOT, L"PrtScr"   },
		{ VK_PRIOR,    L"PageUp"   },
		{ VK_NEXT,     L"PageDown" },
		{ VK_END,      L"End"      },
		{ VK_HOME,     L"Home"     },
		{ VK_LEFT,     L"Left"     },
		{ VK_UP,       L"Up"       },
		{ VK_RIGHT,    L"Right"    },
		{ VK_DOWN,     L"Down"     },
		{ VK_INSERT,   L"Insert"   },
		{ VK_DELETE,   L"Delete"   },
	};

	for (size_t i = 0; i < ARRAYSIZE(Overrides); i++)
	{
		if (Overrides[i].Key == HOT_GET_KEY(KeyMod))
		{
			StrCatW(Text, Overrides[i].Text);
			return;
		}
	}

	WCHAR KeyText[32];
	UINT ScanCode = MapVirtualKeyW(HOT_GET_KEY(KeyMod), MAPVK_VK_TO_VSC);
	if (GetKeyNameTextW(ScanCode << 16, KeyText, _countof(KeyText)) == 0)
	{
		StrFormat(KeyText, L"[0x%02x]", HOT_GET_KEY(KeyMod));
	}

	StrCatW(Text, KeyText);
}

static void Config__SetDialogValues(HWND Window, Config* C)
{
	Config__UpdateVideoProfiles(Window, C->VideoCodec);
	Config__UpdateAudioBitrate(Window, C->AudioCodec, C->AudioBitrate);

	// capture
	CheckDlgButton(Window, ID_MOUSE_CURSOR,          C->MouseCursor);
	CheckDlgButton(Window, ID_ONLY_CLIENT_AREA,      C->OnlyClientArea);
	CheckDlgButton(Window, ID_SHOW_RECORDING_BORDER, C->ShowRecordingBorder);
	CheckDlgButton(Window, ID_ROUNDED_CORNERS,       C->KeepRoundedWindowCorners);
	CheckDlgButton(Window, ID_GPU_ENCODER,           C->HardwareEncoder);
	SendDlgItemMessageW(Window, ID_GPU_ENCODER + 1, CB_SETCURSEL, C->HardwarePreferIntegrated ? 0 : 1, 0);

	// output
	SetDlgItemTextW(Window, ID_OUTPUT_FOLDER,  C->OutputFolder);
	CheckDlgButton(Window, ID_OPEN_FOLDER,     C->OpenFolder);
	CheckDlgButton(Window, ID_FRAGMENTED_MP4,  C->FragmentedOutput);
	CheckDlgButton(Window, ID_LIMIT_LENGTH,    C->EnableLimitLength);
	CheckDlgButton(Window, ID_LIMIT_SIZE,      C->EnableLimitSize);
	SetDlgItemInt(Window, ID_LIMIT_LENGTH + 1, C->LimitLength, FALSE);
	SetDlgItemInt(Window, ID_LIMIT_SIZE + 1,   C->LimitSize,   FALSE);

	// video
	CheckDlgButton(Window, ID_VIDEO_GAMMA_RESIZE,     C->GammaCorrectResize);
	CheckDlgButton(Window, ID_VIDEO_IMPROVED_CONVERT, C->ImprovedColorConversion);
	SendDlgItemMessageW(Window, ID_VIDEO_CODEC, CB_SETCURSEL, C->VideoCodec, 0);
	Config__SelectVideoProfile(Window, C->VideoCodec, C->VideoProfile);
	SetDlgItemInt(Window, ID_VIDEO_MAX_WIDTH,     C->VideoMaxWidth,     FALSE);
	SetDlgItemInt(Window, ID_VIDEO_MAX_HEIGHT,    C->VideoMaxHeight,    FALSE);
	SetDlgItemInt(Window, ID_VIDEO_MAX_FRAMERATE, C->VideoMaxFramerate, FALSE);
	SetDlgItemInt(Window, ID_VIDEO_BITRATE,       C->VideoBitrate,      FALSE);

	// audio
	CheckDlgButton(Window, ID_AUDIO_CAPTURE, C->CaptureAudio);
	CheckDlgButton(Window, ID_AUDIO_APPLICATION_LOCAL, C->ApplicationLocalAudio);
	SendDlgItemMessageW(Window, ID_AUDIO_CODEC, CB_SETCURSEL, C->AudioCodec, 0);
	SendDlgItemMessageW(Window, ID_AUDIO_CHANNELS, CB_SETCURSEL, C->AudioChannels - 1, 0);
	WCHAR Text[64];
	StrFormat(Text, L"%u", C->AudioSamplerate);
	SendDlgItemMessageW(Window, ID_AUDIO_SAMPLERATE, CB_SELECTSTRING, -1, (LPARAM)Text);

	if (C->AudioCodec == CONFIG_AUDIO_AAC)
	{
		StrFormat(Text, L"%u", C->AudioBitrate);
		SendDlgItemMessageW(Window, ID_AUDIO_BITRATE, CB_SELECTSTRING, -1, (LPARAM)Text);
	}
	else if (C->AudioCodec == CONFIG_AUDIO_FLAC)
	{
		SendDlgItemMessageW(Window, ID_AUDIO_BITRATE, CB_SETCURSEL, 0, 0);
	}

	// shortcuts
	Config__FormatKey(C->ShortcutMonitor, Text);
	SetDlgItemTextW(Window, ID_SHORTCUT_MONITOR, Text);
	SetWindowLongW(GetDlgItem(Window, ID_SHORTCUT_MONITOR), GWLP_USERDATA, C->ShortcutMonitor);
	Config__FormatKey(C->ShortcutWindow, Text);
	SetDlgItemTextW(Window, ID_SHORTCUT_WINDOW, Text);
	SetWindowLongW(GetDlgItem(Window, ID_SHORTCUT_WINDOW), GWLP_USERDATA, C->ShortcutWindow);
	Config__FormatKey(C->ShortcutRegion, Text);
	SetDlgItemTextW(Window, ID_SHORTCUT_REGION, Text);
	SetWindowLongW(GetDlgItem(Window, ID_SHORTCUT_REGION), GWLP_USERDATA, C->ShortcutRegion);

	EnableWindow(GetDlgItem(Window, ID_GPU_ENCODER + 1),  C->HardwareEncoder);
	EnableWindow(GetDlgItem(Window, ID_LIMIT_LENGTH + 1), C->EnableLimitLength);
	EnableWindow(GetDlgItem(Window, ID_LIMIT_SIZE + 1),   C->EnableLimitSize);

	EnableWindow(GetDlgItem(Window, ID_MOUSE_CURSOR),            ScreenCapture_CanHideMouseCursor());
	EnableWindow(GetDlgItem(Window, ID_SHOW_RECORDING_BORDER),   ScreenCapture_CanHideRecordingBorder());
	EnableWindow(GetDlgItem(Window, ID_ROUNDED_CORNERS),         ScreenCapture_CanDisableRoundedCorners());
	EnableWindow(GetDlgItem(Window, ID_AUDIO_APPLICATION_LOCAL), AudioCapture_CanCaptureApplicationLocal());
}

void DisableHotKeys(void);
BOOL EnableHotKeys(void);

static LRESULT CALLBACK Config__ShortcutProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
	if (Message == WM_GETDLGCODE)
	{
		return DLGC_WANTALLKEYS;
	}

	if (Message == WM_KEYDOWN || Message == WM_SYSKEYDOWN)
	{
		return TRUE;
	}

	if (Message == WM_KEYUP || Message == WM_SYSKEYUP)
	{
		if (WParam != VK_LCONTROL &&  WParam != VK_RCONTROL && WParam != VK_CONTROL &&
			WParam != VK_LSHIFT && WParam != VK_RSHIFT && WParam != VK_SHIFT &&
			WParam != VK_LMENU && WParam != VK_RMENU && WParam != VK_MENU &&
			WParam != VK_LWIN && WParam != VK_RWIN)
		{
			DWORD Shortcut;
			if (WParam == VK_ESCAPE)
			{
				Shortcut = GetWindowLongW(Window, GWLP_USERDATA);
			}
			else if (WParam == VK_BACK)
			{
				Shortcut = 0;
			}
			else
			{
				DWORD VirtualKey = (DWORD)WParam;
				DWORD Mods = 0
					| ((GetKeyState(VK_CONTROL) >> 15) ? MOD_CONTROL : 0)
					| ((GetKeyState(VK_LWIN) >> 15)    ? MOD_WIN     : 0)
					| ((GetKeyState(VK_RWIN) >> 15)    ? MOD_WIN     : 0)
					| ((GetKeyState(VK_MENU) >> 15)    ? MOD_ALT     : 0)
					| ((GetKeyState(VK_SHIFT) >> 15)   ? MOD_SHIFT   : 0);

				Shortcut = HOT_KEY(VirtualKey, Mods);
			}

			WCHAR Text[64];
			Config__FormatKey(Shortcut, Text);
			SetDlgItemTextW(gDialogWindow, gConfigShortcut.Control, Text);
			SetWindowLongW(Window, GWLP_USERDATA, Shortcut);

			SetWindowLongPtrW(Window, GWLP_WNDPROC, (LONG_PTR)gConfigShortcut.WindowProc);
			gConfigShortcut.Control = 0;
			EnableHotKeys();
			return FALSE;
		}
	}

	return gConfigShortcut.WindowProc(Window, Message, WParam, LParam);
}

static LRESULT CALLBACK Config__DialogProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
	if (Message == WM_INITDIALOG)
	{
		Config* C = (Config*)LParam;
		SetWindowLongPtrW(Window, GWLP_USERDATA, (LONG_PTR)C);

		SendDlgItemMessageW(Window, ID_VIDEO_CODEC, CB_ADDSTRING, 0, (LPARAM)L"H264 / AVC");
		SendDlgItemMessageW(Window, ID_VIDEO_CODEC, CB_ADDSTRING, 0, (LPARAM)L"H265 / HEVC");

		SendDlgItemMessageW(Window, ID_AUDIO_CODEC, CB_ADDSTRING, 0, (LPARAM)L"AAC");
		SendDlgItemMessageW(Window, ID_AUDIO_CODEC, CB_ADDSTRING, 0, (LPARAM)L"FLAC");

		SendDlgItemMessageW(Window, ID_AUDIO_CHANNELS, CB_ADDSTRING, 0, (LPARAM)L"1");
		SendDlgItemMessageW(Window, ID_AUDIO_CHANNELS, CB_ADDSTRING, 0, (LPARAM)L"2");

		SendDlgItemMessageW(Window, ID_AUDIO_SAMPLERATE, CB_ADDSTRING, 0, (LPARAM)L"44100");
		SendDlgItemMessageW(Window, ID_AUDIO_SAMPLERATE, CB_ADDSTRING, 0, (LPARAM)L"48000");

		SendDlgItemMessageW(Window, ID_GPU_ENCODER + 1, CB_ADDSTRING, 0, (LPARAM)L"Prefer iGPU");
		SendDlgItemMessageW(Window, ID_GPU_ENCODER + 1, CB_ADDSTRING, 0, (LPARAM)L"Prefer dGPU");

		Config__SetDialogValues(Window, C);

		SetForegroundWindow(Window);
		gDialogWindow = Window;
		gConfigShortcut.Control = 0;
		return TRUE;
	}
	else if (Message == WM_DESTROY)
	{
		gDialogWindow = NULL;
	}
	else if (Message == WM_COMMAND)
	{
		Config* C = (Config*)GetWindowLongPtrW(Window, GWLP_USERDATA);
		int Control = LOWORD(WParam);
		if (Control == ID_OK)
		{
			// capture
			C->MouseCursor              = IsDlgButtonChecked(Window, ID_MOUSE_CURSOR);
			C->OnlyClientArea           = IsDlgButtonChecked(Window, ID_ONLY_CLIENT_AREA);
			C->ShowRecordingBorder      = IsDlgButtonChecked(Window, ID_SHOW_RECORDING_BORDER);
			C->KeepRoundedWindowCorners = IsDlgButtonChecked(Window, ID_ROUNDED_CORNERS);
			C->HardwareEncoder          = IsDlgButtonChecked(Window, ID_GPU_ENCODER);
			C->HardwarePreferIntegrated = (DWORD)SendDlgItemMessageW(Window, ID_GPU_ENCODER + 1, CB_GETCURSEL, 0, 0) == 0;

			// output
			GetDlgItemTextW(Window, ID_OUTPUT_FOLDER, C->OutputFolder, _countof(C->OutputFolder));
			C->OpenFolder        = IsDlgButtonChecked(Window, ID_OPEN_FOLDER);
			C->FragmentedOutput  = IsDlgButtonChecked(Window, ID_FRAGMENTED_MP4);
			C->EnableLimitLength = IsDlgButtonChecked(Window, ID_LIMIT_LENGTH);
			C->EnableLimitSize   = IsDlgButtonChecked(Window, ID_LIMIT_SIZE);
			C->LimitLength       = GetDlgItemInt(Window,      ID_LIMIT_LENGTH + 1, NULL, FALSE);
			C->LimitSize         = GetDlgItemInt(Window,      ID_LIMIT_SIZE + 1,   NULL, FALSE);
			// video
			C->GammaCorrectResize      = IsDlgButtonChecked(Window, ID_VIDEO_GAMMA_RESIZE);
			C->ImprovedColorConversion = IsDlgButtonChecked(Window, ID_VIDEO_IMPROVED_CONVERT);
			C->VideoCodec              = (DWORD)SendDlgItemMessageW(Window, ID_VIDEO_CODEC,   CB_GETCURSEL, 0, 0);
			C->VideoProfile            = Config__GetSelectedVideoProfile(Window);
			C->VideoMaxWidth           = GetDlgItemInt(Window, ID_VIDEO_MAX_WIDTH,     NULL, FALSE);
			C->VideoMaxHeight          = GetDlgItemInt(Window, ID_VIDEO_MAX_HEIGHT,    NULL, FALSE);
			C->VideoMaxFramerate       = GetDlgItemInt(Window, ID_VIDEO_MAX_FRAMERATE, NULL, FALSE);
			C->VideoBitrate            = GetDlgItemInt(Window, ID_VIDEO_BITRATE,       NULL, FALSE);
			// audio
			C->CaptureAudio          = IsDlgButtonChecked(Window, ID_AUDIO_CAPTURE);
			C->ApplicationLocalAudio = IsDlgButtonChecked(Window, ID_AUDIO_APPLICATION_LOCAL);
			C->AudioCodec            = (DWORD)SendDlgItemMessageW(Window, ID_AUDIO_CODEC,    CB_GETCURSEL, 0, 0);
			C->AudioChannels         = (DWORD)SendDlgItemMessageW(Window, ID_AUDIO_CHANNELS, CB_GETCURSEL, 0, 0) + 1;
			C->AudioSamplerate       = gAudioSamplerates[SendDlgItemMessageW(Window, ID_AUDIO_SAMPLERATE, CB_GETCURSEL, 0, 0)];
			if (C->AudioCodec == CONFIG_AUDIO_AAC)
			{
				C->AudioBitrate = gAudioBitrates[SendDlgItemMessageW(Window, ID_AUDIO_BITRATE, CB_GETCURSEL, 0, 0)];
			}
			// shortcuts
			C->ShortcutMonitor = GetWindowLongW(GetDlgItem(Window, ID_SHORTCUT_MONITOR), GWLP_USERDATA);
			C->ShortcutWindow  = GetWindowLongW(GetDlgItem(Window, ID_SHORTCUT_WINDOW),  GWLP_USERDATA);
			C->ShortcutRegion  = GetWindowLongW(GetDlgItem(Window, ID_SHORTCUT_REGION),  GWLP_USERDATA);

			EndDialog(Window, TRUE);
			return TRUE;
		}
		else if (Control == ID_CANCEL)
		{
			EndDialog(Window, FALSE);
			return TRUE;
		}
		else if (Control == ID_DEFAULTS)
		{
			Config DefaultConfig;
			Config_Defaults(&DefaultConfig);
			Config__SetDialogValues(Window, &DefaultConfig);
			if (gConfigShortcut.Control)
			{
				SetWindowLongPtrW(GetDlgItem(Window, gConfigShortcut.Control), GWLP_WNDPROC, (LONG_PTR)gConfigShortcut.WindowProc);
				gConfigShortcut.Control = 0;
				EnableHotKeys();
			}
			return TRUE;
		}
		else if (Control == ID_VIDEO_CODEC && HIWORD(WParam) == CBN_SELCHANGE)
		{
			LRESULT Index = SendDlgItemMessageW(Window, ID_VIDEO_CODEC, CB_GETCURSEL, 0, 0);
			Config__UpdateVideoProfiles(Window, (DWORD)Index);
			return TRUE;
		}
		else if (Control == ID_AUDIO_CODEC && HIWORD(WParam) == CBN_SELCHANGE)
		{
			LRESULT Index = SendDlgItemMessageW(Window, ID_AUDIO_CODEC, CB_GETCURSEL, 0, 0);
			Config__UpdateAudioBitrate(Window, (DWORD)Index, C->AudioBitrate);
			return TRUE;
		}
		else if (Control == ID_GPU_ENCODER && HIWORD(WParam) == BN_CLICKED)
		{
			EnableWindow(GetDlgItem(Window, ID_GPU_ENCODER + 1), (BOOL)SendDlgItemMessageW(Window, ID_GPU_ENCODER, BM_GETCHECK, 0, 0));
			return TRUE;
		}
		else if (Control == ID_LIMIT_LENGTH && HIWORD(WParam) == BN_CLICKED)
		{
			EnableWindow(GetDlgItem(Window, ID_LIMIT_LENGTH + 1), (BOOL)SendDlgItemMessageW(Window, ID_LIMIT_LENGTH, BM_GETCHECK, 0, 0));
			return TRUE;
		}
		else if (Control == ID_LIMIT_SIZE && HIWORD(WParam) == BN_CLICKED)
		{
			EnableWindow(GetDlgItem(Window, ID_LIMIT_SIZE + 1), (BOOL)SendDlgItemMessageW(Window, ID_LIMIT_SIZE, BM_GETCHECK, 0, 0));
			return TRUE;
		}
		else if (Control == ID_OUTPUT_FOLDER + 1)
		{
			// this expects caller has called CoInitializeEx with single or apartment-threaded model
			IFileDialog* Dialog;
			HR(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC, &IID_IFileDialog, &Dialog));

			WCHAR Text[MAX_PATH];
			GetDlgItemTextW(Window, ID_OUTPUT_FOLDER, Text, _countof(Text));

			IShellItem* Folder;
			if (SUCCEEDED(SHCreateItemFromParsingName(Text, NULL, &IID_IShellItem, &Folder)))
			{
				HR(IFileDialog_SetFolder(Dialog, Folder));
				IShellItem_Release(Folder);
			}

			HR(IFileDialog_SetOptions(Dialog, FOS_NOCHANGEDIR | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST));
			if (SUCCEEDED(IFileDialog_Show(Dialog, Window)) && SUCCEEDED(IFileDialog_GetResult(Dialog, &Folder)))
			{
				LPWSTR Path;
				if (SUCCEEDED(IShellItem_GetDisplayName(Folder, SIGDN_FILESYSPATH, &Path)))
				{
					SetDlgItemTextW(Window, ID_OUTPUT_FOLDER, Path);
					CoTaskMemFree(Path);
				}
				IShellItem_Release(Folder);
			}
			IFileDialog_Release(Dialog);

			return TRUE;
		}
		else if ((Control == ID_SHORTCUT_MONITOR ||
		          Control == ID_SHORTCUT_WINDOW ||
		          Control == ID_SHORTCUT_REGION) && HIWORD(WParam) == BN_CLICKED)
		{
			if (gConfigShortcut.Control == 0)
			{
				SetDlgItemTextW(Window, Control, L"Press new shortcut, [ESC] to cancel, [BACKSPACE] to disable");

				gConfigShortcut.Control = Control;
				gConfigShortcut.Config = C;

				HWND ControlWindow = GetDlgItem(Window, Control);
				gConfigShortcut.WindowProc = (WNDPROC)GetWindowLongPtrW(ControlWindow, GWLP_WNDPROC);
				SetWindowLongPtrW(ControlWindow, GWLP_WNDPROC, (LONG_PTR)&Config__ShortcutProc);
				DisableHotKeys();
			}
		}
	}
	return FALSE;
}

typedef struct {
	int Left;
	int Top;
	int Width;
	int Height;
} Config__DialogRect;

typedef struct {
	const char* Text;
	const WORD Id;
	const WORD Item;
	const DWORD Width;
} Config__DialogItem;

typedef struct {
	const char* Caption;
	const Config__DialogRect Rect;
	const Config__DialogItem* Items;
} Config__DialogGroup;

typedef struct {
	const char* Title;
	const char* Font;
	const WORD FontSize;
	const Config__DialogGroup* Groups;
} Config__DialogLayout;

static void* Config__Align(BYTE* Data, SIZE_T Size)
{
	SIZE_T Pointer = (SIZE_T)Data;
	return Data + ((Pointer + Size - 1) & ~(Size - 1)) - Pointer;
}

static BYTE* Config__DoDialogItem(BYTE* Data, LPCSTR Text, WORD Id, WORD Control, DWORD Style, int x, int y, int w, int h)
{
	Data = Config__Align(Data, sizeof(DWORD));

	*(DLGITEMTEMPLATE*)Data = (DLGITEMTEMPLATE)
	{
		.style = Style | WS_CHILD | WS_VISIBLE,
		.x = x,
		.y = y + (Control == CONTROL_STATIC ? 2 : 0),
		.cx = w,
		.cy = h - (Control == CONTROL_EDIT ? 2 : 0) - (Control == CONTROL_STATIC ? 2 : 0),
		.id = Id,
	};
	Data += sizeof(DLGITEMTEMPLATE);

	// window class
	Data = Config__Align(Data, sizeof(WORD));
	*(WORD*)Data = 0xffff;
	Data += sizeof(WORD);
	*(WORD*)Data = Control;
	Data += sizeof(WORD);

	// item text
	Data = Config__Align(Data, sizeof(WCHAR));
	DWORD ItemChars = MultiByteToWideChar(CP_UTF8, 0, Text, -1, (WCHAR*)Data, 128);
	Data += ItemChars * sizeof(WCHAR);

	// create extras
	Data = Config__Align(Data, sizeof(WORD));
	*(WORD*)Data = 0;
	Data += sizeof(WORD);

	return Data;
}

static void Config__DoDialogLayout(const Config__DialogLayout* Layout, BYTE* Data, SIZE_T DataSize)
{
	BYTE* End = Data + DataSize;

	// header
	DLGTEMPLATE* Dialog = (void*)Data;
	Data += sizeof(DLGTEMPLATE);

	// menu
	Data = Config__Align(Data, sizeof(WCHAR));
	*(WCHAR*)Data = 0;
	Data += sizeof(WCHAR);

	// window class
	Data = Config__Align(Data, sizeof(WCHAR));
	*(WCHAR*)Data = 0;
	Data += sizeof(WCHAR);

	// title
	Data = Config__Align(Data, sizeof(WCHAR));
	DWORD TitleChars = MultiByteToWideChar(CP_UTF8, 0, Layout->Title, -1, (WCHAR*)Data, 128);
	Data += TitleChars * sizeof(WCHAR);

	// font size
	Data = Config__Align(Data, sizeof(WORD));
	*(WORD*)Data = Layout->FontSize;
	Data += sizeof(WORD);

	// font name
	Data = Config__Align(Data, sizeof(WCHAR));
	DWORD FontChars = MultiByteToWideChar(CP_UTF8, 0, Layout->Font, -1, (WCHAR*)Data, 128);
	Data += FontChars * sizeof(WCHAR);

	int ItemCount = 3;

	int ButtonX = PADDING + COL10W + COL11W + PADDING - 3 * (PADDING + BUTTON_WIDTH);
	int ButtonY = PADDING + ROW0H + ROW1H + ROW2H + PADDING;

	DLGITEMTEMPLATE* OkData = Config__Align(Data, sizeof(DWORD));
	Data = Config__DoDialogItem(Data, "OK", ID_OK, CONTROL_BUTTON, WS_TABSTOP | BS_DEFPUSHBUTTON, ButtonX, ButtonY, BUTTON_WIDTH, ITEM_HEIGHT);
	ButtonX += BUTTON_WIDTH + PADDING;

	DLGITEMTEMPLATE* CancelData = Config__Align(Data, sizeof(DWORD));
	Data = Config__DoDialogItem(Data, "Cancel", ID_CANCEL, CONTROL_BUTTON, WS_TABSTOP | BS_PUSHBUTTON, ButtonX, ButtonY, BUTTON_WIDTH, ITEM_HEIGHT);
	ButtonX += BUTTON_WIDTH + PADDING;

	DLGITEMTEMPLATE* DefaultsData = Config__Align(Data, sizeof(DWORD));
	Data = Config__DoDialogItem(Data, "Defaults", ID_DEFAULTS, CONTROL_BUTTON, WS_TABSTOP | BS_PUSHBUTTON, ButtonX, ButtonY, BUTTON_WIDTH, ITEM_HEIGHT);
	ButtonX += BUTTON_WIDTH + PADDING;

	for (const Config__DialogGroup* Group = Layout->Groups; Group->Caption; Group++)
	{
		int X = Group->Rect.Left + PADDING;
		int Y = Group->Rect.Top + PADDING;
		int W = Group->Rect.Width;
		int H = Group->Rect.Height;

		Data = Config__DoDialogItem(Data, Group->Caption, -1, CONTROL_BUTTON, BS_GROUPBOX, X, Y, W, H);
		ItemCount++;

		X += PADDING;
		Y += ITEM_HEIGHT - PADDING;
		W -= 2 * PADDING;

		for (const Config__DialogItem* Item = Group->Items; Item->Text; Item++)
		{
			int HasCheckbox  = !!(Item->Item & ITEM_CHECKBOX);
			int HasNumber    = !!(Item->Item & ITEM_NUMBER);
			int HasCombobox  = !!(Item->Item & ITEM_COMBOBOX);
			int OnlyCheckbox = !(Item->Item & ~ITEM_CHECKBOX);
			int HasHotKey    = !!(Item->Item & ITEM_HOTKEY);

			int ItemX = X;
			int ItemW = W;
			int ItemId = Item->Id;

			if (HasCheckbox)
			{
				if (!OnlyCheckbox)
				{
					// reduce width so checbox can fit other control on the right
					ItemW = Item->Width;
				}
				Data = Config__DoDialogItem(Data, Item->Text, ItemId, CONTROL_BUTTON, WS_TABSTOP | BS_AUTOCHECKBOX, ItemX, Y, ItemW, ITEM_HEIGHT);
				ItemCount++;
				ItemId++;
				if (!OnlyCheckbox)
				{
					ItemX += Item->Width + PADDING;
					ItemW = W - (Item->Width + PADDING);
				}
			}

			if ((HasCombobox && !HasCheckbox) || (HasNumber || HasHotKey) && !HasCheckbox)
			{
				// label, only for controls without checkbox, or combobox
				Data = Config__DoDialogItem(Data, Item->Text, -1, CONTROL_STATIC, 0, ItemX, Y, Item->Width, ITEM_HEIGHT);
				ItemCount++;
				ItemX += Item->Width + PADDING;
				ItemW -= Item->Width + PADDING;
			}

			if (HasNumber)
			{
				Data = Config__DoDialogItem(Data, "", ItemId, CONTROL_EDIT, WS_TABSTOP | WS_BORDER | ES_RIGHT | ES_NUMBER, ItemX, Y, ItemW, ITEM_HEIGHT);
				ItemCount++;
			}

			if (HasHotKey)
			{
				Data = Config__DoDialogItem(Data, "", ItemId, CONTROL_BUTTON, WS_TABSTOP, ItemX, Y, ItemW, ITEM_HEIGHT);
				ItemCount++;
			}

			if (HasCombobox)
			{
				Data = Config__DoDialogItem(Data, "", ItemId, CONTROL_COMBOBOX, WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS, ItemX, Y, ItemW, ITEM_HEIGHT);
				ItemCount++;
			}

			if (Item->Item & ITEM_FOLDER)
			{
				Data = Config__DoDialogItem(Data, "", ItemId, CONTROL_EDIT, WS_TABSTOP | WS_BORDER, X, Y, W - BUTTON_SMALL_WIDTH - PADDING + 2, ITEM_HEIGHT);
				ItemCount++;
				ItemId++;

				Data = Config__DoDialogItem(Data, "...", ItemId, CONTROL_BUTTON, WS_TABSTOP | BS_PUSHBUTTON, X + W - BUTTON_SMALL_WIDTH + 2, Y + 1, BUTTON_SMALL_WIDTH - 4, ITEM_HEIGHT - 3);
				ItemCount++;
			}

			Y += ITEM_HEIGHT;
		}
	}

	*Dialog = (DLGTEMPLATE)
	{
		.style = DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU,
		.cdit = ItemCount,
		.cx = PADDING + COL10W + PADDING + COL11W + PADDING,
		.cy = PADDING + ROW0H + PADDING + ROW1H + PADDING + ROW2H + ITEM_HEIGHT,
	};

	Assert(Data <= End);
}

void Config_Defaults(Config* C)
{
	*C = (Config)
	{
		// capture
		.MouseCursor = TRUE,
		.OnlyClientArea = TRUE,
		.ShowRecordingBorder = TRUE,
		.KeepRoundedWindowCorners = TRUE,
		.HardwareEncoder = TRUE,
		.HardwarePreferIntegrated = FALSE,
		// output
		.OpenFolder = TRUE,
		.FragmentedOutput = FALSE,
		.EnableLimitLength = FALSE,
		.EnableLimitSize = FALSE,
		.LimitLength = 60,
		.LimitSize = 25,
		// video
		.GammaCorrectResize = FALSE,
		.ImprovedColorConversion = FALSE,
		.VideoCodec = CONFIG_VIDEO_H264,
		.VideoProfile = CONFIG_VIDEO_HIGH,
		.VideoMaxWidth = 1920,
		.VideoMaxHeight = 1080,
		.VideoMaxFramerate = 60,
		.VideoBitrate = 8000,
		// audio
		.CaptureAudio = TRUE,
		.ApplicationLocalAudio = TRUE,
		.AudioCodec = CONFIG_AUDIO_AAC,
		.AudioChannels = 2,
		.AudioSamplerate = 48000,
		.AudioBitrate = 160,
		// shortcuts
		.ShortcutMonitor = HOT_KEY(VK_SNAPSHOT, MOD_CONTROL),
		.ShortcutWindow = HOT_KEY(VK_SNAPSHOT, MOD_CONTROL | MOD_WIN),
		.ShortcutRegion = HOT_KEY(VK_SNAPSHOT, MOD_CONTROL | MOD_SHIFT),
	};

	LPWSTR VideoFolder;
	HR(SHGetKnownFolderPath(&FOLDERID_Videos, KF_FLAG_DEFAULT, NULL, &VideoFolder));
	StrCpyNW(C->OutputFolder, VideoFolder, _countof(C->OutputFolder));
	CoTaskMemFree(VideoFolder);
}

static void Config__GetBool(LPCWSTR FileName, LPCWSTR Key, BOOL* Value)
{
	int Data = GetPrivateProfileIntW(INI_SECTION, Key, -1, FileName);
	if (Data >= 0)
	{
		*Value = Data != 0;
	}
}

static void Config__GetInt(LPCWSTR FileName, LPCWSTR Key, DWORD* Value, const DWORD* AllowedList)
{
	int Data = GetPrivateProfileIntW(INI_SECTION, Key, -1, FileName);
	if (Data >= 0)
	{
		if (AllowedList)
		{
			for (const DWORD* Allowed = AllowedList; *Allowed; ++Allowed)
			{
				if (*Allowed == Data)
				{
					*Value = Data;
					break;
				}
			}
		}
		else
		{
			*Value = Data;
		}
	}
}
static void Config__GetStr(LPCWSTR FileName, LPCWSTR Key, DWORD* Value, const LPCWSTR* AllowedList)
{
	WCHAR Text[64];
	GetPrivateProfileStringW(INI_SECTION, Key, L"", Text, _countof(Text), FileName);
	if (Text[0])
	{
		for (const LPCWSTR* Allowed = AllowedList; *Allowed; ++Allowed)
		{
			if (StrCmpW(Text, *Allowed) == 0)
			{
				*Value = (DWORD)(Allowed - AllowedList);
				return;
			}
		}
	}
}

static void Config__ValidateVideoProfile(Config* C)
{
	const int* Profiles = gValidVideoProfiles[C->VideoCodec];
	DWORD LastProfile;
	for (int i=0; Profiles[i] != -1; i++)
	{
		LastProfile = Profiles[i];
		if (C->VideoProfile == LastProfile)
		{
			return;
		}
	}
	C->VideoProfile = LastProfile;
}

void Config_Load(Config* C, LPCWSTR FileName)
{
	// capture
	Config__GetBool(FileName, L"MouseCursor",              &C->MouseCursor);
	Config__GetBool(FileName, L"OnlyClientArea",           &C->OnlyClientArea);
	Config__GetBool(FileName, L"ShowRecordingBorder",      &C->ShowRecordingBorder);
	Config__GetBool(FileName, L"KeepRoundedWindowCorners", &C->KeepRoundedWindowCorners);
	Config__GetBool(FileName, L"HardwareEncoder",          &C->HardwareEncoder);
	Config__GetBool(FileName, L"HardwarePreferIntegrated", &C->HardwarePreferIntegrated);
	// output
	WCHAR OutputFolder[MAX_PATH];
	GetPrivateProfileStringW(INI_SECTION, L"OutputFolder", L"", OutputFolder, _countof(OutputFolder), FileName);
	if (OutputFolder[0]) StrCpyW(C->OutputFolder, OutputFolder);
	Config__GetBool(FileName, L"OpenFolder",        &C->OpenFolder);
	Config__GetBool(FileName, L"FragmentedOutput",  &C->FragmentedOutput);
	Config__GetBool(FileName, L"EnableLimitLength", &C->EnableLimitLength);
	Config__GetBool(FileName, L"EnableLimitSize",   &C->EnableLimitSize);
	Config__GetInt(FileName,  L"LimitLength",       &C->LimitLength, NULL);
	Config__GetInt(FileName,  L"LimitSize",         &C->LimitSize,   NULL);
	// video
	Config__GetBool(FileName, L"GammaCorrectResize",      &C->GammaCorrectResize);
	Config__GetBool(FileName, L"ImprovedColorConversion", &C->ImprovedColorConversion);
	Config__GetStr(FileName, L"VideoCodec",               &C->VideoCodec,        gVideoCodecs);
	Config__GetStr(FileName, L"VideoProfile",             &C->VideoProfile,      gVideoProfiles);
	Config__GetInt(FileName, L"VideoMaxWidth",            &C->VideoMaxWidth,     NULL);
	Config__GetInt(FileName, L"VideoMaxHeight",           &C->VideoMaxHeight,    NULL);
	Config__GetInt(FileName, L"VideoMaxFramerate",        &C->VideoMaxFramerate, NULL);
	Config__GetInt(FileName, L"VideoBitrate",             &C->VideoBitrate,      NULL);
	// audio
	Config__GetBool(FileName, L"CaptureAudio",          &C->CaptureAudio);
	Config__GetBool(FileName, L"ApplicationLocalAudio", &C->ApplicationLocalAudio);
	Config__GetStr(FileName, L"AudioCodec",             &C->AudioCodec,      gAudioCodecs);
	Config__GetInt(FileName, L"AudioChannels",          &C->AudioChannels,   (DWORD[]) { 1, 2, 0 });
	Config__GetInt(FileName, L"AudioSamplerate",        &C->AudioSamplerate, gAudioSamplerates);
	Config__GetInt(FileName, L"AudioBitrate",           &C->AudioBitrate,    gAudioBitrates);
	// shortcuts
	Config__GetInt(FileName, L"ShortcutMonitor", &C->ShortcutMonitor, NULL);
	Config__GetInt(FileName, L"ShortcutWindow",  &C->ShortcutWindow,  NULL);
	Config__GetInt(FileName, L"ShortcutRect",    &C->ShortcutRegion,  NULL);

	Config__ValidateVideoProfile(C);
}

static void Config__WriteInt(LPCWSTR FileName, LPCWSTR Key, DWORD Value)
{
	WCHAR Text[64];
	StrFormat(Text, L"%u", Value);
	WritePrivateProfileStringW(INI_SECTION, Key, Text, FileName);
}

void Config_Save(Config* C, LPCWSTR FileName)
{
	// capture
	WritePrivateProfileStringW(INI_SECTION, L"MouseCursor",              C->MouseCursor              ? L"1" : L"0", FileName);
	WritePrivateProfileStringW(INI_SECTION, L"OnlyClientArea",           C->OnlyClientArea           ? L"1" : L"0", FileName);
	WritePrivateProfileStringW(INI_SECTION, L"ShowRecordingBorder",      C->ShowRecordingBorder      ? L"1" : L"0", FileName);
	WritePrivateProfileStringW(INI_SECTION, L"KeepRoundedWindowCorners", C->KeepRoundedWindowCorners ? L"1" : L"0", FileName);
	WritePrivateProfileStringW(INI_SECTION, L"HardwareEncoder",          C->HardwareEncoder          ? L"1" : L"0", FileName);
	WritePrivateProfileStringW(INI_SECTION, L"HardwarePreferIntegratged",C->HardwarePreferIntegrated ? L"1" : L"0", FileName);
	// output
	WritePrivateProfileStringW(INI_SECTION, L"OutputFolder",      C->OutputFolder, FileName);
	WritePrivateProfileStringW(INI_SECTION, L"OpenFolder",        C->OpenFolder        ? L"1" : L"0", FileName);
	WritePrivateProfileStringW(INI_SECTION, L"FragmentedOutput",  C->FragmentedOutput  ? L"1" : L"0", FileName);
	WritePrivateProfileStringW(INI_SECTION, L"EnableLimitLength", C->EnableLimitLength ? L"1" : L"0", FileName);
	WritePrivateProfileStringW(INI_SECTION, L"EnableLimitSize",   C->EnableLimitSize   ? L"1" : L"0", FileName);
	Config__WriteInt(FileName, L"LimitLength", C->LimitLength);
	Config__WriteInt(FileName, L"LimitSize", C->LimitSize);
	// video
	WritePrivateProfileStringW(INI_SECTION, L"GammaCorrectResize",      C->GammaCorrectResize      ? L"1" : L"0", FileName);
	WritePrivateProfileStringW(INI_SECTION, L"ImprovedColorConversion", C->ImprovedColorConversion ? L"1" : L"0", FileName);
	WritePrivateProfileStringW(INI_SECTION, L"VideoCodec",   gVideoCodecs[C->VideoCodec],     FileName);
	WritePrivateProfileStringW(INI_SECTION, L"VideoProfile", gVideoProfiles[C->VideoProfile], FileName);
	Config__WriteInt(FileName, L"VideoMaxWidth",     C->VideoMaxWidth);
	Config__WriteInt(FileName, L"VideoMaxHeight",    C->VideoMaxHeight);
	Config__WriteInt(FileName, L"VideoMaxFramerate", C->VideoMaxFramerate);
	Config__WriteInt(FileName, L"VideoBitrate",      C->VideoBitrate);
	// audio
	WritePrivateProfileStringW(INI_SECTION, L"CaptureAudio",          C->CaptureAudio          ? L"1" : L"0", FileName);
	WritePrivateProfileStringW(INI_SECTION, L"ApplicationLocalAudio", C->ApplicationLocalAudio ? L"1" : L"0", FileName);
	WritePrivateProfileStringW(INI_SECTION, L"AudioCodec", gAudioCodecs[C->AudioCodec], FileName);
	Config__WriteInt(FileName, L"AudioChannels",   C->AudioChannels);
	Config__WriteInt(FileName, L"AudioSamplerate", C->AudioSamplerate);
	Config__WriteInt(FileName, L"AudioBitrate",    C->AudioBitrate);
	// shortcuts
	Config__WriteInt(FileName, L"ShortcutMonitor", C->ShortcutMonitor);
	Config__WriteInt(FileName, L"ShortcutWindow",  C->ShortcutWindow);
	Config__WriteInt(FileName, L"ShortcutRect",    C->ShortcutRegion);
}

BOOL Config_ShowDialog(Config* C)
{
	if (gDialogWindow)
	{
		SetForegroundWindow(gDialogWindow);
		return FALSE;
	}

	Config__DialogLayout Dialog = (Config__DialogLayout)
	{
		.Title = "wcap Settings",
		.Font = "Segoe UI",
		.FontSize = 9,
		.Groups = (Config__DialogGroup[])
		{
			{
				.Caption = "Capture",
				.Rect = { 0, 0, COL00W, ROW0H },
				.Items = (Config__DialogItem[])
				{
					{ "&Mouse Cursor",                ID_MOUSE_CURSOR,          ITEM_CHECKBOX                     },
					{ "Only &Client Area",            ID_ONLY_CLIENT_AREA,      ITEM_CHECKBOX                     },
					{ "Show Recording &Border",       ID_SHOW_RECORDING_BORDER, ITEM_CHECKBOX                     },
					{ "Keep &Rounded Window Corners", ID_ROUNDED_CORNERS,       ITEM_CHECKBOX                     },
					{ "GPU &Encoder",                 ID_GPU_ENCODER,           ITEM_CHECKBOX | ITEM_COMBOBOX, 50 },
					{ NULL },
				},
			},
			{
				.Caption = "&Output",
				.Rect = { COL00W + PADDING, 0, COL01W, ROW0H },
				.Items = (Config__DialogItem[])
				{
					{ "",                            ID_OUTPUT_FOLDER,  ITEM_FOLDER                     },
					{ "O&pen When Finished",         ID_OPEN_FOLDER,    ITEM_CHECKBOX                   },
					{ "Fragmented MP&4 (H264 only)", ID_FRAGMENTED_MP4, ITEM_CHECKBOX                   },
					{ "Limit &Length (seconds)",     ID_LIMIT_LENGTH,   ITEM_CHECKBOX | ITEM_NUMBER, 80 },
					{ "Limit &Size (MB)",            ID_LIMIT_SIZE,     ITEM_CHECKBOX | ITEM_NUMBER, 80 },
					{ NULL },
				},
			},
			{
				.Caption = "&Video",
				.Rect = { 0, ROW0H, COL10W, ROW1H },
				.Items = (Config__DialogItem[])
				{
					{ "&Gamma Correct Resize",               ID_VIDEO_GAMMA_RESIZE ,    ITEM_CHECKBOX     },
					{ "&Improved Color Conversion (BETA)",   ID_VIDEO_IMPROVED_CONVERT, ITEM_CHECKBOX     },
					{ "Codec",                               ID_VIDEO_CODEC,            ITEM_COMBOBOX, 64 },
					{ "Profile",                             ID_VIDEO_PROFILE,          ITEM_COMBOBOX, 64 },
					{ "Max &Width",                          ID_VIDEO_MAX_WIDTH,        ITEM_NUMBER,   64 },
					{ "Max &Height",                         ID_VIDEO_MAX_HEIGHT,       ITEM_NUMBER,   64 },
					{ "Max &Framerate",                      ID_VIDEO_MAX_FRAMERATE,    ITEM_NUMBER,   64 },
					{ "Bitrate (kbit/s)",                    ID_VIDEO_BITRATE,          ITEM_NUMBER,   64 },
					{ NULL },
				},
			},
			{
				.Caption = "&Audio",
				.Rect = { COL10W + PADDING, ROW0H, COL11W, ROW1H },
				.Items = (Config__DialogItem[])
				{
					{ "Capture Au&dio",           ID_AUDIO_CAPTURE,           ITEM_CHECKBOX     },
					{ "Applicatio&n Local Audio", ID_AUDIO_APPLICATION_LOCAL, ITEM_CHECKBOX     },
					{ "Codec",                    ID_AUDIO_CODEC,             ITEM_COMBOBOX, 60 },
					{ "Channels",                 ID_AUDIO_CHANNELS,          ITEM_COMBOBOX, 60 },
					{ "Samplerate",               ID_AUDIO_SAMPLERATE,        ITEM_COMBOBOX, 60 },
					{ "Bitrate (kbit/s)",         ID_AUDIO_BITRATE,           ITEM_COMBOBOX, 60 },
					{ NULL },
				},
			},
			{
				.Caption = "Shor&tcuts",
				.Rect = { 0, ROW0H + ROW1H, COL00W + PADDING + COL01W, ROW2H },
				.Items = (Config__DialogItem[])
				{
					{ "Capture Monitor", ID_SHORTCUT_MONITOR, ITEM_HOTKEY, 64 },
					{ "Capture Window",  ID_SHORTCUT_WINDOW,  ITEM_HOTKEY, 64 },
					{ "Capture Region",  ID_SHORTCUT_REGION,  ITEM_HOTKEY, 64 },
					{ NULL },
				},
			},
			{ NULL },
		},
	};

	BYTE __declspec(align(4)) Data[4096];
	Config__DoDialogLayout(&Dialog, Data, sizeof(Data));

	return (BOOL)DialogBoxIndirectParamW(GetModuleHandleW(NULL), (LPCDLGTEMPLATEW)Data, NULL, Config__DialogProc, (LPARAM)C);
}
