Texture2D<float4> CompletedUi : register(t0);
SamplerState UiSampler : register(s0);

struct VertexInput
{
	[[vk::location(0)]] float2 Position : POSITION;
};

struct VertexOutput
{
	float4 Position : SV_Position;
	[[vk::location(0)]] float2 Uv : TEXCOORD0;
};

VertexOutput VSMain(VertexInput Input)
{
	VertexOutput Output;
	Output.Position = float4(Input.Position, 0.0f, 1.0f);
	Output.Uv = Input.Position * float2(0.5f, -0.5f) + 0.5f;
	return Output;
}

float SrgbEncode(float Linear)
{
	const float Clamped = saturate(Linear);
	return Clamped <= 0.0031308f
		? Clamped * 12.92f
		: 1.055f * pow(Clamped, 1.0f / 2.4f) - 0.055f;
}

float4 PSMain(VertexOutput Input) : SV_Target0
{
	const float4 LinearUi = CompletedUi.SampleLevel(UiSampler, Input.Uv, 0.0f);
	return float4(
		SrgbEncode(LinearUi.r),
		SrgbEncode(LinearUi.g),
		SrgbEncode(LinearUi.b),
		1.0f);
}
