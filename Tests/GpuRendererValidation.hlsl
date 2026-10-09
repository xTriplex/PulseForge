struct VSInput
{
	float2 Position : POSITION;
	float2 TexCoord : TEXCOORD0;
};

struct VSOutput
{
	float4 Position : SV_Position;
	float2 TexCoord : TEXCOORD0;
};

VSOutput VSMain(VSInput Input)
{
	VSOutput Output;
	Output.Position = float4(Input.Position, 0.0f, 1.0f);
	Output.TexCoord = Input.TexCoord;
	return Output;
}

float4 PSColor() : SV_Target0
{
	return float4(4.0f, 0.5f, 0.25f, 1.0f);
}

Texture2D<float4> SourceTexture : register(t0);
SamplerState SourceSampler : register(s0);

float4 PSSample(VSOutput Input) : SV_Target0
{
	return SourceTexture.Sample(SourceSampler, Input.TexCoord);
}
