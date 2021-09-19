#include "wcap_encoder.h"
#include <evr.h>
#include <cguid.h>
#include <mfapi.h>
#include <codecapi.h>
#include <mferror.h>

#define MFT64(high, low) (((UINT64)high << 32) | (low))

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
	IUnknown* Object;
	IMFSample* Sample;
	IMFTrackedSample* Tracked;

	HR(IMFAsyncResult_GetObject(Result, &Object));
	HR(IUnknown_QueryInterface(Object, &IID_IMFSample, (LPVOID*)&Sample));
	HR(IUnknown_QueryInterface(Object, &IID_IMFTrackedSample, (LPVOID*)&Tracked));

	IUnknown_Release(Object);
	// keep Sample & Tracked object reference count incremented to reuse for new frame submission

	Encoder* Encoder = CONTAINING_RECORD(this, struct Encoder, SampleCallback);
	InterlockedIncrement(&Encoder->SampleCount);

	return S_OK;
}

static IMFAsyncCallbackVtbl Encoder__SampleCallbackVtbl =
{
	&Encoder__QueryInterface,
	&Encoder__AddRef,
	&Encoder__Release,
	&Encoder__GetParameters,
	&Encoder__VideoInvoke,
};

void Encoder_Init(Encoder* Encoder, ID3D11Device* Device, ID3D11DeviceContext* Context)
{
	HR(MFStartup(MF_VERSION, MFSTARTUP_LITE));
	UINT Token;

	IMFDXGIDeviceManager* Manager;
	HR(MFCreateDXGIDeviceManager(&Token, &Manager));
	HR(IMFDXGIDeviceManager_ResetDevice(Manager, (IUnknown*)Device, Token));

	ID3D11Device_AddRef(Device);
	ID3D11DeviceContext_AddRef(Context);

	Encoder->Manager = Manager;
	Encoder->Context = Context;
	Encoder->Device = Device;
	Encoder->SampleCallback.lpVtbl = &Encoder__SampleCallbackVtbl;
}

BOOL Encoder_Start(Encoder* Encoder, LPWSTR FileName, const EncoderConfig* Config)
{
	DWORD InputWidth = Config->Width;
	DWORD InputHeight = Config->Height;
	DWORD OutputWidth = Config->MaxWidth;
	DWORD OutputHeight = Config->MaxHeight;

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
	ID3D11Texture2D* Texture = NULL;
	HRESULT hr;

	// output file
	{
		const GUID* Container = Config->FragmentedOutput ? &MFTranscodeContainerType_FMPEG4 : &MFTranscodeContainerType_MPEG4;

		IMFAttributes* Attributes;
		HR(MFCreateAttributes(&Attributes, 4));
		HR(IMFAttributes_SetUINT32(Attributes, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, Config->HardwareEncoder));
		HR(IMFAttributes_SetUnknown(Attributes, &MF_SINK_WRITER_D3D_MANAGER, (IUnknown*)Encoder->Manager));
		HR(IMFAttributes_SetUINT32(Attributes, &MF_SINK_WRITER_DISABLE_THROTTLING, TRUE));
		HR(IMFAttributes_SetGUID(Attributes, &MF_TRANSCODE_CONTAINERTYPE, Container));

		hr = MFCreateSinkWriterFromURL(FileName, NULL, Attributes, &Writer);
		IMFAttributes_Release(Attributes);

		if (FAILED(hr))
		{
			MessageBoxW(NULL, L"Cannot create output mp4 file!", WCAP_TITLE, MB_ICONERROR);
			goto bail;
		}
	}

	// video output type
	{
		IMFMediaType* Type;
		HR(MFCreateMediaType(&Type));

		HR(IMFMediaType_SetGUID(Type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video));
		HR(IMFMediaType_SetGUID(Type, &MF_MT_SUBTYPE, &MFVideoFormat_H264));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_0_255));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
		HR(IMFMediaType_SetUINT64(Type, &MF_MT_FRAME_RATE, MFT64(Config->FramerateNum, Config->FramerateDen)));
		HR(IMFMediaType_SetUINT64(Type, &MF_MT_FRAME_SIZE, MFT64(OutputWidth, OutputHeight)));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_AVG_BITRATE, Config->VideoBitrate * 1000));

		hr = IMFSinkWriter_AddStream(Writer, Type, &Encoder->VideoStreamIndex);
		IMFMediaType_Release(Type);

		if (FAILED(hr))
		{
			MessageBoxW(NULL, L"Cannot configure H264 encoder output!", WCAP_TITLE, MB_ICONERROR);
			goto bail;
		}
	}

	// video input type, 32-bit RGB - sink writer will automatically do conversion to YUV for H264 codec
	{
		IMFMediaType* Type;
		HR(MFCreateMediaType(&Type));
		HR(IMFMediaType_SetGUID(Type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video));
		HR(IMFMediaType_SetGUID(Type, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
		HR(IMFMediaType_SetUINT64(Type, &MF_MT_FRAME_SIZE, MFT64(InputWidth, InputHeight)));

		hr = IMFSinkWriter_SetInputMediaType(Writer, Encoder->VideoStreamIndex, Type, NULL);
		IMFMediaType_Release(Type);

		if (FAILED(hr))
		{
			MessageBoxW(NULL, L"Cannot configure H264 encoder input!", WCAP_TITLE, MB_ICONERROR);
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
		VARIANT Bitrate = { .vt = VT_UI4, .ulVal = Config->VideoBitrate * 1000 };
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

	hr = IMFSinkWriter_BeginWriting(Writer);
	if (FAILED(hr))
	{
		MessageBoxW(NULL, L"Cannot start writing to mp4 file!", WCAP_TITLE, MB_ICONERROR);
		goto bail;
	}

	// create texture array - as many as there are buffers
	D3D11_TEXTURE2D_DESC TextureDesc =
	{
		.Width = InputWidth,
		.Height = InputHeight,
		.MipLevels = 1,
		.ArraySize = ENCODER_BUFFER_COUNT,
		.Format = DXGI_FORMAT_B8G8R8A8_UNORM,
		.SampleDesc = { 1, 0 },
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_RENDER_TARGET,
	};
	hr = ID3D11Device_CreateTexture2D(Encoder->Device, &TextureDesc, NULL, &Texture);
	if (FAILED(hr))
	{
		MessageBoxW(NULL, L"Cannot allocate GPU resources!", WCAP_TITLE, MB_ICONERROR);
		goto bail;
	}

	UINT32 Size;
	HR(MFCalculateImageSize(&MFVideoFormat_RGB32, InputWidth, InputHeight, &Size));

	// create samples each referencing individual element of texture array
	for (int i = 0; i < ENCODER_BUFFER_COUNT; i++)
	{
		D3D11_RENDER_TARGET_VIEW_DESC ViewDesc =
		{
			.Format = TextureDesc.Format,
			.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY,
			.Texture2DArray.FirstArraySlice = i,
			.Texture2DArray.ArraySize = 1,
		};

		IMFSample* Sample;
		IMFMediaBuffer* Buffer;
		IMFTrackedSample* Tracked;

		HR(MFCreateVideoSampleFromSurface(NULL, &Sample));
		HR(MFCreateDXGISurfaceBuffer(&IID_ID3D11Texture2D, (IUnknown*)Texture, i, FALSE, &Buffer));
		HR(IMFMediaBuffer_SetCurrentLength(Buffer, Size));
		HR(IMFSample_AddBuffer(Sample, Buffer));
		HR(IMFSample_QueryInterface(Sample, &IID_IMFTrackedSample, (LPVOID*)&Tracked));
		HR(ID3D11Device_CreateRenderTargetView(Encoder->Device, (ID3D11Resource*)Texture, &ViewDesc, &Encoder->TextureView[i]));
		IMFMediaBuffer_Release(Buffer);

		Encoder->Sample[i] = Sample;
		Encoder->Tracked[i] = Tracked;
	}

	Encoder->InputWidth = InputWidth;
	Encoder->InputHeight = InputHeight;
	Encoder->OutputWidth = OutputWidth;
	Encoder->OutputHeight = OutputHeight;
	Encoder->FramerateNum = Config->FramerateNum;
	Encoder->FramerateDen = Config->FramerateDen;
	Encoder->Texture = Texture;
	Encoder->SampleIndex = 0;
	Encoder->SampleCount = ENCODER_BUFFER_COUNT;

	Encoder->StartTime = 0;
	Encoder->Writer = Writer;
	Writer = NULL;
	Texture = NULL;
	Result = TRUE;

bail:
	if (Texture)
	{
		ID3D11Texture2D_Release(Texture);
	}
	if (Writer)
	{
		IMFSinkWriter_Release(Writer);
		DeleteFileW(FileName);
	}

	return Result;
}

void Encoder_Stop(Encoder* Encoder)
{
	IMFSinkWriter_Finalize(Encoder->Writer);
	IMFSinkWriter_Release(Encoder->Writer);

	for (int i = 0; i < ENCODER_BUFFER_COUNT; i++)
	{
		ID3D11RenderTargetView_Release(Encoder->TextureView[i]);
		IMFSample_Release(Encoder->Sample[i]);
		IMFTrackedSample_Release(Encoder->Tracked[i]);
	}
	ID3D11Texture2D_Release(Encoder->Texture);

	ID3D11DeviceContext_Flush(Encoder->Context);
}

BOOL Encoder_NewFrame(Encoder* Encoder, ID3D11Texture2D* Texture, RECT Rect, UINT64 Time, UINT64 TimePeriod)
{
	if (Encoder->SampleCount == 0)
	{
		return FALSE;
	}
	DWORD Index = Encoder->SampleIndex;
	Encoder->SampleIndex = (Index + 1) % ENCODER_BUFFER_COUNT;
	InterlockedDecrement(&Encoder->SampleCount);

	IMFSample* Sample = Encoder->Sample[Index];
	IMFTrackedSample* Tracked = Encoder->Tracked[Index];

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
			ID3D11DeviceContext_ClearRenderTargetView(Encoder->Context, Encoder->TextureView[Index], Black);

			Box.right = Box.left + min(Encoder->InputWidth, Box.right);
			Box.bottom = Box.top + min(Encoder->InputHeight, Box.bottom);
		}
		ID3D11DeviceContext_CopySubresourceRegion(Encoder->Context, (ID3D11Resource*)Encoder->Texture, Index, 0, 0, 0, (ID3D11Resource*)Texture, 0, &Box);
	}

	// setup input time & duration
	if (Encoder->StartTime == 0)
	{
		Encoder->StartTime = Time;
	}
	HR(IMFSample_SetSampleDuration(Sample, MFllMulDiv(Encoder->FramerateDen, 10000000, Encoder->FramerateNum, 0)));
	HR(IMFSample_SetSampleTime(Sample, MFllMulDiv(Time - Encoder->StartTime, 10000000, TimePeriod, 0)));

	// submit to encoder which will happen in background
	HR(IMFTrackedSample_SetAllocator(Tracked, &Encoder->SampleCallback, NULL));
	HR(IMFSinkWriter_WriteSample(Encoder->Writer, Encoder->VideoStreamIndex, Sample));
	IMFSample_Release(Sample);
	IMFTrackedSample_Release(Tracked);

	return TRUE;
}

void Encoder_GetStats(Encoder* Encoder, DWORD* Bitrate, DWORD* LengthMsec, UINT64* FileSize)
{
	MF_SINK_WRITER_STATISTICS Stats = { .cb = sizeof(Stats) };
	HR(IMFSinkWriter_GetStatistics(Encoder->Writer, 0, &Stats));

	*Bitrate = (DWORD)MFllMulDiv(8 * Stats.qwByteCountProcessed, 10000000, 1000 * Stats.llLastTimestampProcessed, 0);
	*LengthMsec = (DWORD)(Stats.llLastTimestampProcessed / 10000);
	*FileSize = Stats.qwByteCountProcessed;
}
