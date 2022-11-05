#pragma once

#include "wcap.h"
#include <audioclient.h>
#include <stdbool.h>
#include <stdint.h>

// interface

typedef struct {
	IAudioClient* playClient;
	IAudioClient* captureClient;
	IAudioCaptureClient* capture;
	WAVEFORMATEX* format;
} AudioCapture;

typedef struct {
	void* samples;
	size_t count;
	uint64_t time;
} AudioCaptureData;

// make sure CoInitializeEx has been called before calling Start()
static bool AudioCapture_Start(AudioCapture* capture, uint64_t duration_100ns);
static void AudioCapture_Stop(AudioCapture* capture);
static void AudioCapture_Flush(AudioCapture* capture);

static bool AudioCapture_GetData(AudioCapture* capture, AudioCaptureData* data);
static void AudioCapture_ReleaseData(AudioCapture* capture, AudioCaptureData* data);

// implementation

#include <mmdeviceapi.h>

bool AudioCapture_Start(AudioCapture* capture, uint64_t duration_100ns)
{
	static const GUID CLSID_MMDeviceEnumerator = { 0xbcde0395, 0xe52f, 0x467c, { 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e } };
	static const GUID IID_IMMDeviceEnumerator  = { 0xa95664d2, 0x9614, 0x4f35, { 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6 } };
	static const GUID IID_IAudioClient         = { 0x1cb9ad4c, 0xdbfa, 0x4c32, { 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2 } };
	static const GUID IID_IAudioCaptureClient  = { 0xc8adbd64, 0xe71e, 0x48a0, { 0xa4, 0xde, 0x18, 0x5c, 0x39, 0x5c, 0xd3, 0x17 } };
	static const GUID IID_IAudioRenderClient   = { 0xf294acfc, 0x3146, 0x4483, { 0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2 } };

	bool result = false;

	IMMDeviceEnumerator* enumerator;
	HR(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (LPVOID*)&enumerator));

	IMMDevice* device;
	if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &device)))
	{
		// no playback device found
	}
	else
	{
		// setup playback for slience, otherwise loopback recording does not provide any data if nothing is playing
		{
			REFERENCE_TIME length = 10 * 1000 * 1000; // 1 second in 100 nsec units

			IAudioClient* client;
			HR(IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_ALL, NULL, (LPVOID*)&client));

			WAVEFORMATEX* format;
			HR(IAudioClient_GetMixFormat(client, &format));
			HR(IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, 0, length, 0, format, NULL));

			IAudioRenderClient* render;
			HR(IAudioClient_GetService(client, &IID_IAudioRenderClient, &render));

			BYTE* buffer;
			HR(IAudioRenderClient_GetBuffer(render, format->nSamplesPerSec, &buffer));
			HR(IAudioRenderClient_ReleaseBuffer(render, format->nSamplesPerSec, AUDCLNT_BUFFERFLAGS_SILENT));
			IAudioRenderClient_Release(render);
			CoTaskMemFree(format);

			HR(IAudioClient_Start(client));
			capture->playClient = client;
		}

		// loopback recording
		{
			IAudioClient* client;
			HR(IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_ALL, NULL, (LPVOID*)&client));

			WAVEFORMATEX* format;
			HR(IAudioClient_GetMixFormat(client, &format));
			HR(IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, duration_100ns, 0, format, NULL));
			HR(IAudioClient_GetService(client, &IID_IAudioCaptureClient, (LPVOID*)&capture->capture));

			HR(IAudioClient_Start(client));
			capture->captureClient = client;
			capture->format = format;
		}

		result = true;
	
		IMMDevice_Release(device);
	}

	IMMDeviceEnumerator_Release(enumerator);
	
	return result;
}

void AudioCapture_Stop(AudioCapture* capture)
{
	CoTaskMemFree(capture->format);
	IAudioCaptureClient_Release(capture->capture);
	IAudioClient_Release(capture->playClient);
	IAudioClient_Release(capture->captureClient);
}

void AudioCapture_Flush(AudioCapture* capture)
{
	HR(IAudioClient_Stop(capture->playClient));
	HR(IAudioClient_Stop(capture->captureClient));
}

bool AudioCapture_GetData(AudioCapture* capture, AudioCaptureData* data)
{
	UINT32 frames;
	if (FAILED(IAudioCaptureClient_GetNextPacketSize(capture->capture, &frames)) || frames == 0)
	{
		return false;
	}

	BYTE* buffer;
	DWORD flags;
	UINT64 position;
	if (FAILED(IAudioCaptureClient_GetBuffer(capture->capture, &buffer, &frames, &flags, NULL, &position)))
	{
		return false;
	}

	data->samples = buffer;
	data->count = frames;
	data->time = position;
	return true;
}

void AudioCapture_ReleaseData(AudioCapture* capture, AudioCaptureData* data)
{
	HR(IAudioCaptureClient_ReleaseBuffer(capture->capture, (UINT32)data->count));
}
