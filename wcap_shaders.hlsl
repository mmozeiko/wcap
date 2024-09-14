//

#pragma warning (disable : 3571) // pow(f, e) will not work for negative f, use abs(f) or conditionally handle negative values if you expect them

//
// default sampler = linear, clamp
//

SamplerState LinearSampler : register(s0);

//
// helper functions
//

static float3 LinearToGamma(float3 Color)
{
	return Color < 0.0031308 ? Color * 12.92 : 1.055 * pow(Color, 1.0 / 2.4) - 0.055;
}

static float3 GammaToLinear(float3 Color)
{
	return Color < 0.04045 ? Color / 12.92 : pow((Color + 0.055) / 1.055, 2.4);
}

static uint PackToBGR(float3 Color)
{
	const uint3 Pack = { 1, 1 << 8, 1 << 16 };
	return dot(uint3(saturate(Color.bgr) * 255 + 0.5), Pack);
}

static uint LinearPackToBGR(float3 Color)
{
	return PackToBGR(LinearToGamma(Color));
}

//
// resizer
//

Texture2D<float3> ResizeIn  : register(t0);
RWTexture2D<uint> ResizeOut : register(u0);

static float ResizeFilter(float x)
{
	// https://en.wikipedia.org/wiki/Mitchell%E2%80%93Netravali_filters
	// with B=C=1/3

	x = abs(x);

	if (x < 1.0)
	{
		float x2 = x * x;
		float x3 = x * x2;
		x = (21 * x3 - 36 * x2 + 16) / 18;
	}
	else if (x < 2.0)
	{
		float x2 = x * x;
		float x3 = x * x2;
		x = (-7 * x3 + 36 * x2 - 60 * x + 32) / 18;
	}
	else
	{
		x = 0.0;
	}

	return x;
}

static float3 ResizePass(uint2 OutputPos, int2 Direction)
{
	int2 InSize, OutSize;
	ResizeIn.GetDimensions(InSize.x, InSize.y);
	ResizeOut.GetDimensions(OutSize.x, OutSize.y);

	float2 Scale = float2(OutSize) / InSize;
	float2 Size = 2.0 / Scale;

	float2 Center = float2(OutputPos) + 0.5;

	float2 InputPos = Center / Scale;
	int2 StartPos = clamp(int2(InputPos - Size + 0.5), 0, InSize - 1);
	int2 EndPos = clamp(int2(InputPos + Size + 0.5), 0, InSize - 1);

	// Start & End values are calculated so ResizeFilter(Offset) in loop below for Start-1 and End+1 values is 0
	float Start = dot(Direction, StartPos) + 0.5;
	float End = dot(Direction, EndPos) + 0.5;
	int2 Pos = StartPos * Direction.xy + int2(InputPos - 0.5) * Direction.yx;

	float WeightSum = 0;
	float3 ColorSum = 0;

	for (float Index = Start; Index < End; Index += 1.0)
	{
		float Offset = dot(Direction, Center - Index * Direction * Scale);
		float Weight = ResizeFilter(Offset);

		ColorSum += Weight * ResizeIn[Pos];
		WeightSum += Weight;
		Pos += Direction;
	}

	return ColorSum / WeightSum;
}

static float3 ResizeSingle(uint2 OutputPos)
{
	int2 OutSize;
	ResizeOut.GetDimensions(OutSize.x, OutSize.y);

	float2 InputPos = (float2(OutputPos) + 0.5) / OutSize;
	return ResizeIn.SampleLevel(LinearSampler, InputPos, 0);
}

// fancy resize horizontal & vertical passes

[numthreads(16, 16, 1)]
void ResizePassH(uint3 OutputPos: SV_DispatchThreadID)
{
	ResizeOut[OutputPos.xy] = PackToBGR(ResizePass(OutputPos.xy, int2(1, 0)));
}

[numthreads(16, 16, 1)]
void ResizePassV(uint3 OutputPos: SV_DispatchThreadID)
{
	ResizeOut[OutputPos.xy] = PackToBGR(ResizePass(OutputPos.xy, int2(0, 1)));
}

// fancy resize horizontal & vertical passes for linear color

[numthreads(16, 16, 1)]
void ResizeLinearPassH(uint3 OutputPos: SV_DispatchThreadID)
{
	ResizeOut[OutputPos.xy] = LinearPackToBGR(ResizePass(OutputPos.xy, int2(1, 0)));
}

[numthreads(16, 16, 1)]
void ResizeLinearPassV(uint3 OutputPos: SV_DispatchThreadID)
{
	ResizeOut[OutputPos.xy] = LinearPackToBGR(ResizePass(OutputPos.xy, int2(0, 1)));
}

// simple resize pass

[numthreads(16, 16, 1)]
void ResizeSinglePass(uint3 OutputPos: SV_DispatchThreadID)
{
	ResizeOut[OutputPos.xy] = PackToBGR(ResizeSingle(OutputPos.xy));
}

// simple resize for linear color

[numthreads(16, 16, 1)]
void ResizeSingleLinearPass(uint3 OutputPos: SV_DispatchThreadID)
{
	ResizeOut[OutputPos.xy] = LinearPackToBGR(ResizeSingle(OutputPos.xy));
}

//
// RGB -> YUV converter
//

Texture2D<float3>  ConvertInput    : register(t0);
RWTexture2D<uint>  ConvertOutputY  : register(u0);
RWTexture2D<uint2> ConvertOutputUV : register(u1);

// YUV conversion matrix
cbuffer ConvertBuffer : register(b0)
{
	row_major float3x4 ConvertMtx;
};

float3 RgbToYuv(float3 Rgb)
{
	return mul(ConvertMtx, float4(Rgb, 1.0));
}

[numthreads(16, 8, 1)]
void Convert(uint3 Id: SV_DispatchThreadID)
{
	int2 InSize;
	ConvertInput.GetDimensions(InSize.x, InSize.y);

	int2 Pos = int2(Id.xy);
	int2 Pos2 = Pos * 2;

	int4 Pos4 = int4(Pos2, Pos2 + int2(1, 1));

	int4 Src = int4(Pos2, min(Pos4.zw, InSize - int2(1, 1)));

	// load RGB colors in 2x2 area
	float3 Rgb0 = ConvertInput[Src.xy];
	float3 Rgb1 = ConvertInput[Src.zy];
	float3 Rgb2 = ConvertInput[Src.xw];
	float3 Rgb3 = ConvertInput[Src.zw];

	// convert RGB to YUV
	float3 Yuv0 = RgbToYuv(Rgb0);
	float3 Yuv1 = RgbToYuv(Rgb1);
	float3 Yuv2 = RgbToYuv(Rgb2);
	float3 Yuv3 = RgbToYuv(Rgb3);

	// average UV
	float2 UV = (Yuv0.yz + Yuv1.yz + Yuv2.yz + Yuv3.yz) / 4.0;

	// store Y
	ConvertOutputY[Pos4.xy] = uint(Yuv0.x);
	ConvertOutputY[Pos4.zy] = uint(Yuv1.x);
	ConvertOutputY[Pos4.xw] = uint(Yuv2.x);
	ConvertOutputY[Pos4.zw] = uint(Yuv3.x);

	// store UV
	ConvertOutputUV[Pos] = uint2(UV);
}
