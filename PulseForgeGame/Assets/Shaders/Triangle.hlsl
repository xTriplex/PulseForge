cbuffer Material : register(b0)
{
	float4 BaseColorFactor;
	float4 SurfaceParameters; // metallic, roughness, unused, unused
};

cbuffer Object : register(b1)
{
	float4x4 Model;
	float4x4 ModelViewProjection;
	float4x4 NormalTransform;
};

cbuffer Frame : register(b2)
{
	float4 CameraWorldPosition;
	float4 LightRayDirection;
	float4 LightColorIntensity;
	float4 EnvironmentInverseRotation;
	float4 EnvironmentParameters; // linear radiance intensity, maximum specular mip, unused, unused
	float4x4 InverseViewProjection;
	float4x4 View;
	float4x4 Projection;
	float4 OutputSize;
	float4 CascadeSplitDepths;
	float4 ShadowParameters; // enabled, receiver normal bias, PCF radius (texels), shadow distance
	float4x4 CascadeViewProjection[4];
	float4x4 SpotShadowViewProjection[4];
	float4 SpotShadowParameters[4]; // receiver depth bias, normal bias (world units), PCF radius, enabled
};

struct LocalLightData
{
	float4 PositionRange;
	float4 ColorIntensity;
	float4 DirectionInnerCos;
	float4 OuterCosAndType; // outer cosine, shadow-map slot (-1 = none), unused, 1 for spot / 0 for point
};

cbuffer LocalLighting : register(b3)
{
	uint4 LocalLightCounts; // x is active count, bounded by 32
	LocalLightData LocalLights[32];
};

Texture2D DiffuseTexture : register(t0);
TextureCube IrradianceTexture : register(t1);
TextureCube PrefilteredEnvironment : register(t2);
Texture2D BrdfIntegrationLut : register(t3);
SamplerState DiffuseSampler : register(s0);
SamplerState EnvironmentSampler : register(s1);
Texture2D<float> ShadowCascade0 : register(t0, space1);
Texture2D<float> ShadowCascade1 : register(t1, space1);
Texture2D<float> ShadowCascade2 : register(t2, space1);
Texture2D<float> ShadowCascade3 : register(t3, space1);
SamplerState ShadowSampler : register(s0, space1);
Texture2D<float> AmbientOcclusionTexture : register(t0, space2);
SamplerState AmbientOcclusionSampler : register(s0, space2);
Texture2D<float> SpotShadowMap0 : register(t0, space3);
Texture2D<float> SpotShadowMap1 : register(t1, space3);
Texture2D<float> SpotShadowMap2 : register(t2, space3);
Texture2D<float> SpotShadowMap3 : register(t3, space3);
SamplerState SpotShadowSampler : register(s0, space3);

float3 SafeNormalize(float3 Value, float3 Fallback)
{
	const float LengthSquared = dot(Value, Value);
	return LengthSquared > 1.0e-8f ? Value * rsqrt(LengthSquared) : Fallback;
}

float3 RotateByQuaternion(float3 Value, float4 Rotation)
{
	return Value + 2.0f * cross(Rotation.xyz, cross(Rotation.xyz, Value) + Rotation.w * Value);
}

float FilterShadowMap(Texture2D<float> ShadowMap, float2 UV, float ReceiverDepth, float Radius)
{
	uint Width;
	uint Height;
	ShadowMap.GetDimensions(Width, Height);
	const float2 Texel = 1.0f / float2(Width, Height);
	float Visibility = 0.0f;
	[unroll]
	for (int Y = -1; Y <= 1; ++Y)
	{
		[unroll]
		for (int X = -1; X <= 1; ++X)
		{
			const float StoredDepth = ShadowMap.SampleLevel(
				ShadowSampler, UV + float2(X, Y) * Texel * Radius, 0.0f);
			Visibility += ReceiverDepth <= StoredDepth ? 1.0f : 0.0f;
		}
	}
	return Visibility / 9.0f;
}

float CascadeShadowVisibility(uint CascadeIndex, float3 WorldPosition, float ReceiverBias)
{
	const float4 Clip = mul(CascadeViewProjection[CascadeIndex], float4(WorldPosition, 1.0f));
	if (Clip.w <= 1.0e-6f)
		return 1.0f;
	const float3 Ndc = Clip.xyz / Clip.w;
	const float2 UV = float2(Ndc.x * 0.5f + 0.5f, 0.5f - Ndc.y * 0.5f);
	if (any(UV < 0.0f) || any(UV > 1.0f) || Ndc.z < 0.0f || Ndc.z > 1.0f)
		return 1.0f;
	const float ReceiverDepth = max(Ndc.z - ReceiverBias, 0.0f);
	const float Radius = max(ShadowParameters.z, 0.5f);
	if (CascadeIndex == 0) return FilterShadowMap(ShadowCascade0, UV, ReceiverDepth, Radius);
	if (CascadeIndex == 1) return FilterShadowMap(ShadowCascade1, UV, ReceiverDepth, Radius);
	if (CascadeIndex == 2) return FilterShadowMap(ShadowCascade2, UV, ReceiverDepth, Radius);
	return FilterShadowMap(ShadowCascade3, UV, ReceiverDepth, Radius);
}

float ShadowVisibility(float3 WorldPosition, float ViewDepth)
{
	if (ShadowParameters.x < 0.5f || ViewDepth > ShadowParameters.w)
		return 1.0f;
	uint CascadeIndex = 0;
	if (ViewDepth > CascadeSplitDepths.x) CascadeIndex = 1;
	if (ViewDepth > CascadeSplitDepths.y) CascadeIndex = 2;
	if (ViewDepth > CascadeSplitDepths.z) CascadeIndex = 3;
	const float CurrentVisibility = CascadeShadowVisibility(CascadeIndex, WorldPosition, 0.0005f);
	if (CascadeIndex == 3)
		return CurrentVisibility;
	const float PreviousSplit = CascadeIndex == 0 ? 0.0f : CascadeSplitDepths[CascadeIndex - 1];
	const float CurrentSplit = CascadeSplitDepths[CascadeIndex];
	const float BlendWidth = max((CurrentSplit - PreviousSplit) * 0.08f, 0.1f);
	const float BlendStart = CurrentSplit - BlendWidth;
	if (ViewDepth < BlendStart)
		return CurrentVisibility;
	const float NextVisibility = CascadeShadowVisibility(CascadeIndex + 1, WorldPosition, 0.0005f);
	return lerp(CurrentVisibility, NextVisibility, saturate((ViewDepth - BlendStart) / BlendWidth));
}

float FilterSpotShadowMap(Texture2D<float> ShadowMap, float2 UV, float ReceiverDepth, float Radius)
{
	uint Width;
	uint Height;
	ShadowMap.GetDimensions(Width, Height);
	const float2 Texel = 1.0f / float2(Width, Height);
	float Visibility = 0.0f;
	[unroll]
	for (int Y = -1; Y <= 1; ++Y)
	{
		[unroll]
		for (int X = -1; X <= 1; ++X)
		{
			const float2 SampleUV = UV + float2(X, Y) * Texel * Radius;
			if (any(SampleUV < 0.0f) || any(SampleUV > 1.0f))
				Visibility += 1.0f;
			else
			{
				const float StoredDepth = ShadowMap.SampleLevel(SpotShadowSampler, SampleUV, 0.0f);
				Visibility += ReceiverDepth <= StoredDepth ? 1.0f : 0.0f;
			}
		}
	}
	return Visibility / 9.0f;
}

float SpotlightShadowVisibility(uint ShadowIndex, float3 WorldPosition, float3 Normal, float NdotL)
{
	if (ShadowIndex >= 4u)
		return 1.0f;
	if (SpotShadowParameters[ShadowIndex].w < 0.5f)
		return 1.0f;
	const float4 Parameters = SpotShadowParameters[ShadowIndex];
	const float3 BiasedPosition = WorldPosition + Normal * (Parameters.y * (1.0f - NdotL));
	const float4 Clip = mul(SpotShadowViewProjection[ShadowIndex], float4(BiasedPosition, 1.0f));
	if (Clip.w <= 1.0e-6f)
		return 1.0f;
	const float3 Ndc = Clip.xyz / Clip.w;
	const float2 UV = float2(Ndc.x * 0.5f + 0.5f, 0.5f - Ndc.y * 0.5f);
	if (any(UV < 0.0f) || any(UV > 1.0f) || Ndc.z < 0.0f || Ndc.z > 1.0f)
		return 1.0f;
	const float ReceiverDepth = max(Ndc.z - Parameters.x, 0.0f);
	const float Radius = max(Parameters.z, 0.0f);
	if (ShadowIndex == 0u) return FilterSpotShadowMap(SpotShadowMap0, UV, ReceiverDepth, Radius);
	if (ShadowIndex == 1u) return FilterSpotShadowMap(SpotShadowMap1, UV, ReceiverDepth, Radius);
	if (ShadowIndex == 2u) return FilterSpotShadowMap(SpotShadowMap2, UV, ReceiverDepth, Radius);
	return FilterSpotShadowMap(SpotShadowMap3, UV, ReceiverDepth, Radius);
}

struct VertexInput
{
	[[vk::location(0)]] float3 Position : POSITION;
	[[vk::location(1)]] float3 Color : COLOR0;
	[[vk::location(2)]] float2 TexCoord : TEXCOORD0;
	[[vk::location(3)]] float3 Normal : NORMAL;
};

struct VertexOutput
{
	float4 Position : SV_Position;
	[[vk::location(0)]] float3 Color : COLOR0;
	[[vk::location(1)]] float2 TexCoord : TEXCOORD0;
	[[vk::location(2)]] float3 WorldPosition : TEXCOORD1;
	[[vk::location(3)]] float3 Normal : TEXCOORD2;
	[[vk::location(4)]] float ViewDepth : TEXCOORD3;
};

float3 EvaluateDirectBRDF(
	float3 Albedo,
	float Metallic,
	float Roughness,
	float3 N,
	float3 V,
	float3 L,
	float3 Radiance,
	out float NdotL)
{
	const float3 H = SafeNormalize(V + L, N);
	const float NdotV = max(dot(N, V), 1.0e-4f);
	NdotL = max(dot(N, L), 0.0f);
	const float NdotH = max(dot(N, H), 0.0f);
	const float VdotH = max(dot(V, H), 0.0f);
	const float Alpha = Roughness * Roughness;
	const float AlphaSquared = Alpha * Alpha;
	const float Denominator = NdotH * NdotH * (AlphaSquared - 1.0f) + 1.0f;
	const float Distribution = AlphaSquared / max(3.14159265f * Denominator * Denominator, 1.0e-6f);
	const float GeometryK = (Roughness + 1.0f) * (Roughness + 1.0f) / 8.0f;
	const float GeometryV = NdotV / (NdotV * (1.0f - GeometryK) + GeometryK);
	const float GeometryL = NdotL / (NdotL * (1.0f - GeometryK) + GeometryK);
	const float3 F0 = lerp(0.04f.xxx, Albedo, Metallic);
	const float3 Fresnel = F0 + (1.0f - F0) * pow(1.0f - VdotH, 5.0f);
	const float3 Specular = Distribution * GeometryV * GeometryL * Fresnel /
		max(4.0f * NdotV * NdotL, 1.0e-4f);
	const float3 DiffuseWeight = (1.0f - Fresnel) * (1.0f - Metallic);
	const float3 Diffuse = DiffuseWeight * Albedo / 3.14159265f;
	return (Diffuse + Specular) * Radiance * NdotL;
}

float LocalRangeAttenuation(float Distance, float Range)
{
	if (Distance >= Range)
		return 0.0f;
	const float NormalizedDistance = Distance / Range;
	const float RangeWindow = max(1.0f - NormalizedDistance * NormalizedDistance *
		NormalizedDistance * NormalizedDistance, 0.0f);
	return RangeWindow * RangeWindow / max(Distance * Distance, 0.01f);
}

VertexOutput VSMain(VertexInput Input)
{
	VertexOutput Output;
	Output.Position = mul(ModelViewProjection, float4(Input.Position, 1.0f));
	const float4 WorldPosition = mul(Model, float4(Input.Position, 1.0f));
	Output.WorldPosition = WorldPosition.xyz;
	Output.Normal = mul((float3x3)NormalTransform, Input.Normal);
	Output.ViewDepth = -mul(View, WorldPosition).z;
	Output.Color = Input.Color;
	Output.TexCoord = Input.TexCoord;
	return Output;
}

float4 PSMain(VertexOutput Input) : SV_Target0
{
	const float3 Albedo = max(Input.Color * DiffuseTexture.Sample(DiffuseSampler, Input.TexCoord).rgb * BaseColorFactor.rgb, 0.0f);
	const float Metallic = saturate(SurfaceParameters.x);
	const float Roughness = clamp(SurfaceParameters.y, 0.045f, 1.0f);
	const float3 N = SafeNormalize(Input.Normal, float3(0.0f, 0.0f, 1.0f));
	const float3 V = SafeNormalize(CameraWorldPosition.xyz - Input.WorldPosition, float3(0.0f, 0.0f, 1.0f));
	const float3 L = SafeNormalize(-LightRayDirection.xyz, float3(0.0f, 0.0f, 1.0f));
	const float NdotV = max(dot(N, V), 1.0e-4f);
	const float3 F0 = lerp(0.04f.xxx, Albedo, Metallic);
	const float3 DirectRadiance = LightColorIntensity.rgb * LightColorIntensity.a;
	float NdotL = 0.0f;
	const float3 DirectionalBrdf = EvaluateDirectBRDF(Albedo, Metallic, Roughness, N, V, L, DirectRadiance, NdotL);
	const float3 BiasedWorldPosition = Input.WorldPosition + N * (ShadowParameters.y * (1.0f - NdotL));
	const float3 DirectionalColor = DirectionalBrdf * ShadowVisibility(BiasedWorldPosition, Input.ViewDepth);
	float3 LocalDirectColor = 0.0f.xxx;
	[loop]
	for (uint LightIndex = 0; LightIndex < min(LocalLightCounts.x, 32u); ++LightIndex)
	{
		const LocalLightData LocalLight = LocalLights[LightIndex];
		const float3 ToLight = LocalLight.PositionRange.xyz - Input.WorldPosition;
		const float DistanceSquared = dot(ToLight, ToLight);
		const float Distance = sqrt(max(DistanceSquared, 0.0f));
		if (Distance >= LocalLight.PositionRange.w)
			continue;
		const float3 LocalL = SafeNormalize(ToLight, N);
		float AngularAttenuation = 1.0f;
		if (LocalLight.OuterCosAndType.w > 0.5f)
		{
			const float3 LightToSurface = -LocalL;
			const float CosTheta = dot(LocalLight.DirectionInnerCos.xyz, LightToSurface);
			AngularAttenuation = smoothstep(
				LocalLight.OuterCosAndType.x, LocalLight.DirectionInnerCos.w, CosTheta);
		}
		const float Attenuation = LocalRangeAttenuation(Distance, LocalLight.PositionRange.w) * AngularAttenuation;
		float LocalNdotL = 0.0f;
		const float3 LocalContribution = EvaluateDirectBRDF(Albedo, Metallic, Roughness, N, V, LocalL,
			LocalLight.ColorIntensity.rgb * LocalLight.ColorIntensity.a * Attenuation, LocalNdotL);
		const float ShadowSlotValue = LocalLight.OuterCosAndType.y;
		const float SpotVisibility = LocalLight.OuterCosAndType.w > 0.5f && ShadowSlotValue >= 0.0f
			? SpotlightShadowVisibility((uint)round(ShadowSlotValue), Input.WorldPosition, N, LocalNdotL)
			: 1.0f;
		LocalDirectColor += LocalContribution * SpotVisibility;
	}

	const float3 AmbientFresnel = F0 + (max(1.0f - Roughness, F0) - F0) * pow(1.0f - NdotV, 5.0f);
	const float3 AmbientDiffuseWeight = (1.0f - AmbientFresnel) * (1.0f - Metallic);
	const float3 EnvironmentNormal = SafeNormalize(RotateByQuaternion(N, EnvironmentInverseRotation), N);
	const float3 DiffuseIrradiance = IrradianceTexture.Sample(EnvironmentSampler, EnvironmentNormal).rgb;
	const float3 DiffuseIbl = AmbientDiffuseWeight * Albedo * DiffuseIrradiance / 3.14159265f;
	const float3 Reflection = reflect(-V, N);
	const float3 EnvironmentReflection = SafeNormalize(RotateByQuaternion(Reflection, EnvironmentInverseRotation), Reflection);
	const float3 Prefiltered = PrefilteredEnvironment.SampleLevel(
		EnvironmentSampler, EnvironmentReflection, Roughness * EnvironmentParameters.y).rgb;
	const float2 Brdf = BrdfIntegrationLut.Sample(EnvironmentSampler, float2(NdotV, Roughness)).rg;
	const float3 SpecularIbl = Prefiltered * (AmbientFresnel * Brdf.x + Brdf.y);
	const float2 ScreenUV = Input.Position.xy / max(OutputSize.xy, 1.0f.xx);
	const float AmbientOcclusion = saturate(AmbientOcclusionTexture.Sample(AmbientOcclusionSampler, ScreenUV));
	const float SpecularOcclusion = saturate(1.0f - (1.0f - AmbientOcclusion) * (1.0f - NdotV) * Roughness);
	const float3 EnvironmentIbl = (DiffuseIbl * AmbientOcclusion + SpecularIbl * SpecularOcclusion) * EnvironmentParameters.x;
	const float3 Color = DirectionalColor + LocalDirectColor + EnvironmentIbl;
	return float4(Color, BaseColorFactor.a);
}
