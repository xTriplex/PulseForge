#include "GpuValidation/PointShadowImageAnalysis.h"

#include "Renderer/PointLightShadowMath.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace PulseForge::GpuValidation::PointShadowImageAnalysis
{
	bool SegmentIntersectsShadowCaster(const glm::vec3& Start, const glm::vec3& End, const glm::vec3& CasterOffset) noexcept
	{
		// Independent geometric oracle for a unit cube centered at y=1; bounds are not derived from shadow code.
		const glm::vec3 Minimum = glm::vec3(-0.5f, 0.5f, -0.5f) + CasterOffset;
		const glm::vec3 Maximum = glm::vec3(0.5f, 1.5f, 0.5f) + CasterOffset;
		const glm::vec3 Direction = End - Start;
		float Enter = 0.0f;
		float Exit = 1.0f;
		for (int Axis = 0; Axis < 3; ++Axis)
		{
			if (std::abs(Direction[Axis]) < 1.0e-6f)
			{
				if (Start[Axis] < Minimum[Axis] || Start[Axis] > Maximum[Axis]) return false;
				continue;
			}
			float First = (Minimum[Axis] - Start[Axis]) / Direction[Axis];
			float Second = (Maximum[Axis] - Start[Axis]) / Direction[Axis];
			if (First > Second) std::swap(First, Second);
			Enter = (std::max)(Enter, First);
			Exit = (std::min)(Exit, Second);
			if (Enter > Exit) return false;
		}
		return Enter < 1.0f && Exit > 0.0f;
	}

	float PixelLuminance(const ImageView& Image, size_t PixelIndex)
	{
		const size_t Offset = PixelIndex * 4;
		const auto* Pixel = reinterpret_cast<const uint8_t*>(Image.Pixels.data() + Offset);
		return static_cast<float>((0.2126 * Pixel[0] + 0.7152 * Pixel[1] + 0.0722 * Pixel[2]) / 255.0);
	}

	DepthProbe ProbePointShadowDepth(
		const std::array<std::optional<ImageView>, 6>& Faces, const glm::vec3& LightPosition, const glm::vec3& Receiver, float Range)
	{
		const glm::vec3 Direction = Receiver - LightPosition;
		const glm::vec3 Absolute = glm::abs(Direction);
		const uint32_t SemanticFace = Absolute.x >= Absolute.y && Absolute.x >= Absolute.z ? (Direction.x >= 0.0f ? 0u : 1u)
									  : Absolute.y >= Absolute.z						   ? (Direction.y >= 0.0f ? 2u : 3u)
																						   : (Direction.z >= 0.0f ? 4u : 5u);
		const uint32_t Face = SemanticFace;
		const auto Projections = BuildPointShadowProjections(LightPosition, Range);
		if (!Projections || !Faces[Face]) throw std::runtime_error("Could not probe the selected point-shadow cubemap face");

		const glm::vec4 Clip = Projections->Faces[SemanticFace].ViewProjection * glm::vec4(Receiver, 1.0f);
		const glm::vec2 Ndc = glm::vec2(Clip) / Clip.w;
		const uint32_t Width = Faces[Face]->Width;
		const uint32_t Height = Faces[Face]->Height;
		const int CenterX = static_cast<int>((Ndc.x * 0.5f + 0.5f) * Width);
		const int CenterY = static_cast<int>((0.5f - Ndc.y * 0.5f) * Height);
		const auto MinimumNeighborhood = [&](int PixelY)
		{
			float Minimum = 1.0f;
			for (int OffsetY = -1; OffsetY <= 1; ++OffsetY)
				for (int OffsetX = -1; OffsetX <= 1; ++OffsetX)
				{
					const int X = CenterX + OffsetX;
					const int Y = PixelY + OffsetY;
					if (X < 0 || Y < 0 || X >= static_cast<int>(Width) || Y >= static_cast<int>(Height)) continue;
					float Depth = 1.0f;
					const size_t ByteOffset = (static_cast<size_t>(Y) * Width + static_cast<size_t>(X)) * sizeof(float);
					std::memcpy(&Depth, Faces[Face]->Pixels.data() + ByteOffset, sizeof(float));
					Minimum = (std::min)(Minimum, Depth);
				}
			return Minimum;
		};
		return { Face, static_cast<uint32_t>((std::clamp)(CenterX, 0, static_cast<int>(Width) - 1)),
			static_cast<uint32_t>((std::clamp)(CenterY, 0, static_cast<int>(Height) - 1)), glm::length(Direction) / Range,
			MinimumNeighborhood(CenterY), MinimumNeighborhood(static_cast<int>(Height) - CenterY) };
	}

	Metrics MeasurePointShadowImage(const ImageView& Off, const ImageView& On, const glm::mat4& ViewProjection,
		const glm::vec3& CameraPosition, const glm::vec3& LightPosition, float ReceiverPlaneY, const glm::vec3& CasterOffset)
	{
		Metrics Result;
		const glm::mat4 InverseViewProjection = glm::inverse(ViewProjection);
		for (uint32_t Y = 0; Y < Off.Height; ++Y)
		{
			for (uint32_t X = 0; X < Off.Width; ++X)
			{
				const float NdcX = (static_cast<float>(X) + 0.5f) * 2.0f / static_cast<float>(Off.Width) - 1.0f;
				// NVRHI's negative-height Vulkan viewport makes framebuffer row zero +NDC Y.
				const float NdcY = 1.0f - (static_cast<float>(Y) + 0.5f) * 2.0f / static_cast<float>(Off.Height);
				const glm::vec4 NearH = InverseViewProjection * glm::vec4(NdcX, NdcY, 0.0f, 1.0f);
				const glm::vec4 FarH = InverseViewProjection * glm::vec4(NdcX, NdcY, 1.0f, 1.0f);
				if (std::abs(NearH.w) < 1.0e-6f || std::abs(FarH.w) < 1.0e-6f) continue;
				const glm::vec3 FarPoint = glm::vec3(FarH) / FarH.w;
				const glm::vec3 Ray = glm::normalize(FarPoint - CameraPosition);
				if (std::abs(Ray.y) < 1.0e-6f) continue;
				const float PlaneDistance = (ReceiverPlaneY - CameraPosition.y) / Ray.y;
				if (PlaneDistance <= 0.0f) continue;
				const glm::vec3 Receiver = CameraPosition + Ray * PlaneDistance;
				if (std::abs(Receiver.x) > 4.0f || std::abs(Receiver.z) > 4.0f) continue;
				if (SegmentIntersectsShadowCaster(CameraPosition, Receiver, CasterOffset)) continue;
				if (glm::distance(LightPosition, Receiver) >= 10.0f) continue;

				const size_t PixelIndex = static_cast<size_t>(Y) * Off.Width + X;
				const bool GeometricallyOccluded = SegmentIntersectsShadowCaster(LightPosition, Receiver, CasterOffset);
				const double PixelDelta = static_cast<double>(PixelLuminance(Off, PixelIndex)) - PixelLuminance(On, PixelIndex);
				if (PixelDelta > 0.01)
				{
					++Result.ChangedPixelCount;
					Result.ChangedMean += PixelDelta;
					Result.ChangedWorldX += Receiver.x;
					Result.ChangedWorldZ += Receiver.z;
					if (GeometricallyOccluded) ++Result.ChangedExpectedPixelCount;
					else ++Result.ChangedOtherPixelCount;
				}
				if (GeometricallyOccluded)
				{
					Result.ExpectedOff += PixelLuminance(Off, PixelIndex);
					Result.ExpectedOn += PixelLuminance(On, PixelIndex);
					++Result.ExpectedPixelCount;
				}
				else
				{
					Result.ControlOff += PixelLuminance(Off, PixelIndex);
					Result.ControlOn += PixelLuminance(On, PixelIndex);
					++Result.ControlPixelCount;
				}
			}
		}
		if (Result.ExpectedPixelCount > 0)
		{
			Result.ExpectedOff /= Result.ExpectedPixelCount;
			Result.ExpectedOn /= Result.ExpectedPixelCount;
		}
		if (Result.ControlPixelCount > 0)
		{
			Result.ControlOff /= Result.ControlPixelCount;
			Result.ControlOn /= Result.ControlPixelCount;
		}
		if (Result.ChangedPixelCount > 0)
		{
			const double Count = static_cast<double>(Result.ChangedPixelCount);
			Result.ChangedMean /= Count;
			Result.ChangedWorldX /= Count;
			Result.ChangedWorldZ /= Count;
		}
		return Result;
	}
} // namespace PulseForge::GpuValidation::PointShadowImageAnalysis
