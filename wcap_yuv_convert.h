#pragma once

#include "wcap.h"
#include <d3d11.h>

//
// interface
//

typedef struct
{
	ID3D11Texture2D* Texture;
	ID3D11UnorderedAccessView* ViewY;
	ID3D11UnorderedAccessView* ViewUV;
}
YuvConvertOutput;

typedef struct
{
	ID3D11ShaderResourceView* InputView;
	ID3D11ComputeShader* Shader;
	ID3D11Buffer* ConstantBuffer;
	uint32_t Width;
	uint32_t Height;
}
YuvConvert;

static void YuvConvertOutput_Create(YuvConvertOutput* Output, ID3D11Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT Format);
static void YuvConvertOutput_Release(YuvConvertOutput* Output);

static void YuvConvert_Create(YuvConvert* Convert, ID3D11Device* Device, ID3D11Texture2D* InputTexture, uint32_t Width, uint32_t Height, DXGI_FORMAT Format);
static void YuvConvert_Release(YuvConvert* Convert);

static void YuvConvert_Dispatch(YuvConvert* Convert, ID3D11DeviceContext* Context, YuvConvertOutput* Output);

//
// implementation
//

#include <d3dcompiler.h>

#include "shaders/Convert.h"

void YuvConvertOutput_Create(YuvConvertOutput* Output, ID3D11Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT Format)
{
	Assert(Width % 2 == 0 && Height % 2 == 0);
	Assert(Format == DXGI_FORMAT_NV12 || Format == DXGI_FORMAT_P010);

	D3D11_TEXTURE2D_DESC TextureDesc =
	{
		.Width = Width,
		.Height = Height,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = Format,
		.SampleDesc = { 1, 0 },
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_UNORDERED_ACCESS,
	};

	D3D11_UNORDERED_ACCESS_VIEW_DESC ViewDescY =
	{
		.Format = (Format == DXGI_FORMAT_NV12) ? DXGI_FORMAT_R8_UINT : DXGI_FORMAT_R16_UINT,
		.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
	};

	D3D11_UNORDERED_ACCESS_VIEW_DESC ViewDescUV =
	{
		.Format = (Format == DXGI_FORMAT_NV12) ? DXGI_FORMAT_R8G8_UINT : DXGI_FORMAT_R16G16_UINT,
		.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
	};

	ID3D11Device_CreateTexture2D(Device, &TextureDesc, NULL, &Output->Texture);
	ID3D11Device_CreateUnorderedAccessView(Device, (ID3D11Resource*)Output->Texture, &ViewDescY, &Output->ViewY);
	ID3D11Device_CreateUnorderedAccessView(Device, (ID3D11Resource*)Output->Texture, &ViewDescUV, &Output->ViewUV);
}

void YuvConvertOutput_Release(YuvConvertOutput* Output)
{
	ID3D11Texture2D_Release(Output->Texture);
	ID3D11UnorderedAccessView_Release(Output->ViewY);
	ID3D11UnorderedAccessView_Release(Output->ViewUV);
}

void YuvConvert_Create(YuvConvert* Convert, ID3D11Device* Device, ID3D11Texture2D* InputTexture, uint32_t Width, uint32_t Height, DXGI_FORMAT Format)
{
	Assert(Width % 2 == 0 && Height % 2 == 0);
	Assert(Format == DXGI_FORMAT_NV12 || Format == DXGI_FORMAT_P010);

	D3D11_SHADER_RESOURCE_VIEW_DESC InputViewDesc =
	{
		.Format = DXGI_FORMAT_B8G8R8A8_UNORM,
		.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
		.Texture2D.MipLevels = -1,
	};
	ID3D11Device_CreateShaderResourceView(Device, (ID3D11Resource*)InputTexture, &InputViewDesc, &Convert->InputView);

	ID3DBlob* Shader;
	HR(D3DDecompressShaders(ConvertShaderBytes, sizeof(ConvertShaderBytes), 1, 0, NULL, 0, &Shader, NULL));
	ID3D11Device_CreateComputeShader(Device, ID3D10Blob_GetBufferPointer(Shader), ID3D10Blob_GetBufferSize(Shader), NULL, &Convert->Shader);
	ID3D10Blob_Release(Shader);

	float RangeY, OffsetY;
	float RangeUV, OffsetUV;

	if (Format == DXGI_FORMAT_NV12)
	{
		// Y=[16..235], UV=[16..240]
		RangeY = 219.f;
		OffsetY = 16.5f;
		RangeUV = 224.f;
		OffsetUV = 128.5f;
	}
	else // Format == DXGI_FORMAT_P010
	{
		// Y=[64..940], UV=[64..960]
		// mutiplied by 64, because 10-bit values are positioned at top of 16-bit used for texture storage format
		RangeY = 876.f * 64.f;
		OffsetY = 64.5f * 64.f;
		RangeUV = 896.f * 64.f;
		OffsetUV = 512.5f * 64.f;
	}

	// BT.709 - https://en.wikipedia.org/wiki/YCbCr#ITU-R_BT.709_conversion
	float ConvertMtx[3][4] =
	{
		{  0.2126f * RangeY,   0.7152f * RangeY,   0.0722f * RangeY,  OffsetY  },
		{ -0.1146f * RangeUV, -0.3854f * RangeUV,  0.5f    * RangeUV, OffsetUV },
		{  0.5f    * RangeUV, -0.4542f * RangeUV, -0.0458f * RangeUV, OffsetUV },
	};

	D3D11_BUFFER_DESC ConstantBufferDesc =
	{
		.ByteWidth = sizeof(ConvertMtx),
		.Usage = D3D11_USAGE_IMMUTABLE,
		.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
	};

	D3D11_SUBRESOURCE_DATA ConstantBufferData =
	{
		.pSysMem = ConvertMtx,
	};

	ID3D11Device_CreateBuffer(Device, &ConstantBufferDesc, &ConstantBufferData, &Convert->ConstantBuffer);

	Convert->Width = Width;
	Convert->Height = Height;
}

void YuvConvert_Release(YuvConvert* Convert)
{
	ID3D11ShaderResourceView_Release(Convert->InputView);
	ID3D11ComputeShader_Release(Convert->Shader);
	ID3D11Buffer_Release(Convert->ConstantBuffer);
}

static void YuvConvert_Dispatch(YuvConvert* Convert, ID3D11DeviceContext* Context, YuvConvertOutput* Output)
{
	ID3D11UnorderedAccessView* OutputViews[] = { Output->ViewY, Output->ViewUV };

	ID3D11DeviceContext_ClearState(Context);
	ID3D11DeviceContext_CSSetShader(Context, Convert->Shader, NULL, 0);
	ID3D11DeviceContext_CSSetConstantBuffers(Context, 0, 1, &Convert->ConstantBuffer);
	ID3D11DeviceContext_CSSetShaderResources(Context, 0, 1, &Convert->InputView);
	ID3D11DeviceContext_CSSetUnorderedAccessViews(Context, 0, ARRAYSIZE(OutputViews), OutputViews, NULL);
	ID3D11DeviceContext_Dispatch(Context, DIV_ROUND_UP(Convert->Width, 16), DIV_ROUND_UP(Convert->Height, 16), 1);
}
