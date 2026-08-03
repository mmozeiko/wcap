#pragma once

#include "wcap.h"
#include "wcap_config.h"
#include "wcap_tex_resize.h"
#include "wcap_yuv_convert.h"

#include <d3d11_4.h>
#include <mfidl.h>
#include <mfreadwrite.h>

//
// interface
//

#define ENCODER_VIDEO_BUFFER_COUNT 8
#define ENCODER_AUDIO_BUFFER_COUNT 16

// max number of samples/ticks waiting to be submitted to sink writer by encoder thread
// must be power of two, actual in-flight sample count is limited by buffer counts above
#define ENCODER_QUEUE_COUNT 64

// max time the stop sequence waits for a congested encoder to make progress in each
// phase before abandoning remaining audio / discarding the queued backlog, so that
// stopping the recording cannot hang the UI thread indefinitely
#define ENCODER_STOP_WAIT_TIMEOUT_MS 10000

typedef struct
{
	IMFSample* Sample;         // sample to write, or NULL for stream tick / stop command
	IMFTrackedSample* Tracked; // released after WriteSample (audio only), NULL otherwise
	int StreamIndex;           // -1 = stop command (writer thread exits)
	LONGLONG Tick;             // stream tick timestamp, used only when Sample == NULL
}
EncoderQueueItem;

// single producer (main thread), single consumer (writer thread) ringbuffer queue
typedef struct
{
	EncoderQueueItem Items[ENCODER_QUEUE_COUNT];
	_Atomic(uint32_t) Read;
	_Atomic(uint32_t) Write;
}
EncoderQueue;

typedef struct
{
	DWORD InputWidth;   // width to what input will be cropped
	DWORD InputHeight;  // height to what input will be cropped
	DWORD OutputWidth;  // width of video output
	DWORD OutputHeight; // height of video output
	DWORD FramerateNum; // video output framerate numerator
	DWORD FramerateDen; // video output framerate denumerator
	UINT64 StartTime;   // time in QPC ticks since first call of NewFrame

	IMFAsyncCallback VideoSampleCallback;
	IMFAsyncCallback AudioSampleCallback;
	ID3D11DeviceContext* Context;
	ID3D11Multithread* Multithread;
	IMFSinkWriter* Writer;
	int VideoStreamIndex;
	int AudioStreamIndex;

	// writer threads do all blocking IMFSinkWriter calls (WriteSample/SendStreamTick)
	// separate video & audio queues/threads so a congested video encoder cannot stall audio
	HANDLE VideoThread;
	HANDLE AudioThread; // NULL when there is no audio stream
	EncoderQueue VideoQueue;
	EncoderQueue AudioQueue;

	// when set, writer threads release queued items without writing them to the sink writer
	// (set by the stop sequence when the encoder is too congested to drain the backlog)
	_Atomic(uint32_t) DiscardQueued;

	ID3D11RenderTargetView* InputView;

	TexResize Resize;
	YuvConvert Convert;

	YuvConvertOutput  ConvertOutput[ENCODER_VIDEO_BUFFER_COUNT];
	IMFSample*        VideoSample[ENCODER_VIDEO_BUFFER_COUNT];
	_Atomic(uint64_t) VideoSampleAvailable;

	BOOL   VideoDiscontinuity;
	UINT64 VideoLastTime;

	IMFTransform*     Resampler;
	IMFSample*        AudioSample[ENCODER_AUDIO_BUFFER_COUNT];
	_Atomic(uint64_t) AudioSampleAvailable;

	IMFSample*      AudioInputSample;
	DWORD           AudioFrameSize;
	DWORD           AudioSampleRate;

	// video stall detection (informational only): track last time a video buffer was returned
	_Atomic(uint64_t) VideoLastReturnTime; // QPC timestamp of last video buffer return
	UINT64            VideoStallTimeout;   // max ticks without video buffer return before stall
}
Encoder;

typedef struct
{
	DWORD Width;
	DWORD Height;
	DWORD FramerateNum;
	DWORD FramerateDen;
	WAVEFORMATEX* AudioFormat;
	Config* Config;
}
EncoderConfig;

static void Encoder_Init(Encoder* Encoder);
static BOOL Encoder_Start(Encoder* Encoder, ID3D11Device* Device, LPWSTR FileName, const EncoderConfig* Config);
static void Encoder_Stop(Encoder* Encoder);

static BOOL Encoder_NewFrame(Encoder* Encoder, ID3D11Texture2D* Texture, RECT Rect, UINT64 Time, UINT64 TimePeriod);
static void Encoder_NewSamples(Encoder* Encoder, LPCVOID Samples, DWORD FrameCount, UINT64 Time, UINT64 TimePeriod);
static BOOL Encoder_Update(Encoder* Encoder, UINT64 Time, UINT64 TimePeriod);
static void Encoder_GetStats(Encoder* Encoder, DWORD* Bitrate, DWORD* LengthMsec, UINT64* FileSize);

//
// implementation
//

#include <evr.h>
#include <cguid.h>
#include <mfapi.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <wmcodecdsp.h>

#define MFT64(high, low) (((UINT64)high << 32) | (low))

// counters for throttled callback logging (first 3, then every Nth)
static _Atomic(uint64_t) gEncoderVideoInvokeCount;
static _Atomic(uint64_t) gEncoderAudioInvokeCount;
static _Atomic(uint64_t) gEncoderAudioSubmitCount;

static HRESULT STDMETHODCALLTYPE Encoder__QueryInterface(IMFAsyncCallback* this, REFIID riid, void** Object)
{
	if (Object == NULL)
	{
		return E_POINTER;
	}
	if (IsEqualGUID(&IID_IUnknown, riid) || IsEqualGUID(&IID_IMFAsyncCallback, riid))
	{
		*Object = this;
		return S_OK;
	}
	return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE Encoder__AddRef(IMFAsyncCallback* this)
{
	return 1;
}

static ULONG STDMETHODCALLTYPE Encoder__Release(IMFAsyncCallback* this)
{
	return 1;
}

static HRESULT STDMETHODCALLTYPE Encoder__GetParameters(IMFAsyncCallback* this, DWORD* Flags, DWORD* Queue)
{
	*Flags = 0;
	*Queue = MFASYNC_CALLBACK_QUEUE_MULTITHREADED;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE Encoder__VideoInvoke(IMFAsyncCallback* this, IMFAsyncResult* Result)
{
	Encoder* Enc = CONTAINING_RECORD(this, Encoder, VideoSampleCallback);

	IUnknown* Object;
	IMFSample* Sample;

	HRESULT hr = IMFAsyncResult_GetObject(Result, &Object);
	if (FAILED(hr))
	{
		LOG_ERROR("Encoder__VideoInvoke: IMFAsyncResult_GetObject failed: 0x%08lX", (unsigned long)hr);
		return hr;
	}
	hr = IUnknown_QueryInterface(Object, &IID_IMFSample, (LPVOID*)&Sample);
	IUnknown_Release(Object);
	if (FAILED(hr))
	{
		LOG_ERROR("Encoder__VideoInvoke: QueryInterface for IMFSample failed: 0x%08lX", (unsigned long)hr);
		return hr;
	}
	// keep Sample object reference count incremented to reuse for new frame submission

	for (size_t Index = 0; Index < ARRAYSIZE(Enc->VideoSample); Index++)
	{
		if (Sample == Enc->VideoSample[Index])
		{
			uint64_t Count = atomic_fetch_add(&gEncoderVideoInvokeCount, 1);
			if (Count < 3 || (Count % 100) == 0)
			{
				LOG_INFO("Encoder__VideoInvoke: video buffer %zu returned (total %llu)", Index, Count + 1);
			}
			// record time of last video buffer return for stall detection
			LARGE_INTEGER Now;
			QueryPerformanceCounter(&Now);
			atomic_store(&Enc->VideoLastReturnTime, (uint64_t)Now.QuadPart);

			atomic_fetch_or(&Enc->VideoSampleAvailable, 1ULL << Index);
			WakeByAddressSingle((PVOID)&Enc->VideoSampleAvailable);
			break;
		}
	}

	return S_OK;
}

static HRESULT STDMETHODCALLTYPE Encoder__AudioInvoke(IMFAsyncCallback* this, IMFAsyncResult* Result)
{
	Encoder* Enc = CONTAINING_RECORD(this, Encoder, AudioSampleCallback);

	IUnknown* Object;
	IMFSample* Sample;

	HRESULT hr = IMFAsyncResult_GetObject(Result, &Object);
	if (FAILED(hr))
	{
		LOG_ERROR("Encoder__AudioInvoke: IMFAsyncResult_GetObject failed: 0x%08lX", (unsigned long)hr);
		return hr;
	}
	hr = IUnknown_QueryInterface(Object, &IID_IMFSample, (LPVOID*)&Sample);
	IUnknown_Release(Object);
	if (FAILED(hr))
	{
		LOG_ERROR("Encoder__AudioInvoke: QueryInterface for IMFSample failed: 0x%08lX", (unsigned long)hr);
		return hr;
	}
	// keep Sample object reference count incremented to reuse for new sample submission

	for (size_t Index = 0; Index < ARRAYSIZE(Enc->AudioSample); Index++)
	{
		if (Sample == Enc->AudioSample[Index])
		{
			uint64_t Count = atomic_fetch_add(&gEncoderAudioInvokeCount, 1);
			if (Count < 3 || (Count % 500) == 0)
			{
				LOG_INFO("Encoder__AudioInvoke: audio buffer %zu returned (total %llu)", Index, Count + 1);
			}
			atomic_fetch_or(&Enc->AudioSampleAvailable, 1ULL << Index);
			WakeByAddressSingle((PVOID)&Enc->AudioSampleAvailable);
			break;
		}
	}

	return S_OK;
}

static IMFAsyncCallbackVtbl Encoder__VideoSampleCallbackVtbl =
{
	.QueryInterface = &Encoder__QueryInterface,
	.AddRef         = &Encoder__AddRef,
	.Release        = &Encoder__Release,
	.GetParameters  = &Encoder__GetParameters,
	.Invoke         = &Encoder__VideoInvoke,
};

static IMFAsyncCallbackVtbl Encoder__AudioSampleCallbackVtbl =
{
	.QueryInterface = &Encoder__QueryInterface,
	.AddRef         = &Encoder__AddRef,
	.Release        = &Encoder__Release,
	.GetParameters  = &Encoder__GetParameters,
	.Invoke         = &Encoder__AudioInvoke,
};

static BOOL Encoder__QueueFull(EncoderQueue* Queue)
{
	uint32_t Write = atomic_load_explicit(&Queue->Write, memory_order_relaxed);
	uint32_t Read  = atomic_load_explicit(&Queue->Read, memory_order_acquire);
	return (Write - Read) >= ENCODER_QUEUE_COUNT;
}

// caller must make sure queue is not full (single producer, so full check stays valid)
static void Encoder__Enqueue(EncoderQueue* Queue, IMFSample* Sample, IMFTrackedSample* Tracked, int StreamIndex, LONGLONG Tick)
{
	uint32_t Write = atomic_load_explicit(&Queue->Write, memory_order_relaxed);
	Queue->Items[Write & (ENCODER_QUEUE_COUNT - 1)] = (EncoderQueueItem)
	{
		.Sample = Sample,
		.Tracked = Tracked,
		.StreamIndex = StreamIndex,
		.Tick = Tick,
	};
	atomic_store_explicit(&Queue->Write, Write + 1, memory_order_release);
	WakeByAddressSingle((PVOID)&Queue->Write);
}

static BOOL Encoder__TryEnqueue(EncoderQueue* Queue, IMFSample* Sample, IMFTrackedSample* Tracked, int StreamIndex, LONGLONG Tick)
{
	if (Encoder__QueueFull(Queue))
	{
		return FALSE;
	}
	Encoder__Enqueue(Queue, Sample, Tracked, StreamIndex, Tick);
	return TRUE;
}

// converts wide string to UTF-8 for logging (log file is UTF-8); %ls cannot be used
// because conversion to the C locale fails on non-ASCII characters, and some vendor
// MFT names contain them (Intel driver registers a corrupted friendly name containing
// U+7C27: "Intel" + U+7C27 + " Hardware H265 Encoder MFT")
static void Encoder__WideToUtf8(LPCWSTR Wide, char* Out, int OutSize)
{
	if (WideCharToMultiByte(CP_UTF8, 0, Wide, -1, Out, OutSize, NULL, NULL) == 0)
	{
		Out[0] = 0;
	}
	Out[OutSize - 1] = 0;
}

// enqueue stop command, waiting for queue space; when the encoder is congested this can
// take a while, so after a timeout enable discarding so writer threads skip remaining items
static void Encoder__EnqueueStopCommand(Encoder* Encoder, EncoderQueue* Queue)
{
	DWORD WaitStart = GetTickCount();
	while (!Encoder__TryEnqueue(Queue, NULL, NULL, -1, 0))
	{
		if (GetTickCount() - WaitStart >= ENCODER_STOP_WAIT_TIMEOUT_MS)
		{
			atomic_store_explicit(&Encoder->DiscardQueued, 1, memory_order_release);
		}
		uint32_t Read = atomic_load_explicit(&Queue->Read, memory_order_acquire);
		WaitOnAddress((PVOID)&Queue->Read, &Read, sizeof(Read), 100);
	}
}

// wait for tracked sample callbacks to return all buffers, they fire asynchronously on MF workqueue threads
static BOOL Encoder__WaitForCallbacks(_Atomic(uint64_t)* Available, uint64_t FullMask, DWORD TimeoutMs)
{
	DWORD Start = GetTickCount();
	for (;;)
	{
		uint64_t Value = atomic_load(Available);
		if ((Value & FullMask) == FullMask)
		{
			return TRUE;
		}
		DWORD Elapsed = GetTickCount() - Start;
		if (Elapsed >= TimeoutMs)
		{
			return FALSE;
		}
		WaitOnAddress((PVOID)Available, &Value, sizeof(Value), min(100, TimeoutMs - Elapsed));
	}
}

// all blocking IMFSinkWriter calls happen on writer threads, so encoder MFT backpressure cannot block the main thread
static void Encoder__WriterThread(Encoder* Encoder, EncoderQueue* Queue)
{
	uint32_t Discarded = 0;
	for (;;)
	{
		uint32_t Read = atomic_load_explicit(&Queue->Read, memory_order_relaxed);
		while (Read == atomic_load_explicit(&Queue->Write, memory_order_acquire))
		{
			WaitOnAddress((PVOID)&Queue->Write, &Read, sizeof(Read), INFINITE);
		}

		EncoderQueueItem Item = Queue->Items[Read & (ENCODER_QUEUE_COUNT - 1)];
		atomic_store_explicit(&Queue->Read, Read + 1, memory_order_release);
		WakeByAddressSingle((PVOID)&Queue->Read);

		if (Item.StreamIndex < 0)
		{
			// stop command, everything submitted before it has been written
			break;
		}

		// stop sequence gave up waiting for the congested encoder, drop queued items instead of writing them
		BOOL Discard = atomic_load_explicit(&Encoder->DiscardQueued, memory_order_acquire) != 0;

		if (Item.Sample)
		{
			if (Discard)
			{
				Discarded++;
			}
			else
			{
				// this blocks while encoder MFT is congested, which is fine on this thread
				HRESULT hr = IMFSinkWriter_WriteSample(Encoder->Writer, Item.StreamIndex, Item.Sample);
				if (FAILED(hr))
				{
					LOG_ERROR("Encoder__WriterThread: IMFSinkWriter_WriteSample (stream %d) failed: 0x%08lX", Item.StreamIndex, (unsigned long)hr);
				}
			}
			IMFSample_Release(Item.Sample);
			if (Item.Tracked)
			{
				IMFTrackedSample_Release(Item.Tracked);
			}
		}
		else if (!Discard)
		{
			HRESULT hr = IMFSinkWriter_SendStreamTick(Encoder->Writer, Item.StreamIndex, Item.Tick);
			if (FAILED(hr))
			{
				LOG_ERROR("Encoder__WriterThread: IMFSinkWriter_SendStreamTick failed: 0x%08lX", (unsigned long)hr);
			}
		}
	}
	if (Discarded)
	{
		LOG_WARN("Encoder__WriterThread: discarded %lu queued items (encoder congested)", (unsigned long)Discarded);
	}
}

static DWORD CALLBACK Encoder__VideoThread(LPVOID Arg)
{
	Encoder* Encoder = Arg;
	Encoder__WriterThread(Encoder, &Encoder->VideoQueue);
	return 0;
}

static DWORD CALLBACK Encoder__AudioThread(LPVOID Arg)
{
	Encoder* Encoder = Arg;
	Encoder__WriterThread(Encoder, &Encoder->AudioQueue);
	return 0;
}

static void Encoder__OutputAudioSamples(Encoder* Encoder, BOOL WaitForSpace)
{
	DWORD WaitStart = GetTickCount();
	for (;;)
	{
		// we don't want to drop any audio frames, so wait for available sample/buffer when stopping
		uint64_t Available = atomic_load(&Encoder->AudioSampleAvailable);
		while (Available == 0)
		{
			if (!WaitForSpace)
			{
				// encoder thread is congested, leave pending output in resampler for next call
				return;
			}
			if (GetTickCount() - WaitStart >= ENCODER_STOP_WAIT_TIMEOUT_MS)
			{
				LOG_WARN("Encoder__OutputAudioSamples: timed out waiting for audio buffer (encoder congested), abandoning remaining audio");
				return;
			}
			uint64_t Zero = 0;
			WaitOnAddress((PVOID)&Encoder->AudioSampleAvailable, &Zero, sizeof(Zero), 100);
			Available = atomic_load(&Encoder->AudioSampleAvailable);
		}

		// don't produce more output than audio writer thread queue can hold
		while (Encoder__QueueFull(&Encoder->AudioQueue))
		{
			if (!WaitForSpace)
			{
				return;
			}
			if (GetTickCount() - WaitStart >= ENCODER_STOP_WAIT_TIMEOUT_MS)
			{
				LOG_WARN("Encoder__OutputAudioSamples: timed out waiting for audio queue space (encoder congested), abandoning remaining audio");
				return;
			}
			uint32_t Read = atomic_load_explicit(&Encoder->AudioQueue.Read, memory_order_acquire);
			WaitOnAddress((PVOID)&Encoder->AudioQueue.Read, &Read, sizeof(Read), 100);
		}

		DWORD Index;
		_BitScanForward64(&Index, Available);

		IMFSample* Sample = Encoder->AudioSample[Index];

		DWORD Status;
		MFT_OUTPUT_DATA_BUFFER Output = { .dwStreamID = 0, .pSample = Sample };
		HRESULT hr = IMFTransform_ProcessOutput(Encoder->Resampler, 0, 1, &Output, &Status);
		if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
		{
			// no output is available
			break;
		}
		if (FAILED(hr))
		{
			LOG_ERROR("Encoder__OutputAudioSamples: IMFTransform_ProcessOutput failed: 0x%08lX", (unsigned long)hr);
			break;
		}

		atomic_fetch_and(&Encoder->AudioSampleAvailable, ~(1ULL << Index));

		IMFTrackedSample* Tracked;
		HR(IMFSample_QueryInterface(Sample, &IID_IMFTrackedSample, (LPVOID*)&Tracked));
		HR(IMFTrackedSample_SetAllocator(Tracked, &Encoder->AudioSampleCallback, (IUnknown*)Tracked));

		uint64_t SubmitCount = atomic_fetch_add(&gEncoderAudioSubmitCount, 1);
		if (SubmitCount < 3 || (SubmitCount % 500) == 0)
		{
			LOG_INFO("Encoder__OutputAudioSamples: queueing audio buffer %lu for encoder thread (total %llu)", (unsigned long)Index, SubmitCount + 1);
		}

		// audio writer thread will call WriteSample and release Sample & Tracked references
		Encoder__Enqueue(&Encoder->AudioQueue, Sample, Tracked, Encoder->AudioStreamIndex, 0);
	}
}

void Encoder_Init(Encoder* Encoder)
{
	HR(MFStartup(MF_VERSION, MFSTARTUP_LITE));
	Encoder->VideoSampleCallback.lpVtbl = &Encoder__VideoSampleCallbackVtbl;
	Encoder->AudioSampleCallback.lpVtbl = &Encoder__AudioSampleCallbackVtbl;
}

BOOL Encoder_Start(Encoder* Encoder, ID3D11Device* Device, LPWSTR FileName, const EncoderConfig* Config)
{
	LOG_INFO("Encoder_Start: input=%lux%lu output=%lux%lu fps=%lu/%lu bitrate=%lu kbps codec=%lu",
		Config->Width, Config->Height,
		Config->Config->VideoMaxWidth, Config->Config->VideoMaxHeight,
		Config->FramerateNum, Config->FramerateDen,
		Config->Config->VideoBitrate, Config->Config->VideoCodec);

	ID3D11DeviceContext* Context;
	ID3D11Device_GetImmediateContext(Device, &Context);

	ID3D11Multithread* Multithread;

	// it is very unclear if this is needed or not, it used to be that D3D11 debug runtime
	// was complaining if this is not done, but nowadays it doesn't complain anymore? very confusing
	HR(ID3D11DeviceContext_QueryInterface(Context, &IID_ID3D11Multithread, &Multithread));
	ID3D11Multithread_SetMultithreadProtected(Multithread, TRUE);

	DWORD InputWidth = Config->Width;
	DWORD InputHeight = Config->Height;
	DWORD OutputWidth = Config->Config->VideoMaxWidth;
	DWORD OutputHeight = Config->Config->VideoMaxHeight;

	if (OutputWidth != 0 && OutputHeight == 0)
	{
		// limit max width
		if (OutputWidth < InputWidth)
		{
			OutputHeight = InputHeight * OutputWidth / InputWidth;
		}
		else
		{
			OutputWidth = InputWidth;
			OutputHeight = InputHeight;
		}
	}
	else if (OutputWidth == 0 && OutputHeight != 0)
	{
		// limit max height
		if (OutputHeight < InputHeight)
		{
			OutputWidth = InputWidth * OutputHeight / InputHeight;
		}
		else
		{
			OutputWidth = InputWidth;
			OutputHeight = InputHeight;
		}
	}
	else if (OutputWidth != 0 && OutputHeight != 0)
	{
		// limit max width and max height
		if (OutputWidth * InputHeight < OutputHeight * InputWidth)
		{
			if (OutputWidth < InputWidth)
			{
				OutputHeight = InputHeight * OutputWidth / InputWidth;
			}
			else
			{
				OutputWidth = InputWidth;
				OutputHeight = InputHeight;
			}
		}
		else
		{
			if (OutputHeight < InputHeight)
			{
				OutputWidth = InputWidth * OutputHeight / InputHeight;
			}
			else
			{
				OutputWidth = InputWidth;
				OutputHeight = InputHeight;
			}
		}
	}
	else
	{
		// use original size
		OutputWidth = InputWidth;
		OutputHeight = InputHeight;
	}

	// must be multiple of 2, round upwards
	InputWidth = (InputWidth + 1) & ~1;
	InputHeight = (InputHeight + 1) & ~1;
	OutputWidth = (OutputWidth + 1) & ~1;
	OutputHeight = (OutputHeight + 1) & ~1;

	BOOL Result = FALSE;
	IMFSinkWriter* Writer = NULL;
	IMFTransform* Resampler = NULL;
	HRESULT hr;

	Encoder->VideoStreamIndex = -1;
	Encoder->AudioStreamIndex = -1;

	const GUID* Container;
	const GUID* Codec;
	UINT32 Profile;
	const GUID* VideoInputFormat;
	if (Config->Config->VideoCodec == CONFIG_VIDEO_H264)
	{
		VideoInputFormat = &MFVideoFormat_NV12;
		Container = Config->Config->FragmentedOutput ? &MFTranscodeContainerType_FMPEG4 : &MFTranscodeContainerType_MPEG4;
		Codec = &MFVideoFormat_H264;
		Profile = ((UINT32[]) { eAVEncH264VProfile_Base, eAVEncH264VProfile_Main, eAVEncH264VProfile_High })[Config->Config->VideoProfile];
	}
	else if (Config->Config->VideoCodec == CONFIG_VIDEO_H265 && Config->Config->VideoProfile == CONFIG_VIDEO_MAIN)
	{
		VideoInputFormat = &MFVideoFormat_NV12;
		Container = &MFTranscodeContainerType_MPEG4;
		Codec = &MFVideoFormat_HEVC;
		Profile = eAVEncH265VProfile_Main_420_8;
	}
	else if (Config->Config->VideoCodec == CONFIG_VIDEO_H265 && Config->Config->VideoProfile == CONFIG_VIDEO_MAIN_10)
	{
		VideoInputFormat = &MFVideoFormat_P010;
		Container = &MFTranscodeContainerType_MPEG4;
		Codec = &MFVideoFormat_HEVC;
		Profile = eAVEncH265VProfile_Main_420_10;
	}
	else if (Config->Config->VideoCodec == CONFIG_VIDEO_AV1 && Config->Config->VideoProfile == CONFIG_VIDEO_MAIN)
	{
		VideoInputFormat = &MFVideoFormat_NV12;
		Container = &MFTranscodeContainerType_MPEG4;
		Codec = &MFVideoFormat_AV1;
		Profile = eAVEncAV1VProfile_Main_420_8;
	}
	else if (Config->Config->VideoCodec == CONFIG_VIDEO_AV1 && Config->Config->VideoProfile == CONFIG_VIDEO_MAIN_10)
	{
		VideoInputFormat = &MFVideoFormat_P010;
		Container = &MFTranscodeContainerType_MPEG4;
		Codec = &MFVideoFormat_AV1;
		Profile = eAVEncAV1VProfile_Main_420_10;
	}
	else
	{
		Assert(0);
	}

	// make sure MFT video encoder exists, some vendors wrongly allow SinkWriter to be created for invalid configuration
	{
		bool Ok = false;

		MFT_REGISTER_TYPE_INFO InputType = { MFMediaType_Video, *VideoInputFormat };
		MFT_REGISTER_TYPE_INFO OutputType = { MFMediaType_Video, *Codec };

		IMFAttributes* EnumAttributes;
		HR(MFCreateAttributes(&EnumAttributes, 1));

		UINT32 Flags = MFT_ENUM_FLAG_SORTANDFILTER;

		if (Config->Config->HardwareEncoder)
		{
			IDXGIDevice* DxgiDevice;
			HR(ID3D11Device_QueryInterface(Device, &IID_IDXGIDevice, (void**)&DxgiDevice));

			IDXGIAdapter* DxgiAdapter;
			HR(IDXGIDevice_GetAdapter(DxgiDevice, &DxgiAdapter));
			IDXGIDevice_Release(DxgiDevice);

			DXGI_ADAPTER_DESC AdapterDesc;
			IDXGIAdapter_GetDesc(DxgiAdapter, &AdapterDesc);
			IDXGIAdapter_Release(DxgiAdapter);

			HR(IMFAttributes_SetBlob(EnumAttributes, &MFT_ENUM_ADAPTER_LUID, (UINT8*)&AdapterDesc.AdapterLuid, sizeof(AdapterDesc.AdapterLuid)));

			Flags |= MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_HARDWARE;
		}
		else
		{
			Flags |= MFT_ENUM_FLAG_SYNCMFT;
		}

		UINT32 ActivateCount = 0;
		IMFActivate** Activate = NULL;
		if (SUCCEEDED(MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, Flags, &InputType, &OutputType, EnumAttributes, &Activate, &ActivateCount)) && ActivateCount != 0)
		{
			Ok = true;
		}

		if (Activate)
		{
			for (size_t ActivateIndex = 0; ActivateIndex != ActivateCount; ActivateIndex++)
			{
				LPWSTR Name;
				UINT32 NameLength;
				if (SUCCEEDED(IMFActivate_GetAllocatedString(Activate[ActivateIndex], &MFT_FRIENDLY_NAME_Attribute, &Name, &NameLength)))
				{
					char NameUtf8[256];
					Encoder__WideToUtf8(Name, NameUtf8, sizeof(NameUtf8));
					LOG_INFO("Video encoder MFT candidate %zu (hardware=%d): %s", ActivateIndex, Config->Config->HardwareEncoder, NameUtf8);
					CoTaskMemFree(Name);
				}
				IMFActivate_Release(Activate[ActivateIndex]);
			}
			CoTaskMemFree(Activate);
		}
		IMFAttributes_Release(EnumAttributes);

		if (!Ok)
		{
			LOG_ERROR("Cannot find video encoder (hardware=%d)", Config->Config->HardwareEncoder);
			MessageBoxW(NULL, L"Cannot find video encoder!", WCAP_TITLE, MB_ICONERROR);
			goto bail;
		}
	}

	// https://github.com/mpv-player/mpv/blob/release/0.38/video/csputils.c#L150-L153
	bool IsHD = (OutputWidth >= 1280) || (OutputHeight > 576)
		|| Config->Config->VideoCodec == CONFIG_VIDEO_H265
		|| Config->Config->VideoCodec == CONFIG_VIDEO_AV1;

	// output file
	{
		IMFAttributes* Attributes;
		HR(MFCreateAttributes(&Attributes, 4));
		if (Config->Config->HardwareEncoder)
		{
			HR(IMFAttributes_SetUINT32(Attributes, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE));

			UINT Token;
			IMFDXGIDeviceManager* Manager;
			HR(MFCreateDXGIDeviceManager(&Token, &Manager));
			HR(IMFDXGIDeviceManager_ResetDevice(Manager, (IUnknown*)Device, Token));
			HR(IMFAttributes_SetUnknown(Attributes, &MF_SINK_WRITER_D3D_MANAGER, (IUnknown*)Manager));

			IMFDXGIDeviceManager_Release(Manager);
		}
		// Throttling must stay enabled: it paces WriteSample so the audio/video encoders
		// are not overwhelmed. Without it, audio buffers are submitted faster than the AAC
		// encoder can process, causing all 16 buffers to get stuck within seconds.
		// The video stall is handled by Encoder_Update stall detection + watchdog thread.
		HR(IMFAttributes_SetUINT32(Attributes, &MF_SINK_WRITER_DISABLE_THROTTLING, FALSE));
		HR(IMFAttributes_SetGUID(Attributes, &MF_TRANSCODE_CONTAINERTYPE, Container));

		hr = MFCreateSinkWriterFromURL(FileName, NULL, Attributes, &Writer);
		IMFAttributes_Release(Attributes);

		if (FAILED(hr))
		{
			LOG_ERROR("MFCreateSinkWriterFromURL failed: 0x%08lX file=%ls", (unsigned long)hr, FileName);
			MessageBoxW(NULL, L"Cannot create output mp4 file!", WCAP_TITLE, MB_ICONERROR);
			goto bail;
		}
		LOG_INFO("SinkWriter created: %ls", FileName);
	}

	// video output type
	{
		IMFMediaType* Type;
		HR(MFCreateMediaType(&Type));

		HR(IMFMediaType_SetGUID(Type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video));
		HR(IMFMediaType_SetGUID(Type, &MF_MT_SUBTYPE, Codec));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_MPEG2_PROFILE, Profile));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_VIDEO_CHROMA_SITING, MFVideoChromaSubsampling_MPEG2));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_Wide));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_VIDEO_PRIMARIES, IsHD ? MFVideoPrimaries_BT709 : MFVideoPrimaries_SMPTE170M));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_YUV_MATRIX, IsHD ? MFVideoTransferMatrix_BT709 : MFVideoTransferMatrix_BT601));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_TRANSFER_FUNCTION, MFVideoPrimaries_BT709));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
		HR(IMFMediaType_SetUINT64(Type, &MF_MT_FRAME_RATE, MFT64(Config->FramerateNum, Config->FramerateDen)));
		HR(IMFMediaType_SetUINT64(Type, &MF_MT_FRAME_SIZE, MFT64(OutputWidth, OutputHeight)));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_AVG_BITRATE, Config->Config->VideoBitrate * 1000));

		hr = IMFSinkWriter_AddStream(Writer, Type, &Encoder->VideoStreamIndex);
		IMFMediaType_Release(Type);

		if (FAILED(hr))
		{
			LOG_ERROR("IMFSinkWriter_AddStream (video) failed: 0x%08lX", (unsigned long)hr);
			MessageBoxW(NULL, L"Cannot configure video encoder!", WCAP_TITLE, MB_ICONERROR);
			goto bail;
		}
		LOG_INFO("Video stream added: index=%d", Encoder->VideoStreamIndex);
	}

	// video input type, NV12 or P010 format
	{
		IMFMediaType* Type;
		HR(MFCreateMediaType(&Type));
		HR(IMFMediaType_SetGUID(Type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video));
		HR(IMFMediaType_SetGUID(Type, &MF_MT_SUBTYPE, VideoInputFormat));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
		HR(IMFMediaType_SetUINT64(Type, &MF_MT_FRAME_RATE, MFT64(Config->FramerateNum, Config->FramerateDen)));
		HR(IMFMediaType_SetUINT64(Type, &MF_MT_FRAME_SIZE, MFT64(OutputWidth, OutputHeight)));

		hr = IMFSinkWriter_SetInputMediaType(Writer, Encoder->VideoStreamIndex, Type, NULL);
		IMFMediaType_Release(Type);

		if (FAILED(hr))
		{
			LOG_ERROR("IMFSinkWriter_SetInputMediaType (video) failed: 0x%08lX", (unsigned long)hr);
			MessageBoxW(NULL, L"Cannot configure video encoder input!", WCAP_TITLE, MB_ICONERROR);
			goto bail;
		}
	}

	// video encoder parameters
	{
		ICodecAPI* Codec;
		HR(IMFSinkWriter_GetServiceForStream(Writer, 0, &GUID_NULL, &IID_ICodecAPI, (LPVOID*)&Codec));

		// VBR rate control
		VARIANT RateControl = { .vt = VT_UI4, .ulVal = eAVEncCommonRateControlMode_UnconstrainedVBR };
		ICodecAPI_SetValue(Codec, &CODECAPI_AVEncCommonRateControlMode, &RateControl);

		// VBR bitrate to use, some MFT encoders override MF_MT_AVG_BITRATE setting with this one
		VARIANT Bitrate = { .vt = VT_UI4, .ulVal = Config->Config->VideoBitrate * 1000 };
		ICodecAPI_SetValue(Codec, &CODECAPI_AVEncCommonMeanBitRate, &Bitrate);

		// set GOP size to 4 seconds
		VARIANT GopSize = { .vt = VT_UI4, .ulVal = MUL_DIV_ROUND_UP(4, Config->FramerateNum, Config->FramerateDen) };
		ICodecAPI_SetValue(Codec, &CODECAPI_AVEncMPVGOPSize, &GopSize);

		// disable low latency, for higher quality & better performance
		VARIANT LowLatency = { .vt = VT_BOOL, .boolVal = VARIANT_FALSE };
		ICodecAPI_SetValue(Codec, &CODECAPI_AVLowLatencyMode, &LowLatency);

		// enable 2 B-frames, for better compression
		VARIANT Bframes = { .vt = VT_UI4, .ulVal = 2 };
		ICodecAPI_SetValue(Codec, &CODECAPI_AVEncMPVDefaultBPictureCount, &Bframes);

		ICodecAPI_Release(Codec);
	}

	if (Config->AudioFormat)
	{
		HR(CoCreateInstance(&CLSID_CResamplerMediaObject, NULL, CLSCTX_INPROC_SERVER, &IID_IMFTransform, (LPVOID*)&Resampler));

		// audio resampler input
		{
			IMFMediaType* Type;
			HR(MFCreateMediaType(&Type));
			HR(MFInitMediaTypeFromWaveFormatEx(Type, Config->AudioFormat, sizeof(*Config->AudioFormat) + Config->AudioFormat->cbSize));
			HR(IMFTransform_SetInputType(Resampler, 0, Type, 0));
			IMFMediaType_Release(Type);
		}

		// audio resampler output
		{
			WAVEFORMATEX Format =
			{
				.wFormatTag = WAVE_FORMAT_PCM,
				.nChannels = (WORD)Config->Config->AudioChannels,
				.nSamplesPerSec = Config->Config->AudioSamplerate,
				.wBitsPerSample = sizeof(short) * 8,
			};
			Format.nBlockAlign = Format.nChannels * Format.wBitsPerSample / 8;
			Format.nAvgBytesPerSec = Format.nSamplesPerSec * Format.nBlockAlign;

			IMFMediaType* Type;
			HR(MFCreateMediaType(&Type));
			HR(MFInitMediaTypeFromWaveFormatEx(Type, &Format, sizeof(Format)));
			HR(IMFTransform_SetOutputType(Resampler, 0, Type, 0));
			IMFMediaType_Release(Type);
		}

		HR(IMFTransform_ProcessMessage(Resampler, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));

		// audio output type
		{
			const GUID* Codec = &((GUID[]){ MFAudioFormat_AAC, MFAudioFormat_FLAC })[Config->Config->AudioCodec];

			IMFMediaType* Type;
			HR(MFCreateMediaType(&Type));
			HR(IMFMediaType_SetGUID(Type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio));
			HR(IMFMediaType_SetGUID(Type, &MF_MT_SUBTYPE, Codec));
			HR(IMFMediaType_SetUINT32(Type, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
			HR(IMFMediaType_SetUINT32(Type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, Config->Config->AudioSamplerate));
			HR(IMFMediaType_SetUINT32(Type, &MF_MT_AUDIO_NUM_CHANNELS, Config->Config->AudioChannels));
			if (Config->Config->AudioCodec == CONFIG_AUDIO_AAC)
			{
				HR(IMFMediaType_SetUINT32(Type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, Config->Config->AudioBitrate * 1000 / 8));
			}

			hr = IMFSinkWriter_AddStream(Writer, Type, &Encoder->AudioStreamIndex);
			IMFMediaType_Release(Type);

			if (FAILED(hr))
			{
				LOG_ERROR("IMFSinkWriter_AddStream (audio) failed: 0x%08lX", (unsigned long)hr);
				MessageBoxW(NULL, L"Cannot configure audio encoder output!", WCAP_TITLE, MB_ICONERROR);
				goto bail;
			}
			LOG_INFO("Audio stream added: index=%d", Encoder->AudioStreamIndex);
		}

		// audio input type
		{
			IMFMediaType* Type;
			HR(MFCreateMediaType(&Type));
			HR(IMFMediaType_SetGUID(Type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio));
			HR(IMFMediaType_SetGUID(Type, &MF_MT_SUBTYPE, &MFAudioFormat_PCM));
			HR(IMFMediaType_SetUINT32(Type, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
			HR(IMFMediaType_SetUINT32(Type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, Config->Config->AudioSamplerate));
			HR(IMFMediaType_SetUINT32(Type, &MF_MT_AUDIO_NUM_CHANNELS, Config->Config->AudioChannels));

			hr = IMFSinkWriter_SetInputMediaType(Writer, Encoder->AudioStreamIndex, Type, NULL);
			IMFMediaType_Release(Type);

			if (FAILED(hr))
			{
				LOG_ERROR("IMFSinkWriter_SetInputMediaType (audio) failed: 0x%08lX", (unsigned long)hr);
				MessageBoxW(NULL, L"Cannot configure audio encoder input!", WCAP_TITLE, MB_ICONERROR);
				goto bail;
			}
		}
	}

	hr = IMFSinkWriter_BeginWriting(Writer);
	if (FAILED(hr))
	{
		LOG_ERROR("IMFSinkWriter_BeginWriting failed: 0x%08lX", (unsigned long)hr);
		MessageBoxW(NULL, L"Cannot start writing to mp4 file!", WCAP_TITLE, MB_ICONERROR);
		goto bail;
	}
	LOG_INFO("SinkWriter BeginWriting OK");

	// log which encoder MFT the sink writer actually selected, so the log shows whether
	// hardware (GPU vendor) or software (Microsoft) encoder is being used
	{
		IMFSinkWriterEx* WriterEx;
		HRESULT hrEx = IMFSinkWriter_QueryInterface(Writer, &IID_IMFSinkWriterEx, (LPVOID*)&WriterEx);
		if (SUCCEEDED(hrEx))
		{
			IMFTransform* Transform;
			HRESULT hrTransform = IMFSinkWriterEx_GetTransformForStream(WriterEx, Encoder->VideoStreamIndex, 0, NULL, &Transform);
			if (SUCCEEDED(hrTransform))
			{
				IMFAttributes* TransformAttributes;
				HRESULT hrAttr = IMFTransform_GetAttributes(Transform, &TransformAttributes);
				if (SUCCEEDED(hrAttr))
				{
					LPWSTR Name;
					UINT32 NameLength;
					HRESULT hrName = IMFAttributes_GetAllocatedString(TransformAttributes, &MFT_FRIENDLY_NAME_Attribute, &Name, &NameLength);
					if (SUCCEEDED(hrName))
					{
						char NameUtf8[256];
						Encoder__WideToUtf8(Name, NameUtf8, sizeof(NameUtf8));
						LOG_INFO("Video encoder MFT: %s", NameUtf8);
						CoTaskMemFree(Name);
					}
					else
					{
						LOG_WARN("Video encoder MFT has no friendly name attribute: 0x%08lX", (unsigned long)hrName);
					}
					IMFAttributes_Release(TransformAttributes);
				}
				else
				{
					LOG_WARN("Cannot query video encoder MFT attributes: 0x%08lX", (unsigned long)hrAttr);
				}
				IMFTransform_Release(Transform);
			}
			else
			{
				LOG_WARN("Cannot query video encoder MFT: 0x%08lX", (unsigned long)hrTransform);
			}
			IMFSinkWriterEx_Release(WriterEx);
		}
		else
		{
			LOG_WARN("Cannot get IMFSinkWriterEx: 0x%08lX", (unsigned long)hrEx);
		}
	}

	// input texture
	{
		TexResize_Create(&Encoder->Resize, Device, InputWidth, InputHeight, OutputWidth, OutputHeight, Config->Config->GammaCorrectResize, D3D11_BIND_RENDER_TARGET);

		D3D11_RENDER_TARGET_VIEW_DESC InputViewDesc =
		{
			.Format = DXGI_FORMAT_B8G8R8A8_UNORM,
			.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D,
		};
		ID3D11Device_CreateRenderTargetView(Device, (ID3D11Resource*)Encoder->Resize.InputTexture, &InputViewDesc, &Encoder->InputView);

		FLOAT Black[] = { 0, 0, 0, 0 };
		ID3D11DeviceContext_ClearRenderTargetView(Context, Encoder->InputView, Black);
	}

	// yuv converter
	{
		YuvColorSpace ColorSpace = IsHD ? YuvColorSpace_BT709 : YuvColorSpace_BT601;
		YuvConvert_Create(&Encoder->Convert, Device, Encoder->Resize.OutputTexture, OutputWidth, OutputHeight, ColorSpace, Config->Config->ImprovedColorConversion);

		DXGI_FORMAT ConvertFormat = IsEqualGUID(VideoInputFormat, &MFVideoFormat_NV12) ? DXGI_FORMAT_NV12 : DXGI_FORMAT_P010;

		for (size_t OutputIndex = 0; OutputIndex < ENCODER_VIDEO_BUFFER_COUNT; OutputIndex++)
		{
			YuvConvertOutput_Create(&Encoder->ConvertOutput[OutputIndex], Device, OutputWidth, OutputHeight, ConvertFormat);
			ID3D11Texture2D* ConvertOutputTexture = Encoder->ConvertOutput[OutputIndex].Texture;

			IMFSample* VideoSample;
			HR(MFCreateVideoSampleFromSurface(NULL, &VideoSample));

			IMFMediaBuffer* Buffer;
			HR(MFCreateDXGISurfaceBuffer(&IID_ID3D11Texture2D, (IUnknown*)ConvertOutputTexture, 0, FALSE, &Buffer));

			UINT32 MaxLength;
			HR(IMFMediaBuffer_GetMaxLength(Buffer, &MaxLength));
			HR(IMFMediaBuffer_SetCurrentLength(Buffer, MaxLength));

			HR(IMFSample_AddBuffer(VideoSample, Buffer));
			IMFMediaBuffer_Release(Buffer);

			Encoder->VideoSample[OutputIndex] = VideoSample;
		}
	}

	Encoder->InputWidth = Config->Width;
	Encoder->InputHeight = Config->Height;
	Encoder->OutputWidth = OutputWidth;
	Encoder->OutputHeight = OutputHeight;
	Encoder->FramerateNum = Config->FramerateNum;
	Encoder->FramerateDen = Config->FramerateDen;
	Encoder->VideoDiscontinuity = FALSE;
	Encoder->VideoLastTime = 0x8000000000000000ULL; // some large time in future

	Assert(ENCODER_VIDEO_BUFFER_COUNT <= 64);
	atomic_init(&Encoder->VideoSampleAvailable, (1ULL << ENCODER_VIDEO_BUFFER_COUNT) - 1);

	if (Encoder->AudioStreamIndex >= 0)
	{
		// resampler input sample
		HR(MFCreateSample(&Encoder->AudioInputSample));

		// resampler output & audio encoding input buffer/samples
		for (int i = 0; i < ENCODER_AUDIO_BUFFER_COUNT; i++)
		{
			IMFSample* Sample;
			IMFMediaBuffer* Buffer;
			IMFTrackedSample* Tracked;

			HR(MFCreateTrackedSample(&Tracked));
			HR(IMFTrackedSample_QueryInterface(Tracked, &IID_IMFSample, (LPVOID*)&Sample));
			HR(MFCreateMemoryBuffer(Config->Config->AudioSamplerate * Config->Config->AudioChannels * sizeof(short), &Buffer));
			HR(IMFSample_AddBuffer(Sample, Buffer));
			IMFMediaBuffer_Release(Buffer);
			IMFTrackedSample_Release(Tracked);

			Encoder->AudioSample[i] = Sample;
		}

		Encoder->AudioFrameSize = Config->AudioFormat->nBlockAlign;
		Encoder->AudioSampleRate = Config->AudioFormat->nSamplesPerSec;
		Encoder->Resampler = Resampler;

		Assert(ENCODER_AUDIO_BUFFER_COUNT <= 64);
		atomic_init(&Encoder->AudioSampleAvailable, (1ULL << ENCODER_AUDIO_BUFFER_COUNT) - 1);
	}

	ID3D11DeviceContext_AddRef(Context);
	ID3D11Multithread_AddRef(Multithread);
	Encoder->Context = Context;
	Encoder->Multithread = Multithread;

	Encoder->StartTime = 0;
	atomic_init(&Encoder->VideoLastReturnTime, 0);
	atomic_init(&Encoder->DiscardQueued, 0);
	// stall timeout: 10 seconds without a video buffer return while recording = encoder stuck
	{
		LARGE_INTEGER Freq;
		QueryPerformanceFrequency(&Freq);
		Encoder->VideoStallTimeout = 10 * (UINT64)Freq.QuadPart;
	}
	Encoder->Writer = Writer;
	Writer = NULL;
	Resampler = NULL;

	atomic_init(&Encoder->VideoQueue.Read, 0);
	atomic_init(&Encoder->VideoQueue.Write, 0);
	atomic_init(&Encoder->AudioQueue.Read, 0);
	atomic_init(&Encoder->AudioQueue.Write, 0);
	Encoder->VideoThread = CreateThread(NULL, 0, &Encoder__VideoThread, Encoder, 0, NULL);
	Assert(Encoder->VideoThread);
	Encoder->AudioThread = NULL;
	if (Encoder->AudioStreamIndex >= 0)
	{
		Encoder->AudioThread = CreateThread(NULL, 0, &Encoder__AudioThread, Encoder, 0, NULL);
		Assert(Encoder->AudioThread);
	}

	Result = TRUE;

bail:
	if (Resampler)
	{
		IMFTransform_Release(Resampler);
	}
	if (Writer)
	{
		IMFSinkWriter_Release(Writer);
		DeleteFileW(FileName);
	}

	ID3D11Multithread_Release(Multithread);
	ID3D11DeviceContext_Release(Context);

	return Result;
}

void Encoder_Stop(Encoder* Encoder)
{
	LOG_INFO("Encoder_Stop: finalizing...");

	if (Encoder->AudioStreamIndex >= 0)
	{
		LOG_INFO("Encoder_Stop: draining audio resampler...");
		HRESULT hr = IMFTransform_ProcessMessage(Encoder->Resampler, MFT_MESSAGE_COMMAND_DRAIN, 0);
		if (FAILED(hr))
		{
			LOG_ERROR("Encoder_Stop: IMFTransform_ProcessMessage(DRAIN) failed: 0x%08lX", (unsigned long)hr);
		}
		Encoder__OutputAudioSamples(Encoder, TRUE);
	}

	// Check buffer availability before finalization
	uint64_t VideoAvailable = atomic_load(&Encoder->VideoSampleAvailable);
	uint64_t AudioAvailable = (Encoder->AudioStreamIndex >= 0) ? atomic_load(&Encoder->AudioSampleAvailable) : 0;
	LOG_INFO("Encoder_Stop: video buffers mask: 0x%016llX (expected 0x%016llX)",
		VideoAvailable, (1ULL << ENCODER_VIDEO_BUFFER_COUNT) - 1);
	if (Encoder->AudioStreamIndex >= 0)
	{
		LOG_INFO("Encoder_Stop: audio buffers mask: 0x%016llX (expected 0x%016llX)",
			AudioAvailable, (1ULL << ENCODER_AUDIO_BUFFER_COUNT) - 1);
	}

	// Log which video buffers are still in use
	if (VideoAvailable != ((1ULL << ENCODER_VIDEO_BUFFER_COUNT) - 1))
	{
		LOG_WARN("Encoder_Stop: some video buffers still in use by encoder!");
		for (int i = 0; i < ENCODER_VIDEO_BUFFER_COUNT; i++)
		{
			if (!(VideoAvailable & (1ULL << i)))
			{
				LOG_WARN("  Video buffer %d is still pending callback", i);
			}
		}
	}

	// enqueue stop commands, writer threads exit after everything before it is written
	uint32_t VideoQueued = atomic_load(&Encoder->VideoQueue.Write) - atomic_load(&Encoder->VideoQueue.Read);
	uint32_t AudioQueued = (Encoder->AudioStreamIndex >= 0)
		? atomic_load(&Encoder->AudioQueue.Write) - atomic_load(&Encoder->AudioQueue.Read) : 0;
	LOG_INFO("Encoder_Stop: waiting for writer threads to drain queues (video=%lu audio=%lu queued)...",
		(unsigned long)VideoQueued, (unsigned long)AudioQueued);
	Encoder__EnqueueStopCommand(Encoder, &Encoder->VideoQueue);
	if (Encoder->AudioStreamIndex >= 0)
	{
		Encoder__EnqueueStopCommand(Encoder, &Encoder->AudioQueue);
	}

	// wait for writer threads to finish; when the encoder is too congested to drain the backlog
	// in reasonable time, writer threads are told to discard the remaining queued samples
	{
		DWORD WaitStart = GetTickCount();
		DWORD LastLog = 0;
		for (;;)
		{
			BOOL VideoDone = WaitForSingleObject(Encoder->VideoThread, 100) == WAIT_OBJECT_0;
			BOOL AudioDone = TRUE;
			if (Encoder->AudioStreamIndex >= 0)
			{
				AudioDone = WaitForSingleObject(Encoder->AudioThread, 0) == WAIT_OBJECT_0;
			}
			if (VideoDone && AudioDone)
			{
				break;
			}
			DWORD Elapsed = GetTickCount() - WaitStart;
			if (Elapsed >= ENCODER_STOP_WAIT_TIMEOUT_MS)
			{
				if (!atomic_exchange_explicit(&Encoder->DiscardQueued, 1, memory_order_acq_rel))
				{
					LOG_WARN("Encoder_Stop: encoder too congested to drain backlog, discarding queued samples...");
				}
			}
			if (Elapsed - LastLog >= 5000)
			{
				uint32_t VQ = atomic_load(&Encoder->VideoQueue.Write) - atomic_load(&Encoder->VideoQueue.Read);
				uint32_t AQ = (Encoder->AudioStreamIndex >= 0)
					? atomic_load(&Encoder->AudioQueue.Write) - atomic_load(&Encoder->AudioQueue.Read) : 0;
				LOG_INFO("Encoder_Stop: still waiting for writer threads (video=%lu audio=%lu queued)...",
					(unsigned long)VQ, (unsigned long)AQ);
				LastLog = Elapsed;
			}
		}
	}
	CloseHandle(Encoder->VideoThread);
	if (Encoder->AudioStreamIndex >= 0)
	{
		CloseHandle(Encoder->AudioThread);
	}

	// everything queued has been written, finalize the file (may take a while if encoder was congested)
	LOG_INFO("Encoder_Stop: calling IMFSinkWriter_Finalize (this may take a while if encoder is congested)...");
	HRESULT hr = IMFSinkWriter_Finalize(Encoder->Writer);
	if (FAILED(hr))
	{
		LOG_ERROR("IMFSinkWriter_Finalize failed: 0x%08lX", (unsigned long)hr);
	}
	else
	{
		LOG_INFO("IMFSinkWriter_Finalize completed successfully");
	}

	// tracked sample callbacks fire asynchronously on MF workqueue threads and reference
	// this Encoder struct, wait for all buffers to come back before releasing them so
	// a late callback cannot race cleanup (or a subsequent recording session)
	uint64_t VideoFullMask = (1ULL << ENCODER_VIDEO_BUFFER_COUNT) - 1;
	if (!Encoder__WaitForCallbacks(&Encoder->VideoSampleAvailable, VideoFullMask, 5000))
	{
		LOG_WARN("Encoder_Stop: video sample callbacks still pending after timeout, releasing anyway (mask: 0x%016llX)",
			atomic_load(&Encoder->VideoSampleAvailable));
	}
	if (Encoder->AudioStreamIndex >= 0)
	{
		uint64_t AudioFullMask = (1ULL << ENCODER_AUDIO_BUFFER_COUNT) - 1;
		if (!Encoder__WaitForCallbacks(&Encoder->AudioSampleAvailable, AudioFullMask, 5000))
		{
			LOG_WARN("Encoder_Stop: audio sample callbacks still pending after timeout, releasing anyway (mask: 0x%016llX)",
				atomic_load(&Encoder->AudioSampleAvailable));
		}
	}

	if (Encoder->AudioStreamIndex >= 0)
	{
		IMFTransform_Release(Encoder->Resampler);
	}

	IMFSinkWriter_Release(Encoder->Writer);

	if (Encoder->AudioStreamIndex >= 0)
	{
		for (int i = 0; i < ENCODER_AUDIO_BUFFER_COUNT; i++)
		{
			IMFSample_Release(Encoder->AudioSample[i]);
		}
		IMFSample_Release(Encoder->AudioInputSample);
	}

	for (size_t OutputIndex = 0; OutputIndex < ENCODER_VIDEO_BUFFER_COUNT; OutputIndex++)
	{
		YuvConvertOutput_Release(&Encoder->ConvertOutput[OutputIndex]);
		IMFSample_Release(Encoder->VideoSample[OutputIndex]);
	}
	YuvConvert_Release(&Encoder->Convert);
	TexResize_Release(&Encoder->Resize);
	ID3D11RenderTargetView_Release(Encoder->InputView);

	ID3D11Multithread_Release(Encoder->Multithread);
	ID3D11DeviceContext_Release(Encoder->Context);
}

BOOL Encoder_NewFrame(Encoder* Encoder, ID3D11Texture2D* Texture, RECT Rect, UINT64 Time, UINT64 TimePeriod)
{
	Encoder->VideoLastTime = Time;

	uint64_t Available = atomic_load(&Encoder->VideoSampleAvailable);
	if (Available == 0)
	{
		// dropped frame - no SendStreamTick here, Encoder_Update handles
		// periodic discontinuity ticks through the writer thread queue
		Encoder->VideoDiscontinuity = TRUE;
		return FALSE;
	}

	DWORD Index;
	_BitScanForward64(&Index, Available);
	atomic_fetch_and(&Encoder->VideoSampleAvailable, ~(1ULL << Index));

	ID3D11DeviceContext* Context = Encoder->Context;
	ID3D11Multithread_Enter(Encoder->Multithread);

	// copy to input texture
	{
		D3D11_BOX Box =
		{
			.left = Rect.left,
			.top = Rect.top,
			.right = Rect.right,
			.bottom = Rect.bottom,
			.front = 0,
			.back = 1,
		};

		DWORD Width = Box.right - Box.left;
		DWORD Height = Box.bottom - Box.top;
		if (Width < Encoder->InputWidth || Height < Encoder->InputHeight)
		{
			FLOAT Black[] = { 0, 0, 0, 0 };
			ID3D11DeviceContext_ClearRenderTargetView(Context, Encoder->InputView, Black);

			Box.right = Box.left + min(Encoder->InputWidth, Box.right);
			Box.bottom = Box.top + min(Encoder->InputHeight, Box.bottom);
		}
		ID3D11DeviceContext_CopySubresourceRegion(Context, (ID3D11Resource*)Encoder->Resize.InputTexture, 0, 0, 0, 0, (ID3D11Resource*)Texture, 0, &Box);
	}

	// resize if needed
	TexResize_Dispatch(&Encoder->Resize, Context);

	// convert to YUV
	YuvConvert_Dispatch(&Encoder->Convert, Context, &Encoder->ConvertOutput[Index]);

	ID3D11DeviceContext_Flush(Context);
	ID3D11Multithread_Leave(Encoder->Multithread);

	// setup input time & duration
	if (Encoder->StartTime == 0)
	{
		Encoder->StartTime = Time;
	}

	IMFSample* Sample = Encoder->VideoSample[Index];
	HR(IMFSample_SetSampleDuration(Sample, MFllMulDiv(Encoder->FramerateDen, MF_UNITS_PER_SECOND, Encoder->FramerateNum, 0)));
	HR(IMFSample_SetSampleTime(Sample, MFllMulDiv(Time - Encoder->StartTime, MF_UNITS_PER_SECOND, TimePeriod, 0)));

	if (Encoder->VideoDiscontinuity)
	{
		HR(IMFSample_SetUINT32(Sample, &MFSampleExtension_Discontinuity, TRUE));
		Encoder->VideoDiscontinuity = FALSE;
	}
	else
	{
		// don't care about success or no, we just don't want this attribute set at all
		IMFSample_DeleteItem(Sample, &MFSampleExtension_Discontinuity);
	}

	// if video writer thread queue is full, drop this frame instead of piling up
	if (Encoder__QueueFull(&Encoder->VideoQueue))
	{
		atomic_fetch_or(&Encoder->VideoSampleAvailable, 1ULL << Index);
		Encoder->VideoDiscontinuity = TRUE;
		return FALSE;
	}

	IMFTrackedSample* Tracked;
	HR(IMFSample_QueryInterface(Sample, &IID_IMFTrackedSample, (LPVOID*)&Tracked));
	IMFTrackedSample_SetAllocator(Tracked, &Encoder->VideoSampleCallback, NULL);
	IMFTrackedSample_Release(Tracked);

	// submit to video writer thread which calls WriteSample in background (may block there without freezing us)
	// writer thread releases our Sample reference after WriteSample
	Encoder__Enqueue(&Encoder->VideoQueue, Sample, NULL, Encoder->VideoStreamIndex, 0);

	return TRUE;
}

typedef struct
{
	IMFMediaBuffer Buffer;
	uint8_t* SampleData;
	uint32_t SampleByteCount;
	uint32_t References;
}
EncoderAudioBuffer;

static HRESULT STDMETHODCALLTYPE EncoderAudioBuffer__QueryInterface(IMFMediaBuffer* This, REFIID Riid, void** Object)
{
	if (Object == NULL)
	{
		return E_POINTER;
	}
	if (IsEqualGUID(Riid, &IID_IMFMediaBuffer) || IsEqualGUID(Riid, &IID_IUnknown))
	{
		*Object = This;
		return S_OK;
	}
	return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE EncoderAudioBuffer__AddRef(IMFMediaBuffer* This)
{
	EncoderAudioBuffer* Buffer = CONTAINING_RECORD(This, EncoderAudioBuffer, Buffer);
	return Buffer->References += 1;
}

static ULONG STDMETHODCALLTYPE EncoderAudioBuffer__Release(IMFMediaBuffer* This)
{
	EncoderAudioBuffer* Buffer = CONTAINING_RECORD(This, EncoderAudioBuffer, Buffer);
	return Buffer->References -= 1;
}

static HRESULT STDMETHODCALLTYPE EncoderAudioBuffer__Lock(IMFMediaBuffer* This, BYTE** OutBuffer, DWORD* OutMaxLength, DWORD* OutCurrentLength)
{
	EncoderAudioBuffer* Buffer = CONTAINING_RECORD(This, EncoderAudioBuffer, Buffer);
	*OutBuffer = Buffer->SampleData;
	if (OutMaxLength)
	{
		*OutMaxLength = Buffer->SampleByteCount;
	}
	if (OutCurrentLength)
	{
		*OutCurrentLength = Buffer->SampleByteCount;
	}
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE EncoderAudioBuffer__Unlock(IMFMediaBuffer* This)
{
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE EncoderAudioBuffer__GetCurrentLength(IMFMediaBuffer* This, DWORD* OutCurrentLength)
{
	EncoderAudioBuffer* Buffer = CONTAINING_RECORD(This, EncoderAudioBuffer, Buffer);
	*OutCurrentLength = Buffer->SampleByteCount;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE EncoderAudioBuffer__SetCurrentLength(IMFMediaBuffer* This, DWORD InCurrentLength)
{
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE EncoderAudioBuffer__GetMaxLength(IMFMediaBuffer* This, DWORD* OutMaxLength)
{
	EncoderAudioBuffer* Buffer = CONTAINING_RECORD(This, EncoderAudioBuffer, Buffer);
	*OutMaxLength = Buffer->SampleByteCount;
	return S_OK;
}

static IMFMediaBufferVtbl EncoderAudioBufferVtbl =
{
	.QueryInterface   = &EncoderAudioBuffer__QueryInterface,
	.AddRef           = &EncoderAudioBuffer__AddRef,
	.Release          = &EncoderAudioBuffer__Release,
	.Lock             = &EncoderAudioBuffer__Lock,
	.Unlock           = &EncoderAudioBuffer__Unlock,
	.GetCurrentLength = &EncoderAudioBuffer__GetCurrentLength,
	.SetCurrentLength = &EncoderAudioBuffer__SetCurrentLength,
	.GetMaxLength     = &EncoderAudioBuffer__GetMaxLength,
};

void Encoder_NewSamples(Encoder* Encoder, LPCVOID Samples, DWORD FrameCount, UINT64 Time, UINT64 TimePeriod)
{
	static uint32_t LogCount;
	LogCount++;
	if (LogCount <= 5 || (LogCount % 100) == 0)
	{
		LOG_INFO("Encoder_NewSamples: frame_count=%lu time=%llu (call #%u)", (unsigned long)FrameCount, Time, LogCount);
	}

	EncoderAudioBuffer Input =
	{
		.Buffer.lpVtbl = &EncoderAudioBufferVtbl,
		.SampleData = (BYTE*)Samples,
		.SampleByteCount = FrameCount * Encoder->AudioFrameSize,
	};

	IMFSample* AudioSample = Encoder->AudioInputSample;
	HR(IMFSample_AddBuffer(AudioSample, &Input.Buffer));

	// setup input time & duration
	Assert(Encoder->StartTime != 0);
	HR(IMFSample_SetSampleDuration(AudioSample, MFllMulDiv(FrameCount, MF_UNITS_PER_SECOND, Encoder->AudioSampleRate, 0)));
	HR(IMFSample_SetSampleTime(AudioSample, MFllMulDiv(Time - Encoder->StartTime, MF_UNITS_PER_SECOND, TimePeriod, 0)));

	// drain pending resampler output first, without blocking on encoder thread congestion
	Encoder__OutputAudioSamples(Encoder, FALSE);

	HRESULT hr = IMFTransform_ProcessInput(Encoder->Resampler, 0, AudioSample, 0);
	if (hr == MF_E_NOTACCEPTING)
	{
		// resampler is full because encoder thread is congested, drop this audio chunk
		// instead of blocking the caller, capture ringbuffer keeps getting drained
		static uint32_t DropLogCount;
		if (++DropLogCount <= 3 || (DropLogCount % 100) == 0)
		{
			LOG_WARN("Encoder_NewSamples: resampler not accepting input (encoder congested), dropping audio chunk (total %u)", DropLogCount);
		}
	}
	else if (SUCCEEDED(hr))
	{
		Encoder__OutputAudioSamples(Encoder, FALSE);
	}
	else
	{
		LOG_ERROR("Encoder_NewSamples: IMFTransform_ProcessInput failed: 0x%08lX", (unsigned long)hr);
	}

	HR(IMFSample_RemoveAllBuffers(AudioSample));
	Assert(Input.References == 0);
}

// Returns TRUE if encoder is healthy, FALSE if video encoder is stalled (informational only)
BOOL Encoder_Update(Encoder* Encoder, UINT64 Time, UINT64 TimePeriod)
{
	// if there was no frame during last second, add discontinuity
	// tick goes through video writer thread queue, so it cannot block the caller
	if ((int64_t)(Time - Encoder->VideoLastTime) >= (int64_t)TimePeriod)
	{
		Encoder->VideoLastTime = Time;
		Encoder->VideoDiscontinuity = TRUE;

		LONGLONG Timestamp = MFllMulDiv(Time - Encoder->StartTime, MF_UNITS_PER_SECOND, TimePeriod, 0);
		Encoder__TryEnqueue(&Encoder->VideoQueue, NULL, NULL, Encoder->VideoStreamIndex, Timestamp);
	}

	// video stall detection: log when encoder is stalled (congestion guard in
	// Encoder_NewFrame handles the actual frame dropping to avoid blocking)
	if (Encoder->StartTime != 0 && Encoder->VideoStallTimeout != 0)
	{
		uint64_t LastReturn = atomic_load(&Encoder->VideoLastReturnTime);
		if (LastReturn != 0 && (Time - LastReturn) > Encoder->VideoStallTimeout)
		{
			uint64_t Available = atomic_load(&Encoder->VideoSampleAvailable);
			if (Available != ((1ULL << ENCODER_VIDEO_BUFFER_COUNT) - 1))
			{
				static uint64_t LastStallLog;
				if (Time - LastStallLog > 10 * TimePeriod)
				{
					LOG_WARN("Encoder_Update: video encoder stalled for %llu seconds, dropping frames until recovery. Available mask: 0x%016llX",
						(Time - LastReturn) / TimePeriod, Available);
					LastStallLog = Time;
				}
				return FALSE;
			}
		}
	}

	return TRUE;
}

void Encoder_GetStats(Encoder* Encoder, DWORD* Bitrate, DWORD* LengthMsec, UINT64* FileSize)
{
	MF_SINK_WRITER_STATISTICS Stats = { .cb = sizeof(Stats) };
	HR(IMFSinkWriter_GetStatistics(Encoder->Writer, Encoder->VideoStreamIndex, &Stats));

	*Bitrate = (DWORD)MFllMulDiv(8 * Stats.qwByteCountProcessed, MF_UNITS_PER_SECOND, 1000 * Stats.llLastTimestampProcessed, 0);
	*LengthMsec = (DWORD)(Stats.llLastTimestampProcessed / 10000);
	*FileSize = Stats.qwByteCountProcessed;

	if (Encoder->AudioStreamIndex >= 0)
	{
		HR(IMFSinkWriter_GetStatistics(Encoder->Writer, Encoder->AudioStreamIndex, &Stats));
		*Bitrate += (DWORD)MFllMulDiv(8 * Stats.qwByteCountProcessed, MF_UNITS_PER_SECOND, 1000 * Stats.llLastTimestampProcessed, 0);
		*FileSize += Stats.qwByteCountProcessed;
	}
}
