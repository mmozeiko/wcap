#pragma once

#include "wcap.h"
#include <d3d11.h>

//
// interface
//

typedef struct
{
	ID3D11Texture2D* Texture;
	ID3D11ShaderResourceView* ViewInUV;
	ID3D11UnorderedAccessView* ViewOutUV;
	ID3D11UnorderedAccessView* ViewOutY;
}
YuvConvertOutput;

typedef struct
{
	ID3D11ShaderResourceView* InputView;
	ID3D11ComputeShader* SinglePass;
	ID3D11ComputeShader* Pass1;
	ID3D11ComputeShader* Pass2;
	ID3D11Buffer* ConstantBuffer;
	uint32_t Width;
	uint32_t Height;
}
YuvConvert;

typedef enum
{
	YuvColorSpace_BT601,
	YuvColorSpace_BT709,
	YuvColorSpace_BT2020,
}
YuvColorSpace;

static void YuvConvertOutput_Create(YuvConvertOutput* Output, ID3D11Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT Format);
static void YuvConvertOutput_Release(YuvConvertOutput* Output);

static void YuvConvert_Create(YuvConvert* Convert, ID3D11Device* Device, ID3D11Texture2D* InputTexture, uint32_t Width, uint32_t Height, YuvColorSpace ColorSpace, bool ImprovedConversion);
static void YuvConvert_Release(YuvConvert* Convert);

static void YuvConvert_Dispatch(YuvConvert* Convert, ID3D11DeviceContext* Context, YuvConvertOutput* Output);

//
// implementation
//

#include <d3dcompiler.h>

#include "shaders/ConvertSinglePass.h"
#include "shaders/ConvertPass1.h"
#include "shaders/ConvertPass2.h"

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
		.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
	};

	D3D11_UNORDERED_ACCESS_VIEW_DESC ViewOutDescY =
	{
		.Format = (Format == DXGI_FORMAT_NV12) ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R16_UNORM,
		.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
	};

	D3D11_UNORDERED_ACCESS_VIEW_DESC ViewOutDescUV =
	{
		.Format = (Format == DXGI_FORMAT_NV12) ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R16G16_UNORM,
		.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
	};

	D3D11_SHADER_RESOURCE_VIEW_DESC ViewInDescUV =
	{
		.Format = (Format == DXGI_FORMAT_NV12) ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R16G16_UNORM,
		.ViewDimension = D3D_SRV_DIMENSION_TEXTURE2D,
		.Texture2D.MipLevels = -1,
	};

	ID3D11Device_CreateTexture2D(Device, &TextureDesc, NULL, &Output->Texture);
	ID3D11Device_CreateUnorderedAccessView(Device, (ID3D11Resource*)Output->Texture, &ViewOutDescY, &Output->ViewOutY);
	ID3D11Device_CreateUnorderedAccessView(Device, (ID3D11Resource*)Output->Texture, &ViewOutDescUV, &Output->ViewOutUV);
	ID3D11Device_CreateShaderResourceView(Device, (ID3D11Resource*)Output->Texture, &ViewInDescUV, &Output->ViewInUV);
}

void YuvConvertOutput_Release(YuvConvertOutput* Output)
{
	ID3D11Texture2D_Release(Output->Texture);
	ID3D11UnorderedAccessView_Release(Output->ViewOutUV);
	ID3D11UnorderedAccessView_Release(Output->ViewOutY);
	ID3D11ShaderResourceView_Release(Output->ViewInUV);
}

void YuvConvert_Create(YuvConvert* Convert, ID3D11Device* Device, ID3D11Texture2D* InputTexture, uint32_t Width, uint32_t Height, YuvColorSpace ColorSpace, bool ImprovedConversion)
{
	Assert(Width % 2 == 0 && Height % 2 == 0);

	D3D11_SHADER_RESOURCE_VIEW_DESC InputViewDesc =
	{
		.Format = DXGI_FORMAT_B8G8R8A8_UNORM,
		.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
		.Texture2D.MipLevels = -1,
	};
	ID3D11Device_CreateShaderResourceView(Device, (ID3D11Resource*)InputTexture, &InputViewDesc, &Convert->InputView);

	// https://en.wikipedia.org/wiki/YCbCr#ITU-R_BT.2020_conversion
	static const float BT2020[6][4] =
	{
		// RGB to YUV
		{ +0.26270f, +0.678000f, +0.0593000f },
		{ -0.13963f, -0.360370f, +0.5000000f },
		{ +0.50000f, -0.459786f, -0.0402143f },
		// YUV to RGB
		{ 1.f, +0.000000f, +1.474600f },
		{ 1.f, -0.164553f, -0.571353f },
		{ 1.f, +1.881400f, +0.000000f },
	};

	// https://en.wikipedia.org/wiki/YCbCr#ITU-R_BT.709_conversion
	static const float BT709[6][4] =
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
	static const float BT601[6][4] =
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

	const float (*ConvertMatrix)[4];
	switch (ColorSpace)
	{
	case YuvColorSpace_BT601:  ConvertMatrix = BT601;  break;
	case YuvColorSpace_BT709:  ConvertMatrix = BT709;  break;
	case YuvColorSpace_BT2020: ConvertMatrix = BT2020; break;
	default: Assert(false);
	}

	D3D11_BUFFER_DESC ConstantBufferDesc =
	{
		.ByteWidth = 6 * 4 * sizeof(float),
		.Usage = D3D11_USAGE_IMMUTABLE,
		.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
	};

	D3D11_SUBRESOURCE_DATA ConstantBufferData =
	{
		.pSysMem = ConvertMatrix,
	};

	ID3D11Device_CreateBuffer(Device, &ConstantBufferDesc, &ConstantBufferData, &Convert->ConstantBuffer);

	if (ImprovedConversion)
	{
		ID3DBlob* Shader;
		HR(D3DDecompressShaders(ConvertPass1ShaderBytes, sizeof(ConvertPass1ShaderBytes), 1, 0, NULL, 0, &Shader, NULL));
		ID3D11Device_CreateComputeShader(Device, ID3D10Blob_GetBufferPointer(Shader), ID3D10Blob_GetBufferSize(Shader), NULL, &Convert->Pass1);
		ID3D10Blob_Release(Shader);

		HR(D3DDecompressShaders(ConvertPass2ShaderBytes, sizeof(ConvertPass2ShaderBytes), 1, 0, NULL, 0, &Shader, NULL));
		ID3D11Device_CreateComputeShader(Device, ID3D10Blob_GetBufferPointer(Shader), ID3D10Blob_GetBufferSize(Shader), NULL, &Convert->Pass2);
		ID3D10Blob_Release(Shader);
		
		Convert->SinglePass = NULL;
	}
	else
	{
		ID3DBlob* Shader;
		HR(D3DDecompressShaders(ConvertSinglePassShaderBytes, sizeof(ConvertSinglePassShaderBytes), 1, 0, NULL, 0, &Shader, NULL));
		ID3D11Device_CreateComputeShader(Device, ID3D10Blob_GetBufferPointer(Shader), ID3D10Blob_GetBufferSize(Shader), NULL, &Convert->SinglePass);
		ID3D10Blob_Release(Shader);
	}

	Convert->Width = Width;
	Convert->Height = Height;
}

void YuvConvert_Release(YuvConvert* Convert)
{
	ID3D11ShaderResourceView_Release(Convert->InputView);
	ID3D11Buffer_Release(Convert->ConstantBuffer);

	if (Convert->SinglePass)
	{
		ID3D11ComputeShader_Release(Convert->SinglePass);
	}
	else
	{
		ID3D11ComputeShader_Release(Convert->Pass1);
		ID3D11ComputeShader_Release(Convert->Pass2);
	}
}

static void YuvConvert_Dispatch(YuvConvert* Convert, ID3D11DeviceContext* Context, YuvConvertOutput* Output)
{
	if (Convert->SinglePass)
	{
		ID3D11UnorderedAccessView* OutputViews[] = { Output->ViewOutY, Output->ViewOutUV };

		ID3D11DeviceContext_ClearState(Context);
		ID3D11DeviceContext_CSSetShader(Context, Convert->SinglePass, NULL, 0);
		ID3D11DeviceContext_CSSetConstantBuffers(Context, 0, 1, &Convert->ConstantBuffer);
		ID3D11DeviceContext_CSSetShaderResources(Context, 0, 1, &Convert->InputView);
		ID3D11DeviceContext_CSSetUnorderedAccessViews(Context, 0, ARRAYSIZE(OutputViews), OutputViews, NULL);
		ID3D11DeviceContext_Dispatch(Context, DIV_ROUND_UP(Convert->Width / 2, 16), DIV_ROUND_UP(Convert->Height / 2, 16), 1);
	}
	else
	{
		ID3D11DeviceContext_ClearState(Context);
		ID3D11DeviceContext_CSSetShader(Context, Convert->Pass1, NULL, 0);
		ID3D11DeviceContext_CSSetConstantBuffers(Context, 0, 1, &Convert->ConstantBuffer);
		ID3D11DeviceContext_CSSetShaderResources(Context, 0, 1, &Convert->InputView);
		ID3D11DeviceContext_CSSetUnorderedAccessViews(Context, 1, 1, &Output->ViewOutUV, NULL);
		ID3D11DeviceContext_Dispatch(Context, DIV_ROUND_UP(Convert->Width / 2, 16), DIV_ROUND_UP(Convert->Height / 2, 16), 1);

		ID3D11ShaderResourceView* InputViews[] = { Convert->InputView, Output->ViewInUV };

		ID3D11DeviceContext_ClearState(Context);
		ID3D11DeviceContext_CSSetShader(Context, Convert->Pass2, NULL, 0);
		ID3D11DeviceContext_CSSetConstantBuffers(Context, 0, 1, &Convert->ConstantBuffer);
		ID3D11DeviceContext_CSSetShaderResources(Context, 0, ARRAYSIZE(InputViews), InputViews);
		ID3D11DeviceContext_CSSetUnorderedAccessViews(Context, 0, 1, &Output->ViewOutY, NULL);
		ID3D11DeviceContext_Dispatch(Context, DIV_ROUND_UP(Convert->Width, 16), DIV_ROUND_UP(Convert->Height, 16), 1);
	}
}
