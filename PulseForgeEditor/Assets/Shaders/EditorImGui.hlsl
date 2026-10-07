Texture2D EditorTexture : register(t0);
SamplerState EditorSampler : register(s0);

struct VertexInput
{
	[[vk::location(0)]] float2 Position : POSITION;
	[[vk::location(1)]] float2 TexCoord : TEXCOORD0;
	[[vk::location(2)]] float4 Color : COLOR0;
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
	return Input.Color * EditorTexture.Sample(EditorSampler, Input.TexCoord);
}
