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
	const float3 H = SafeNormalize(V + L, N);
	const float NdotV = max(dot(N, V), 1.0e-4f);
	const float NdotL = max(dot(N, L), 0.0f);
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
	const float3 DirectDiffuseWeight = (1.0f - Fresnel) * (1.0f - Metallic);
	const float3 DirectDiffuse = DirectDiffuseWeight * Albedo / 3.14159265f;
	const float3 DirectRadiance = LightColorIntensity.rgb * LightColorIntensity.a;
	const float3 BiasedWorldPosition = Input.WorldPosition + N * (ShadowParameters.y * (1.0f - NdotL));
	const float3 DirectColor = (DirectDiffuse + Specular) * DirectRadiance * NdotL *
		ShadowVisibility(BiasedWorldPosition, Input.ViewDepth);

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
	const float3 Color = DirectColor + EnvironmentIbl;
	return float4(Color, BaseColorFactor.a);
}
