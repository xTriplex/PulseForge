#include "Core/PulseForgePCH.h"
#include "Renderer/ScreenSpaceAmbientOcclusion.h"

#include <algorithm>
#include <cmath>

namespace PulseForge
{
	AmbientOcclusionKernel GenerateAmbientOcclusionKernel() noexcept
	{
		AmbientOcclusionKernel Kernel;
		constexpr float GoldenAngle = 2.39996322972865332f;
		for (size_t Index = 0; Index < Kernel.Samples.size(); ++Index)
		{
			const float Fraction = (static_cast<float>(Index) + 0.5f) / static_cast<float>(Kernel.Samples.size());
			const float Z = Fraction;
			const float Radius = std::sqrt(std::max(1.0f - Z * Z, 0.0f));
			const float Angle = static_cast<float>(Index) * GoldenAngle;
			const float Distribution = std::lerp(0.1f, 1.0f, Fraction * Fraction);
			Kernel.Samples[Index] = glm::vec4(
				Radius * std::cos(Angle) * Distribution,
				Radius * std::sin(Angle) * Distribution,
				Z * Distribution,
				0.0f);
		}
		return Kernel;
	}

	std::array<uint32_t, 2> CalculateAmbientOcclusionExtent(uint32_t Width, uint32_t Height) noexcept
	{
		return {
			std::max(Width / 2u + Width % 2u, 1u),
			std::max(Height / 2u + Height % 2u, 1u)
		};
	}

	std::expected<glm::vec3, bool> ReconstructViewPosition(
		const glm::mat4& InverseProjection,
		const glm::vec2& TextureUV,
		float Depth) noexcept
	{
		if (!std::isfinite(TextureUV.x) || !std::isfinite(TextureUV.y) || !std::isfinite(Depth) ||
			Depth < 0.0f || Depth > 1.0f)
			return std::unexpected(false);
		const glm::vec4 Homogeneous = InverseProjection * glm::vec4(
			TextureUV.x * 2.0f - 1.0f,
			1.0f - TextureUV.y * 2.0f,
			Depth,
			1.0f);
		if (!std::isfinite(Homogeneous.x) || !std::isfinite(Homogeneous.y) ||
			!std::isfinite(Homogeneous.z) || !std::isfinite(Homogeneous.w) || std::abs(Homogeneous.w) < 1.0e-7f)
			return std::unexpected(false);
		const glm::vec3 Position = glm::vec3(Homogeneous) / Homogeneous.w;
		if (!std::isfinite(Position.x) || !std::isfinite(Position.y) || !std::isfinite(Position.z))
			return std::unexpected(false);
		return Position;
	}

	glm::vec3 EncodeViewNormal(const glm::vec3& Normal) noexcept
	{
		const float LengthSquared = glm::dot(Normal, Normal);
		const glm::vec3 SafeNormal = LengthSquared > 1.0e-8f && std::isfinite(LengthSquared)
			? Normal / std::sqrt(LengthSquared)
			: glm::vec3(0.0f, 0.0f, 1.0f);
		return SafeNormal * 0.5f + 0.5f;
	}

	glm::vec3 DecodeViewNormal(const glm::vec3& EncodedNormal) noexcept
	{
		const glm::vec3 Normal = EncodedNormal * 2.0f - 1.0f;
		const float LengthSquared = glm::dot(Normal, Normal);
		return LengthSquared > 1.0e-8f && std::isfinite(LengthSquared)
			? Normal / std::sqrt(LengthSquared)
			: glm::vec3(0.0f, 0.0f, 1.0f);
	}

	float AmbientOcclusionRangeWeight(float ViewDepthDifference, float Radius) noexcept
	{
		if (!std::isfinite(ViewDepthDifference) || !std::isfinite(Radius) || Radius <= 0.0f)
			return 0.0f;
		const float NormalizedDifference = std::abs(ViewDepthDifference) / Radius;
		const float T = std::clamp(1.0f - NormalizedDifference, 0.0f, 1.0f);
		return T * T * (3.0f - 2.0f * T);
	}

	float ComputeSpecularOcclusion(float AmbientOcclusion, float NdotV, float Roughness) noexcept
	{
		if (!std::isfinite(AmbientOcclusion) || !std::isfinite(NdotV) || !std::isfinite(Roughness))
			return 1.0f;
		const float Occlusion = std::clamp(AmbientOcclusion, 0.0f, 1.0f);
		const float Grazing = 1.0f - std::clamp(NdotV, 0.0f, 1.0f);
		const float RoughnessWeight = std::clamp(Roughness, 0.0f, 1.0f);
		return std::clamp(1.0f - (1.0f - Occlusion) * Grazing * RoughnessWeight, 0.0f, 1.0f);
	}
}
