cbuffer ShadowObject : register(b0)
{
	float4x4 LightViewProjectionModel;
};

struct VertexInput
{
	[[vk::location(0)]] float3 Position : POSITION;
};

float4 VSMain(VertexInput Input) : SV_Position
{
	return mul(LightViewProjectionModel, float4(Input.Position, 1.0f));
}
