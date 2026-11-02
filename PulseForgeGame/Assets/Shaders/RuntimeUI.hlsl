cbuffer RuntimeUIOutput : register(b0)
{
	uint EncodeSrgb;
	uint3 Padding;
};

Texture2D<float4> RuntimeTexture : register(t0);
SamplerState RuntimeSampler : register(s0);

struct VertexInput
{
	[[vk::location(0)]] float2 Position : POSITION;
	[[vk::location(1)]] float4 Color : COLOR0;
	[[vk::location(2)]] float2 UV : TEXCOORD0;
};

struct VertexOutput
{
	float4 Position : SV_Position;
	[[vk::location(0)]] float4 Color : COLOR0;
	[[vk::location(1)]] float2 UV : TEXCOORD0;
};

VertexOutput VSMain(VertexInput Input)
{
	VertexOutput Output;
	Output.Position = float4(Input.Position, 0.0f, 1.0f);
	Output.Color = Input.Color;
	Output.UV = Input.UV;
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
	// Managed images are sRGB textures (sampled as linear); font atlases are linear RGBA8
	// with white RGB and glyph coverage in alpha. Both use straight-alpha tinting here.
	const float4 Sampled = RuntimeTexture.Sample(RuntimeSampler, Input.UV);
	const float3 LinearTintedColor = Sampled.rgb * Input.Color.rgb;
	const float3 DisplayColor = EncodeSrgb != 0 ? LinearToSrgb(LinearTintedColor) : LinearTintedColor;
	return float4(DisplayColor, Sampled.a * Input.Color.a);
}
