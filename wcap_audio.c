#include "wcap_audio.h"

#include <avrt.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#define PLAYBACK_BUFFER_DURATION_100NS 1000000  // 100 msec
#define CAPTURE_BUFFER_DURATION_100NS  10000000 // 1 second

DEFINE_GUID(CLSID_MMDeviceEnumerator, 0xbcde0395, 0xe52f, 0x467c, 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e);
DEFINE_GUID(IID_IMMDeviceEnumerator,  0xa95664d2, 0x9614, 0x4f35, 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6);
DEFINE_GUID(IID_IAudioClient,         0x1cb9ad4c, 0xdbfa, 0x4c32, 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2);
DEFINE_GUID(IID_IAudioCaptureClient,  0xc8adbd64, 0xe71e, 0x48a0, 0xa4, 0xde, 0x18, 0x5c, 0x39, 0x5c, 0xd3, 0x17);
DEFINE_GUID(IID_IAudioRenderClient,   0xf294acfc, 0x3146, 0x4483, 0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2);

static DWORD WINAPI Audio__PlayThread(LPVOID Arg)
{
	Audio* Audio = Arg;

	IAudioRenderClient* Render;
	HR(IAudioClient_GetService(Audio->Play, &IID_IAudioRenderClient, &Render));

	DWORD TaskIndex = 0;
	HANDLE TaskHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &TaskIndex);
	Assert(TaskHandle);

	UINT32 BufferSize;
	HR(IAudioClient_GetBufferSize(Audio->Play, &BufferSize));

	DWORD FrameSize = Audio->PlayFrameSize;

	for (;;)
	{
		DWORD Wait = WaitForSingleObject(Audio->Event, INFINITE);
		Assert(Wait == WAIT_OBJECT_0);
		if (Audio->Stop)
		{
			break;
		}

		UINT32 BufferFilled;
		HR(IAudioClient_GetCurrentPadding(Audio->Play, &BufferFilled));

		UINT32 FrameCount = BufferSize - BufferFilled;
		if (FrameCount != 0)
		{
			BYTE* Data;
			HR(IAudioRenderClient_GetBuffer(Render, FrameCount, &Data));
			ZeroMemory(Data, FrameCount * FrameSize);
			HR(IAudioRenderClient_ReleaseBuffer(Render, FrameCount, 0));
		}
	}

	AvRevertMmThreadCharacteristics(TaskHandle);
	IAudioRenderClient_Release(Render);

	return 0;
}

void Audio_Init(Audio* Audio)
{
	HR(CoInitializeEx(0, COINIT_MULTITHREADED));
	HR(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (LPVOID*)&Audio->Enumerator));
}

BOOL Audio_Start(Audio* Audio)
{
	IMMDevice* Device;
	if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(Audio->Enumerator, eRender, eConsole, &Device)))
	{
		// no devices found?
		return FALSE;
	}

	// setup playback for slience, otherwise loopback recording does not provide any data
	{
		Audio->Event = CreateEventW(NULL, FALSE, FALSE, NULL);
		Assert(Audio->Event);

		IAudioClient* Client;
		HR(IMMDevice_Activate(Device, &IID_IAudioClient, CLSCTX_ALL, NULL, (LPVOID*)&Client));

		WAVEFORMATEX* Format;
		HR(IAudioClient_GetMixFormat(Client, &Format));
		HR(IAudioClient_Initialize(Client, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, PLAYBACK_BUFFER_DURATION_100NS, 0, Format, NULL));
		HR(IAudioClient_SetEventHandle(Client, Audio->Event));
		HR(IAudioClient_Start(Client));

		Audio->Stop = FALSE;
		Audio->Thread = CreateThread(NULL, 0, &Audio__PlayThread, Audio, 0, NULL);
		Assert(Audio->Thread);

		Audio->Play = Client;
		Audio->PlayFrameSize = Format->nBlockAlign;
		CoTaskMemFree(Format);
	}

	// setup loopback recording
	{
		IAudioClient* Client;
		HR(IMMDevice_Activate(Device, &IID_IAudioClient, CLSCTX_ALL, NULL, (LPVOID*)&Client));

		WAVEFORMATEX* Format;
		HR(IAudioClient_GetMixFormat(Client, &Format));
		HR(IAudioClient_Initialize(Client, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, CAPTURE_BUFFER_DURATION_100NS, 0, Format, NULL));
		HR(IAudioClient_GetService(Client, &IID_IAudioCaptureClient, (LPVOID*)&Audio->Capture));
		HR(IAudioClient_Start(Client));

		Audio->Format = Format;
		Audio->Record = Client;
	}

	IMMDevice_Release(Device);

	return TRUE;
}

void Audio_Flush(Audio* Audio)
{
	IAudioClient_Stop(Audio->Play);
	IAudioClient_Stop(Audio->Record);
}

void Audio_Stop(Audio* Audio)
{
	Audio->Stop = TRUE;
	WaitForSingleObject(Audio->Thread, INFINITE);
	CloseHandle(Audio->Thread);
	CloseHandle(Audio->Event);

	IAudioCaptureClient_Release(Audio->Capture);
	IAudioClient_Release(Audio->Record);
	IAudioClient_Release(Audio->Play);

	CoTaskMemFree(Audio->Format);
}

BOOL Audio_GetNextBuffer(Audio* Audio, LPVOID* Data, UINT32* FrameCount, UINT64* Time)
{
	if (FAILED(IAudioCaptureClient_GetNextPacketSize(Audio->Capture, FrameCount)))
	{
		return FALSE;
	}

	if (*FrameCount == 0)
	{
		HR(IAudioCaptureClient_ReleaseBuffer(Audio->Capture, 0));
		return FALSE;
	}

	BYTE* Buffer;
	DWORD Flags;
	HR(IAudioCaptureClient_GetBuffer(Audio->Capture, &Buffer, FrameCount, &Flags, NULL, Time));

	*Data = Flags & AUDCLNT_BUFFERFLAGS_SILENT ? NULL : Buffer;
	return TRUE;
}

void Audio_ReleaseBuffer(Audio* Audio, UINT32 FrameCount)
{
	HR(IAudioCaptureClient_ReleaseBuffer(Audio->Capture, FrameCount));
}
