#pragma once

#include "wcap.h"

#define CONFIG_VIDEO_H264 0
#define CONFIG_VIDEO_H265 1

#define CONFIG_VIDEO_BASE 0
#define CONFIG_VIDEO_MAIN 1
#define CONFIG_VIDEO_HIGH 2

#define CONFIG_AUDIO_AAC  0
#define CONFIG_AUDIO_FLAC 1

typedef struct Config {
	// capture
	BOOL ShowNotifications;
	BOOL MouseCursor;
	BOOL OnlyClientArea;
	BOOL HardwareEncoder;
	BOOL CaptureAudio;
	// output
	WCHAR OutputFolder[MAX_PATH];
	BOOL OpenFolder;
	BOOL FragmentedOutput;
	BOOL EnableLimitLength;
	BOOL EnableLimitSize;
	DWORD LimitLength;
	DWORD LimitSize;
	// video
	DWORD VideoCodec;
	DWORD VideoProfile;
	DWORD VideoMaxWidth;
	DWORD VideoMaxHeight;
	DWORD VideoMaxFramerate;
	DWORD VideoBitrate;
	// audio
	DWORD AudioCodec;
	DWORD AudioChannels;
	DWORD AudioSamplerate;
	DWORD AudioBitrate;
} Config;

void Config_Defaults(Config* Config);
void Config_Load(Config* Config, LPCWSTR FileName);
void Config_Save(Config* Config, LPCWSTR FileName);
BOOL Config_ShowDialog(Config* Config, HWND Window);
