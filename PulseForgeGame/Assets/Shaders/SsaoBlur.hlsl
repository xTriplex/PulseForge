cbuffer BlurParameters : register(b0, space4)
{
	float4x4 InverseProjection;
	float4 OutputSize;
	float4 HalfSize;
	float4 Direction; // x/y half-resolution texel offset
};

Texture2D<float4> AmbientOcclusionTexture : register(t0, space4);
Texture2D<float4> ViewNormalTexture : register(t1, space4);
Texture2D<float> CameraDepth : register(t2, space4);
SamplerState LinearClampSampler : register(s0, space4);
SamplerState PointClampSampler : register(s1, space4);

float3 ReconstructViewPosition(float2 UV, float Depth)
{
	const float4 H = mul(InverseProjection, float4(UV.x * 2.0f - 1.0f, 1.0f - UV.y * 2.0f, Depth, 1.0f));
	return H.xyz / max(abs(H.w), 1.0e-7f) * sign(H.w);
}

struct VertexInput
{
	[[vk::location(0)]] float2 Position : POSITION;
};

struct VertexOutput
{
	float4 Position : SV_Position;
	[[vk::location(0)]] float2 UV : TEXCOORD0;
};

VertexOutput VSMain(VertexInput Input)
{
	VertexOutput Output;
	Output.Position = float4(Input.Position, 0.0f, 1.0f);
	Output.UV = float2(Input.Position.x * 0.5f + 0.5f, 0.5f - Input.Position.y * 0.5f);
	return Output;
}

float4 PSMain(VertexOutput Input) : SV_Target0
{
	const float CenterDepth = CameraDepth.SampleLevel(PointClampSampler, Input.UV, 0.0f);
	if (CenterDepth >= 0.999999f)
		return 1.0f.xxxx;
	const float3 CenterNormal = normalize(ViewNormalTexture.SampleLevel(PointClampSampler, Input.UV, 0.0f).xyz * 2.0f - 1.0f);
	const float CenterViewDepth = ReconstructViewPosition(Input.UV, CenterDepth).z;
	const float CenterAo = AmbientOcclusionTexture.SampleLevel(LinearClampSampler, Input.UV, 0.0f).r;
	const float Weights[5] = { 1.0f, 4.0f, 6.0f, 4.0f, 1.0f };
	float Sum = 0.0f;
	float WeightSum = 0.0f;
	[unroll]
	for (int Tap = -2; Tap <= 2; ++Tap)
	{
		const float2 UV = Input.UV + Direction.xy * float(Tap);
		const float Depth = CameraDepth.SampleLevel(PointClampSampler, UV, 0.0f);
		if (Depth >= 0.999999f)
			continue;
		const float3 Normal = normalize(ViewNormalTexture.SampleLevel(PointClampSampler, UV, 0.0f).xyz * 2.0f - 1.0f);
		const float Ao = AmbientOcclusionTexture.SampleLevel(LinearClampSampler, UV, 0.0f).r;
		const float NeighborViewDepth = ReconstructViewPosition(UV, Depth).z;
		const float DepthWeight = exp(-abs(NeighborViewDepth - CenterViewDepth) * 12.0f);
		const float NormalWeight = pow(saturate(dot(CenterNormal, Normal)), 16.0f);
		const float Weight = Weights[Tap + 2] * DepthWeight * NormalWeight;
		Sum += Ao * Weight;
		WeightSum += Weight;
	}
	const float Result = WeightSum > 1.0e-5f ? Sum / WeightSum : CenterAo;
	return saturate(Result).xxxx;
}
