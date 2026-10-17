#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace PulseForge::GpuValidation::PointShadowImageAnalysis
{
	struct ImageView
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
		std::span<const std::byte> Pixels;
	};

	struct Metrics
	{
		size_t ExpectedPixelCount = 0;
		size_t ControlPixelCount = 0;
		size_t ChangedPixelCount = 0;
		size_t ChangedExpectedPixelCount = 0;
		size_t ChangedOtherPixelCount = 0;
		double ExpectedOff = 0.0;
		double ExpectedOn = 0.0;
		double ControlOff = 0.0;
		double ControlOn = 0.0;
		double ChangedMean = 0.0;
		double ChangedWorldX = 0.0;
		double ChangedWorldZ = 0.0;
	};

	struct DepthProbe
	{
		uint32_t RenderedFace = 0;
		uint32_t X = 0;
		uint32_t Y = 0;
		float ReceiverDepth = 1.0f;
		float DirectDepth = 1.0f;
		float VerticallyFlippedDepth = 1.0f;
	};

	[[nodiscard]] bool SegmentIntersectsShadowCaster(
		const glm::vec3& Start, const glm::vec3& End, const glm::vec3& CasterOffset = glm::vec3(0.0f)) noexcept;

	[[nodiscard]] float PixelLuminance(const ImageView& Image, size_t PixelIndex);

	[[nodiscard]] DepthProbe ProbePointShadowDepth(
		const std::array<std::optional<ImageView>, 6>& Faces, const glm::vec3& LightPosition, const glm::vec3& Receiver, float Range);

	[[nodiscard]] Metrics MeasurePointShadowImage(const ImageView& Off, const ImageView& On, const glm::mat4& ViewProjection,
		const glm::vec3& CameraPosition, const glm::vec3& LightPosition, float ReceiverPlaneY, const glm::vec3& CasterOffset);
} // namespace PulseForge::GpuValidation::PointShadowImageAnalysis
