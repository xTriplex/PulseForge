#include "TestSupport.h"

#include "GpuValidation/PointShadowImageAnalysis.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <cstring>

namespace
{
	using namespace PulseForge::GpuValidation::PointShadowImageAnalysis;

	void TestPointShadowImageOracle(TestRunner& Tests)
	{
		PF_CHECK(Tests, SegmentIntersectsShadowCaster({ -2.0f, 1.0f, 0.0f }, { 2.0f, 1.0f, 0.0f }));
		PF_CHECK(Tests, !SegmentIntersectsShadowCaster({ -2.0f, 2.0f, 0.0f }, { 2.0f, 2.0f, 0.0f }));
		PF_CHECK(Tests, SegmentIntersectsShadowCaster({ -2.0f, 1.0f, 2.0f }, { 2.0f, 1.0f, 2.0f }, { 0.0f, 0.0f, 2.0f }));

		const std::array<std::byte, 4> RedPixel = { std::byte{ 255 }, std::byte{ 0 }, std::byte{ 0 }, std::byte{ 255 } };
		const ImageView RedImage{ 1, 1, RedPixel };
		PF_CHECK(Tests, std::abs(PixelLuminance(RedImage, 0) - 0.2126f) < 0.0001f);
	}

	void TestPointShadowDepthProbe(TestRunner& Tests)
	{
		constexpr uint32_t Extent = 5;
		constexpr float StoredDepth = 0.375f;
		std::array<float, Extent * Extent> DepthPixels{};
		DepthPixels.fill(StoredDepth);
		const auto DepthBytes = std::as_bytes(std::span(DepthPixels));
		std::array<std::optional<ImageView>, 6> Faces{};
		Faces[0] = ImageView{ Extent, Extent, DepthBytes };

		const DepthProbe Probe = ProbePointShadowDepth(Faces, { 0.0f, 0.0f, 0.0f }, { 2.0f, 0.0f, 0.0f }, 4.0f);
		PF_CHECK(Tests, Probe.RenderedFace == 0);
		PF_CHECK(Tests, Probe.X < Extent && Probe.Y < Extent);
		PF_CHECK(Tests, std::abs(Probe.ReceiverDepth - 0.5f) < 0.0001f);
		PF_CHECK(Tests, std::abs(Probe.DirectDepth - StoredDepth) < 0.0001f);
		PF_CHECK(Tests, std::abs(Probe.VerticallyFlippedDepth - StoredDepth) < 0.0001f);
	}

	void TestPointShadowImageMeasurement(TestRunner& Tests)
	{
		constexpr uint32_t Extent = 64;
		std::array<std::byte, Extent * Extent * 4> OffPixels{};
		std::array<std::byte, Extent * Extent * 4> OnPixels{};
		for (size_t Pixel = 0; Pixel < Extent * Extent; ++Pixel)
		{
			for (size_t Channel = 0; Channel < 3; ++Channel)
			{
				OffPixels[Pixel * 4 + Channel] = std::byte{ 200 };
				OnPixels[Pixel * 4 + Channel] = std::byte{ 100 };
			}
			OffPixels[Pixel * 4 + 3] = std::byte{ 255 };
			OnPixels[Pixel * 4 + 3] = std::byte{ 255 };
		}

		const ImageView Off{ Extent, Extent, OffPixels };
		const ImageView On{ Extent, Extent, OnPixels };
		const glm::vec3 CameraPosition(0.0f, 5.0f, 8.0f);
		const glm::mat4 ViewProjection = glm::perspective(glm::radians(55.0f), 1.0f, 0.1f, 30.0f) *
										 glm::lookAt(CameraPosition, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		const Metrics Result =
			MeasurePointShadowImage(Off, On, ViewProjection, CameraPosition, { 2.0f, 3.0f, 0.0f }, 0.0f, glm::vec3(0.0f));

		PF_CHECK(Tests, Result.ExpectedPixelCount > 0);
		PF_CHECK(Tests, Result.ControlPixelCount > 0);
		PF_CHECK(Tests, Result.ChangedPixelCount == Result.ExpectedPixelCount + Result.ControlPixelCount);
		PF_CHECK(Tests, Result.ChangedExpectedPixelCount == Result.ExpectedPixelCount);
		PF_CHECK(Tests, Result.ChangedOtherPixelCount == Result.ControlPixelCount);
		PF_CHECK(Tests, Result.ExpectedOff > Result.ExpectedOn);
		PF_CHECK(Tests, Result.ControlOff > Result.ControlOn);
	}
} // namespace

void RunPointShadowImageAnalysisTests(TestRunner& Tests)
{
	TestPointShadowImageOracle(Tests);
	TestPointShadowDepthProbe(Tests);
	TestPointShadowImageMeasurement(Tests);
}
