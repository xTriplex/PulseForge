cbuffer SsaoParameters : register(b0, space4)
{
	float4x4 InverseProjection;
	float4x4 Projection;
	float4 OutputSize;
	float4 HalfSize;
	float4 Parameters; // radius, depth bias, strength, unused
	float4 KernelSamples[32];
};

Texture2D<float> CameraDepth : register(t0, space4);
Texture2D<float4> ViewNormalTexture : register(t1, space4);
SamplerState PointClampSampler : register(s0, space4);

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

float3 ReconstructViewPosition(float2 UV, float Depth)
{
	const float4 H = mul(InverseProjection, float4(UV.x * 2.0f - 1.0f, 1.0f - UV.y * 2.0f, Depth, 1.0f));
	return H.xyz / max(abs(H.w), 1.0e-7f) * sign(H.w);
}

float HashAngle(uint2 Pixel)
{
	uint Hash = Pixel.x * 0x8da6b343u ^ Pixel.y * 0xd8163841u;
	Hash ^= Hash >> 16;
	Hash *= 0x7feb352du;
	Hash ^= Hash >> 15;
	Hash *= 0x846ca68bu;
	Hash ^= Hash >> 16;
	return (float(Hash) / 4294967296.0f) * 6.28318530718f;
}

float4 PSMain(VertexOutput Input) : SV_Target0
{
	const float Depth = CameraDepth.SampleLevel(PointClampSampler, Input.UV, 0.0f);
	if (Depth >= 0.999999f)
		return 1.0f.xxxx;

	const float3 Position = ReconstructViewPosition(Input.UV, Depth);
	const float3 EncodedNormal = ViewNormalTexture.SampleLevel(PointClampSampler, Input.UV, 0.0f).xyz;
	const float3 Normal = normalize(EncodedNormal * 2.0f - 1.0f);
	const uint2 Pixel = uint2(Input.Position.xy);
	const float Angle = HashAngle(Pixel);
	const float3 RandomVector = float3(cos(Angle), sin(Angle), 0.0f);
	const float3 TangentCandidate = RandomVector - Normal * dot(RandomVector, Normal);
	const float3 FallbackAxis = abs(Normal.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(0.0f, 1.0f, 0.0f);
	const float3 Tangent = dot(TangentCandidate, TangentCandidate) > 1.0e-8f
		? normalize(TangentCandidate)
		: normalize(cross(FallbackAxis, Normal));
	const float3 Bitangent = normalize(cross(Normal, Tangent));
	float Occlusion = 0.0f;
	[unroll]
	for (uint Index = 0; Index < 32; ++Index)
	{
		const float3 LocalSample = KernelSamples[Index].xyz;
		const float3 Offset = (Tangent * LocalSample.x + Bitangent * LocalSample.y + Normal * LocalSample.z) * Parameters.x;
		const float3 SamplePosition = Position + Offset;
		const float4 Projected = mul(Projection, float4(SamplePosition, 1.0f));
		if (Projected.w <= 1.0e-6f)
			continue;
		const float3 Ndc = Projected.xyz / Projected.w;
		const float2 SampleUV = float2(Ndc.x * 0.5f + 0.5f, 0.5f - Ndc.y * 0.5f);
		if (any(SampleUV <= 0.0f) || any(SampleUV >= 1.0f) || Ndc.z < 0.0f || Ndc.z > 1.0f)
			continue;
		const float SampleDepth = CameraDepth.SampleLevel(PointClampSampler, SampleUV, 0.0f);
		if (SampleDepth >= 0.999999f)
			continue;
		const float3 SurfacePosition = ReconstructViewPosition(SampleUV, SampleDepth);
		const float Difference = abs(Position.z - SurfacePosition.z);
		const float Range = saturate(1.0f - Difference / max(Parameters.x, 1.0e-4f));
		const float RangeWeight = Range * Range * (3.0f - 2.0f * Range);
		Occlusion += (SurfacePosition.z >= SamplePosition.z + Parameters.y ? 1.0f : 0.0f) * RangeWeight;
	}
	const float Visibility = pow(saturate(1.0f - Occlusion / 32.0f), max(Parameters.z, 0.0f));
	return Visibility.xxxx;
}
