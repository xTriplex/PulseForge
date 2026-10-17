#include "TestSupport.h"
#include "GpuValidation/TransparencyImageAnalysis.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

void RunTransparencyImageAnalysisTests(TestRunner& Tests)
{
	using namespace PulseForge::GpuValidation::TransparencyImageAnalysis;
	std::array<std::byte, 16> Before{};
	std::array<std::byte, 16> After{};
	std::array<std::byte, 16> AlphaOnlyPixels{};
	After[0] = std::byte{ 20 };
	After[1] = std::byte{ 10 };
	After[2] = std::byte{ 5 };
	After[3] = std::byte{ 255 };
	After[15] = std::byte{ 255 };
	AlphaOnlyPixels[3] = std::byte{ 255 };
	const ImageView BeforeView{ 2, 2, Before };
	const ImageView AfterView{ 2, 2, After };

	const auto Difference = CompareRgba8(BeforeView, AfterView, Region{ 0, 0, 1, 1 }, true);
	PF_CHECK(Tests, Difference.has_value());
	if (!Difference) return;
	PF_CHECK(Tests, Difference->ChangedPixels == 1);
	PF_CHECK(Tests, Difference->RegionChangedPixels == 1);
	PF_CHECK(Tests, Difference->MinX == 0 && Difference->MinY == 0 && Difference->MaxX == 0 && Difference->MaxY == 0);
	PF_CHECK(Tests, std::abs(Difference->MeanAbsoluteRgb - (35.0 / 12.0)) < 1.0e-9);
	PF_CHECK(Tests, std::abs(Difference->RegionMeanAbsoluteRgb - (35.0 / 3.0)) < 1.0e-9);
	PF_CHECK(Tests, Difference->DifferenceRgba.size() == 16);
	PF_CHECK(Tests, Difference->DifferenceRgba[0] == 100 && Difference->DifferenceRgba[1] == 50 && Difference->DifferenceRgba[2] == 25 &&
						Difference->DifferenceRgba[3] == 255);

	const auto AlphaOnly = CompareRgba8(BeforeView, ImageView{ 2, 2, AlphaOnlyPixels });
	PF_CHECK(Tests, AlphaOnly && AlphaOnly->ChangedPixels == 0 && AlphaOnly->MeanAbsoluteRgb == 0.0);
	const auto InvalidExtent = CompareRgba8(BeforeView, ImageView{ 1, 2, After });
	PF_CHECK(Tests, !InvalidExtent);
	const auto InvalidBuffer = CompareRgba8(BeforeView, ImageView{ 2, 2, std::span<const std::byte>(After).first(12) });
	PF_CHECK(Tests, !InvalidBuffer);
	const auto InvalidRegion = CompareRgba8(BeforeView, AfterView, Region{ 1, 1, 3, 2 });
	PF_CHECK(Tests, !InvalidRegion);
}
