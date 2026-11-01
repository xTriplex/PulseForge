cbuffer RuntimeUIOutput : register(b0)
{
	uint EncodeSrgb;
	uint3 Padding;
};

struct VertexInput
{
	[[vk::location(0)]] float2 Position : POSITION;
	[[vk::location(1)]] float4 Color : COLOR0;
};

struct VertexOutput
{
	float4 Position : SV_Position;
	[[vk::location(0)]] float4 Color : COLOR0;
};

VertexOutput VSMain(VertexInput Input)
{
	VertexOutput Output;
	Output.Position = float4(Input.Position, 0.0f, 1.0f);
	Output.Color = Input.Color;
	return Output;
}

float3 LinearToSrgb(float3 Color)
{
	const float3 Low = Color * 12.92f;
	const float3 High = 1.055f * pow(max(Color, 0.0f), 1.0f / 2.4f) - 0.055f;
	return lerp(High, Low, step(Color, 0.0031308f));
}

float4 PSMain(VertexOutput Input) : SV_Target0
{
	const float3 DisplayColor = EncodeSrgb != 0 ? LinearToSrgb(Input.Color.rgb) : Input.Color.rgb;
	return float4(DisplayColor, Input.Color.a);
}
