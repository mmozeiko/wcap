#include "wcap.h"
#include "wcap_config.h"

#include <d3d11.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#define ENCODER_VIDEO_BUFFER_COUNT 8
#define ENCODER_AUDIO_BUFFER_COUNT 16

typedef struct Encoder {
	DWORD InputWidth;   // width to what input will be cropped
	DWORD InputHeight;  // height to what input will be cropped
	DWORD OutputWidth;  // width of video output
	DWORD OutputHeight; // height of video output
	DWORD FramerateNum; // video output framerate numerator
	DWORD FramerateDen; // video output framerate denumerator
	UINT64 StartTime;   // time in QPC ticks since first call of NewFrame

	IMFAsyncCallback VideoSampleCallback;
	IMFAsyncCallback AudioSampleCallback;
	IMFDXGIDeviceManager* Manager;
	ID3D11Device* Device;
	ID3D11DeviceContext* Context;
	IMFSinkWriter* Writer;
	int VideoStreamIndex;
	int AudioStreamIndex;

	ID3D11ComputeShader* ResizeShader;
	ID3D11ComputeShader* ConvertShader;

	// RGB input texture
	ID3D11Texture2D* InputTexture;
	ID3D11RenderTargetView* InputRenderTarget;
	ID3D11UnorderedAccessView* InputTextureView;

	// RGB resized texture
	ID3D11Texture2D* ResizedTexture;
	ID3D11UnorderedAccessView* ResizedTextureView;

	// NV12 converted texture
	ID3D11Texture2D*           ConvertedTexture[ENCODER_VIDEO_BUFFER_COUNT];
	ID3D11UnorderedAccessView* ConvertedTextureViewY[ENCODER_VIDEO_BUFFER_COUNT];
	ID3D11UnorderedAccessView* ConvertedTextureViewUV[ENCODER_VIDEO_BUFFER_COUNT];
	IMFSample*                 VideoSample[ENCODER_VIDEO_BUFFER_COUNT];
	IMFTrackedSample*          VideoTracked[ENCODER_VIDEO_BUFFER_COUNT];

	DWORD         VideoIndex; // next index to use
	volatile LONG VideoCount; // how many samples are currently available to use

	IMFTransform* Resampler;
	IMFSample* AudioSample[ENCODER_AUDIO_BUFFER_COUNT];
	IMFTrackedSample* AudioTracked[ENCODER_AUDIO_BUFFER_COUNT];
	IMFSample* AudioInputSample;
	IMFMediaBuffer* AudioInputBuffer;
	HANDLE AudioSemaphore;
	DWORD AudioFrameSize;
	DWORD AudioSampleRate;
	DWORD AudioIndex; // next index to use

} Encoder;

typedef struct {
	DWORD Width;
	DWORD Height;
	DWORD FramerateNum;
	DWORD FramerateDen;
	WAVEFORMATEX* AudioFormat;
	Config* Config;
} EncoderConfig;

void Encoder_Init(Encoder* Encoder, ID3D11Device* Device, ID3D11DeviceContext* Context);
BOOL Encoder_Start(Encoder* Encoder, LPWSTR FileName, const EncoderConfig* Config);
void Encoder_Stop(Encoder* Encoder);

BOOL Encoder_NewFrame(Encoder* Encoder, ID3D11Texture2D* Texture, RECT Rect, UINT64 Time, UINT64 TimePeriod);
void Encoder_NewSamples(Encoder* Encoder, LPCVOID Samples, DWORD VideoCount, UINT64 Time, UINT64 TimePeriod);
void Encoder_GetStats(Encoder* Encoder, DWORD* Bitrate, DWORD* LengthMsec, UINT64* FileSize);
