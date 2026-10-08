#include "Core/PulseForgePCH.h"
#include "Renderer/EnvironmentLighting.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#include <glm/geometric.hpp>

namespace PulseForge
{
	namespace
	{
		constexpr uint64_t MaxGeneratedBytes = 64ull * 1024ull * 1024ull;
		constexpr uint32_t MaxFaceSize = 1024;
		constexpr uint32_t MaxLutSize = 512;
		constexpr uint32_t MaxSamples = 1024;
		constexpr float Pi = std::numbers::pi_v<float>;

		EnvironmentProcessingError MakeError(EnvironmentProcessingErrorCode Code, std::string Message)
		{
			return { Code, std::move(Message) };
		}

		float RadicalInverse(uint32_t Bits) noexcept
		{
			Bits = (Bits << 16u) | (Bits >> 16u);
			Bits = ((Bits & 0x55555555u) << 1u) | ((Bits & 0xAAAAAAAAu) >> 1u);
			Bits = ((Bits & 0x33333333u) << 2u) | ((Bits & 0xCCCCCCCCu) >> 2u);
			Bits = ((Bits & 0x0F0F0F0Fu) << 4u) | ((Bits & 0xF0F0F0F0u) >> 4u);
			Bits = ((Bits & 0x00FF00FFu) << 8u) | ((Bits & 0xFF00FF00u) >> 8u);
			return static_cast<float>(Bits) * 2.3283064365386963e-10f;
		}

		glm::vec2 Hammersley(uint32_t Index, uint32_t Count) noexcept
		{
			return { static_cast<float>(Index) / static_cast<float>(Count), RadicalInverse(Index) };
		}

		glm::vec3 SafeNormalize(const glm::vec3& Value, const glm::vec3& Fallback = { 0.0f, 1.0f, 0.0f }) noexcept
		{
			const float LengthSquared = glm::dot(Value, Value);
			return LengthSquared > 1.0e-12f ? Value * glm::inversesqrt(LengthSquared) : Fallback;
		}

		glm::vec3 TangentToWorld(const glm::vec3& Normal, const glm::vec3& Local) noexcept
		{
			const glm::vec3 Up = std::abs(Normal.y) < 0.999f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
			const glm::vec3 Tangent = SafeNormalize(glm::cross(Up, Normal), { 1.0f, 0.0f, 0.0f });
			const glm::vec3 Bitangent = glm::cross(Normal, Tangent);
			return SafeNormalize(Tangent * Local.x + Bitangent * Local.y + Normal * Local.z, Normal);
		}

		glm::vec3 SampleEquirectangular(const ImportedFloatImage& Image, const glm::vec3& Direction) noexcept
		{
			const glm::vec2 Uv = EquirectangularUvFromDirection(Direction);
			const float X = Uv.x * static_cast<float>(Image.Width) - 0.5f;
			const float Y = Uv.y * static_cast<float>(Image.Height) - 0.5f;
			const int X0 = static_cast<int>(std::floor(X));
			const int Y0 = static_cast<int>(std::floor(Y));
			const float Tx = X - static_cast<float>(X0);
			const float Ty = Y - static_cast<float>(Y0);
			const auto Texel = [&Image](int TexelX, int TexelY)
			{
				const int Width = static_cast<int>(Image.Width);
				const int Height = static_cast<int>(Image.Height);
				TexelX = ((TexelX % Width) + Width) % Width;
				TexelY = std::clamp(TexelY, 0, Height - 1);
				const size_t Offset = (static_cast<size_t>(TexelY) * Image.Width + static_cast<uint32_t>(TexelX)) * 4;
				return glm::vec3(Image.RGBA32FPixels[Offset], Image.RGBA32FPixels[Offset + 1], Image.RGBA32FPixels[Offset + 2]);
			};
			return glm::mix(
				glm::mix(Texel(X0, Y0), Texel(X0 + 1, Y0), Tx),
				glm::mix(Texel(X0, Y0 + 1), Texel(X0 + 1, Y0 + 1), Tx),
				Ty);
		}

		struct FaceCoordinate
		{
			CubeFace Face;
			float U;
			float V;
		};

		FaceCoordinate DirectionToCubeFace(const glm::vec3& Input) noexcept
		{
			const glm::vec3 Direction = SafeNormalize(Input);
			const glm::vec3 Absolute = glm::abs(Direction);
			float Sc = 0.0f;
			float Tc = 0.0f;
			float Major = 1.0f;
			CubeFace Face = CubeFace::PositiveX;
			if (Absolute.x >= Absolute.y && Absolute.x >= Absolute.z)
			{
				Major = Absolute.x;
				if (Direction.x >= 0.0f)
				{
					Face = CubeFace::PositiveX;
					Sc = -Direction.z;
					Tc = -Direction.y;
				}
				else
				{
					Face = CubeFace::NegativeX;
					Sc = Direction.z;
					Tc = -Direction.y;
				}
			}
			else if (Absolute.y >= Absolute.z)
			{
				Major = Absolute.y;
				if (Direction.y >= 0.0f)
				{
					Face = CubeFace::PositiveY;
					Sc = Direction.x;
					Tc = Direction.z;
				}
				else
				{
					Face = CubeFace::NegativeY;
					Sc = Direction.x;
					Tc = -Direction.z;
				}
			}
			else
			{
				Major = Absolute.z;
				if (Direction.z >= 0.0f)
				{
					Face = CubeFace::PositiveZ;
					Sc = Direction.x;
					Tc = -Direction.y;
				}
				else
				{
					Face = CubeFace::NegativeZ;
					Sc = -Direction.x;
					Tc = -Direction.y;
				}
			}
			return { Face, (Sc / Major + 1.0f) * 0.5f, (Tc / Major + 1.0f) * 0.5f };
		}

		glm::vec3 SampleCube(const FloatCubeLevel& Cube, const glm::vec3& Direction) noexcept
		{
			const FaceCoordinate Coordinate = DirectionToCubeFace(Direction);
			const uint32_t FaceIndex = static_cast<uint32_t>(Coordinate.Face);
			const float X = Coordinate.U * static_cast<float>(Cube.Size) - 0.5f;
			const float Y = Coordinate.V * static_cast<float>(Cube.Size) - 0.5f;
			const int X0 = static_cast<int>(std::floor(X));
			const int Y0 = static_cast<int>(std::floor(Y));
			const float Tx = X - static_cast<float>(X0);
			const float Ty = Y - static_cast<float>(Y0);
			const auto Texel = [&Cube, FaceIndex](int TexelX, int TexelY)
			{
				const uint32_t XIndex = static_cast<uint32_t>(std::clamp(TexelX, 0, static_cast<int>(Cube.Size) - 1));
				const uint32_t YIndex = static_cast<uint32_t>(std::clamp(TexelY, 0, static_cast<int>(Cube.Size) - 1));
				const size_t Offset = (static_cast<size_t>(YIndex) * Cube.Size + XIndex) * 4;
				const std::vector<float>& Face = Cube.Faces[FaceIndex];
				return glm::vec3(Face[Offset], Face[Offset + 1], Face[Offset + 2]);
			};
			return glm::mix(
				glm::mix(Texel(X0, Y0), Texel(X0 + 1, Y0), Tx),
				glm::mix(Texel(X0, Y0 + 1), Texel(X0 + 1, Y0 + 1), Tx),
				Ty);
		}

		void SetCubeTexel(FloatCubeLevel& Cube, uint32_t Face, uint32_t X, uint32_t Y, const glm::vec3& Color)
		{
			const size_t Offset = (static_cast<size_t>(Y) * Cube.Size + X) * 4;
			Cube.Faces[Face][Offset] = Color.r;
			Cube.Faces[Face][Offset + 1] = Color.g;
			Cube.Faces[Face][Offset + 2] = Color.b;
			Cube.Faces[Face][Offset + 3] = 1.0f;
		}

		glm::vec3 ImportanceSampleGgx(const glm::vec2& Xi, float Roughness, const glm::vec3& Normal) noexcept
		{
			const float Alpha = Roughness * Roughness;
			const float Phi = 2.0f * Pi * Xi.x;
			const float CosTheta = std::sqrt((1.0f - Xi.y) / std::max(1.0f + (Alpha * Alpha - 1.0f) * Xi.y, 1.0e-7f));
			const float SinTheta = std::sqrt(std::max(0.0f, 1.0f - CosTheta * CosTheta));
			const glm::vec3 HalfVector(std::cos(Phi) * SinTheta, std::sin(Phi) * SinTheta, CosTheta);
			return TangentToWorld(Normal, HalfVector);
		}

		uint32_t MipCountFor(uint32_t Size) noexcept
		{
			uint32_t Count = 1;
			while (Size > 1)
			{
				Size >>= 1;
				++Count;
			}
			return Count;
		}

		uint64_t GeneratedPixelCount(const EnvironmentProcessingDesc& Description)
		{
			uint64_t Pixels = static_cast<uint64_t>(Description.EnvironmentFaceSize) * Description.EnvironmentFaceSize * 6;
			Pixels += static_cast<uint64_t>(Description.IrradianceFaceSize) * Description.IrradianceFaceSize * 6;
			uint32_t MipSize = Description.SpecularFaceSize;
			for (uint32_t Mip = 0; Mip < MipCountFor(MipSize); ++Mip)
			{
				Pixels += static_cast<uint64_t>(MipSize) * MipSize * 6;
				MipSize = std::max(1u, MipSize / 2);
			}
			Pixels += static_cast<uint64_t>(Description.BrdfLutSize) * Description.BrdfLutSize;
			return Pixels;
		}

		bool IsPowerOfTwo(uint32_t Value) noexcept
		{
			return Value != 0 && (Value & (Value - 1)) == 0;
		}
	}

	glm::vec2 EquirectangularUvFromDirection(const glm::vec3& Input) noexcept
	{
		const glm::vec3 Direction = SafeNormalize(Input);
		float U = std::atan2(Direction.z, Direction.x) / (2.0f * Pi) + 0.5f;
		U -= std::floor(U);
		const float V = std::acos(std::clamp(Direction.y, -1.0f, 1.0f)) / Pi;
		return { U, V };
	}

	glm::vec3 CubeFaceTexelDirection(CubeFace Face, float U, float V) noexcept
	{
		const float Sc = U * 2.0f - 1.0f;
		const float Tc = V * 2.0f - 1.0f;
		switch (Face)
		{
			case CubeFace::PositiveX: return SafeNormalize({ 1.0f, -Tc, -Sc });
			case CubeFace::NegativeX: return SafeNormalize({ -1.0f, -Tc, Sc });
			case CubeFace::PositiveY: return SafeNormalize({ Sc, 1.0f, Tc });
			case CubeFace::NegativeY: return SafeNormalize({ Sc, -1.0f, -Tc });
			case CubeFace::PositiveZ: return SafeNormalize({ Sc, -Tc, 1.0f });
			case CubeFace::NegativeZ: return SafeNormalize({ -Sc, -Tc, -1.0f });
		}
		return { 0.0f, 1.0f, 0.0f };
	}

	std::expected<EnvironmentLightingData, EnvironmentProcessingError> ProcessEnvironmentImage(
		const ImportedFloatImage& Source,
		const EnvironmentProcessingDesc& Description)
	{
		if (Source.Width == 0 || Source.Height == 0 ||
			static_cast<uint64_t>(Source.Width) * Source.Height > 8ull * 1024ull * 1024ull ||
			Source.RGBA32FPixels.size() != static_cast<size_t>(Source.Width) * Source.Height * 4)
		{
			return std::unexpected(MakeError(EnvironmentProcessingErrorCode::InvalidSource,
				"Environment source dimensions or RGBA32F storage are invalid"));
		}
		if (!IsPowerOfTwo(Description.EnvironmentFaceSize) || Description.EnvironmentFaceSize > MaxFaceSize ||
			!IsPowerOfTwo(Description.IrradianceFaceSize) || Description.IrradianceFaceSize > MaxFaceSize ||
			!IsPowerOfTwo(Description.SpecularFaceSize) || Description.SpecularFaceSize > MaxFaceSize ||
			!IsPowerOfTwo(Description.BrdfLutSize) || Description.BrdfLutSize > MaxLutSize ||
			Description.IrradianceSamples == 0 || Description.IrradianceSamples > MaxSamples ||
			Description.PrefilterSamples == 0 || Description.PrefilterSamples > MaxSamples ||
			Description.BrdfSamples == 0 || Description.BrdfSamples > MaxSamples)
		{
			return std::unexpected(MakeError(EnvironmentProcessingErrorCode::InvalidDescription,
				"Environment output dimensions must be supported powers of two and sample counts must be within 1..1024"));
		}
		if (GeneratedPixelCount(Description) > MaxGeneratedBytes / (sizeof(float) * 4))
		{
			return std::unexpected(MakeError(EnvironmentProcessingErrorCode::ResourceLimitExceeded,
				"Generated environment lighting data would exceed the 64 MiB CPU-output limit"));
		}
		for (float Channel : Source.RGBA32FPixels)
		{
			if (!std::isfinite(Channel))
				return std::unexpected(MakeError(EnvironmentProcessingErrorCode::InvalidSource,
					"Environment source contains a non-finite channel"));
		}

		try
		{
			EnvironmentLightingData Output;
			Output.Environment.Size = Description.EnvironmentFaceSize;
			for (std::vector<float>& Face : Output.Environment.Faces)
				Face.resize(static_cast<size_t>(Description.EnvironmentFaceSize) * Description.EnvironmentFaceSize * 4);
			for (uint32_t Face = 0; Face < 6; ++Face)
			{
				for (uint32_t Y = 0; Y < Description.EnvironmentFaceSize; ++Y)
				{
					for (uint32_t X = 0; X < Description.EnvironmentFaceSize; ++X)
					{
						const float U = (static_cast<float>(X) + 0.5f) / static_cast<float>(Description.EnvironmentFaceSize);
						const float V = (static_cast<float>(Y) + 0.5f) / static_cast<float>(Description.EnvironmentFaceSize);
						SetCubeTexel(Output.Environment, Face, X, Y,
							SampleEquirectangular(Source, CubeFaceTexelDirection(static_cast<CubeFace>(Face), U, V)));
					}
				}
			}

			Output.DiffuseIrradiance.Size = Description.IrradianceFaceSize;
			for (std::vector<float>& Face : Output.DiffuseIrradiance.Faces)
				Face.resize(static_cast<size_t>(Description.IrradianceFaceSize) * Description.IrradianceFaceSize * 4);
			for (uint32_t Face = 0; Face < 6; ++Face)
			{
				for (uint32_t Y = 0; Y < Description.IrradianceFaceSize; ++Y)
				{
					for (uint32_t X = 0; X < Description.IrradianceFaceSize; ++X)
					{
						const glm::vec3 Normal = CubeFaceTexelDirection(
							static_cast<CubeFace>(Face),
							(static_cast<float>(X) + 0.5f) / Description.IrradianceFaceSize,
							(static_cast<float>(Y) + 0.5f) / Description.IrradianceFaceSize);
						glm::vec3 Irradiance(0.0f);
						for (uint32_t Sample = 0; Sample < Description.IrradianceSamples; ++Sample)
						{
							const glm::vec2 Xi = Hammersley(Sample, Description.IrradianceSamples);
							const float Phi = 2.0f * Pi * Xi.y;
							const float Radius = std::sqrt(Xi.x);
							const glm::vec3 Local(Radius * std::cos(Phi), Radius * std::sin(Phi), std::sqrt(1.0f - Xi.x));
							Irradiance += SampleCube(Output.Environment, TangentToWorld(Normal, Local));
						}
						Irradiance *= Pi / static_cast<float>(Description.IrradianceSamples);
						SetCubeTexel(Output.DiffuseIrradiance, Face, X, Y, Irradiance);
					}
				}
			}

			const uint32_t SpecularMipCount = MipCountFor(Description.SpecularFaceSize);
			Output.PrefilteredSpecular.reserve(SpecularMipCount);
			uint32_t MipSize = Description.SpecularFaceSize;
			for (uint32_t Mip = 0; Mip < SpecularMipCount; ++Mip)
			{
				FloatCubeLevel& Level = Output.PrefilteredSpecular.emplace_back();
				Level.Size = MipSize;
				for (std::vector<float>& Face : Level.Faces)
					Face.resize(static_cast<size_t>(MipSize) * MipSize * 4);
				const float Roughness = SpecularMipCount == 1 ? 0.0f : static_cast<float>(Mip) / (SpecularMipCount - 1);
				for (uint32_t Face = 0; Face < 6; ++Face)
				{
					for (uint32_t Y = 0; Y < MipSize; ++Y)
					{
						for (uint32_t X = 0; X < MipSize; ++X)
						{
							const glm::vec3 Normal = CubeFaceTexelDirection(
								static_cast<CubeFace>(Face),
								(static_cast<float>(X) + 0.5f) / MipSize,
								(static_cast<float>(Y) + 0.5f) / MipSize);
							glm::vec3 Prefiltered(0.0f);
							float TotalWeight = 0.0f;
							if (Roughness <= 1.0e-4f)
							{
								Prefiltered = SampleCube(Output.Environment, Normal);
								TotalWeight = 1.0f;
							}
							else
							{
								for (uint32_t Sample = 0; Sample < Description.PrefilterSamples; ++Sample)
								{
									const glm::vec3 HalfVector = ImportanceSampleGgx(
										Hammersley(Sample, Description.PrefilterSamples), Roughness, Normal);
									const glm::vec3 Light = SafeNormalize(2.0f * glm::dot(Normal, HalfVector) * HalfVector - Normal, Normal);
									const float NdotL = std::max(glm::dot(Normal, Light), 0.0f);
									if (NdotL > 0.0f)
									{
										Prefiltered += SampleCube(Output.Environment, Light) * NdotL;
										TotalWeight += NdotL;
									}
								}
							}
							if (TotalWeight > 1.0e-6f)
								Prefiltered /= TotalWeight;
							SetCubeTexel(Level, Face, X, Y, Prefiltered);
						}
					}
				}
				MipSize = std::max(1u, MipSize / 2);
			}

			Output.BrdfIntegrationLut.Width = Description.BrdfLutSize;
			Output.BrdfIntegrationLut.Height = Description.BrdfLutSize;
			Output.BrdfIntegrationLut.RGBA32FPixels.resize(
				static_cast<size_t>(Description.BrdfLutSize) * Description.BrdfLutSize * 4);
			for (uint32_t Y = 0; Y < Description.BrdfLutSize; ++Y)
			{
				for (uint32_t X = 0; X < Description.BrdfLutSize; ++X)
				{
					const float NdotV = std::max((static_cast<float>(X) + 0.5f) / Description.BrdfLutSize, 1.0e-4f);
					const float Roughness = (static_cast<float>(Y) + 0.5f) / Description.BrdfLutSize;
					const glm::vec3 View(std::sqrt(std::max(0.0f, 1.0f - NdotV * NdotV)), 0.0f, NdotV);
					float A = 0.0f;
					float B = 0.0f;
					for (uint32_t Sample = 0; Sample < Description.BrdfSamples; ++Sample)
					{
						const glm::vec3 HalfVector = ImportanceSampleGgx(Hammersley(Sample, Description.BrdfSamples), Roughness, { 0.0f, 0.0f, 1.0f });
						const glm::vec3 Light = SafeNormalize(2.0f * glm::dot(View, HalfVector) * HalfVector - View,
							{ 0.0f, 0.0f, 1.0f });
						const float NdotL = std::max(Light.z, 0.0f);
						const float NdotH = std::max(HalfVector.z, 0.0f);
						const float VdotH = std::max(glm::dot(View, HalfVector), 0.0f);
						if (NdotL <= 0.0f || NdotH <= 0.0f)
							continue;
						const float K = Roughness * Roughness * 0.5f;
						const float GeometryV = NdotV / std::max(NdotV * (1.0f - K) + K, 1.0e-6f);
						const float GeometryL = NdotL / std::max(NdotL * (1.0f - K) + K, 1.0e-6f);
						const float Visibility = GeometryV * GeometryL * VdotH /
							std::max(NdotH * NdotV, 1.0e-6f);
						const float Fc = std::pow(1.0f - VdotH, 5.0f);
						A += (1.0f - Fc) * Visibility;
						B += Fc * Visibility;
					}
					const float Scale = 1.0f / static_cast<float>(Description.BrdfSamples);
					const size_t Offset = (static_cast<size_t>(Y) * Description.BrdfLutSize + X) * 4;
					Output.BrdfIntegrationLut.RGBA32FPixels[Offset] = A * Scale;
					Output.BrdfIntegrationLut.RGBA32FPixels[Offset + 1] = B * Scale;
					Output.BrdfIntegrationLut.RGBA32FPixels[Offset + 2] = 0.0f;
					Output.BrdfIntegrationLut.RGBA32FPixels[Offset + 3] = 1.0f;
				}
			}
			return Output;
		}
		catch (const std::bad_alloc&)
		{
			return std::unexpected(MakeError(EnvironmentProcessingErrorCode::ResourceLimitExceeded,
				"Environment preprocessing could not allocate the bounded generated images"));
		}
	}
}
