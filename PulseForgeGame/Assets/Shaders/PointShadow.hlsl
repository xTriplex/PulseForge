cbuffer PointShadowPass : register(b0)
{
	float4x4 LightViewProjection;
	float4 LightPositionRange;
	float4x4 Model;
};

struct VertexInput
{
	[[vk::location(0)]] float3 Position : POSITION;
};

struct VertexOutput
{
	float4 Position : SV_Position;
	[[vk::location(0)]] float3 WorldPosition : TEXCOORD0;
};

VertexOutput VSMain(VertexInput Input)
{
	VertexOutput Output;
	const float4 World = mul(Model, float4(Input.Position, 1.0f));
	Output.Position = mul(LightViewProjection, World);
	Output.WorldPosition = World.xyz;
	return Output;
}

float PSMain(VertexOutput Input) : SV_Depth
{
	return saturate(length(Input.WorldPosition - LightPositionRange.xyz) / LightPositionRange.w);
}
