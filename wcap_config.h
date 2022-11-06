#pragma once

#include "wcap.h"

#define CONFIG_VIDEO_H264 0
#define CONFIG_VIDEO_H265 1

#define CONFIG_VIDEO_BASE    0
#define CONFIG_VIDEO_MAIN    1
#define CONFIG_VIDEO_HIGH    2
#define CONFIG_VIDEO_MAIN_10 3

#define CONFIG_AUDIO_AAC  0
#define CONFIG_AUDIO_FLAC 1

typedef struct Config {
	// capture
	BOOL MouseCursor;
	BOOL OnlyClientArea;
	BOOL CaptureAudio;
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
	// shortcuts
	DWORD ShortcutMonitor;
	DWORD ShortcutWindow;
	DWORD ShortcutRect;
} Config;

#define HOT_KEY(Key, Mod) ((Key) | ((Mod) << 24))
#define HOT_GET_KEY(KeyMod) ((KeyMod) & 0xffffff)
#define HOT_GET_MOD(KeyMod) (((KeyMod) >> 24) & 0xff)

void Config_Defaults(Config* Config);
void Config_Load(Config* Config, LPCWSTR FileName);
void Config_Save(Config* Config, LPCWSTR FileName);
BOOL Config_ShowDialog(Config* Config);
