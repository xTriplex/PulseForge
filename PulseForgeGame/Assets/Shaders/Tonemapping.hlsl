Texture2D<float4> SceneLinearHdr : register(t0);
SamplerState SceneSampler : register(s0);

cbuffer ToneMapping : register(b0)
{
	float ExposureEV;
	uint EncodeSrgbForUnorm;
	float2 Padding;
};

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

float AcesFittedChannel(float Value)
{
	if (isnan(Value) || Value <= 0.0f)
		return 0.0f;
	if (isinf(Value))
		return 1.0f;
	const float Numerator = Value * (2.51f * Value + 0.03f);
	const float Denominator = Value * (2.43f * Value + 0.59f) + 0.14f;
	return saturate(Numerator / max(Denominator, 1.0e-6f));
}

float3 AcesFitted(float3 Color)
{
	// Narkowicz-style ACES-fitted approximation; this is not the complete ACES color-management system.
	return float3(
		AcesFittedChannel(Color.r),
		AcesFittedChannel(Color.g),
		AcesFittedChannel(Color.b));
}

float3 LinearToSrgb(float3 Color)
{
	const float3 Low = Color * 12.92f;
	const float3 High = 1.055f * pow(max(Color, 0.0f), 1.0f / 2.4f) - 0.055f;
	return lerp(High, Low, step(Color, 0.0031308f));
}

float4 PSMain(VertexOutput Input) : SV_Target0
{
	const float3 SceneLinear = SceneLinearHdr.SampleLevel(SceneSampler, Input.Uv, 0.0f).rgb;
	const float Exposure = exp2(ExposureEV);
	const float3 DisplayLinear = AcesFitted(SceneLinear * Exposure);
	const float3 OutputColor = EncodeSrgbForUnorm != 0 ? LinearToSrgb(DisplayLinear) : DisplayLinear;
	return float4(OutputColor, 1.0f);
}
