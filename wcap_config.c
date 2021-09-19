#include "wcap_config.h"
#include "wcap_capture.h"

#define INI_SECTION L"wcap"

#define ID_OK                         IDOK     // 1
#define ID_CANCEL                     IDCANCEL // 2
#define ID_SHOW_NOTIFICATIONS         3
#define ID_OPEN_VIDEO_FOLDER          4
#define ID_MOUSE_CURSOR               5
#define ID_ONLY_CLIENT_AREA           6
#define ID_FRAGMENTED_MP4             7
#define ID_HARDWARE_ENCODER           8
#define ID_MAX_VIDEO_WIDTH_LABEL      9
#define ID_MAX_VIDEO_WIDTH           10
#define ID_MAX_VIDEO_HEIGHT_LABEL    11
#define ID_MAX_VIDEO_HEIGHT          12
#define ID_MAX_VIDEO_FRAMERATE_LABEL 13
#define ID_MAX_VIDEO_FRAMERATE       14
#define ID_VIDEO_BITRATE_LABEL       15
#define ID_VIDEO_BITRATE             16

#define CONTROL_BUTTON    0x0080
#define CONTROL_EDIT      0x0081
#define CONTROL_STATIC    0x0082
#define CONTROL_LISTBOX   0x0083
#define CONTROL_SCROLLBAR 0x0084
#define CONTROL_COMBOBOX  0x0085

static LRESULT CALLBACK Config__DialogProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
	if (Message == WM_INITDIALOG)
	{
		Config* C = (Config*)LParam;
		SetWindowLongPtrW(Window, GWLP_USERDATA, (LONG_PTR)C);

		CheckDlgButton(Window, ID_SHOW_NOTIFICATIONS,  C->ShowNotifications);
		CheckDlgButton(Window, ID_OPEN_VIDEO_FOLDER,   C->OpenVideoFolder);
		CheckDlgButton(Window, ID_MOUSE_CURSOR,        C->MouseCursor);
		CheckDlgButton(Window, ID_ONLY_CLIENT_AREA,    C->OnlyClientArea);
		CheckDlgButton(Window, ID_HARDWARE_ENCODER,    C->HardwareEncoder);
		CheckDlgButton(Window, ID_FRAGMENTED_MP4,      C->FragmentedOutput);
		SetDlgItemInt(Window,  ID_MAX_VIDEO_WIDTH,     C->MaxVideoWidth,     FALSE);
		SetDlgItemInt(Window,  ID_MAX_VIDEO_HEIGHT,    C->MaxVideoHeight,    FALSE);
		SetDlgItemInt(Window,  ID_MAX_VIDEO_FRAMERATE, C->MaxVideoFramerate, FALSE);
		SetDlgItemInt(Window,  ID_VIDEO_BITRATE,       C->VideoBitrate,      FALSE);

		SetForegroundWindow(Window);
		return TRUE;
	}
	else if (Message == WM_COMMAND)
	{
		if (LOWORD(WParam) == ID_OK)
		{
			Config* C = (Config*)GetWindowLongPtrW(Window, GWLP_USERDATA);
			C->ShowNotifications = IsDlgButtonChecked(Window, ID_SHOW_NOTIFICATIONS);
			C->OpenVideoFolder   = IsDlgButtonChecked(Window, ID_OPEN_VIDEO_FOLDER);
			C->MouseCursor       = IsDlgButtonChecked(Window, ID_MOUSE_CURSOR);
			C->OnlyClientArea    = IsDlgButtonChecked(Window, ID_ONLY_CLIENT_AREA);
			C->HardwareEncoder   = IsDlgButtonChecked(Window, ID_HARDWARE_ENCODER);
			C->FragmentedOutput  = IsDlgButtonChecked(Window, ID_FRAGMENTED_MP4);
			C->MaxVideoWidth     = GetDlgItemInt(Window,      ID_MAX_VIDEO_WIDTH,     NULL, FALSE);
			C->MaxVideoHeight    = GetDlgItemInt(Window,      ID_MAX_VIDEO_HEIGHT,    NULL, FALSE);
			C->MaxVideoFramerate = GetDlgItemInt(Window,      ID_MAX_VIDEO_FRAMERATE, NULL, FALSE);
			C->VideoBitrate      = GetDlgItemInt(Window,      ID_VIDEO_BITRATE,       NULL, FALSE);
			EndDialog(Window, TRUE);
			return TRUE;
		}
		else if (LOWORD(WParam) == ID_CANCEL)
		{
			EndDialog(Window, FALSE);
			return TRUE;
		}
	}
	return FALSE;
}

typedef struct {
	const char* Text;
	const WORD Id;
	const WORD Control;
	const DWORD Style;
} Config__DialogItem;

typedef struct {
	int Width;
	const Config__DialogItem* Rows;
} Config__DialogColumn;

typedef struct {
	const char* Title;
	WORD FontSize;
	const char* Font;
	const Config__DialogColumn* Columns;
} Config__DialogLayout;

static void* Config__Align(BYTE* Data, SIZE_T Size)
{
	SIZE_T Pointer = (SIZE_T)Data;
	return Data + ((Pointer + Size - 1) & ~(Size - 1)) - Pointer;
}

static BYTE* Config__DoDialogItem(BYTE* Data, const Config__DialogItem* Item, int x, int y, int w, int h)
{
	Data = Config__Align(Data, sizeof(DWORD));

	*(DLGITEMTEMPLATE*)Data = (DLGITEMTEMPLATE)
	{
		.style = Item->Style | WS_CHILD | WS_VISIBLE | (Item->Control == CONTROL_STATIC ? 0 : WS_TABSTOP),
		.x = x,
		.y = y + (Item->Control == CONTROL_STATIC ? 2 : 0),
		.cx = w,
		.cy = h - (Item->Control == CONTROL_EDIT ? 2 : 0) - (Item->Control == CONTROL_STATIC ? 2 : 0),
		.id = Item->Id,
	};
	Data += sizeof(DLGITEMTEMPLATE);

	// window class
	Data = Config__Align(Data, sizeof(WORD));
	*(WORD*)Data = 0xffff;
	Data += sizeof(WORD);
	*(WORD*)Data = Item->Control;
	Data += sizeof(WORD);

	// item text
	Data = Config__Align(Data, sizeof(WCHAR));
	DWORD ItemChars = MultiByteToWideChar(CP_UTF8, 0, Item->Text, -1, (WCHAR*)Data, 128);
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

	const int DialogPadding = 4;
	const int ButtonWidth = 50;
	const int RowHeight = 14;

	int x = DialogPadding;

	int MaxY = 0;
	int ItemCount = 0;

	static const Config__DialogItem OkButton     = { "OK",     ID_OK,     CONTROL_BUTTON, BS_DEFPUSHBUTTON };
	static const Config__DialogItem CancelButton = { "Cancel", ID_CANCEL, CONTROL_BUTTON, BS_PUSHBUTTON    };

	ItemCount += 2;

	DLGITEMTEMPLATE* OkData = Config__Align(Data, sizeof(DWORD));
	Data = Config__DoDialogItem(Data, &OkButton, 0, 0, ButtonWidth, RowHeight - 2);

	DLGITEMTEMPLATE* CancelData = Config__Align(Data, sizeof(DWORD));
	Data = Config__DoDialogItem(Data, &CancelButton, 0, 0, ButtonWidth, RowHeight - 2);

	for (int c = 0; Layout->Columns[c].Width; c++)
	{
		const Config__DialogColumn* Column = &Layout->Columns[c];

		int y = DialogPadding;
		for (int r = 0; Column->Rows[r].Text; r++)
		{
			const Config__DialogItem* Item = &Column->Rows[r];

			if (Item->Text[0] != 0)
			{
				ItemCount++;

				Data = Config__DoDialogItem(Data, Item, x, y, Column->Width, RowHeight);
				if (Item->Control == CONTROL_STATIC)
				{
					ItemCount++;
					Data = Config__DoDialogItem(Data, &Column[1].Rows[r], x + Column->Width, y, Column[1].Width, RowHeight);
				}
			}
			y += RowHeight;
		}
		x += Column->Width;
		MaxY = max(MaxY, y);
	}

	OkData->x = x - 2 * ButtonWidth - DialogPadding;
	OkData->y = MaxY - RowHeight + 2;

	CancelData->x = x - ButtonWidth;
	CancelData->y = MaxY - RowHeight + 2;

	x += DialogPadding;
	MaxY += DialogPadding;

	*Dialog = (DLGTEMPLATE)
	{
		.style = DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU,
		.cdit = ItemCount,
		.cx = x,
		.cy = MaxY,
	};

	Assert(Data <= End);
}

void Config_Load(Config* Config, LPCWSTR FileName)
{
	LPCWSTR wcap = L"wcap";

	Config->ShowNotifications = GetPrivateProfileIntW(INI_SECTION, L"ShowNotifications", TRUE,  FileName) != 0;
	Config->OpenVideoFolder   = GetPrivateProfileIntW(INI_SECTION, L"OpenVideoFolder",   TRUE,  FileName) != 0;
	Config->MouseCursor       = GetPrivateProfileIntW(INI_SECTION, L"MouseCursor",       TRUE,  FileName) != 0;
	Config->OnlyClientArea    = GetPrivateProfileIntW(INI_SECTION, L"OnlyClientArea",    TRUE,  FileName) != 0;
	Config->FragmentedOutput  = GetPrivateProfileIntW(INI_SECTION, L"FragmentedOutput",  FALSE, FileName) != 0;
	Config->HardwareEncoder   = GetPrivateProfileIntW(INI_SECTION, L"HardwareEncoder",   TRUE,  FileName) != 0;
	Config->MaxVideoWidth     = GetPrivateProfileIntW(INI_SECTION, L"MaxVideoWidth",     0,     FileName);
	Config->MaxVideoHeight    = GetPrivateProfileIntW(INI_SECTION, L"MaxVideoHeight",    0,     FileName);
	Config->MaxVideoFramerate = GetPrivateProfileIntW(INI_SECTION, L"MaxVideoFramerate", 60,    FileName);
	Config->VideoBitrate      = GetPrivateProfileIntW(INI_SECTION, L"VideoBitrate",      8000,  FileName);
}

void Config_Save(Config* Config, LPCWSTR FileName)
{
	LPCWSTR wcap = L"wcap";

	WritePrivateProfileStringW(INI_SECTION, L"ShowNotifications", Config->ShowNotifications ? L"1" : L"0", FileName);
	WritePrivateProfileStringW(INI_SECTION, L"OpenVideoFolder",   Config->OpenVideoFolder   ? L"1" : L"0", FileName);
	WritePrivateProfileStringW(INI_SECTION, L"MouseCursor",       Config->MouseCursor       ? L"1" : L"0", FileName);
	WritePrivateProfileStringW(INI_SECTION, L"OnlyClientArea",    Config->OnlyClientArea    ? L"1" : L"0", FileName);
	WritePrivateProfileStringW(INI_SECTION, L"FragmentedOutput",  Config->FragmentedOutput  ? L"1" : L"0", FileName);
	WritePrivateProfileStringW(INI_SECTION, L"HardwareEncoder",   Config->HardwareEncoder   ? L"1" : L"0", FileName);

	WCHAR Text[64];

	wsprintfW(Text, L"%u", Config->MaxVideoWidth);
	WritePrivateProfileStringW(INI_SECTION, L"MaxVideoWidth", Text, FileName);

	wsprintfW(Text, L"%u", Config->MaxVideoHeight);
	WritePrivateProfileStringW(INI_SECTION, L"MaxVideoHeight", Text, FileName);

	wsprintfW(Text, L"%u", Config->MaxVideoFramerate);
	WritePrivateProfileStringW(INI_SECTION, L"MaxVideoFramerate", Text, FileName);

	wsprintfW(Text, L"%u", Config->VideoBitrate);
	WritePrivateProfileStringW(INI_SECTION, L"VideoBitrate", Text, FileName);
}

BOOL Config_ShowDialog(Config* Config, HWND Window)
{
	DWORD MouseCursorStyle = (Capture_CanHideMouseCursor() ? 0 : WS_DISABLED) | BS_AUTOCHECKBOX;

	Config__DialogLayout Dialog = (Config__DialogLayout)
	{
		.Title = "wcap Settings",
		.FontSize = 9,
		.Font = "Segoe UI",
		.Columns = (Config__DialogColumn[])
		{
			{
				.Width = 90,
				.Rows = (Config__DialogItem[])
				{
					{ "Show &Notifications", ID_SHOW_NOTIFICATIONS, CONTROL_BUTTON, BS_AUTOCHECKBOX  },
					{ "O&pen Video Folder",  ID_OPEN_VIDEO_FOLDER,  CONTROL_BUTTON, BS_AUTOCHECKBOX  },
					{ "&Mouse Cursor",       ID_MOUSE_CURSOR,       CONTROL_BUTTON, MouseCursorStyle },
					{ "Only &Client Area",   ID_ONLY_CLIENT_AREA,   CONTROL_BUTTON, BS_AUTOCHECKBOX  },
					{ "Fragmented MP&4",     ID_FRAGMENTED_MP4,     CONTROL_BUTTON, BS_AUTOCHECKBOX  },
					{ "Hardware &Encoder",   ID_HARDWARE_ENCODER,   CONTROL_BUTTON, BS_AUTOCHECKBOX  },
					{ NULL },
				},
			},
			{
				.Width = 72,
				.Rows = (Config__DialogItem[])
				{
					{ "Max Video &Width",        ID_MAX_VIDEO_WIDTH_LABEL,     CONTROL_STATIC, SS_LEFT },
					{ "Max Video &Height",       ID_MAX_VIDEO_HEIGHT_LABEL,    CONTROL_STATIC, SS_LEFT },
					{ "Max Video &Framerate",    ID_MAX_VIDEO_FRAMERATE_LABEL, CONTROL_STATIC, SS_LEFT },
					{ "&Video Bitrate (kbit/s)", ID_VIDEO_BITRATE_LABEL,       CONTROL_STATIC, SS_LEFT },
					{ NULL },
				},
			},
			{
				.Width = 40,
				.Rows = (Config__DialogItem[])
				{
					{ "", ID_MAX_VIDEO_WIDTH,     CONTROL_EDIT, WS_BORDER | ES_RIGHT | ES_NUMBER },
					{ "", ID_MAX_VIDEO_HEIGHT,    CONTROL_EDIT, WS_BORDER | ES_RIGHT | ES_NUMBER },
					{ "", ID_MAX_VIDEO_FRAMERATE, CONTROL_EDIT, WS_BORDER | ES_RIGHT | ES_NUMBER },
					{ "", ID_VIDEO_BITRATE,       CONTROL_EDIT, WS_BORDER | ES_RIGHT | ES_NUMBER },
					{ NULL },
				},
			},
			{ 0 },
		},
	};

	BYTE __declspec(align(4)) Data[4096];
	Config__DoDialogLayout(&Dialog, Data, sizeof(Data));

	return (BOOL)DialogBoxIndirectParamW(GetModuleHandleW(NULL), (LPCDLGTEMPLATEW)Data, Window, Config__DialogProc, (LPARAM)Config);
}
