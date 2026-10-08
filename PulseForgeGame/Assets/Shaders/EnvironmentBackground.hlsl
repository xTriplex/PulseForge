cbuffer Frame : register(b2)
{
	float4 CameraWorldPosition;
	float4 LightRayDirection;
	float4 LightColorIntensity;
	float4 EnvironmentInverseRotation;
	float4 EnvironmentParameters;
	float4x4 InverseViewProjection;
};

TextureCube EnvironmentTexture : register(t4);
SamplerState EnvironmentSampler : register(s1);

struct VertexInput
{
	[[vk::location(0)]] float2 Position : POSITION;
};

struct VertexOutput
{
	float4 Position : SV_Position;
	[[vk::location(0)]] float2 Ndc : TEXCOORD0;
};

VertexOutput VSMain(VertexInput Input)
{
	VertexOutput Output;
	Output.Position = float4(Input.Position, 1.0f, 1.0f);
	Output.Ndc = Input.Position;
	return Output;
}

float3 RotateByQuaternion(float3 Value, float4 Rotation)
{
	return Value + 2.0f * cross(Rotation.xyz, cross(Rotation.xyz, Value) + Rotation.w * Value);
}

float4 PSMain(VertexOutput Input) : SV_Target0
{
	const float4 FarPointHomogeneous = mul(InverseViewProjection, float4(Input.Ndc, 1.0f, 1.0f));
	const float3 FarPoint = FarPointHomogeneous.xyz / max(abs(FarPointHomogeneous.w), 1.0e-7f) * sign(FarPointHomogeneous.w);
	const float3 WorldDirection = normalize(FarPoint - CameraWorldPosition.xyz);
	const float3 EnvironmentDirection = normalize(RotateByQuaternion(WorldDirection, EnvironmentInverseRotation));
	const float3 Radiance = EnvironmentTexture.Sample(EnvironmentSampler, EnvironmentDirection).rgb;
	return float4(Radiance * EnvironmentParameters.x, 1.0f);
}
