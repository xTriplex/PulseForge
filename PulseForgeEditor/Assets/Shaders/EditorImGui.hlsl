Texture2D EditorTexture : register(t0);
SamplerState EditorSampler : register(s0);

cbuffer EditorColorSpace : register(b0)
{
	uint OutputIsUnorm;
	uint TextureIsSrgb;
	float2 Padding;
};

struct VertexInput
{
	// Vulkan input locations follow PulseForge's VertexSemantic order: Position, Color, then TexCoord.
	[[vk::location(0)]] float2 Position : POSITION;
	[[vk::location(1)]] float4 Color : COLOR0;
	[[vk::location(2)]] float2 TexCoord : TEXCOORD0;
};

struct VertexOutput
{
	float4 Position : SV_Position;
	[[vk::location(0)]] float2 TexCoord : TEXCOORD0;
	[[vk::location(1)]] float4 Color : COLOR0;
};

VertexOutput VSMain(VertexInput Input)
{
	VertexOutput Output;
	Output.Position = float4(Input.Position, 0.0f, 1.0f);
	Output.TexCoord = Input.TexCoord;
	Output.Color = Input.Color;
	return Output;
}

float4 PSMain(VertexOutput Input) : SV_Target0
{
	float4 TextureColor = EditorTexture.Sample(EditorSampler, Input.TexCoord);
	if (OutputIsUnorm != 0 && TextureIsSrgb != 0)
	{
		const float3 Low = TextureColor.rgb * 12.92f;
		const float3 High = 1.055f * pow(max(TextureColor.rgb, 0.0f), 1.0f / 2.4f) - 0.055f;
		TextureColor.rgb = lerp(High, Low, step(TextureColor.rgb, 0.0031308f));
	}
	return Input.Color * TextureColor;
}
