cbuffer ShadowObject : register(b0, space3)
{
	float4x4 LightViewProjectionModel;
};

cbuffer Material : register(b0)
{
	float4 BaseColorFactor;
	float4 SurfaceParameters;
	float4 EmissiveFactor;
	float4 AlphaParameters;
};

Texture2D DiffuseTexture : register(t0);
SamplerState DiffuseSampler : register(s0);

struct VertexInput
{
	[[vk::location(0)]] float3 Position : POSITION;
	[[vk::location(1)]] float4 Color : COLOR0;
	[[vk::location(2)]] float2 TexCoord : TEXCOORD0;
};

struct VertexOutput
{
	float4 Position : SV_Position;
	[[vk::location(0)]] float4 Color : COLOR0;
	[[vk::location(1)]] float2 TexCoord : TEXCOORD0;
};

VertexOutput VSMain(VertexInput Input)
{
	VertexOutput Output;
	Output.Position = mul(LightViewProjectionModel, float4(Input.Position, 1.0f));
	Output.Color = Input.Color;
	Output.TexCoord = Input.TexCoord;
	return Output;
}

void PSMain(VertexOutput Input)
{
	if (AlphaParameters.x > 0.5f && AlphaParameters.x < 1.5f)
	{
		const float Alpha = BaseColorFactor.a * DiffuseTexture.SampleLevel(DiffuseSampler, Input.TexCoord, 0.0f).a * Input.Color.a;
		if (Alpha < AlphaParameters.y)
			discard;
	}
}
