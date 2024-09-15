#pragma once

#include "wcap.h"
#include <audioclient.h>

//
// interface
//

typedef struct
{
	IAudioClient* PlayClient;
	IAudioClient* RecordClient;
	IAudioCaptureClient* CaptureClient;
	WAVEFORMATEX* Format;
	uint64_t StartQpc;
	uint64_t StartPos;
	uint64_t Freq;
	bool UseDeviceTimestamp;
	bool CheckDeviceTimestamp;

	bool Stop;
	HANDLE Event;
	HANDLE Thread;

	uint8_t* Buffer;
	uint32_t BufferSize;
	uint32_t BufferRead;
	uint32_t BufferWrite;
}
AudioCapture;

typedef struct
{
	void* Samples;
	size_t Count;
	uint64_t Time; // compatible with QPC
}
AudioCaptureData;

static bool AudioCapture_CanCaptureApplicationLocal(void);

// make sure CoInitializeEx has been called before calling Start()
static bool AudioCapture_Start(AudioCapture* Capture, HWND ApplicationWindow);
static void AudioCapture_Stop(AudioCapture* Capture);
static void AudioCapture_Flush(AudioCapture* Capture);

// expectedTimestamp is used only first time GetData() is called to detect abnormal device timestamps
static bool AudioCapture_GetData(AudioCapture* Capture, AudioCaptureData* Data, uint64_t ExpectedTimestamp);
static void AudioCapture_ReleaseData(AudioCapture* Capture, AudioCaptureData* Data);

//
// implementation
//

#include <mmdeviceapi.h>
#include <audioclientactivationparams.h>
#include <mfapi.h>
#include <avrt.h>

// from ntdll.dll
extern __declspec(dllimport) LONG WINAPI RtlGetVersion(RTL_OSVERSIONINFOW*);

bool AudioCapture_CanCaptureApplicationLocal(void)
{
	RTL_OSVERSIONINFOW Version = { sizeof(Version) };
	RtlGetVersion(&Version);

	// not exactly sure which version
	// available since Windows 10 version 2004, May 2020 Update (20H1), build 10.0.19041.0
	return Version.dwMajorVersion > 10 || (Version.dwMajorVersion == 10 && Version.dwBuildNumber >= 19041);
}

DEFINE_GUID(CLSID_MMDeviceEnumerator,                     0xbcde0395, 0xe52f, 0x467c, 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e);
DEFINE_GUID(IID_IMMDeviceEnumerator,                      0xa95664d2, 0x9614, 0x4f35, 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6);
DEFINE_GUID(IID_IAudioClient,                             0x1cb9ad4c, 0xdbfa, 0x4c32, 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2);
DEFINE_GUID(IID_IAudioCaptureClient,                      0xc8adbd64, 0xe71e, 0x48a0, 0xa4, 0xde, 0x18, 0x5c, 0x39, 0x5c, 0xd3, 0x17);
DEFINE_GUID(IID_IAudioRenderClient,                       0xf294acfc, 0x3146, 0x4483, 0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2);
DEFINE_GUID(IID_IActivateAudioInterfaceCompletionHandler, 0x41d949ab, 0x9862, 0x444a, 0x80, 0xf6, 0xc2, 0x61, 0x33, 0x4d, 0xa5, 0xeb);

typedef struct
{
	IActivateAudioInterfaceCompletionHandler Handler;
	uint32_t ReadyFlag;
}
AudioCaptureActivate;

static HRESULT STDMETHODCALLTYPE AudioCaptureActivate__QueryInterface(IActivateAudioInterfaceCompletionHandler* This, REFIID Riid, void** Object)
{
	if (Object == NULL)
	{
		return E_POINTER;
	}
	if (IsEqualGUID(Riid, &IID_IActivateAudioInterfaceCompletionHandler) ||
		IsEqualGUID(Riid, &IID_IAgileObject) ||
		IsEqualGUID(Riid, &IID_IUnknown))
	{
		*Object = This;
		return S_OK;
	}
	return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE AudioCaptureActivate__AddRef(IActivateAudioInterfaceCompletionHandler* This)
{
	return 1;
}

static ULONG STDMETHODCALLTYPE AudioCaptureActivate__Release(IActivateAudioInterfaceCompletionHandler* This)
{
	return 1;
}

static HRESULT STDMETHODCALLTYPE AudioCaptureActivate__ActivateCompleted(IActivateAudioInterfaceCompletionHandler* This, IActivateAudioInterfaceAsyncOperation* ActivateOperation)
{
	AudioCaptureActivate* Activate = CONTAINING_RECORD(This, AudioCaptureActivate, Handler);

	InterlockedIncrement(&Activate->ReadyFlag);
	WakeByAddressSingle(&Activate->ReadyFlag);

	return S_OK;
}

static IActivateAudioInterfaceCompletionHandlerVtbl AudioCaptureActivateVtbl =
{
	.QueryInterface    = &AudioCaptureActivate__QueryInterface,
	.AddRef            = &AudioCaptureActivate__AddRef,
	.Release           = &AudioCaptureActivate__Release,
	.ActivateCompleted = &AudioCaptureActivate__ActivateCompleted,
};

static DWORD CALLBACK AudioCapture__Thread(LPVOID Arg)
{
	AudioCapture* Capture = Arg;

	DWORD Task = 0;
	HANDLE Handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &Task);
	Assert(Handle);

	IAudioCaptureClient* CaptureClient = Capture->CaptureClient;
	uint32_t BytesPerFrame = Capture->Format->nBlockAlign;
	uint32_t BufferSize = Capture->BufferSize;
	uint32_t BufferWrite = 0;
	HANDLE Event = Capture->Event;

	while (WaitForSingleObject(Event, INFINITE) == WAIT_OBJECT_0)
	{
		if (Capture->Stop)
		{
			break;
		}

		BYTE* Buffer = NULL;
		DWORD Flags = 0;
		UINT32 Frames = 0;
		UINT64 Position = 0; // in sample count from beginning of stream
		UINT64 Timestamp = 0; // in QPC unuts
		while (SUCCEEDED(IAudioCaptureClient_GetBuffer(CaptureClient, &Buffer, &Frames, &Flags, &Position, &Timestamp)) && Frames != 0)
		{
			uint32_t BufferAvailable = BufferSize - (BufferWrite - Capture->BufferRead);

			uint32_t WriteSize = sizeof(Frames) + sizeof(Position) + sizeof(Timestamp) + Frames * BytesPerFrame;
			if (WriteSize <= BufferAvailable)
			{
				uint8_t* BufferPtr = Capture->Buffer + (BufferWrite & (BufferSize - 1));
				CopyMemory(BufferPtr, &Frames, sizeof(Frames)); BufferPtr += sizeof(Frames);
				CopyMemory(BufferPtr, &Position, sizeof(Position)); BufferPtr += sizeof(Position);
				CopyMemory(BufferPtr, &Timestamp, sizeof(Timestamp)); BufferPtr += sizeof(Timestamp);
				if (Flags & AUDCLNT_BUFFERFLAGS_SILENT)
				{
					ZeroMemory(BufferPtr, Frames * BytesPerFrame);
				}
				else
				{
					CopyMemory(BufferPtr, Buffer, Frames * BytesPerFrame);
				}
				BufferWrite = InterlockedAdd(&Capture->BufferWrite, WriteSize);
			}
			else
			{
				// TODO: logging/stats when audio ringbuffer is overflowing
			}

			HR(IAudioCaptureClient_ReleaseBuffer(CaptureClient, Frames));
			Buffer = NULL;
			Frames = 0;
			Position = 0;
			Timestamp = 0;
		}
	}
	
	AvRevertMmThreadCharacteristics(Handle);

	return 0;
}

bool AudioCapture_Start(AudioCapture* Capture, HWND ApplicationWindow)
{
	bool Result = false;

	if (ApplicationWindow)
	{
		DWORD ProcessId;
		DWORD ThreadId = GetWindowThreadProcessId(ApplicationWindow, &ProcessId);
		if (!ThreadId)
		{
			return false;
		}

		AUDIOCLIENT_ACTIVATION_PARAMS Activation =
		{
			.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK,
			.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE,
			.ProcessLoopbackParams.TargetProcessId = ProcessId,
		};

		PROPVARIANT Params =
		{
			.vt = VT_BLOB,
			.blob.cbSize = sizeof(Activation),
			.blob.pBlobData = (BYTE*)&Activation,
		};

		AudioCaptureActivate ActivateCompletion =
		{
			.Handler.lpVtbl = &AudioCaptureActivateVtbl,
		};

		IActivateAudioInterfaceAsyncOperation* AsyncOperation;
		if (FAILED(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, &IID_IAudioClient, &Params, &ActivateCompletion.Handler, &AsyncOperation)))
		{
			return false;
		}

		while (!ActivateCompletion.ReadyFlag)
		{
			uint32_t ReadyFlag = 0;
			WaitOnAddress(&ActivateCompletion.ReadyFlag, &ReadyFlag, sizeof(ReadyFlag), INFINITE);
		}

		HRESULT ActivateResult;
		IUnknown* ActivateUnknown;
		if (SUCCEEDED(IActivateAudioInterfaceAsyncOperation_GetActivateResult(AsyncOperation, &ActivateResult, &ActivateUnknown)) && SUCCEEDED(ActivateResult))
		{
			IAudioClient* Client;
			HR(IUnknown_QueryInterface(ActivateUnknown, &IID_IAudioClient, &Client));
			IUnknown_Release(ActivateUnknown);

			WAVEFORMATEXTENSIBLE* FormatEx = CoTaskMemAlloc(sizeof(*FormatEx));
			Assert(FormatEx);

			// IAudioClient you get from ActivateAudioInterfaceAsync does not support GetMixFormat() and GetDevicePeriod() methods
			FormatEx->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
			FormatEx->Format.nChannels = 2;
			FormatEx->Format.wBitsPerSample = 32;
			FormatEx->Format.nSamplesPerSec = 48000;
			FormatEx->Format.nBlockAlign = (FormatEx->Format.nChannels * FormatEx->Format.wBitsPerSample) / 8;
			FormatEx->Format.nAvgBytesPerSec = FormatEx->Format.nSamplesPerSec * FormatEx->Format.nBlockAlign;
			FormatEx->Format.cbSize = sizeof(*FormatEx) - sizeof(FormatEx->Format);
			FormatEx->Samples.wValidBitsPerSample = FormatEx->Format.wBitsPerSample;
			FormatEx->dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
			FormatEx->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

			DWORD Flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
			HR(IAudioClient_Initialize(Client, AUDCLNT_SHAREMODE_SHARED, Flags, MF_UNITS_PER_SECOND, 0, &FormatEx->Format, NULL));
			HR(IAudioClient_GetService(Client, &IID_IAudioCaptureClient, (void**)&Capture->CaptureClient));

			Capture->PlayClient = NULL;
			Capture->RecordClient = Client;
			Capture->Format = &FormatEx->Format;

			Capture->StartPos = 0;
			Capture->UseDeviceTimestamp = true;
			Capture->CheckDeviceTimestamp = false;

			Result = true;

			IActivateAudioInterfaceAsyncOperation_Release(AsyncOperation);
		}
	}
	else
	{
		IMMDeviceEnumerator* Enumerator;
		HR(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void**)&Enumerator));

		IMMDevice* Device;
		if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(Enumerator, eRender, eConsole, &Device)))
		{
			// no playback device found
			IMMDeviceEnumerator_Release(Enumerator);
			return false;
		}

		// setup playback for slience, otherwise loopback recording does not provide any data if nothing is playing
		{
			IAudioClient* Client;
			HR(IMMDevice_Activate(Device, &IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&Client));

			WAVEFORMATEX* Format;
			HR(IAudioClient_GetMixFormat(Client, &Format));
			HR(IAudioClient_Initialize(Client, AUDCLNT_SHAREMODE_SHARED, 0, MF_UNITS_PER_SECOND, 0, Format, NULL));

			IAudioRenderClient* Render;
			HR(IAudioClient_GetService(Client, &IID_IAudioRenderClient, &Render));

			BYTE* Buffer;
			HR(IAudioRenderClient_GetBuffer(Render, Format->nSamplesPerSec, &Buffer));
			HR(IAudioRenderClient_ReleaseBuffer(Render, Format->nSamplesPerSec, AUDCLNT_BUFFERFLAGS_SILENT));
			IAudioRenderClient_Release(Render);
			CoTaskMemFree(Format);

			HR(IAudioClient_Start(Client));
			Capture->PlayClient = Client;
		}

		// loopback recording
		{
			IAudioClient* Client;
			HR(IMMDevice_Activate(Device, &IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&Client));

			WAVEFORMATEX* Format;
			HR(IAudioClient_GetMixFormat(Client, &Format));

			REFERENCE_TIME DefaultPeriod, MinimumPeriod;
			HR(IAudioClient_GetDevicePeriod(Client, &DefaultPeriod, &MinimumPeriod));

			DWORD Flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
			HR(IAudioClient_Initialize(Client, AUDCLNT_SHAREMODE_SHARED, Flags, DefaultPeriod, 0, Format, NULL));
			HR(IAudioClient_GetService(Client, &IID_IAudioCaptureClient, (void**)&Capture->CaptureClient));

			Capture->RecordClient = Client;
			Capture->Format = Format;

			Capture->StartPos = 0;
			Capture->UseDeviceTimestamp = true;
			Capture->CheckDeviceTimestamp = true;
		}

		Result = true;

		IMMDevice_Release(Device);
		IMMDeviceEnumerator_Release(Enumerator);
	}

	if (Result)
	{
		// it seems process local loopback device does not use any buffering, even when we asked for 1 second of buffer
		// so we must implement our own ringbuffer to be able to dequeue incoming data as fast as possible
		
		DWORD BufferSizeIndex;
		_BitScanReverse(&BufferSizeIndex, max(65535, Capture->Format->nAvgBytesPerSec - 1));
		uint32_t BufferSize = 1 << (BufferSizeIndex + 1);
		Assert(BufferSize % 65536 == 0);

		// allocate ringbuffer for 1 second of data, rounded up to next pow2, at least 64KB
		uint8_t* Placeholder1 = (uint8_t*)VirtualAlloc2(NULL, NULL, 2 * BufferSize, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
		uint8_t* Placeholder2 = (uint8_t*)Placeholder1 + BufferSize;
		Assert(Placeholder1);

		BOOL Ok = VirtualFree(Placeholder1, BufferSize, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
		Assert(Ok);

		HANDLE Section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, BufferSize, NULL);
		Assert(Section);

		void* View1 = MapViewOfFile3(Section, NULL, Placeholder1, 0, BufferSize, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
		Assert(View1);

		void* View2 = MapViewOfFile3(Section, NULL, Placeholder2, 0, BufferSize, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
		Assert(View2);

		CloseHandle(Section);
		VirtualFree(Placeholder1, 0, MEM_RELEASE);
		VirtualFree(Placeholder2, 0, MEM_RELEASE);

		Capture->Buffer = View1;
		Capture->BufferSize = BufferSize;
		Capture->BufferRead = 0;
		Capture->BufferWrite = 0;

		Capture->Stop = false;

		Capture->Event = CreateEventW(NULL, FALSE, FALSE, NULL);
		Assert(Capture->Event);

		Capture->Thread = CreateThread(NULL, 0, &AudioCapture__Thread, Capture, 0, NULL);
		Assert(Capture->Thread);

		HR(IAudioClient_SetEventHandle(Capture->RecordClient, Capture->Event));
		HR(IAudioClient_Start(Capture->RecordClient));

		LARGE_INTEGER Start;
		QueryPerformanceCounter(&Start);
		Capture->StartQpc = Start.QuadPart;

		LARGE_INTEGER Freq;
		QueryPerformanceFrequency(&Freq);
		Capture->Freq = Freq.QuadPart;
	}

	return Result;
}

void AudioCapture_Stop(AudioCapture* Capture)
{
	Capture->Stop = true;
	SetEvent(Capture->Event);
	WaitForSingleObject(Capture->Thread, INFINITE);

	CloseHandle(Capture->Thread);
	CloseHandle(Capture->Event);

	UnmapViewOfFileEx(Capture->Buffer, 0);
	UnmapViewOfFileEx(Capture->Buffer + Capture->BufferSize, 0);

	CoTaskMemFree(Capture->Format);
	if (Capture->PlayClient)
	{
		IAudioClient_Release(Capture->PlayClient);
		Capture->PlayClient = NULL;
	}
	IAudioCaptureClient_Release(Capture->CaptureClient);
	IAudioClient_Release(Capture->RecordClient);
}

void AudioCapture_Flush(AudioCapture* Capture)
{
	if (Capture->PlayClient)
	{
		HR(IAudioClient_Stop(Capture->PlayClient));
	}
	HR(IAudioClient_Stop(Capture->RecordClient));
}

bool AudioCapture_GetData(AudioCapture* Capture, AudioCaptureData* Data, uint64_t ExpectedTimestamp)
{
	uint32_t Frames;
	uint64_t Position;
	uint64_t Timestamp;

	uint32_t AvailableSize = Capture->BufferWrite - Capture->BufferRead;
	if (AvailableSize < sizeof(Frames) + sizeof(Position) + sizeof(Timestamp))
	{
		return false;
	}

	uint8_t* BufferPtr = Capture->Buffer + (Capture->BufferRead & (Capture->BufferSize - 1));
	CopyMemory(&Frames, BufferPtr, sizeof(Frames)); BufferPtr += sizeof(Frames);
	CopyMemory(&Position, BufferPtr, sizeof(Position)); BufferPtr += sizeof(Position);
	CopyMemory(&Timestamp, BufferPtr, sizeof(Timestamp)); BufferPtr += sizeof(Timestamp);

	uint32_t ReadSize = sizeof(Frames) + sizeof(Position) + sizeof(Timestamp) + Frames * Capture->Format->nBlockAlign;
	if (AvailableSize < ReadSize)
	{
		return false;
	}

	if (Capture->CheckDeviceTimestamp)
	{
		// first time we check if device timestamp is resonable - not more than 500 msec away from expected
		if (ExpectedTimestamp)
		{
			const int64_t MaxDelta = 500 * Capture->Freq;
			int64_t Delta = 1000 * (ExpectedTimestamp - Timestamp);

			if (Delta < -MaxDelta || Delta > +MaxDelta)
			{
				Capture->UseDeviceTimestamp = false;
			}
			Capture->StartPos = Position;
		}
		Capture->CheckDeviceTimestamp = false;
	}

	if (Capture->UseDeviceTimestamp)
	{
		Data->Time = MFllMulDiv(Timestamp, Capture->Freq, MF_UNITS_PER_SECOND, 0);
	}
	else
	{
		Data->Time = Capture->StartQpc + MFllMulDiv(Position - Capture->StartPos, Capture->Freq, Capture->Format->nSamplesPerSec, 0);
	}

	Data->Samples = BufferPtr;
	Data->Count = Frames;

	return true;
}

void AudioCapture_ReleaseData(AudioCapture* Capture, AudioCaptureData* Data)
{
	uint32_t ReadSize = (uint32_t)(sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint64_t) + Data->Count * Capture->Format->nBlockAlign);
	Assert(ReadSize <= Capture->BufferWrite - Capture->BufferRead);
	InterlockedAdd(&Capture->BufferRead, ReadSize);
}
