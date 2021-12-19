// resize & convert input
Texture2D<float4> Input : register(t0);

// resize output, packed BGRA format
RWTexture2D<uint> Output : register(u0);

// convert output
RWTexture2D<uint>  OutputY  : register(u0);
RWTexture2D<uint2> OutputUV : register(u1);

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

[numthreads(16, 16, 1)]
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
	float4 Color = float4(0, 0, 0, 0);
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
	Output[Id.xy] = dot(uint3(floor(clamp(Color.bgr, 0, 1) * 255 + 0.5)), uint3(1, 1 << 8, 1 << 16));
}

// BT.709, limited range, Y=[16..235], UV=[16..240]
// https://en.wikipedia.org/wiki/YCbCr#ITU-R_BT.709_conversion
static const float3 ConvertY = float3(  0.2126,  0.7152,  0.0722 ) * 219.0 / 255.0;
static const float3 ConvertU = float3( -0.1146, -0.3854,  0.5    ) * 224.0 / 255.0;
static const float3 ConvertV = float3(  0.5,    -0.4542, -0.0458 ) * 224.0 / 255.0;

float Average(float4 Color)
{
	return floor(dot(Color, 1) * 0.25 + 0.5);
}

[numthreads(16, 16, 1)]
void Convert(uint3 Id: SV_DispatchThreadID)
{
	int2 InSize;
	Input.GetDimensions(InSize.x, InSize.y);

	int2 Pos = int2(Id.xy);
	int2 Pos2 = Pos * 2;

	int4 Pos4 = int4(Pos2, Pos2 + int2(1, 1));

	int4 Src = int4(Pos2, min(Pos4.zw, InSize - int2(1, 1)));

	float4 C0 = Input[Src.xy] * 255;
	float4 C1 = Input[Src.zy] * 255;
	float4 C2 = Input[Src.xw] * 255;
	float4 C3 = Input[Src.zw] * 255;

	float4 R = float4(C0.r, C1.r, C2.r, C3.r);
	float4 G = float4(C0.g, C1.g, C2.g, C3.g);
	float4 B = float4(C0.b, C1.b, C2.b, C3.b);

	float3 Avg = float3(Average(R), Average(G), Average(B));

	float4 Yf = ConvertY.x * R + ConvertY.y * G + ConvertY.z * B + 16.0;
	uint4 Y = uint4(clamp(Yf, 16, 255));
	OutputY[Pos4.xy] = Y.x;
	OutputY[Pos4.zy] = Y.y;
	OutputY[Pos4.xw] = Y.z;
	OutputY[Pos4.zw] = Y.w;

	float Uf = dot(ConvertU.xyz, Avg) + 128.5;
	float Vf = dot(ConvertV.xyz, Avg) + 128.5;
	uint U = uint(clamp(Uf, 16, 240));
	uint V = uint(clamp(Vf, 16, 240));
	OutputUV[Pos] = uint2(U, V);
}
