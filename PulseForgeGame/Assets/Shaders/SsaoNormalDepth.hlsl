cbuffer PerObject : register(b0, space4)
{
	float4x4 ViewProjectionModel;
	float4x4 ViewNormalTransform;
};

struct VertexInput
{
	[[vk::location(0)]] float3 Position : POSITION;
	[[vk::location(1)]] float3 Normal : NORMAL;
};

struct VertexOutput
{
	float4 Position : SV_Position;
	[[vk::location(0)]] float3 ViewNormal : TEXCOORD0;
};

VertexOutput VSMain(VertexInput Input)
{
	VertexOutput Output;
	Output.Position = mul(ViewProjectionModel, float4(Input.Position, 1.0f));
	Output.ViewNormal = mul((float3x3)ViewNormalTransform, Input.Normal);
	return Output;
}

float4 PSMain(VertexOutput Input) : SV_Target0
{
	const float LengthSquared = dot(Input.ViewNormal, Input.ViewNormal);
	const float3 Normal = LengthSquared > 1.0e-8f
		? Input.ViewNormal * rsqrt(LengthSquared)
		: float3(0.0f, 0.0f, 1.0f);
	return float4(Normal * 0.5f + 0.5f, 1.0f);
}
