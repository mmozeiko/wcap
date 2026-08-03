#pragma once

#include "wcap.h"
#include <audioclient.h>
#include <mfapi.h>

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

	// microphone capture
	IAudioClient* MicClient;
	IAudioCaptureClient* MicCaptureClient;
	HANDLE MicEvent;
	WAVEFORMATEX* MicFormat;
	float MicGain;
	bool MicEnabled;

	bool Stop;
	HANDLE Event;
	HANDLE Thread;

	uint8_t* Buffer;
	uint32_t BufferSize;
	_Atomic(uint32_t) BufferRead;
	_Atomic(uint32_t) BufferWrite;
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
// CaptureMicrophone: enable mic capture and mix into loopback audio
// MicrophoneGain: gain multiplier (1.0 = 100%, 0.5 = 50%, 2.0 = 200%)
static bool AudioCapture_Start(AudioCapture* Capture, HWND ApplicationWindow, bool CaptureMicrophone, float MicrophoneGain);
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
	_Atomic(uint32_t) ReadyFlag;
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

	atomic_store_explicit(&Activate->ReadyFlag, 1, memory_order_release);
	WakeByAddressSingle((PVOID)&Activate->ReadyFlag);

	return S_OK;
}

static IActivateAudioInterfaceCompletionHandlerVtbl AudioCaptureActivateVtbl =
{
	.QueryInterface    = &AudioCaptureActivate__QueryInterface,
	.AddRef            = &AudioCaptureActivate__AddRef,
	.Release           = &AudioCaptureActivate__Release,
	.ActivateCompleted = &AudioCaptureActivate__ActivateCompleted,
};

// Helper: convert audio sample to float (for mixing)
static float AudioCapture__SampleToFloat(const BYTE* Sample, WORD BitsPerSample, bool IsFloat)
{
	if (IsFloat)
	{
		if (BitsPerSample == 32)
		{
			return *(const float*)Sample;
		}
		else if (BitsPerSample == 64)
		{
			return (float)(*(const double*)Sample);
		}
	}
	else
	{
		if (BitsPerSample == 16)
		{
			return (float)(*(const INT16*)Sample) / 32768.0f;
		}
		else if (BitsPerSample == 24)
		{
			INT32 Val = (INT32)(Sample[0] | (Sample[1] << 8) | (Sample[2] << 16));
			if (Val & 0x800000) Val |= 0xFF000000; // sign extend
			return (float)Val / 8388608.0f;
		}
		else if (BitsPerSample == 32)
		{
			return (float)(*(const INT32*)Sample) / 2147483648.0f;
		}
	}
	return 0.0f;
}

// Helper: convert float to audio sample
static void AudioCapture__FloatToSample(BYTE* Sample, float Value, WORD BitsPerSample, bool IsFloat)
{
	// clamp
	if (Value > 1.0f) Value = 1.0f;
	if (Value < -1.0f) Value = -1.0f;

	if (IsFloat)
	{
		if (BitsPerSample == 32)
		{
			*(float*)Sample = Value;
		}
		else if (BitsPerSample == 64)
		{
			*(double*)Sample = (double)Value;
		}
	}
	else
	{
		if (BitsPerSample == 16)
		{
			*(INT16*)Sample = (INT16)(Value * 32767.0f);
		}
		else if (BitsPerSample == 24)
		{
			INT32 Val = (INT32)(Value * 8388607.0f);
			Sample[0] = (BYTE)(Val & 0xFF);
			Sample[1] = (BYTE)((Val >> 8) & 0xFF);
			Sample[2] = (BYTE)((Val >> 16) & 0xFF);
		}
		else if (BitsPerSample == 32)
		{
			*(INT32*)Sample = (INT32)(Value * 2147483647.0f);
		}
	}
}

// Helper: check if format is float
static bool AudioCapture__IsFloatFormat(const WAVEFORMATEX* Format)
{
	if (Format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
	{
		return true;
	}
	if (Format->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
	{
		const WAVEFORMATEXTENSIBLE* Ex = (const WAVEFORMATEXTENSIBLE*)Format;
		return IsEqualGUID(&Ex->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
	}
	return false;
}

// Helper: read mic frames, resample to loopback format, and mix into loopback buffer
static void AudioCapture__MixMic(AudioCapture* Capture, BYTE* LoopbackBuffer, UINT32 LoopbackFrames)
{
	if (!Capture->MicEnabled || !Capture->MicCaptureClient)
	{
		return;
	}

	// read available mic frames
	BYTE* MicBuffer = NULL;
	DWORD MicFlags = 0;
	UINT32 MicFrames = 0;
	UINT64 MicPosition = 0;
	UINT64 MicTimestamp = 0;

	HRESULT hr = IAudioCaptureClient_GetBuffer(Capture->MicCaptureClient, &MicBuffer, &MicFrames, &MicFlags, &MicPosition, &MicTimestamp);
	if (FAILED(hr))
	{
		return;
	}
	if (MicFrames == 0)
	{
		return;
	}

	// Simple software resampler: convert mic format to loopback format and mix
	// This handles sample rate conversion, channel count conversion, and format conversion
	
	bool LoopbackIsFloat = AudioCapture__IsFloatFormat(Capture->Format);
	WORD LoopbackBits = Capture->Format->wBitsPerSample;
	UINT32 LoopbackChannels = Capture->Format->nChannels;
	UINT32 LoopbackSampleRate = Capture->Format->nSamplesPerSec;
	
	bool MicIsFloat = AudioCapture__IsFloatFormat(Capture->MicFormat);
	WORD MicBits = Capture->MicFormat->wBitsPerSample;
	UINT32 MicChannels = Capture->MicFormat->nChannels;
	UINT32 MicSampleRate = Capture->MicFormat->nSamplesPerSec;

	// Calculate how many loopback frames correspond to the mic frames
	// mic_frames / mic_rate = loopback_frames / loopback_rate
	// loopback_frames = mic_frames * loopback_rate / mic_rate
	UINT32 ResampledFrames = (UINT32)((UINT64)MicFrames * LoopbackSampleRate / MicSampleRate);
	if (ResampledFrames > LoopbackFrames)
	{
		ResampledFrames = LoopbackFrames;
	}

	// Mix resampled mic into loopback buffer
	for (UINT32 i = 0; i < ResampledFrames; i++)
	{
		// Find corresponding mic frame using linear interpolation
		float MicFramePos = (float)i * MicSampleRate / LoopbackSampleRate;
		UINT32 MicFrameIndex = (UINT32)MicFramePos;
		float Frac = MicFramePos - MicFrameIndex;

		if (MicFrameIndex >= MicFrames - 1)
		{
			MicFrameIndex = MicFrames - 1;
			Frac = 0.0f;
		}

		for (UINT32 ch = 0; ch < LoopbackChannels; ch++)
		{
			// Get loopback sample
			float LoopbackSample = AudioCapture__SampleToFloat(
				LoopbackBuffer + i * Capture->Format->nBlockAlign + ch * (LoopbackBits / 8),
				LoopbackBits, LoopbackIsFloat);

			// Get mic sample (handle mono->stereo by duplicating)
			UINT32 MicCh = (ch < MicChannels) ? ch : 0;
			
			float MicSample0 = AudioCapture__SampleToFloat(
				MicBuffer + MicFrameIndex * Capture->MicFormat->nBlockAlign + MicCh * (MicBits / 8),
				MicBits, MicIsFloat);
			
			float MicSample1 = MicSample0;
			if (MicFrameIndex + 1 < MicFrames)
			{
				MicSample1 = AudioCapture__SampleToFloat(
					MicBuffer + (MicFrameIndex + 1) * Capture->MicFormat->nBlockAlign + MicCh * (MicBits / 8),
					MicBits, MicIsFloat);
			}

			// Linear interpolation
			float MicSample = MicSample0 + (MicSample1 - MicSample0) * Frac;

			// Mix with gain
			float Mixed = LoopbackSample + MicSample * Capture->MicGain;
			
			AudioCapture__FloatToSample(
				LoopbackBuffer + i * Capture->Format->nBlockAlign + ch * (LoopbackBits / 8),
				Mixed, LoopbackBits, LoopbackIsFloat);
		}
	}

	IAudioCaptureClient_ReleaseBuffer(Capture->MicCaptureClient, MicFrames);
}

static DWORD CALLBACK AudioCapture__Thread(LPVOID Arg)
{
	AudioCapture* Capture = Arg;
	LOG_INFO("AudioCapture__Thread: started (tid=%lu, mic=%d, mic_event=%p)", GetCurrentThreadId(), Capture->MicEnabled, (void*)Capture->MicEvent);

	DWORD Task = 0;
	HANDLE Handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &Task);
	Assert(Handle);

	IAudioCaptureClient* CaptureClient = Capture->CaptureClient;
	uint32_t BytesPerFrame = Capture->Format->nBlockAlign;
	uint32_t BufferSize = Capture->BufferSize;
	uint32_t BufferWrite = 0;
	HANDLE Event = Capture->Event;

	// set up event array for WaitForMultipleObjects
	HANDLE Events[2];
	DWORD EventCount = 1;
	Events[0] = Event;
	if (Capture->MicEnabled && Capture->MicEvent)
	{
		Events[1] = Capture->MicEvent;
		EventCount = 2;
		LOG_INFO("AudioCapture__Thread: waiting on 2 events (loopback + mic)");
	}
	else
	{
		LOG_INFO("AudioCapture__Thread: waiting on 1 event (loopback only)");
	}

	while (WaitForMultipleObjects(EventCount, Events, FALSE, INFINITE) != WAIT_FAILED)
	{
		if (Capture->Stop)
		{
			LOG_INFO("AudioCapture__Thread: stop flag set, exiting loop");
			break;
		}

		// read loopback data
		BYTE* Buffer = NULL;
		DWORD Flags = 0;
		UINT32 Frames = 0;
		UINT64 Position = 0; // in sample count from beginning of stream
		UINT64 Timestamp = 0; // in QPC units

		while (SUCCEEDED(IAudioCaptureClient_GetBuffer(CaptureClient, &Buffer, &Frames, &Flags, &Position, &Timestamp)) && Frames != 0)
		{
			uint32_t BufferAvailable = BufferSize - (BufferWrite - atomic_load_explicit(&Capture->BufferRead, memory_order_relaxed));

			uint32_t WriteSize = sizeof(Frames) + sizeof(Position) + sizeof(Timestamp) + Frames * BytesPerFrame;
			if (WriteSize <= BufferAvailable)
			{
				uint8_t* BufferPtr = Capture->Buffer + (BufferWrite & (BufferSize - 1));
				CopyMemory(BufferPtr, &Frames, sizeof(Frames)); BufferPtr += sizeof(Frames);
				CopyMemory(BufferPtr, &Position, sizeof(Position)); BufferPtr += sizeof(Position);
				CopyMemory(BufferPtr, &Timestamp, sizeof(Timestamp)); BufferPtr += sizeof(Timestamp);

				// copy loopback data to temp buffer for mixing
				BYTE* MixedBuffer = (BYTE*)BufferPtr;
				if (Flags & AUDCLNT_BUFFERFLAGS_SILENT)
				{
					ZeroMemory(MixedBuffer, Frames * BytesPerFrame);
				}
				else
				{
					CopyMemory(MixedBuffer, Buffer, Frames * BytesPerFrame);
				}

				// mix mic data if enabled
				if (Capture->MicEnabled)
				{
					AudioCapture__MixMic(Capture, MixedBuffer, Frames);
				}

				BufferWrite += WriteSize;
				atomic_store_explicit(&Capture->BufferWrite, BufferWrite, memory_order_release);
			}
			else
			{
				// throttle: this can fire every few msec when the encoder stalls
				static uint32_t OverflowCount;
				OverflowCount++;
				if (OverflowCount <= 3 || (OverflowCount % 1000) == 0)
				{
					LOG_WARN("AudioCapture__Thread: ringbuffer overflow! available=%u needed=%u (total %u)", BufferAvailable, WriteSize, OverflowCount);
				}
			}

			HR(IAudioCaptureClient_ReleaseBuffer(CaptureClient, Frames));
			Buffer = NULL;
			Frames = 0;
			Position = 0;
			Timestamp = 0;
		}
	}
	
	AvRevertMmThreadCharacteristics(Handle);

	LOG_INFO("AudioCapture__Thread: exiting");
	return 0;
}

bool AudioCapture_Start(AudioCapture* Capture, HWND ApplicationWindow, bool CaptureMicrophone, float MicrophoneGain)
{
	LOG_INFO("AudioCapture_Start: app_window=%p, mic=%d, gain=%.2f", (void*)ApplicationWindow, CaptureMicrophone, MicrophoneGain);

	bool Result = false;

	// initialize mic fields
	Capture->MicClient = NULL;
	Capture->MicCaptureClient = NULL;
	Capture->MicEvent = NULL;
	Capture->MicFormat = NULL;
	Capture->MicGain = MicrophoneGain;
	Capture->MicEnabled = false;

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
		atomic_init(&ActivateCompletion.ReadyFlag, 0);

		IActivateAudioInterfaceAsyncOperation* AsyncOperation;
		if (FAILED(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, &IID_IAudioClient, &Params, &ActivateCompletion.Handler, &AsyncOperation)))
		{
			return false;
		}

		while (!atomic_load_explicit(&ActivateCompletion.ReadyFlag, memory_order_acquire))
		{
			uint32_t ReadyFlag = 0;
			WaitOnAddress((PVOID)&ActivateCompletion.ReadyFlag, &ReadyFlag, sizeof(ReadyFlag), INFINITE);
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

		// microphone capture (only for monitor/region capture, not application-local)
		if (CaptureMicrophone)
		{
			LOG_INFO("AudioCapture_Start: attempting to initialize microphone capture...");
			IMMDevice* MicDevice = NULL;
			if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(Enumerator, eCapture, eConsole, &MicDevice)))
			{
				LOG_INFO("AudioCapture_Start: got default capture device");
				IAudioClient* MicClient = NULL;
				if (SUCCEEDED(IMMDevice_Activate(MicDevice, &IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&MicClient)))
				{
					LOG_INFO("AudioCapture_Start: activated mic audio client");
					WAVEFORMATEX* MicFormat = NULL;
					if (SUCCEEDED(IAudioClient_GetMixFormat(MicClient, &MicFormat)))
					{
						LOG_INFO("AudioCapture_Start: got mic format: %luHz %luch %lubps", MicFormat->nSamplesPerSec, MicFormat->nChannels, MicFormat->wBitsPerSample);
						REFERENCE_TIME DefaultPeriod, MinimumPeriod;
						if (SUCCEEDED(IAudioClient_GetDevicePeriod(MicClient, &DefaultPeriod, &MinimumPeriod)))
						{
							DWORD MicFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
							if (SUCCEEDED(IAudioClient_Initialize(MicClient, AUDCLNT_SHAREMODE_SHARED, MicFlags, DefaultPeriod, 0, MicFormat, NULL)))
							{
								LOG_INFO("AudioCapture_Start: initialized mic client");
								IAudioCaptureClient* MicCaptureClient = NULL;
								if (SUCCEEDED(IAudioClient_GetService(MicClient, &IID_IAudioCaptureClient, (void**)&MicCaptureClient)))
								{
									LOG_INFO("AudioCapture_Start: got mic capture client");
									Capture->MicClient = MicClient;
									Capture->MicCaptureClient = MicCaptureClient;
									Capture->MicFormat = MicFormat;
									Capture->MicEvent = CreateEventW(NULL, FALSE, FALSE, NULL);

									if (Capture->MicEvent)
									{
										HR(IAudioClient_SetEventHandle(MicClient, Capture->MicEvent));

										// Software resampler is used in AudioCapture__MixMic
										// No need for MF resampler MFT
										Capture->MicEnabled = true;
										LOG_INFO("AudioCapture_Start: microphone capture enabled (software resampler): %luHz/%luch -> %luHz/%luch",
											MicFormat->nSamplesPerSec, MicFormat->nChannels,
											Capture->Format->nSamplesPerSec, Capture->Format->nChannels);
									}
									else
									{
										LOG_ERROR("AudioCapture_Start: failed to create mic event");
									}
								}
								else
								{
									LOG_ERROR("AudioCapture_Start: failed to get mic capture client");
								}
							}
							else
							{
								LOG_ERROR("AudioCapture_Start: failed to initialize mic client");
							}
						}
						else
						{
							LOG_ERROR("AudioCapture_Start: failed to get mic device period");
						}
					if (!Capture->MicEnabled)
					{
						CoTaskMemFree(MicFormat);
					}
				}
				else
				{
					LOG_ERROR("AudioCapture_Start: failed to get mic mix format");
				}
				if (!Capture->MicEnabled)
				{
					if (Capture->MicCaptureClient)
					{
						IAudioCaptureClient_Release(Capture->MicCaptureClient);
						Capture->MicCaptureClient = NULL;
					}
					if (Capture->MicEvent)
					{
						CloseHandle(Capture->MicEvent);
						Capture->MicEvent = NULL;
					}
					IAudioClient_Release(MicClient);
				}
				}
				else
				{
					LOG_ERROR("AudioCapture_Start: failed to activate mic device");
				}
				IMMDevice_Release(MicDevice);
			}
			else
			{
				LOG_ERROR("AudioCapture_Start: failed to get default capture device");
			}
			if (!Capture->MicEnabled)
			{
				LOG_WARN("AudioCapture_Start: microphone capture requested but failed to initialize");
			}
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
		atomic_init(&Capture->BufferRead, 0);
		atomic_init(&Capture->BufferWrite, 0);

		Capture->Stop = false;

		Capture->Event = CreateEventW(NULL, FALSE, FALSE, NULL);
		Assert(Capture->Event);

		Capture->Thread = CreateThread(NULL, 0, &AudioCapture__Thread, Capture, 0, NULL);
		Assert(Capture->Thread);

		HR(IAudioClient_SetEventHandle(Capture->RecordClient, Capture->Event));
		HR(IAudioClient_Start(Capture->RecordClient));

		// start mic client if enabled
		if (Capture->MicEnabled)
		{
			HR(IAudioClient_Start(Capture->MicClient));
		}

		LARGE_INTEGER Start;
		QueryPerformanceCounter(&Start);
		Capture->StartQpc = Start.QuadPart;

		LARGE_INTEGER Freq;
		QueryPerformanceFrequency(&Freq);
		Capture->Freq = Freq.QuadPart;

		LOG_INFO("AudioCapture_Start: thread started, buffer_size=%u format: rate=%lu channels=%lu blockalign=%lu mic=%d",
			Capture->BufferSize, Capture->Format->nSamplesPerSec, Capture->Format->nChannels, Capture->Format->nBlockAlign, Capture->MicEnabled);
		Result = true;
	}

	return Result;
}

void AudioCapture_Stop(AudioCapture* Capture)
{
	LOG_INFO("AudioCapture_Stop: setting stop flag and signaling event");
	Capture->Stop = true;
	SetEvent(Capture->Event);
	if (Capture->MicEvent)
	{
		SetEvent(Capture->MicEvent);
	}

	LOG_INFO("AudioCapture_Stop: waiting for thread to exit...");
	DWORD WaitResult = WaitForSingleObject(Capture->Thread, 5000); // 5 second timeout
	if (WaitResult == WAIT_TIMEOUT)
	{
		LOG_ERROR("AudioCapture_Stop: thread did not exit within 5 seconds!");
	}
	else if (WaitResult == WAIT_FAILED)
	{
		LOG_ERROR("AudioCapture_Stop: WaitForSingleObject failed: %lu", GetLastError());
	}
	else
	{
		LOG_INFO("AudioCapture_Stop: thread exited normally");
	}

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

	// cleanup mic resources
	if (Capture->MicClient)
	{
		IAudioClient_Release(Capture->MicClient);
		Capture->MicClient = NULL;
	}
	if (Capture->MicCaptureClient)
	{
		IAudioCaptureClient_Release(Capture->MicCaptureClient);
		Capture->MicCaptureClient = NULL;
	}
	if (Capture->MicEvent)
	{
		CloseHandle(Capture->MicEvent);
		Capture->MicEvent = NULL;
	}
	if (Capture->MicFormat)
	{
		CoTaskMemFree(Capture->MicFormat);
		Capture->MicFormat = NULL;
	}
	Capture->MicEnabled = false;
}

void AudioCapture_Flush(AudioCapture* Capture)
{
	LOG_INFO("AudioCapture_Flush: stopping audio clients");
	if (Capture->PlayClient)
	{
		HR(IAudioClient_Stop(Capture->PlayClient));
	}
	HR(IAudioClient_Stop(Capture->RecordClient));
	if (Capture->MicClient)
	{
		HR(IAudioClient_Stop(Capture->MicClient));
	}
}

bool AudioCapture_GetData(AudioCapture* Capture, AudioCaptureData* Data, uint64_t ExpectedTimestamp)
{
	uint32_t Frames;
	uint64_t Position;
	uint64_t Timestamp;

	uint32_t BufferRead = atomic_load_explicit(&Capture->BufferRead, memory_order_relaxed);
	uint32_t AvailableSize = atomic_load_explicit(&Capture->BufferWrite, memory_order_acquire) - BufferRead;
	if (AvailableSize < sizeof(Frames) + sizeof(Position) + sizeof(Timestamp))
	{
		return false;
	}

	uint8_t* BufferPtr = Capture->Buffer + (BufferRead & (Capture->BufferSize - 1));
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
	Assert(ReadSize <= atomic_load_explicit(&Capture->BufferWrite, memory_order_relaxed) - atomic_load_explicit(&Capture->BufferRead, memory_order_relaxed));
	atomic_fetch_add_explicit(&Capture->BufferRead, ReadSize, memory_order_release);
}
