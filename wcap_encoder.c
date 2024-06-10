#include "wcap_encoder.h"
#include <d3dcompiler.h>
#include <evr.h>
#include <cguid.h>
#include <mfapi.h>
#include <mferror.h>
#include <codecapi.h>
#include <wmcodecdsp.h>

#include "wcap_shader_Resize.h"
#include "wcap_shader_ConvertSimple.h"
#include "wcap_shader_ConvertPass1.h"
#include "wcap_shader_ConvertPass2.h"
#include "wcap_shader_PrepareLookup8.h"
#include "wcap_shader_PrepareLookup16.h"

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

	HR(IMFAsyncResult_GetObject(Result, &Object));
	HR(IUnknown_QueryInterface(Object, &IID_IMFSample, (LPVOID*)&Sample));
	IUnknown_Release(Object);
	// keep Sample object reference count incremented to reuse for new frame submission

	Encoder* Encoder = CONTAINING_RECORD(this, struct Encoder, VideoSampleCallback);
	InterlockedIncrement(&Encoder->VideoCount);

	return S_OK;
}

static HRESULT STDMETHODCALLTYPE Encoder__AudioInvoke(IMFAsyncCallback* this, IMFAsyncResult* Result)
{
	IUnknown* Object;
	IMFSample* Sample;

	HR(IMFAsyncResult_GetObject(Result, &Object));
	HR(IUnknown_QueryInterface(Object, &IID_IMFSample, (LPVOID*)&Sample));
	IUnknown_Release(Object);
	// keep Sample object reference count incremented to reuse for new sample submission

	Encoder* Encoder = CONTAINING_RECORD(this, struct Encoder, AudioSampleCallback);
	InterlockedIncrement(&Encoder->AudioCount);
	WakeByAddressSingle(&Encoder->AudioCount);

	return S_OK;
}

static IMFAsyncCallbackVtbl Encoder__VideoSampleCallbackVtbl =
{
	&Encoder__QueryInterface,
	&Encoder__AddRef,
	&Encoder__Release,
	&Encoder__GetParameters,
	&Encoder__VideoInvoke,
};

static IMFAsyncCallbackVtbl Encoder__AudioSampleCallbackVtbl =
{
	&Encoder__QueryInterface,
	&Encoder__AddRef,
	&Encoder__Release,
	&Encoder__GetParameters,
	&Encoder__AudioInvoke,
};

static void Encoder__OutputAudioSamples(Encoder* Encoder)
{
	for (;;)
	{
		// we don't want to drop any audio frames, so wait for available sample/buffer
		LONG Count = Encoder->AudioCount;
		while (Count == 0)
		{
			LONG Zero = 0;
			WaitOnAddress(&Encoder->AudioCount, &Zero, sizeof(LONG), INFINITE);
			Count = Encoder->AudioCount;
		}

		DWORD Index = Encoder->AudioIndex;

		IMFSample* Sample = Encoder->AudioSample[Index];

		DWORD Status;
		MFT_OUTPUT_DATA_BUFFER Output = { .dwStreamID = 0, .pSample = Sample };
		HRESULT hr = IMFTransform_ProcessOutput(Encoder->Resampler, 0, 1, &Output, &Status);
		if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
		{
			// no output is available
			break;
		}
		Assert(SUCCEEDED(hr));

		Encoder->AudioIndex = (Index + 1) % ENCODER_AUDIO_BUFFER_COUNT;
		InterlockedDecrement(&Encoder->AudioCount);

		IMFTrackedSample* Tracked;
		HR(IMFSample_QueryInterface(Sample, &IID_IMFTrackedSample, (LPVOID*)&Tracked));
		HR(IMFTrackedSample_SetAllocator(Tracked, &Encoder->AudioSampleCallback, (IUnknown*)Tracked));

		HR(IMFSinkWriter_WriteSample(Encoder->Writer, Encoder->AudioStreamIndex, Sample));

		IMFSample_Release(Sample);
		IMFTrackedSample_Release(Tracked);
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
	UINT Token;
	IMFDXGIDeviceManager* Manager;
	HR(MFCreateDXGIDeviceManager(&Token, &Manager));
	HR(IMFDXGIDeviceManager_ResetDevice(Manager, (IUnknown*)Device, Token));

	ID3D11DeviceContext* Context;
	ID3D11Device_GetImmediateContext(Device, &Context);

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
	const GUID* MediaFormatYUV;
	DXGI_FORMAT FormatYUV;
	DXGI_FORMAT FormatY;
	DXGI_FORMAT FormatUV;
	if (Config->Config->VideoCodec == CONFIG_VIDEO_H264)
	{
		MediaFormatYUV = &MFVideoFormat_NV12;
		FormatYUV = DXGI_FORMAT_NV12;
		FormatY = DXGI_FORMAT_R8_UNORM;
		FormatUV = DXGI_FORMAT_R8G8_UNORM;

		Container = Config->Config->FragmentedOutput ? &MFTranscodeContainerType_FMPEG4 : &MFTranscodeContainerType_MPEG4;
		Codec = &MFVideoFormat_H264;
		Profile = ((UINT32[]) { eAVEncH264VProfile_Base, eAVEncH264VProfile_Main, eAVEncH264VProfile_High })[Config->Config->VideoProfile];

	}
	else if (Config->Config->VideoCodec == CONFIG_VIDEO_H265 && Config->Config->VideoProfile == CONFIG_VIDEO_MAIN)
	{
		MediaFormatYUV = &MFVideoFormat_NV12;
		FormatYUV = DXGI_FORMAT_NV12;
		FormatY = DXGI_FORMAT_R8_UNORM;
		FormatUV = DXGI_FORMAT_R8G8_UNORM;

		Container = &MFTranscodeContainerType_MPEG4;
		Codec = &MFVideoFormat_HEVC;
		Profile = eAVEncH265VProfile_Main_420_8;

	}
	else // Config->Config->VideoCodec == CONFIG_VIDEO_H265 && Config->Config->VideoProfile == CONFIG_VIDEO_MAIN_10
	{
		MediaFormatYUV = &MFVideoFormat_P010;
		FormatYUV = DXGI_FORMAT_P010;
		FormatY = DXGI_FORMAT_R16_UNORM;
		FormatUV = DXGI_FORMAT_R16G16_UNORM;

		Container = &MFTranscodeContainerType_MPEG4;
		Codec = &MFVideoFormat_HEVC;
		Profile = eAVEncH265VProfile_Main_420_10;
	}

	// output file
	{
		IMFAttributes* Attributes;
		HR(MFCreateAttributes(&Attributes, 4));
		HR(IMFAttributes_SetUINT32(Attributes, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, Config->Config->HardwareEncoder));
		HR(IMFAttributes_SetUnknown(Attributes, &MF_SINK_WRITER_D3D_MANAGER, (IUnknown*)Manager));
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
		HR(IMFMediaType_SetGUID(Type, &MF_MT_SUBTYPE, Codec));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_MPEG2_PROFILE, Profile));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
		HR(IMFMediaType_SetUINT64(Type, &MF_MT_FRAME_RATE, MFT64(Config->FramerateNum, Config->FramerateDen)));
		HR(IMFMediaType_SetUINT64(Type, &MF_MT_FRAME_SIZE, MFT64(OutputWidth, OutputHeight)));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_AVG_BITRATE, Config->Config->VideoBitrate * 1000));

		hr = IMFSinkWriter_AddStream(Writer, Type, &Encoder->VideoStreamIndex);
		IMFMediaType_Release(Type);

		if (FAILED(hr))
		{
			MessageBoxW(NULL, L"Cannot configure video encoder!", WCAP_TITLE, MB_ICONERROR);
			goto bail;
		}
	}

	// video input type, NV12 or P010 format
	{
		IMFMediaType* Type;
		HR(MFCreateMediaType(&Type));
		HR(IMFMediaType_SetGUID(Type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video));
		HR(IMFMediaType_SetGUID(Type, &MF_MT_SUBTYPE, MediaFormatYUV));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
		HR(IMFMediaType_SetUINT64(Type, &MF_MT_FRAME_RATE, MFT64(Config->FramerateNum, Config->FramerateDen)));
		HR(IMFMediaType_SetUINT64(Type, &MF_MT_FRAME_SIZE, MFT64(OutputWidth, OutputHeight)));

		hr = IMFSinkWriter_SetInputMediaType(Writer, Encoder->VideoStreamIndex, Type, NULL);
		IMFMediaType_Release(Type);

		if (FAILED(hr))
		{
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
				MessageBoxW(NULL, L"Cannot configure audio encoder output!", WCAP_TITLE, MB_ICONERROR);
				goto bail;
			}
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
				MessageBoxW(NULL, L"Cannot configure audio encoder input!", WCAP_TITLE, MB_ICONERROR);
				goto bail;
			}
		}
	}

	hr = IMFSinkWriter_BeginWriting(Writer);
	if (FAILED(hr))
	{
		MessageBoxW(NULL, L"Cannot start writing to mp4 file!", WCAP_TITLE, MB_ICONERROR);
		goto bail;
	}

#if ENABLE_TIMING_QUERIES
	D3D11_QUERY_DESC DisjointDesc = { D3D11_QUERY_TIMESTAMP_DISJOINT };
	D3D11_QUERY_DESC TimestampDesc = { D3D11_QUERY_TIMESTAMP };

	for (int i = 0; i < 2; i++)
	{
		ID3D11Device_CreateQuery(Device, &DisjointDesc, &Encoder->QueryDisjoint[i]);
		for (int k = 0; k < _countof(Encoder->QueryTimestamp[i]); k++)
		{
			ID3D11Device_CreateQuery(Device, &TimestampDesc, &Encoder->QueryTimestamp[i][k]);
		}
	}

	Encoder->QueryIndex = -1;
#endif

	// https://en.wikipedia.org/wiki/YCbCr#ITU-R_BT.709_conversion
	const float BT709[7][4] =
	{
		// RGB to YUV
		{ +0.2126f, +0.7152f, +0.0722f },
		{ -0.1146f, -0.3854f, +0.5000f },
		{ +0.5000f, -0.4542f, -0.0458f },
		// YUV to RGB
		{ 1.f, +0.0000f, +1.5748f },
		{ 1.f, -0.1873f, -0.4681f },
		{ 1.f, +1.8556f, +0.0000f },
	};

	// https://en.wikipedia.org/wiki/YCbCr#ITU-R_BT.601_conversion
	const float BT601[7][4] =
	{
		// RGB to YUV
		{ +0.299000f, +0.587000f, +0.114000f },
		{ -0.168736f, -0.331264f, +0.500000f },
		{ +0.500000f, -0.418688f, -0.081312f },
		// YUV to RGB
		{ 1.f, +0.000000f, +1.402000f },
		{ 1.f, -0.344136f, -0.714136f },
		{ 1.f, +1.772000f, +0.000000f },
	};

	// https://github.com/mpv-player/mpv/blob/master/video/csputils.c#L150-L153
	BOOL IsHD = (OutputWidth >= 1280) || (OutputHeight > 576);

	D3D11_BUFFER_DESC BufferDesc =
	{
		.ByteWidth = IsHD ? sizeof(BT709) : sizeof(BT601),
		.Usage = D3D11_USAGE_IMMUTABLE,
		.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
	};
	D3D11_SUBRESOURCE_DATA BufferData =
	{
		.pSysMem = IsHD ? BT709 : BT601,
	};
	ID3D11Device_CreateBuffer(Device, &BufferDesc, &BufferData, &Encoder->ConvertBuffer);

	ID3DBlob* ShaderBlob;

	if (Config->Config->ImprovedColorConversion)
	{
		Encoder->ConvertSimpleShader = NULL;

		HR(D3DDecompressShaders(ConvertPass1ShaderBytes, sizeof(ConvertPass1ShaderBytes), 1, 0, NULL, 0, &ShaderBlob, NULL));
		ID3D11Device_CreateComputeShader(Device, ID3D10Blob_GetBufferPointer(ShaderBlob), ID3D10Blob_GetBufferSize(ShaderBlob), NULL, &Encoder->ConvertPass1Shader);
		ID3D10Blob_Release(ShaderBlob);

		HR(D3DDecompressShaders(ConvertPass2ShaderBytes, sizeof(ConvertPass2ShaderBytes), 1, 0, NULL, 0, &ShaderBlob, NULL));
		ID3D11Device_CreateComputeShader(Device, ID3D10Blob_GetBufferPointer(ShaderBlob), ID3D10Blob_GetBufferSize(ShaderBlob), NULL, &Encoder->ConvertPass2Shader);
		ID3D10Blob_Release(ShaderBlob);

		D3D11_TEXTURE3D_DESC LookupDesc =
		{
			.Width  = 256, // L
			.Height = 225, // U
			.Depth  = 225, // V
			.MipLevels = 1,
			.Format = FormatY, // 8-bit or 16-bit unorm
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
		};

		ID3D11Texture3D* LookupTexture;
		ID3D11Device_CreateTexture3D(Device, &LookupDesc, NULL, &LookupTexture);

		ID3D11UnorderedAccessView* LookupView;
		ID3D11Device_CreateUnorderedAccessView(Device, (ID3D11Resource*)LookupTexture, NULL, &LookupView);
		ID3D11Device_CreateShaderResourceView(Device, (ID3D11Resource*)LookupTexture, NULL, &Encoder->LookupView);

		LPCVOID PrepareLookupShaderBytes = FormatY == DXGI_FORMAT_R8_UNORM ? PrepareLookup8ShaderBytes : PrepareLookup16ShaderBytes;
		SIZE_T PrepareLookupShaderSize = FormatY == DXGI_FORMAT_R8_UNORM ? sizeof(PrepareLookup8ShaderBytes) : sizeof(PrepareLookup16ShaderBytes);

		ID3D11ComputeShader* PrepareLookupShader;
		HR(D3DDecompressShaders(PrepareLookupShaderBytes, PrepareLookupShaderSize, 1, 0, NULL, 0, &ShaderBlob, NULL));
		ID3D11Device_CreateComputeShader(Device, ID3D10Blob_GetBufferPointer(ShaderBlob), ID3D10Blob_GetBufferSize(ShaderBlob), NULL, &PrepareLookupShader);
		ID3D10Blob_Release(ShaderBlob);

		ID3D11DeviceContext_CSSetShader(Context, PrepareLookupShader, NULL, 0);
		ID3D11DeviceContext_CSSetUnorderedAccessViews(Context, 0, 1, &LookupView, NULL);
		ID3D11DeviceContext_CSSetConstantBuffers(Context, 0, 1, &Encoder->ConvertBuffer);

#if ENABLE_TIMING_QUERIES
		ID3D11DeviceContext_Begin(Context, (ID3D11Asynchronous*)Encoder->QueryDisjoint[0]);
		ID3D11DeviceContext_End(Context, (ID3D11Asynchronous*)Encoder->QueryTimestamp[0][0]);
#endif

		ID3D11DeviceContext_Dispatch(Context, DIV_ROUND_UP(LookupDesc.Width, 4), DIV_ROUND_UP(LookupDesc.Height, 4), DIV_ROUND_UP(LookupDesc.Depth, 4));

#if ENABLE_TIMING_QUERIES
		ID3D11DeviceContext_End(Context, (ID3D11Asynchronous*)Encoder->QueryTimestamp[1][0]);
		ID3D11DeviceContext_End(Context, (ID3D11Asynchronous*)Encoder->QueryDisjoint[0]);

		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT Disjoint;
		while (ID3D11DeviceContext_GetData(Context, (ID3D11Asynchronous*)Encoder->QueryDisjoint[0], &Disjoint, sizeof(Disjoint), 0) != S_OK) SwitchToThread();

		if (Disjoint.Disjoint == FALSE)
		{
			UINT64 Timestamp[2];

			while (ID3D11DeviceContext_GetData(Context, (ID3D11Asynchronous*)Encoder->QueryTimestamp[0][0], &Timestamp[0], sizeof(Timestamp[0]), 0) != S_OK) SwitchToThread();
			while (ID3D11DeviceContext_GetData(Context, (ID3D11Asynchronous*)Encoder->QueryTimestamp[1][0], &Timestamp[1], sizeof(Timestamp[1]), 0) != S_OK) SwitchToThread();

			wchar_t Str[1024];
			StrFormat(Str, L"PrepareLookup = %4llu usec\n", 1000000ULL * (Timestamp[1] - Timestamp[0]) / Disjoint.Frequency);
			OutputDebugStringW(Str);
		}
#endif

#if 0
		{
			LookupDesc.Usage = D3D11_USAGE_STAGING;
			LookupDesc.BindFlags = 0;
			LookupDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

			ID3D11Texture3D* TempTexture;
			ID3D11Device_CreateTexture3D(Device, &LookupDesc, NULL, &TempTexture);

			ID3D11DeviceContext_CopyResource(Context, (ID3D11Resource*)TempTexture, (ID3D11Resource*)LookupTexture);

			ID3D11DeviceContext_End(Context, (ID3D11Asynchronous*)QueryTimestamp[1]);
			ID3D11DeviceContext_End(Context, (ID3D11Asynchronous*)QueryDisjoint);

			D3D11_MAPPED_SUBRESOURCE Mapped;
			ID3D11DeviceContext_Map(Context, (ID3D11Resource*)TempTexture, 0, D3D11_MAP_READ, 0, &Mapped);

			HANDLE File = CreateFileA("lookup8.bin", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
			for (UINT v = 0; v < LookupDesc.Depth; v++)
			{
				for (UINT u = 0; u < LookupDesc.Height; u++)
				{
					void* Row = (char*)Mapped.pData + v * Mapped.DepthPitch + u * Mapped.RowPitch;

					DWORD Written;
					WriteFile(File, Row, LookupDesc.Width, &Written, NULL);
				}
			}
			CloseHandle(File);

			ID3D11Texture3D_Release(TempTexture);

			ExitProcess(0);
		}
#endif

		ID3D11ComputeShader_Release(PrepareLookupShader);

		ID3D11UnorderedAccessView_Release(LookupView);
		ID3D11Texture3D_Release(LookupTexture);
	}
	else
	{
		HR(D3DDecompressShaders(ConvertSimpleShaderBytes, sizeof(ConvertSimpleShaderBytes), 1, 0, NULL, 0, &ShaderBlob, NULL));
		ID3D11Device_CreateComputeShader(Device, ID3D10Blob_GetBufferPointer(ShaderBlob), ID3D10Blob_GetBufferSize(ShaderBlob), NULL, &Encoder->ConvertSimpleShader);
		ID3D10Blob_Release(ShaderBlob);
	}

	D3D11_SAMPLER_DESC LinearSamplerDesc =
	{
		.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT,
		.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
		.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
		.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
	};
	ID3D11Device_CreateSamplerState(Device, &LinearSamplerDesc, &Encoder->LinearSampler);

	// video texture/buffers/samples
	{
		// RGB input texture
		{
			D3D11_TEXTURE2D_DESC TextureDesc =
			{
				.Width = InputWidth,
				.Height = InputHeight,
				.MipLevels = 1,
				.ArraySize = 1,
				.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS,
				.SampleDesc = { 1, 0 },
				.Usage = D3D11_USAGE_DEFAULT,
				.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
			};
			ID3D11Device_CreateTexture2D(Device, &TextureDesc, NULL, &Encoder->InputTexture);

			D3D11_RENDER_TARGET_VIEW_DESC ResizeRenderTargetView =
			{
				.Format = DXGI_FORMAT_B8G8R8A8_UNORM,
				.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D,
			};
			ID3D11Device_CreateRenderTargetView(Device, (ID3D11Resource*)Encoder->InputTexture, &ResizeRenderTargetView, &Encoder->InputRenderTarget);

			FLOAT Black[] = { 0, 0, 0, 0 };
			ID3D11DeviceContext_ClearRenderTargetView(Context, Encoder->InputRenderTarget, Black);
		}

		ID3D11Texture2D* ResizedTexture;

		// RGB resized texture & resize shader
		if (InputWidth == OutputWidth && InputHeight == OutputHeight)
		{
			// no resizing needed, use input texture as input to converter shader directly
			ResizedTexture = Encoder->InputTexture;
			ID3D11Texture2D_AddRef(ResizedTexture);

			Encoder->ResizeShader = NULL;
		}
		else
		{
			D3D11_TEXTURE2D_DESC TextureDesc =
			{
				.Width = OutputWidth,
				.Height = OutputHeight,
				.MipLevels = 1,
				.ArraySize = 1,
				.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS,
				.SampleDesc = { 1, 0 },
				.Usage = D3D11_USAGE_DEFAULT,
				.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE,
			};
			ID3D11Device_CreateTexture2D(Device, &TextureDesc, NULL, &ResizedTexture);

			// TODO: resizer should use SRGB
			D3D11_SHADER_RESOURCE_VIEW_DESC ResizeInputView =
			{
				.Format = DXGI_FORMAT_B8G8R8A8_UNORM,
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
				.Texture2D.MipLevels = -1,
			};
			ID3D11Device_CreateShaderResourceView(Device, (ID3D11Resource*)Encoder->InputTexture, &ResizeInputView, &Encoder->ResizeInputView);

			// because D3D 11.0 does not support B8G8R8A8_UNORM for UAV, create uint UAV used on BGRA texture
			D3D11_UNORDERED_ACCESS_VIEW_DESC ResizeOutputView =
			{
				.Format = DXGI_FORMAT_R32_UINT,
				.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			};
			ID3D11Device_CreateUnorderedAccessView(Device, (ID3D11Resource*)ResizedTexture, &ResizeOutputView, &Encoder->ResizeOutputView);

			HR(D3DDecompressShaders(ResizeShaderBytes, sizeof(ResizeShaderBytes), 1, 0, NULL, 0, &ShaderBlob, NULL));
			ID3D11Device_CreateComputeShader(Device, ID3D10Blob_GetBufferPointer(ShaderBlob), ID3D10Blob_GetBufferSize(ShaderBlob), NULL, &Encoder->ResizeShader);
			ID3D10Blob_Release(ShaderBlob);
		}

		D3D11_SHADER_RESOURCE_VIEW_DESC ConvertInputView =
		{
			.Format = Encoder->LookupView ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D.MipLevels = -1,
		};
		ID3D11Device_CreateShaderResourceView(Device, (ID3D11Resource*)ResizedTexture, &ConvertInputView, &Encoder->ConvertInputView);
		ID3D11Texture2D_Release(ResizedTexture);

		// YUV converted texture
		{
			UINT32 Size;
			HR(MFCalculateImageSize(MediaFormatYUV, OutputWidth, OutputHeight, &Size));

			D3D11_TEXTURE2D_DESC TextureDesc =
			{
				.Width = OutputWidth,
				.Height = OutputHeight,
				.MipLevels = 1,
				.ArraySize = 1,
				.Format = FormatYUV,
				.SampleDesc = { 1, 0 },
				.Usage = D3D11_USAGE_DEFAULT,
				.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			};

			D3D11_UNORDERED_ACCESS_VIEW_DESC OutputViewY =
			{
				.Format = FormatY,
				.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			};

			D3D11_UNORDERED_ACCESS_VIEW_DESC OutputViewUV =
			{
				.Format = FormatUV,
				.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			};

			D3D11_SHADER_RESOURCE_VIEW_DESC InputViewUV =
			{
				.Format = FormatUV,
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
				.Texture2D.MipLevels = -1,
			};

			// create media foundation samples each referencing its own texture
			for (int i = 0; i < ENCODER_VIDEO_BUFFER_COUNT; i++)
			{
				ID3D11Texture2D* Texture;
				ID3D11Device_CreateTexture2D(Device, &TextureDesc, NULL, &Texture);
				ID3D11Device_CreateUnorderedAccessView(Device, (ID3D11Resource*)Texture, &OutputViewY,  &Encoder->ConvertOutputY[i]);
				ID3D11Device_CreateUnorderedAccessView(Device, (ID3D11Resource*)Texture, &OutputViewUV, &Encoder->ConvertOutputUV[i]);
				ID3D11Device_CreateShaderResourceView(Device, (ID3D11Resource*)Texture, &InputViewUV, &Encoder->ConvertInputUV[i]);

				IMFMediaBuffer* Buffer;
				HR(MFCreateDXGISurfaceBuffer(&IID_ID3D11Texture2D, (IUnknown*)Texture, 0, FALSE, &Buffer));
				HR(IMFMediaBuffer_SetCurrentLength(Buffer, Size));

				IMFSample* VideoSample;
				HR(MFCreateVideoSampleFromSurface(NULL, &VideoSample));
				HR(IMFSample_AddBuffer(VideoSample, Buffer));

				IMFMediaBuffer_Release(Buffer);
				ID3D11Texture2D_Release(Texture);

				Encoder->VideoSample[i] = VideoSample;
			}
		}

		Encoder->InputWidth = Config->Width;
		Encoder->InputHeight = Config->Height;
		Encoder->OutputWidth = OutputWidth;
		Encoder->OutputHeight = OutputHeight;
		Encoder->FramerateNum = Config->FramerateNum;
		Encoder->FramerateDen = Config->FramerateDen;
		Encoder->VideoDiscontinuity = FALSE;
		Encoder->VideoLastTime = 1ULL << 63; // some large time in future
		Encoder->VideoIndex = 0;
		Encoder->VideoCount = ENCODER_VIDEO_BUFFER_COUNT;
	}

	if (Encoder->AudioStreamIndex >= 0)
	{
		// resampler input buffer/sample
		{
			IMFSample* Sample;
			IMFMediaBuffer* Buffer;

			HR(MFCreateSample(&Sample));
			HR(MFCreateMemoryBuffer(Config->AudioFormat->nAvgBytesPerSec, &Buffer));
			HR(IMFSample_AddBuffer(Sample, Buffer));

			Encoder->AudioInputSample = Sample;
			Encoder->AudioInputBuffer = Buffer;
		}

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
		Encoder->AudioIndex = 0;
		Encoder->AudioCount = ENCODER_AUDIO_BUFFER_COUNT;
	}

	ID3D11Device_AddRef(Device);
	ID3D11DeviceContext_AddRef(Context);

	Encoder->Context = Context;
	Encoder->Device = Device;

	Encoder->StartTime = 0;
	Encoder->Writer = Writer;
	Writer = NULL;
	Resampler = NULL;
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
	ID3D11DeviceContext_Release(Context);
	IMFDXGIDeviceManager_Release(Manager);

	return Result;
}

void Encoder_Stop(Encoder* Encoder)
{
	if (Encoder->AudioStreamIndex >= 0)
	{
		HR(IMFTransform_ProcessMessage(Encoder->Resampler, MFT_MESSAGE_COMMAND_DRAIN, 0));
		Encoder__OutputAudioSamples(Encoder);
		IMFTransform_Release(Encoder->Resampler);
	}

	IMFSinkWriter_Finalize(Encoder->Writer);
	IMFSinkWriter_Release(Encoder->Writer);

	if (Encoder->AudioStreamIndex >= 0)
	{
		for (int i = 0; i < ENCODER_AUDIO_BUFFER_COUNT; i++)
		{
			IMFSample_Release(Encoder->AudioSample[i]);
		}
		IMFSample_Release(Encoder->AudioInputSample);
		IMFMediaBuffer_Release(Encoder->AudioInputBuffer);
	}

	for (int i = 0; i < ENCODER_VIDEO_BUFFER_COUNT; i++)
	{
		ID3D11ShaderResourceView_Release(Encoder->ConvertInputUV[i]);
		ID3D11UnorderedAccessView_Release(Encoder->ConvertOutputUV[i]);
		ID3D11UnorderedAccessView_Release(Encoder->ConvertOutputY[i]);
		IMFSample_Release(Encoder->VideoSample[i]);
	}
	ID3D11ShaderResourceView_Release(Encoder->ConvertInputView);

	ID3D11RenderTargetView_Release(Encoder->InputRenderTarget);
	ID3D11Texture2D_Release(Encoder->InputTexture);

	if (Encoder->ResizeShader)
	{
		ID3D11ShaderResourceView_Release(Encoder->ResizeInputView);
		ID3D11UnorderedAccessView_Release(Encoder->ResizeOutputView);
		ID3D11ComputeShader_Release(Encoder->ResizeShader);
	}

	if (Encoder->ConvertSimpleShader)
	{
		ID3D11ComputeShader_Release(Encoder->ConvertSimpleShader);
	}
	else
	{
		ID3D11ComputeShader_Release(Encoder->ConvertPass1Shader);
		ID3D11ComputeShader_Release(Encoder->ConvertPass2Shader);
		ID3D11ShaderResourceView_Release(Encoder->LookupView);
	}

	ID3D11SamplerState_Release(Encoder->LinearSampler);
	ID3D11Buffer_Release(Encoder->ConvertBuffer);

#if ENABLE_TIMING_QUERIES
	for (int i = 0; i < 2; i++)
	{
		ID3D11Query_Release(Encoder->QueryDisjoint[i]);
		for (int k = 0; k < _countof(Encoder->QueryTimestamp[i]); k++)
		{
			ID3D11Query_Release(Encoder->QueryTimestamp[i][k]);
		}
	}
#endif

	ID3D11DeviceContext_Release(Encoder->Context);
	ID3D11Device_Release(Encoder->Device);
}

BOOL Encoder_NewFrame(Encoder* Encoder, ID3D11Texture2D* Texture, RECT Rect, UINT64 Time, UINT64 TimePeriod)
{
	Encoder->VideoLastTime = Time;

	if (Encoder->VideoCount == 0)
	{
		// dropped frame
		LONGLONG Timestamp = MFllMulDiv(Time - Encoder->StartTime, MF_UNITS_PER_SECOND, TimePeriod, 0);
		HR(IMFSinkWriter_SendStreamTick(Encoder->Writer, Encoder->VideoStreamIndex, Timestamp));
		Encoder->VideoDiscontinuity = TRUE;
		return FALSE;
	}
	DWORD Index = Encoder->VideoIndex;
	Encoder->VideoIndex = (Index + 1) % ENCODER_VIDEO_BUFFER_COUNT;
	InterlockedDecrement(&Encoder->VideoCount);

	IMFSample* Sample = Encoder->VideoSample[Index];

	ID3D11DeviceContext* Context = Encoder->Context;

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
			ID3D11DeviceContext_ClearRenderTargetView(Context, Encoder->InputRenderTarget, Black);

			Box.right = Box.left + min(Encoder->InputWidth, Box.right);
			Box.bottom = Box.top + min(Encoder->InputHeight, Box.bottom);
		}
		ID3D11DeviceContext_CopySubresourceRegion(Context, (ID3D11Resource*)Encoder->InputTexture, 0, 0, 0, 0, (ID3D11Resource*)Texture, 0, &Box);
	}

	UINT GroupWidth = DIV_ROUND_UP(Encoder->OutputWidth, 16);
	UINT GroupHeight = DIV_ROUND_UP(Encoder->OutputHeight, 8);

	UINT GroupWidth2 = DIV_ROUND_UP(Encoder->OutputWidth / 2, 16);
	UINT GroupHeight2 = DIV_ROUND_UP(Encoder->OutputHeight / 2, 8);

#if ENABLE_TIMING_QUERIES
	BOOL UseQueryResults = TRUE;
	if (Encoder->QueryIndex < 0)
	{
		Encoder->QueryIndex = 0;
		UseQueryResults = FALSE;
	}
	int QueryIndex = Encoder->QueryIndex;

	ID3D11DeviceContext_Begin(Context, (ID3D11Asynchronous*)Encoder->QueryDisjoint[QueryIndex]);
	ID3D11DeviceContext_End(Context, (ID3D11Asynchronous*)Encoder->QueryTimestamp[QueryIndex][0]);
#endif

	// resize if needed
	if (Encoder->ResizeShader != NULL)
	{
		ID3D11DeviceContext_ClearState(Context);
		// input
		ID3D11DeviceContext_CSSetShaderResources(Context, 0, 1, &Encoder->ResizeInputView);
		// output
		ID3D11DeviceContext_CSSetUnorderedAccessViews(Context, 0, 1, &Encoder->ResizeOutputView, NULL);
		// shader
		ID3D11DeviceContext_CSSetShader(Context, Encoder->ResizeShader, NULL, 0);
		ID3D11DeviceContext_Dispatch(Context, GroupWidth, GroupHeight, 1);
	}

#if ENABLE_TIMING_QUERIES
	ID3D11DeviceContext_End(Context, (ID3D11Asynchronous*)Encoder->QueryTimestamp[QueryIndex][1]);
#endif

	// convert to YUV
	if (Encoder->ConvertSimpleShader)
	{
		ID3D11DeviceContext_ClearState(Context);
		// input
		ID3D11DeviceContext_CSSetShaderResources(Context, 0, 1, &Encoder->ConvertInputView);
		ID3D11DeviceContext_CSSetConstantBuffers(Context, 0, 1, &Encoder->ConvertBuffer);
		ID3D11DeviceContext_CSSetSamplers(Context, 0, 1, &Encoder->LinearSampler);
		// output
		ID3D11UnorderedAccessView* OutputViews[] = { Encoder->ConvertOutputY[Index], Encoder->ConvertOutputUV[Index] };
		ID3D11DeviceContext_CSSetUnorderedAccessViews(Context, 0, _countof(OutputViews), OutputViews, NULL);
		// shader
		ID3D11DeviceContext_CSSetShader(Context, Encoder->ConvertSimpleShader, NULL, 0);
		ID3D11DeviceContext_Dispatch(Context, GroupWidth2, GroupHeight2, 1);
	}
	else
	{
		ID3D11DeviceContext_ClearState(Context);
		// input
		ID3D11DeviceContext_CSSetShaderResources(Context, 0, 1, &Encoder->ConvertInputView);
		ID3D11DeviceContext_CSSetConstantBuffers(Context, 0, 1, &Encoder->ConvertBuffer);
		ID3D11DeviceContext_CSSetSamplers(Context, 0, 1, &Encoder->LinearSampler);
		// output
		ID3D11DeviceContext_CSSetUnorderedAccessViews(Context, 1, 1, &Encoder->ConvertOutputUV[Index], NULL);
		// shader
		ID3D11DeviceContext_CSSetShader(Context, Encoder->ConvertPass1Shader, NULL, 0);
		ID3D11DeviceContext_Dispatch(Context, GroupWidth2, GroupHeight2, 1);

		ID3D11DeviceContext_ClearState(Context);
		// input
		ID3D11ShaderResourceView* InputViews[] = { Encoder->ConvertInputView, Encoder->ConvertInputUV[Index], Encoder->LookupView };
		ID3D11DeviceContext_CSSetShaderResources(Context, 0, _countof(InputViews), InputViews);
		ID3D11DeviceContext_CSSetConstantBuffers(Context, 0, 1, &Encoder->ConvertBuffer);
		ID3D11DeviceContext_CSSetSamplers(Context, 0, 1, &Encoder->LinearSampler);
		// output
		ID3D11DeviceContext_CSSetUnorderedAccessViews(Context, 0, 1, &Encoder->ConvertOutputY[Index], NULL);
		// shader
		ID3D11DeviceContext_CSSetShader(Context, Encoder->ConvertPass2Shader, NULL, 0);
		ID3D11DeviceContext_Dispatch(Context, GroupWidth, GroupHeight, 1);
	}

#if ENABLE_TIMING_QUERIES
	ID3D11DeviceContext_End(Context, (ID3D11Asynchronous*)Encoder->QueryTimestamp[QueryIndex][2]);
	ID3D11DeviceContext_End(Context, (ID3D11Asynchronous*)Encoder->QueryDisjoint[QueryIndex]);

	QueryIndex = 1 - QueryIndex;
	Encoder->QueryIndex = QueryIndex;

	if (UseQueryResults)
	{
		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT Disjoint;
		while (ID3D11DeviceContext_GetData(Context, (ID3D11Asynchronous*)Encoder->QueryDisjoint[QueryIndex], &Disjoint, sizeof(Disjoint), 0) != S_OK) SwitchToThread();

		if (Disjoint.Disjoint == FALSE)
		{
			UINT64 Timestamp[_countof(Encoder->QueryTimestamp[QueryIndex])];

			for (int i = 0; i < _countof(Timestamp); i++)
			{
				while (ID3D11DeviceContext_GetData(Context, (ID3D11Asynchronous*)Encoder->QueryTimestamp[QueryIndex][i], &Timestamp[i], sizeof(Timestamp[i]), 0) != S_OK) SwitchToThread();
			}

			wchar_t Str[1024];
			StrFormat(Str, L"FrameTime = %4llu usec, Resize = %4llu usec, Convert = %4llu usec\n",
				1000000ULL * (Timestamp[2] - Timestamp[0]) / Disjoint.Frequency,
				1000000ULL * (Timestamp[1] - Timestamp[0]) / Disjoint.Frequency,
				1000000ULL * (Timestamp[2] - Timestamp[1]) / Disjoint.Frequency
				);
			OutputDebugStringW(Str);
		}
	}
#endif

	// setup input time & duration
	if (Encoder->StartTime == 0)
	{
		Encoder->StartTime = Time;
	}
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

	IMFTrackedSample* Tracked;
	HR(IMFSample_QueryInterface(Sample, &IID_IMFTrackedSample, (LPVOID*)&Tracked));
	IMFTrackedSample_SetAllocator(Tracked, &Encoder->VideoSampleCallback, NULL);
	IMFTrackedSample_Release(Tracked);

	// submit to encoder which will happen in background
	HR(IMFSinkWriter_WriteSample(Encoder->Writer, Encoder->VideoStreamIndex, Sample));

	IMFSample_Release(Sample);

	return TRUE;
}

void Encoder_NewSamples(Encoder* Encoder, LPCVOID Samples, DWORD VideoCount, UINT64 Time, UINT64 TimePeriod)
{
	IMFSample* AudioSample = Encoder->AudioInputSample;
	IMFMediaBuffer* Buffer = Encoder->AudioInputBuffer;

	BYTE* BufferData;
	DWORD MaxLength;
	HR(IMFMediaBuffer_Lock(Buffer, &BufferData, &MaxLength, NULL));

	DWORD BufferSize = VideoCount * Encoder->AudioFrameSize;
	Assert(BufferSize <= MaxLength);

	if (Samples)
	{
		CopyMemory(BufferData, Samples, BufferSize);
	}
	else
	{
		ZeroMemory(BufferData, BufferSize);
	}

	HR(IMFMediaBuffer_Unlock(Buffer));
	HR(IMFMediaBuffer_SetCurrentLength(Buffer, BufferSize));

	// setup input time & duration
	Assert(Encoder->StartTime != 0);
	HR(IMFSample_SetSampleDuration(AudioSample, MFllMulDiv(VideoCount, MF_UNITS_PER_SECOND, Encoder->AudioSampleRate, 0)));
	HR(IMFSample_SetSampleTime(AudioSample, MFllMulDiv(Time - Encoder->StartTime, MF_UNITS_PER_SECOND, TimePeriod, 0)));

	HR(IMFTransform_ProcessInput(Encoder->Resampler, 0, AudioSample, 0));
	Encoder__OutputAudioSamples(Encoder);
}

void Encoder_Update(Encoder* Encoder, UINT64 Time, UINT64 TimePeriod)
{
	// if there was no frame during last second, add discontinuity
	if (Time - Encoder->VideoLastTime >= TimePeriod)
	{
		Encoder->VideoLastTime = Time;
		LONGLONG Timestamp = MFllMulDiv(Time - Encoder->StartTime, MF_UNITS_PER_SECOND, TimePeriod, 0);
		HR(IMFSinkWriter_SendStreamTick(Encoder->Writer, Encoder->VideoStreamIndex, Timestamp));
		Encoder->VideoDiscontinuity = TRUE;
	}
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