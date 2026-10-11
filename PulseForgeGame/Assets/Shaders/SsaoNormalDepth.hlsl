cbuffer PerObject : register(b0, space4)
{
	float4x4 ViewProjectionModel;
	float4x4 ViewNormalTransform;
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
	[[vk::location(3)]] float3 Normal : NORMAL;
};

struct VertexOutput
{
	float4 Position : SV_Position;
	[[vk::location(0)]] float4 Color : COLOR0;
	[[vk::location(1)]] float2 TexCoord : TEXCOORD1;
	[[vk::location(2)]] float3 ViewNormal : TEXCOORD0;
};

VertexOutput VSMain(VertexInput Input)
{
	VertexOutput Output;
	Output.Position = mul(ViewProjectionModel, float4(Input.Position, 1.0f));
	Output.ViewNormal = mul((float3x3)ViewNormalTransform, Input.Normal);
	Output.Color = Input.Color;
	Output.TexCoord = Input.TexCoord;
	return Output;
}

float4 PSMain(VertexOutput Input, bool IsFrontFace : SV_IsFrontFace) : SV_Target0
{
	if (AlphaParameters.x > 0.5f && AlphaParameters.x < 1.5f)
	{
		const float Alpha = BaseColorFactor.a * DiffuseTexture.SampleLevel(DiffuseSampler, Input.TexCoord, 0.0f).a * Input.Color.a;
		if (Alpha < AlphaParameters.y)
			discard;
	}
	const float LengthSquared = dot(Input.ViewNormal, Input.ViewNormal);
	float3 Normal = LengthSquared > 1.0e-8f
		? Input.ViewNormal * rsqrt(LengthSquared)
		: float3(0.0f, 0.0f, 1.0f);
	if (AlphaParameters.z > 0.5f && !IsFrontFace)
		Normal = -Normal;
	return float4(Normal * 0.5f + 0.5f, 1.0f);
}
