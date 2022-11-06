// resize & convert input
Texture2D<float3> Input : register(t0);

// resize output, packed BGRA format
RWTexture2D<uint> Output : register(u0);

// convert output
RWTexture2D<uint>  OutputY  : register(u0);
RWTexture2D<uint2> OutputUV : register(u1);

// YUV conversion matrix
cbuffer ConvertBuffer : register(b0)
{
	row_major float3x4 ConvertMtx;
};

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

float3 RgbToYuv(float3 Rgb)
{
	return mul(ConvertMtx, float4(Rgb, 1.0));
}

[numthreads(16, 8, 1)]
void Convert(uint3 Id: SV_DispatchThreadID)
{
	int2 InSize;
	Input.GetDimensions(InSize.x, InSize.y);

	int2 Pos = int2(Id.xy);
	int2 Pos2 = Pos * 2;

	int4 Pos4 = int4(Pos2, Pos2 + int2(1, 1));

	int4 Src = int4(Pos2, min(Pos4.zw, InSize - int2(1, 1)));

	// load RGB colors in 2x2 area
	float3 Rgb0 = Input[Src.xy];
	float3 Rgb1 = Input[Src.zy];
	float3 Rgb2 = Input[Src.xw];
	float3 Rgb3 = Input[Src.zw];

	// convert RGB to YUV
	float3 Yuv0 = RgbToYuv(Rgb0);
	float3 Yuv1 = RgbToYuv(Rgb1);
	float3 Yuv2 = RgbToYuv(Rgb2);
	float3 Yuv3 = RgbToYuv(Rgb3);

	// average UV
	float2 UV = (Yuv0.yz + Yuv1.yz + Yuv2.yz + Yuv3.yz) / 4.0;

	// store Y
	OutputY[Pos4.xy] = uint(Yuv0.x);
	OutputY[Pos4.zy] = uint(Yuv1.x);
	OutputY[Pos4.xw] = uint(Yuv2.x);
	OutputY[Pos4.zw] = uint(Yuv3.x);

	// store UV
	OutputUV[Pos] = uint2(UV);
}
