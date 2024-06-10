//

#pragma warning (disable : 3571) // pow(f, e) will not work for negative f, use abs(f) or conditionally handle negative values if you expect them

// when enabled shader uses square & square-root to approximate gamma/linear conversions
#define USE_APPROXIMATE_GAMMA 0

// resize input
Texture2D<float3> Input : register(t0);

// resize output, packed BGRA format
RWTexture2D<uint> Output : register(u0);

float Filter(float x)
{
	// https://en.wikipedia.org/wiki/Mitchell%E2%80%93Netravali_filters
	// with B=C=1/3

	x = abs(x);

	if (x < 1.0)
	{
		float x2 = x * x;
		float x3 = x * x2;
		return (21 * x3 - 36 * x2 + 16) / 18;
	}
	if (x < 2.0)
	{
		float x2 = x * x;
		float x3 = x * x2;
		return (-7 * x3 + 36 * x2 - 60 * x + 32) / 18;
	}
	return 0.0;
}

[numthreads(16, 8, 1)]
void Resize(uint3 Id: SV_DispatchThreadID)
{
	int2 InSize, OutSize;
	Input.GetDimensions(InSize.x, InSize.y);
	Output.GetDimensions(OutSize.x, OutSize.y);

	float2 Scale = float2(OutSize) / InSize;
	float2 Size = 2.0 / Scale;

	float2 Center = (float2(Id.xy) + 0.5) / Scale;
	int2 Start = clamp(int2(Center - Size), int2(0, 0), InSize - int2(1, 1));
	int2 End = clamp(int2(Center + Size), int2(0, 0), InSize - int2(1, 1));

	float Weight = 0;
	float3 Color = float3(0, 0, 0);
	for (int y = Start.y; y <= End.y; y++)
	{
		float dy = (Center.y - y - 0.5) * Scale.y;
		float fy = Filter(dy);

		for (int x = Start.x; x <= End.x; x++)
		{
			float dx = (Center.x - x - 0.5) * Scale.x;
			float fx = Filter(dx);

			float w = fx * fy;
			Color += Input[int2(x, y)] * w;
			Weight += w;
		}
	}

	if (Weight > 0)
	{
		Color /= Weight;
	}

	// packs float3 Color to uint BGRA output
	Output[Id.xy] = dot(uint3(clamp(Color.bgr, 0, 1) * 255 + 0.5), uint3(1, 1 << 8, 1 << 16));
}

cbuffer ConvertMatrix : register(b0)
{
	row_major float3x3 RGB_To_YUV;
	row_major float3x3 YUV_To_RGB;
}

// https://en.wikipedia.org/wiki/Luma_(video)#Use_of_relative_luminance
static const float3 LumaWeights = float3(0.2126, 0.7152, 0.0722);

// adjustments for "TV" limited range YUV
// Y  = [ 0.0, +1.0] -> [16, 16+219]
// UV = [-0.5, +0.5] -> [16, 16+224]

#define OFFSET_Y 16
#define RANGE_Y  219
#define RANGE_UV 224

float LumaFromRGB(float3 LinearColor)
{
	return dot(LumaWeights, LinearColor);
}

float3 LinearToGamma(float3 X)
{
	return X < 0.0031308 ? X * 12.92 : 1.055 * pow(X, 1.0 / 2.4) - 0.055;
}

float3 GammaToLinear(float3 X)
{
	return X < 0.04045 ? X / 12.92 : pow((X + 0.055) / 1.055, 2.4);
}

float LumaFromYUV(float InY, float InU, float InV)
{
	float3 YUV;
	YUV.x = InY * (255.0 / RANGE_Y) - float(OFFSET_Y) / RANGE_Y;
	YUV.y = InU;
	YUV.z = InV;
	return LumaFromRGB(GammaToLinear(mul(YUV_To_RGB, YUV)));
}

Texture2D<float3> InColor  : register(t0);
Texture2D<float2> InUV     : register(t1);
Texture3D<float>  InLookup : register(t2);

RWTexture2D<unorm float>  OutY  : register(u0);
RWTexture2D<unorm float2> OutUV : register(u1);

SamplerState SamplerLinear : register(s0);

[numthreads(16, 8, 1)]
void ConvertSimple(uint3 Id: SV_DispatchThreadID)
{
	float2 InSize;
	InColor.GetDimensions(InSize.x, InSize.y);

	// bilinear interpolation of RGB color input
	// this uses horizontally co-sited & vertically centered locations of chroma values
	float2 ColorPos = float2(Id.xy * 2) / InSize;
	float3 Color0 = InColor.SampleLevel(SamplerLinear, ColorPos, 0, int2(0, 1));
	float3 Color1 = InColor.SampleLevel(SamplerLinear, ColorPos, 0, int2(1, 1));
	float3 Color = lerp(Color0, Color1, 0.5);

	// convert to YUV & store chroma values
	OutUV[Id.xy] = mul(RGB_To_YUV, Color).yz * (RANGE_UV / 255.0) + 0.5; // NV12 cannot be snorm, add 0.5 to output unorm

	// load RGB colors from exact pixel locations, convert & store Y value
	int4 Pos4 = Id.xyxy * 2 + int4(0, 0, 1, 1);
	OutY[Pos4.xy] = mul(RGB_To_YUV, InColor[Pos4.xy]).x * (RANGE_Y / 255.0) + (OFFSET_Y / 255.0);
	OutY[Pos4.zy] = mul(RGB_To_YUV, InColor[Pos4.zy]).x * (RANGE_Y / 255.0) + (OFFSET_Y / 255.0);
	OutY[Pos4.xw] = mul(RGB_To_YUV, InColor[Pos4.xw]).x * (RANGE_Y / 255.0) + (OFFSET_Y / 255.0);
	OutY[Pos4.zw] = mul(RGB_To_YUV, InColor[Pos4.zw]).x * (RANGE_Y / 255.0) + (OFFSET_Y / 255.0);
}

[numthreads(16, 8, 1)]
void ConvertPass1(uint3 Id: SV_DispatchThreadID)
{
	float2 InSize;
	InColor.GetDimensions(InSize.x, InSize.y);

	// bilinear interpolation of linear RGB color input
	// this uses horizontally co-sited & vertically centered chroma locations
	float2 ColorPos = float2(Id.xy * 2) / InSize;
	float3 Color0 = InColor.SampleLevel(SamplerLinear, ColorPos, 0, int2(0, 1));
	float3 Color1 = InColor.SampleLevel(SamplerLinear, ColorPos, 0, int2(1, 1));
	float3 Color = LinearToGamma(lerp(Color0, Color1, 0.5));

	// convert to YUV & store only chroma values
	OutUV[Id.xy] = mul(RGB_To_YUV, Color).yz * (RANGE_UV / 255.0) + 0.5; // NV12 cannot be snorm, add 0.5 to output unorm
}

[numthreads(16, 8, 1)]
void ConvertPass2(uint3 Id: SV_DispatchThreadID)
{
	float2 InSize;
	InUV.GetDimensions(InSize.x, InSize.y);

	// bilinear interpolation of chroma values that decoder is expected to calculate
	// this uses horizontally co-sited & vertically centered chroma locations
	float2 PosBase = float2(Id.xy / 2);
	float2 PosOffset = (Id.xy & 1) ? float2(0.5, 0.25) : float2(1.0, 0.75);
	float2 UV = InUV.SampleLevel(SamplerLinear, (PosBase + PosOffset) / InSize, 0);

	// RGB color value, loaded from exact pixel location
	float3 Color = InColor[Id.xy];

	// now for each pixel we want to solve for Y in the following equation:
	//   dot(W, F(mul(M, T)) == L
	// where:
	//   W is vector of 3 luma weights for RGB channels
	//   M is YUV to RGB conversion matrix
	//   T is [Y,U,V] column vector
	//   F(x) is gamma-to-linear function
	//   L = relative luminance - desired brightness

#ifdef USE_APPROXIMATE_GAMMA

	// this code solves equation above assuming F(x)=x*x approximation
	// result is quite good, but not great

	Color = LinearToGamma(Color); // undo SRGB conversion
	float L = LumaFromRGB(Color*Color);
	float3 W = LumaWeights;
	float3 K = mul(YUV_To_RGB, float3(0, UV * (255.0 / RANGE_UV) - 0.5 * (255.0 / RANGE_UV)));
	float A = dot(W, K);
	float B = dot(W, K*K);
	float C = A*A - B + L;
	float D = sqrt(C) - A;
	float Y = saturate(D) * (RANGE_Y / 255.0) + (OFFSET_Y / 255.0);

#else // use precalculated lookup table for (L,U,V) -> Y mapping

	// relative luminance of linear input RGB color
	float L = LumaFromRGB(Color);

	// lookup table does not have first and last 16 values of UV, this adjusts the lookup index for that
	UV = UV * (255.0 / 224.0) - (16.0 / 224.0);

	float3 LookupPos = float3(L, UV);
	float3 LookupOffset = 0.5 / float3(256, 225, 225); // InLookup texture dimensions

	float Y = InLookup.SampleLevel(SamplerLinear, LookupPos + LookupOffset, 0);

#endif

	// store Y value, it is clamped to [16, 16+219] range as unorm
	OutY[Id.xy] = Y;
}

RWTexture3D<unorm float> OutLookup : register(u0);

void PrepareLookup(int3 Id, int BitCount)
{
	float InU = float(Id.y + 16 - 128) / RANGE_UV;
	float InV = float(Id.z + 16 - 128) / RANGE_UV;

	// relative luminance - desired brightness
	float Target = float(Id.x) / 255.0;

	// binary search of Y value
	float StartY = 0.0;
	float RangeY = 1.0;

	[unroll]
	for (int i=0; i<BitCount-1; i++)
	{
		RangeY /= 2;
		float InY = StartY + RangeY;
		float Luma = LumaFromYUV(InY, InU, InV);
		StartY += (Luma < Target) ? RangeY : 0.0;
	}
	RangeY /= 2;

	// decide between last two Y values, whichever will be closest
	float Best0 = StartY;
	float Best1 = StartY + RangeY;

	float Luma0 = LumaFromYUV(Best0, InU, InV);
	float Luma1 = LumaFromYUV(Best1, InU, InV);

	float BestY = abs(Luma0 - Target) < abs(Luma1 - Target) ? Best0 : Best1;

	float MinY = float(OFFSET_Y) / 255.0;
	float MaxY = float(OFFSET_Y + RANGE_Y) / 255.0;

	// BestY is Y value for (L,U,V) input
	OutLookup[Id.xyz] = clamp(BestY, MinY, MaxY);
}

[numthreads(4, 4, 4)]
void PrepareLookup8(uint3 Id: SV_DispatchThreadID)
{
	PrepareLookup(int3(Id), 8);
}

[numthreads(4, 4, 4)]
void PrepareLookup16(uint3 Id: SV_DispatchThreadID)
{
	// this does not need full 16 bits, something like 12 probably would be enough
	// but generating full 16 vs 12 does not not cost much difference in performance
	// so let's do full precision!
	PrepareLookup(int3(Id), 16);
}
