#include "wcap_config.h"
#include "wcap_capture.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <knownfolders.h>
#include <windowsx.h>

#define INI_SECTION L"wcap"

// control id's
#define ID_OK                  IDOK     // 1
#define ID_CANCEL              IDCANCEL // 2
#define ID_DEFAULTS            3
#define ID_MOUSE_CURSOR        20
#define ID_ONLY_CLIENT_AREA    30
#define ID_CAPTURE_AUDIO       40
#define ID_GPU_ENCODER         50
#define ID_OUTPUT_FOLDER       60
#define ID_OPEN_FOLDER         70
#define ID_FRAGMENTED_MP4      80
#define ID_LIMIT_LENGTH        90
#define ID_LIMIT_SIZE          100
#define ID_VIDEO_CODEC         110
#define ID_VIDEO_PROFILE       120
#define ID_VIDEO_MAX_WIDTH     130
#define ID_VIDEO_MAX_HEIGHT    140
#define ID_VIDEO_MAX_FRAMERATE 150
#define ID_VIDEO_BITRATE       160
#define ID_AUDIO_CODEC         170
#define ID_AUDIO_CHANNELS      180
#define ID_AUDIO_SAMPLERATE    190
#define ID_AUDIO_BITRATE       200
#define ID_SHORTCUT_MONITOR    210
#define ID_SHORTCUT_WINDOW     220
#define ID_SHORTCUT_RECT       230

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
#define ROW0H 86
#define ROW1H 96
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
struct {
	WNDPROC WindowProc;
	Config* Config;
	int Control;
} gConfigShortcut;

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

	WCHAR KeyText[32];
	UINT ScanCode = MapVirtualKeyW(HOT_GET_KEY(KeyMod), MAPVK_VK_TO_VSC);
	if (GetKeyNameTextW(ScanCode << 16, KeyText, _countof(KeyText)) == 0)
	{
		StrFormat(KeyText, L"[0x%02x]", HOT_GET_KEY(KeyMod));
	}

	StrCatW(Text, KeyText);
}

static void Config__SetDialogValues(HWND Window, Config* Config)
{
	Config__UpdateVideoProfiles(Window, Config->VideoCodec);
	Config__UpdateAudioBitrate(Window, Config->AudioCodec, Config->AudioBitrate);

	// capture
	CheckDlgButton(Window, ID_MOUSE_CURSOR,       Config->MouseCursor);
	CheckDlgButton(Window, ID_ONLY_CLIENT_AREA,   Config->OnlyClientArea);
	CheckDlgButton(Window, ID_CAPTURE_AUDIO,      Config->CaptureAudio);
	CheckDlgButton(Window, ID_GPU_ENCODER,        Config->HardwareEncoder);
	SendDlgItemMessageW(Window, ID_GPU_ENCODER + 1, CB_SETCURSEL, Config->HardwarePreferIntegrated ? 0 : 1, 0);

	// output
	SetDlgItemTextW(Window, ID_OUTPUT_FOLDER,  Config->OutputFolder);
	CheckDlgButton(Window, ID_OPEN_FOLDER,     Config->OpenFolder);
	CheckDlgButton(Window, ID_FRAGMENTED_MP4,  Config->FragmentedOutput);
	CheckDlgButton(Window, ID_LIMIT_LENGTH,    Config->EnableLimitLength);
	CheckDlgButton(Window, ID_LIMIT_SIZE,      Config->EnableLimitSize);
	SetDlgItemInt(Window, ID_LIMIT_LENGTH + 1, Config->LimitLength, FALSE);
	SetDlgItemInt(Window, ID_LIMIT_SIZE + 1,   Config->LimitSize,   FALSE);

	// video
	SendDlgItemMessageW(Window, ID_VIDEO_CODEC, CB_SETCURSEL, Config->VideoCodec, 0);
	Config__SelectVideoProfile(Window, Config->VideoCodec, Config->VideoProfile);
	SetDlgItemInt(Window, ID_VIDEO_MAX_WIDTH,     Config->VideoMaxWidth,     FALSE);
	SetDlgItemInt(Window, ID_VIDEO_MAX_HEIGHT,    Config->VideoMaxHeight,    FALSE);
	SetDlgItemInt(Window, ID_VIDEO_MAX_FRAMERATE, Config->VideoMaxFramerate, FALSE);
	SetDlgItemInt(Window, ID_VIDEO_BITRATE,       Config->VideoBitrate,      FALSE);

	// audio
	SendDlgItemMessageW(Window, ID_AUDIO_CODEC, CB_SETCURSEL, Config->AudioCodec, 0);
	SendDlgItemMessageW(Window, ID_AUDIO_CHANNELS, CB_SETCURSEL, Config->AudioChannels - 1, 0);
	WCHAR Text[64];
	StrFormat(Text, L"%u", Config->AudioSamplerate);
	SendDlgItemMessageW(Window, ID_AUDIO_SAMPLERATE, CB_SELECTSTRING, -1, (LPARAM)Text);

	if (Config->AudioCodec == CONFIG_AUDIO_AAC)
	{
		StrFormat(Text, L"%u", Config->AudioBitrate);
		SendDlgItemMessageW(Window, ID_AUDIO_BITRATE, CB_SELECTSTRING, -1, (LPARAM)Text);
	}
	else if (Config->AudioCodec == CONFIG_AUDIO_FLAC)
	{
		SendDlgItemMessageW(Window, ID_AUDIO_BITRATE, CB_SETCURSEL, 0, 0);
	}

	// shortcuts
	Config__FormatKey(Config->ShortcutMonitor, Text);
	SetDlgItemTextW(Window, ID_SHORTCUT_MONITOR, Text);
	SetWindowLongW(GetDlgItem(Window, ID_SHORTCUT_MONITOR), GWLP_USERDATA, Config->ShortcutMonitor);
	Config__FormatKey(Config->ShortcutWindow, Text);
	SetDlgItemTextW(Window, ID_SHORTCUT_WINDOW, Text);
	SetWindowLongW(GetDlgItem(Window, ID_SHORTCUT_WINDOW), GWLP_USERDATA, Config->ShortcutWindow);
	Config__FormatKey(Config->ShortcutRect, Text);
	SetDlgItemTextW(Window, ID_SHORTCUT_RECT, Text);
	SetWindowLongW(GetDlgItem(Window, ID_SHORTCUT_RECT), GWLP_USERDATA, Config->ShortcutRect);

	EnableWindow(GetDlgItem(Window, ID_GPU_ENCODER + 1),  Config->HardwareEncoder);
	EnableWindow(GetDlgItem(Window, ID_LIMIT_LENGTH + 1), Config->EnableLimitLength);
	EnableWindow(GetDlgItem(Window, ID_LIMIT_SIZE + 1),   Config->EnableLimitSize);
	EnableWindow(GetDlgItem(Window, ID_MOUSE_CURSOR),     Capture_CanHideMouseCursor());
}

void DisableHotKeys(void);
void EnableHotKeys(void);

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
		Config* Config = (struct Config*)LParam;
		SetWindowLongPtrW(Window, GWLP_USERDATA, (LONG_PTR)Config);

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

		Config__SetDialogValues(Window, Config);

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
		Config* Config = (struct Config*)GetWindowLongPtrW(Window, GWLP_USERDATA);
		int Control = LOWORD(WParam);
		if (Control == ID_OK)
		{
			// capture
			Config->MouseCursor       = IsDlgButtonChecked(Window, ID_MOUSE_CURSOR);
			Config->OnlyClientArea    = IsDlgButtonChecked(Window, ID_ONLY_CLIENT_AREA);
			Config->CaptureAudio      = IsDlgButtonChecked(Window, ID_CAPTURE_AUDIO);
			Config->HardwareEncoder   = IsDlgButtonChecked(Window, ID_GPU_ENCODER);
			Config->HardwarePreferIntegrated = (DWORD)SendDlgItemMessageW(Window, ID_GPU_ENCODER + 1, CB_GETCURSEL, 0, 0) == 0;

			// output
			GetDlgItemTextW(Window, ID_OUTPUT_FOLDER, Config->OutputFolder, _countof(Config->OutputFolder));
			Config->OpenFolder        = IsDlgButtonChecked(Window, ID_OPEN_FOLDER);
			Config->FragmentedOutput  = IsDlgButtonChecked(Window, ID_FRAGMENTED_MP4);
			Config->EnableLimitLength = IsDlgButtonChecked(Window, ID_LIMIT_LENGTH);
			Config->EnableLimitSize   = IsDlgButtonChecked(Window, ID_LIMIT_SIZE);
			Config->LimitLength       = GetDlgItemInt(Window,      ID_LIMIT_LENGTH + 1, NULL, FALSE);
			Config->LimitSize         = GetDlgItemInt(Window,      ID_LIMIT_SIZE + 1,   NULL, FALSE);
			// video
			Config->VideoCodec        = (DWORD)SendDlgItemMessageW(Window, ID_VIDEO_CODEC,   CB_GETCURSEL, 0, 0);
			Config->VideoProfile      = Config__GetSelectedVideoProfile(Window);
			Config->VideoMaxWidth     = GetDlgItemInt(Window, ID_VIDEO_MAX_WIDTH,     NULL, FALSE);
			Config->VideoMaxHeight    = GetDlgItemInt(Window, ID_VIDEO_MAX_HEIGHT,    NULL, FALSE);
			Config->VideoMaxFramerate = GetDlgItemInt(Window, ID_VIDEO_MAX_FRAMERATE, NULL, FALSE);
			Config->VideoBitrate      = GetDlgItemInt(Window, ID_VIDEO_BITRATE,       NULL, FALSE);
			// audio
			Config->AudioCodec      = (DWORD)SendDlgItemMessageW(Window, ID_AUDIO_CODEC,    CB_GETCURSEL, 0, 0);
			Config->AudioChannels   = (DWORD)SendDlgItemMessageW(Window, ID_AUDIO_CHANNELS, CB_GETCURSEL, 0, 0) + 1;
			Config->AudioSamplerate = gAudioSamplerates[SendDlgItemMessageW(Window, ID_AUDIO_SAMPLERATE, CB_GETCURSEL, 0, 0)];
			if (Config->AudioCodec == CONFIG_AUDIO_AAC)
			{
				Config->AudioBitrate = gAudioBitrates[SendDlgItemMessageW(Window, ID_AUDIO_BITRATE, CB_GETCURSEL, 0, 0)];
			}
			// shortcuts
			Config->ShortcutMonitor = GetWindowLongW(GetDlgItem(Window, ID_SHORTCUT_MONITOR), GWLP_USERDATA);
			Config->ShortcutWindow  = GetWindowLongW(GetDlgItem(Window, ID_SHORTCUT_WINDOW),  GWLP_USERDATA);
			Config->ShortcutRect    = GetWindowLongW(GetDlgItem(Window, ID_SHORTCUT_RECT),    GWLP_USERDATA);

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
			Config_Defaults(Config);
			Config__SetDialogValues(Window, Config);
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
			Config__UpdateAudioBitrate(Window, (DWORD)Index, Config->AudioBitrate);
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
		          Control == ID_SHORTCUT_RECT) && HIWORD(WParam) == BN_CLICKED)
		{
			if (gConfigShortcut.Control == 0)
			{
				SetDlgItemTextW(Window, Control, L"Press new shortcut, [ESC] to cancel, [BACKSPACE] to disable");

				gConfigShortcut.Control = Control;
				gConfigShortcut.Config = Config;

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

void Config_Defaults(Config* Config)
{
	*Config = (struct Config)
	{
		// capture
		.MouseCursor = TRUE,
		.OnlyClientArea = TRUE,
		.CaptureAudio = TRUE,
		.HardwareEncoder = TRUE,
		.HardwarePreferIntegrated = FALSE,
		// output
		.OpenFolder = TRUE,
		.FragmentedOutput = FALSE,
		.EnableLimitLength = FALSE,
		.EnableLimitSize = FALSE,
		.LimitLength = 60,
		.LimitSize = 8,
		// video
		.VideoCodec = CONFIG_VIDEO_H264,
		.VideoProfile = CONFIG_VIDEO_HIGH,
		.VideoMaxWidth = 1920,
		.VideoMaxHeight = 1080,
		.VideoMaxFramerate = 60,
		.VideoBitrate = 8000,
		// audio
		.AudioCodec = CONFIG_AUDIO_AAC,
		.AudioChannels = 2,
		.AudioSamplerate = 48000,
		.AudioBitrate = 160,
		// shortcuts
		.ShortcutMonitor = HOT_KEY(VK_SNAPSHOT, MOD_CONTROL),
		.ShortcutWindow = HOT_KEY(VK_SNAPSHOT, MOD_CONTROL | MOD_WIN),
		.ShortcutRect = HOT_KEY(VK_SNAPSHOT, MOD_CONTROL | MOD_SHIFT),
	};

	LPWSTR VideoFolder;
	HR(SHGetKnownFolderPath(&FOLDERID_Videos, KF_FLAG_DEFAULT, NULL, &VideoFolder));
	StrCpyNW(Config->OutputFolder, VideoFolder, _countof(Config->OutputFolder));
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

static void Config__ValidateVideoProfile(Config* Config)
{
	const int* Profiles = gValidVideoProfiles[Config->VideoCodec];
	DWORD LastProfile;
	for (int i=0; Profiles[i] != -1; i++)
	{
		LastProfile = Profiles[i];
		if (Config->VideoProfile == LastProfile)
		{
			return;
		}
	}
	Config->VideoProfile = LastProfile;
}

void Config_Load(Config* Config, LPCWSTR FileName)
{
	// capture
	Config__GetBool(FileName, L"MouseCursor",              &Config->MouseCursor);
	Config__GetBool(FileName, L"OnlyClientArea",           &Config->OnlyClientArea);
	Config__GetBool(FileName, L"CaptureAudio",             &Config->CaptureAudio);
	Config__GetBool(FileName, L"HardwareEncoder",          &Config->HardwareEncoder);
	Config__GetBool(FileName, L"HardwarePreferIntegrated", &Config->HardwarePreferIntegrated);
	// output
	WCHAR OutputFolder[MAX_PATH];
	GetPrivateProfileStringW(INI_SECTION, L"OutputFolder", L"", OutputFolder, _countof(OutputFolder), FileName);
	if (OutputFolder[0]) StrCpyW(Config->OutputFolder, OutputFolder);
	Config__GetBool(FileName, L"OpenFolder",        &Config->OpenFolder);
	Config__GetBool(FileName, L"FragmentedOutput",  &Config->FragmentedOutput);
	Config__GetBool(FileName, L"EnableLimitLength", &Config->EnableLimitLength);
	Config__GetBool(FileName, L"EnableLimitSize",   &Config->EnableLimitSize);
	Config__GetInt(FileName,  L"LimitLength",       &Config->LimitLength, NULL);
	Config__GetInt(FileName,  L"LimitSize",         &Config->LimitSize,   NULL);
	// video
	Config__GetStr(FileName, L"VideoCodec",        &Config->VideoCodec,        gVideoCodecs);
	Config__GetStr(FileName, L"VideoProfile",      &Config->VideoProfile,      gVideoProfiles);
	Config__GetInt(FileName, L"VideoMaxWidth",     &Config->VideoMaxWidth,     NULL);
	Config__GetInt(FileName, L"VideoMaxHeight",    &Config->VideoMaxHeight,    NULL);
	Config__GetInt(FileName, L"VideoMaxFramerate", &Config->VideoMaxFramerate, NULL);
	Config__GetInt(FileName, L"VideoBitrate",      &Config->VideoBitrate,      NULL);
	// audio
	Config__GetStr(FileName, L"AudioCodec",      &Config->AudioCodec,      gAudioCodecs);
	Config__GetInt(FileName, L"AudioChannels",   &Config->AudioChannels,   (DWORD[]) { 1, 2, 0 });
	Config__GetInt(FileName, L"AudioSamplerate", &Config->AudioSamplerate, gAudioSamplerates);
	Config__GetInt(FileName, L"AudioBitrate",    &Config->AudioBitrate,    gAudioBitrates);
	// shortcuts
	Config__GetInt(FileName, L"ShortcutMonitor", &Config->ShortcutMonitor, NULL);
	Config__GetInt(FileName, L"ShortcutWindow",  &Config->ShortcutWindow,  NULL);
	Config__GetInt(FileName, L"ShortcutRect",    &Config->ShortcutRect,    NULL);

	Config__ValidateVideoProfile(Config);
}

static void Config__WriteInt(LPCWSTR FileName, LPCWSTR Key, DWORD Value)
{
	WCHAR Text[64];
	StrFormat(Text, L"%u", Value);
	WritePrivateProfileStringW(INI_SECTION, Key, Text, FileName);
}

void Config_Save(Config* Config, LPCWSTR FileName)
{
	// capture
	WritePrivateProfileStringW(INI_SECTION, L"MouseCursor",               Config->MouseCursor              ? L"1" : L"0", FileName);
	WritePrivateProfileStringW(INI_SECTION, L"OnlyClientArea",            Config->OnlyClientArea           ? L"1" : L"0", FileName);
	WritePrivateProfileStringW(INI_SECTION, L"CaptureAudio",              Config->CaptureAudio             ? L"1" : L"0", FileName);
	WritePrivateProfileStringW(INI_SECTION, L"HardwareEncoder",           Config->HardwareEncoder          ? L"1" : L"0", FileName);
	WritePrivateProfileStringW(INI_SECTION, L"HardwarePreferIntegratged", Config->HardwarePreferIntegrated ? L"1" : L"0", FileName);
	// output
	WritePrivateProfileStringW(INI_SECTION, L"OutputFolder",      Config->OutputFolder, FileName);
	WritePrivateProfileStringW(INI_SECTION, L"OpenFolder",        Config->OpenFolder        ? L"1" : L"0", FileName);
	WritePrivateProfileStringW(INI_SECTION, L"FragmentedOutput",  Config->FragmentedOutput  ? L"1" : L"0", FileName);
	WritePrivateProfileStringW(INI_SECTION, L"EnableLimitLength", Config->EnableLimitLength ? L"1" : L"0", FileName);
	WritePrivateProfileStringW(INI_SECTION, L"EnableLimitSize",   Config->EnableLimitSize   ? L"1" : L"0", FileName);
	Config__WriteInt(FileName, L"LimitLength", Config->LimitLength);
	Config__WriteInt(FileName, L"LimitSize", Config->LimitSize);
	// video
	WritePrivateProfileStringW(INI_SECTION, L"VideoCodec",   gVideoCodecs[Config->VideoCodec],     FileName);
	WritePrivateProfileStringW(INI_SECTION, L"VideoProfile", gVideoProfiles[Config->VideoProfile], FileName);
	Config__WriteInt(FileName, L"VideoMaxWidth",     Config->VideoMaxWidth);
	Config__WriteInt(FileName, L"VideoMaxHeight",    Config->VideoMaxHeight);
	Config__WriteInt(FileName, L"VideoMaxFramerate", Config->VideoMaxFramerate);
	Config__WriteInt(FileName, L"VideoBitrate",      Config->VideoBitrate);
	// audio
	WritePrivateProfileStringW(INI_SECTION, L"AudioCodec",    gAudioCodecs[Config->AudioCodec], FileName);
	Config__WriteInt(FileName, L"AudioChannels",   Config->AudioChannels);
	Config__WriteInt(FileName, L"AudioSamplerate", Config->AudioSamplerate);
	Config__WriteInt(FileName, L"AudioBitrate",    Config->AudioBitrate);
	// shortcuts
	Config__WriteInt(FileName, L"ShortcutMonitor", Config->ShortcutMonitor);
	Config__WriteInt(FileName, L"ShortcutWindow",  Config->ShortcutWindow);
	Config__WriteInt(FileName, L"ShortcutRect",    Config->ShortcutRect);
}

BOOL Config_ShowDialog(Config* Config)
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
					{ "&Mouse Cursor",       ID_MOUSE_CURSOR,       ITEM_CHECKBOX                     },
					{ "Only &Client Area",   ID_ONLY_CLIENT_AREA,   ITEM_CHECKBOX                     },
					{ "Capture Au&dio",      ID_CAPTURE_AUDIO,      ITEM_CHECKBOX                     },
					{ "GPU &Encoder",        ID_GPU_ENCODER,        ITEM_CHECKBOX | ITEM_COMBOBOX, 50 },
					{ NULL },
				},
			},
			{
				.Caption = "&Output",
				.Rect = { COL00W + PADDING, 0, COL01W, ROW0H },
				.Items = (Config__DialogItem[])
				{
					{ "",                        ID_OUTPUT_FOLDER,  ITEM_FOLDER                     },
					{ "O&pen When Finished",     ID_OPEN_FOLDER,    ITEM_CHECKBOX                   },
					{ "Fragmented MP&4",         ID_FRAGMENTED_MP4, ITEM_CHECKBOX                   },
					{ "Limit &Length (seconds)", ID_LIMIT_LENGTH,   ITEM_CHECKBOX | ITEM_NUMBER, 80 },
					{ "Limit &Size (MB)",        ID_LIMIT_SIZE,     ITEM_CHECKBOX | ITEM_NUMBER, 80 },
					{ NULL },
				},
			},
			{
				.Caption = "&Video",
				.Rect = { 0, ROW0H, COL10W, ROW1H },
				.Items = (Config__DialogItem[])
				{
					{ "Codec",            ID_VIDEO_CODEC,         ITEM_COMBOBOX, 64 },
					{ "Profile",          ID_VIDEO_PROFILE,       ITEM_COMBOBOX, 64 },
					{ "Max &Width",       ID_VIDEO_MAX_WIDTH,     ITEM_NUMBER,   64 },
					{ "Max &Height",      ID_VIDEO_MAX_HEIGHT,    ITEM_NUMBER,   64 },
					{ "Max &Framerate",   ID_VIDEO_MAX_FRAMERATE, ITEM_NUMBER,   64 },
					{ "Bitrate (kbit/s)", ID_VIDEO_BITRATE,       ITEM_NUMBER,   64 },
					{ NULL },
				},
			},
			{
				.Caption = "&Audio",
				.Rect = { COL10W + PADDING, ROW0H, COL11W, ROW1H },
				.Items = (Config__DialogItem[])
				{
					{ "Codec",            ID_AUDIO_CODEC,      ITEM_COMBOBOX, 60 },
					{ "Channels",         ID_AUDIO_CHANNELS,   ITEM_COMBOBOX, 60 },
					{ "Samplerate",       ID_AUDIO_SAMPLERATE, ITEM_COMBOBOX, 60 },
					{ "Bitrate (kbit/s)", ID_AUDIO_BITRATE,    ITEM_COMBOBOX, 60 },
					{ NULL },
				},
			},
			{
				.Caption = "Shor&tcuts",
				.Rect = { 0, ROW0H + ROW1H, COL00W + PADDING + COL01W, ROW2H },
				.Items = (Config__DialogItem[])
				{
					{ "Capture Monitor",   ID_SHORTCUT_MONITOR, ITEM_HOTKEY, 64 },
					{ "Capture Window",    ID_SHORTCUT_WINDOW,  ITEM_HOTKEY, 64 },
					{ "Capture Rectangle", ID_SHORTCUT_RECT,    ITEM_HOTKEY, 64 },
					{ NULL },
				},
			},
			{ NULL },
		},
	};

	BYTE __declspec(align(4)) Data[4096];
	Config__DoDialogLayout(&Dialog, Data, sizeof(Data));

	return (BOOL)DialogBoxIndirectParamW(GetModuleHandleW(NULL), (LPCDLGTEMPLATEW)Data, NULL, Config__DialogProc, (LPARAM)Config);
}
