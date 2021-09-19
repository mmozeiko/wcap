#include "wcap.h"

#include <d3d11.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#define ENCODER_BUFFER_COUNT 8

typedef struct Encoder {
	DWORD InputWidth;   // width to what input will be cropped
	DWORD InputHeight;  // height to what input will be cropped
	DWORD OutputWidth;  // width of video output
	DWORD OutputHeight; // height of video output
	DWORD FramerateNum; // video output framerate numerator
	DWORD FramerateDen; // video output framerate denumerator
	UINT64 StartTime;   // time in QPC ticks since first call of NewFrame

	IMFAsyncCallback SampleCallback;
	IMFDXGIDeviceManager* Manager;
	ID3D11Device* Device;
	ID3D11DeviceContext* Context;
	IMFSinkWriter* Writer;
	int VideoStreamIndex;

	ID3D11Texture2D*        Texture;
	ID3D11RenderTargetView* TextureView[ENCODER_BUFFER_COUNT];
	IMFSample*              Sample[ENCODER_BUFFER_COUNT];
	IMFTrackedSample*       Tracked[ENCODER_BUFFER_COUNT];

	DWORD         SampleIndex; // next index to use for frame submission
	volatile LONG SampleCount; // how many samples are currently available to use
} Encoder;

typedef struct {
	BOOL FragmentedOutput;
	BOOL HardwareEncoder;
	DWORD Width;
	DWORD Height;
	DWORD MaxWidth;
	DWORD MaxHeight;
	DWORD FramerateNum;
	DWORD FramerateDen;
	DWORD VideoBitrate;
} EncoderConfig;

void Encoder_Init(Encoder* Encoder, ID3D11Device* Device, ID3D11DeviceContext* Context);
BOOL Encoder_Start(Encoder* Encoder, LPWSTR FileName, const EncoderConfig* Config);
void Encoder_Stop(Encoder* Encoder);

BOOL Encoder_NewFrame(Encoder* Encoder, ID3D11Texture2D* Texture, RECT Rect, UINT64 Time, UINT64 TimePeriod);
void Encoder_GetStats(Encoder* Encoder, DWORD* Bitrate, DWORD* LengthMsec, UINT64* FileSize);
