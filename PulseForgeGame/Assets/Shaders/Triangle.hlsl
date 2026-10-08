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
};

Texture2D DiffuseTexture : register(t0);
SamplerState DiffuseSampler : register(s0);

float3 SafeNormalize(float3 Value, float3 Fallback)
{
	const float LengthSquared = dot(Value, Value);
	return LengthSquared > 1.0e-8f ? Value * rsqrt(LengthSquared) : Fallback;
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
};

VertexOutput VSMain(VertexInput Input)
{
	VertexOutput Output;
	Output.Position = mul(ModelViewProjection, float4(Input.Position, 1.0f));
	const float4 WorldPosition = mul(Model, float4(Input.Position, 1.0f));
	Output.WorldPosition = WorldPosition.xyz;
	Output.Normal = mul((float3x3)NormalTransform, Input.Normal);
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
	const float3 DiffuseWeight = (1.0f - Fresnel) * (1.0f - Metallic);
	const float3 Diffuse = DiffuseWeight * Albedo / 3.14159265f;
	const float3 Radiance = LightColorIntensity.rgb * LightColorIntensity.a;
	const float3 Color = (Diffuse + Specular) * Radiance * NdotL;
	return float4(Color, BaseColorFactor.a);
}
