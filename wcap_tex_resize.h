#pragma once

#include "wcap.h"
#include <d3d11.h>

//
// interface
//

typedef struct
{
	ID3D11Texture2D* InputTexture;
	ID3D11Texture2D* OutputTexture;

	ID3D11ShaderResourceView* InputViewIn;
	ID3D11UnorderedAccessView* OutputViewOut;
	ID3D11ShaderResourceView* FancyViewIn;
	ID3D11UnorderedAccessView* FancyViewOut;
	ID3D11ComputeShader* SinglePass;
	ID3D11ComputeShader* PassH;
	ID3D11ComputeShader* PassV;
	uint32_t InputWidth;
	uint32_t InputHeight;
	uint32_t OutputWidth;
	uint32_t OutputHeight;
}
TexResize;

static void TexResize_Create(TexResize* Resize, ID3D11Device* Device, uint32_t InputWidth, uint32_t InputHeight, uint32_t OutputWidth, uint32_t OutputHeight, bool LinearSpace, D3D11_BIND_FLAG InputUsage);
static void TexResize_Release(TexResize* Resize);

static void TexResize_Dispatch(TexResize* Resize, ID3D11DeviceContext* Context);

//
// implementation
//

#include <d3dcompiler.h>

#include "shaders/ResizePassH.h"
#include "shaders/ResizePassV.h"

#include "shaders/ResizeLinearPassH.h"
#include "shaders/ResizeLinearPassV.h"

#include "shaders/ResizeSinglePass.h"
#include "shaders/ResizeSingleLinearPass.h"

void TexResize_Create(TexResize* Resize, ID3D11Device* Device, uint32_t InputWidth, uint32_t InputHeight, uint32_t OutputWidth, uint32_t OutputHeight, bool LinearSpace, D3D11_BIND_FLAG InputUsage)
{
	D3D11_TEXTURE2D_DESC InputTextureDesc =
	{
		.Width = InputWidth,
		.Height = InputHeight,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS,
		.SampleDesc = { 1, 0 },
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE | InputUsage,
	};
	ID3D11Device_CreateTexture2D(Device, &InputTextureDesc, NULL, &Resize->InputTexture);

	if (InputWidth == OutputWidth && InputHeight == OutputHeight)
	{
		Resize->InputViewIn = NULL;
		Resize->OutputTexture = Resize->InputTexture;
	}
	else
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC ViewInDesc =
		{
			.Format = LinearSpace ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D.MipLevels = -1,
		};
		ID3D11Device_CreateShaderResourceView(Device, (ID3D11Resource*)Resize->InputTexture, &ViewInDesc, &Resize->InputViewIn);

		D3D11_TEXTURE2D_DESC OutputTextureDesc =
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
		ID3D11Device_CreateTexture2D(Device, &OutputTextureDesc, NULL, &Resize->OutputTexture);

		// because D3D 11.0 does not support B8G8R8A8 for UAV stores, create R32_UINT for packing BGRA bytes manually in shader
		D3D11_UNORDERED_ACCESS_VIEW_DESC ViewOutDesc =
		{
			.Format = DXGI_FORMAT_R32_UINT,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D.MipSlice = 0,
		};
		ID3D11Device_CreateUnorderedAccessView(Device, (ID3D11Resource*)Resize->OutputTexture, &ViewOutDesc, &Resize->OutputViewOut);

		ID3DBlob* Shader;

		if (OutputWidth >= InputWidth/2 && OutputHeight >= InputHeight/2)
		{
			HR(D3DDecompressShaders(
				LinearSpace ? ResizeSingleLinearPassShaderBytes : ResizeSinglePassShaderBytes,
				LinearSpace ? sizeof(ResizeSingleLinearPassShaderBytes) : sizeof(ResizeSinglePassShaderBytes),
				1, 0, NULL, 0, &Shader, NULL));
			ID3D11Device_CreateComputeShader(Device, ID3D10Blob_GetBufferPointer(Shader), ID3D10Blob_GetBufferSize(Shader), NULL, &Resize->SinglePass);
			ID3D10Blob_Release(Shader);
		}
		else
		{
			Resize->SinglePass = NULL;

			HR(D3DDecompressShaders(
				LinearSpace ? ResizeLinearPassHShaderBytes : ResizePassHShaderBytes,
				LinearSpace ? sizeof(ResizeLinearPassHShaderBytes) : sizeof(ResizePassHShaderBytes),
				1, 0, NULL, 0, &Shader, NULL));
			ID3D11Device_CreateComputeShader(Device, ID3D10Blob_GetBufferPointer(Shader), ID3D10Blob_GetBufferSize(Shader), NULL, &Resize->PassH);
			ID3D10Blob_Release(Shader);

			HR(D3DDecompressShaders(
				LinearSpace ? ResizeLinearPassHShaderBytes : ResizePassVShaderBytes,
				LinearSpace ? sizeof(ResizeLinearPassHShaderBytes) : sizeof(ResizePassVShaderBytes),
				1, 0, NULL, 0, &Shader, NULL));
			ID3D11Device_CreateComputeShader(Device, ID3D10Blob_GetBufferPointer(Shader), ID3D10Blob_GetBufferSize(Shader), NULL, &Resize->PassV);
			ID3D10Blob_Release(Shader);

			D3D11_TEXTURE2D_DESC FancyTextureDesc =
			{
				.Width = OutputWidth,
				.Height = InputHeight,
				.MipLevels = 1,
				.ArraySize = 1,
				.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS,
				.SampleDesc = { 1, 0 },
				.Usage = D3D11_USAGE_DEFAULT,
				.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			};

			ID3D11Texture2D* FancyTexture;
			ID3D11Device_CreateTexture2D(Device, &FancyTextureDesc, NULL, &FancyTexture);
			ID3D11Device_CreateUnorderedAccessView(Device, (ID3D11Resource*)FancyTexture, &ViewOutDesc, &Resize->FancyViewOut);
			ID3D11Device_CreateShaderResourceView(Device, (ID3D11Resource*)FancyTexture, &ViewInDesc, &Resize->FancyViewIn);
			ID3D11Texture2D_Release(FancyTexture);
		}
	}

	Resize->InputWidth = InputWidth;
	Resize->InputHeight = InputHeight;
	Resize->OutputWidth = OutputWidth;
	Resize->OutputHeight = OutputHeight;
}

void TexResize_Release(TexResize* Resize)
{
	ID3D11Texture2D_Release(Resize->InputTexture);

	if (Resize->InputViewIn)
	{
		ID3D11ShaderResourceView_Release(Resize->InputViewIn);
		ID3D11UnorderedAccessView_Release(Resize->OutputViewOut);
		ID3D11Texture2D_Release(Resize->OutputTexture);

		if (Resize->SinglePass)
		{
			ID3D11ComputeShader_Release(Resize->SinglePass);
		}
		else
		{
			ID3D11ComputeShader_Release(Resize->PassH);
			ID3D11ComputeShader_Release(Resize->PassV);

			ID3D11UnorderedAccessView_Release(Resize->FancyViewOut);
			ID3D11ShaderResourceView_Release(Resize->FancyViewIn);
		}
	}
}

void TexResize_Dispatch(TexResize* Resize, ID3D11DeviceContext* Context)
{
	if (Resize->InputViewIn == NULL)
	{
		return;
	}

	if (Resize->SinglePass)
	{
		ID3D11DeviceContext_ClearState(Context);
		ID3D11DeviceContext_CSSetShader(Context, Resize->SinglePass, NULL, 0);
		ID3D11DeviceContext_CSSetShaderResources(Context, 0, 1, &Resize->InputViewIn);
		ID3D11DeviceContext_CSSetUnorderedAccessViews(Context, 0, 1, &Resize->OutputViewOut, NULL);
		ID3D11DeviceContext_Dispatch(Context, DIV_ROUND_UP(Resize->OutputWidth, 16), DIV_ROUND_UP(Resize->OutputHeight, 16), 1);
	}
	else
	{
		ID3D11DeviceContext_ClearState(Context);
		ID3D11DeviceContext_CSSetShader(Context, Resize->PassH, NULL, 0);
		ID3D11DeviceContext_CSSetShaderResources(Context, 0, 1, &Resize->InputViewIn);
		ID3D11DeviceContext_CSSetUnorderedAccessViews(Context, 0, 1, &Resize->FancyViewOut, NULL);
		ID3D11DeviceContext_Dispatch(Context, DIV_ROUND_UP(Resize->OutputWidth, 16), DIV_ROUND_UP(Resize->InputHeight, 16), 1);

		ID3D11DeviceContext_ClearState(Context);
		ID3D11DeviceContext_CSSetShader(Context, Resize->PassV, NULL, 0);
		ID3D11DeviceContext_CSSetShaderResources(Context, 0, 1, &Resize->FancyViewIn);
		ID3D11DeviceContext_CSSetUnorderedAccessViews(Context, 0, 1, &Resize->OutputViewOut, NULL);
		ID3D11DeviceContext_Dispatch(Context, DIV_ROUND_UP(Resize->OutputWidth, 16), DIV_ROUND_UP(Resize->OutputHeight, 16), 1);
	}
}
