#include "Core/PulseForgePCH.h"
#include "Renderer/EnvironmentLightingCache.h"

#include "Assets/ImageAssetImporter.h"
#include "Core/Application.h"

#include <array>
#include <exception>
#include <span>

namespace PulseForge
{
	namespace
	{
		EnvironmentLightingCacheError MakeError(const AssetID& Asset, std::string Message)
		{
			return { Asset, std::move(Message) };
		}

		std::expected<TextureHandle, TextureError> CreateCubeTexture(
			Application& Runtime,
			const FloatCubeLevel& Level,
			uint32_t MipLevels,
			std::span<const FloatCubeLevel> Mips,
			std::string DebugName)
		{
			TextureDesc Description;
			Description.Width = Mips.empty() ? Level.Size : Mips.front().Size;
			Description.Height = Description.Width;
			Description.Format = TextureFormat::RGBA32_Float;
			Description.Dimension = TextureDimension::TextureCube;
			Description.MipLevels = MipLevels;
			Description.DebugName = std::move(DebugName);

			std::vector<TextureSubresourceData> Subresources;
			Subresources.reserve(static_cast<size_t>(MipLevels) * 6);
			for (uint32_t Mip = 0; Mip < MipLevels; ++Mip)
			{
				const FloatCubeLevel& Source = Mips.empty() ? Level : Mips[Mip];
				for (uint32_t Face = 0; Face < 6; ++Face)
				{
					const std::span<const float> Pixels(Source.Faces[Face]);
					Subresources.push_back({ Mip, Face, std::as_bytes(Pixels), static_cast<size_t>(Source.Size) * 4 * sizeof(float) });
				}
			}
			return Runtime.CreateTexture(Description, Subresources);
		}
	}

	EnvironmentLightingCache::EnvironmentLightingCache(
		Application& Runtime,
		std::filesystem::path ProjectRoot,
		const AssetRegistry& Registry)
		: m_Runtime(Runtime), m_ProjectRoot(std::move(ProjectRoot)), m_Registry(Registry)
	{
	}

	std::expected<std::reference_wrapper<const EnvironmentLightingTextures>, EnvironmentLightingCacheError>
	EnvironmentLightingCache::GetOrLoad(const AssetID& Asset)
	{
		if (const auto Existing = m_Textures.find(Asset); Existing != m_Textures.end())
			return std::cref(Existing->second);

		const auto Imported = ImageAssetImporter::ImportRGBA32F(Asset, m_ProjectRoot, m_Registry);
		if (!Imported)
			return std::unexpected(MakeError(Asset, "HDR image import failed: " + Imported.error().Message));

		EnvironmentProcessingDesc Processing;
		Processing.EnvironmentFaceSize = 256;
		Processing.IrradianceFaceSize = 32;
		Processing.SpecularFaceSize = 128;
		Processing.BrdfLutSize = 256;
		auto Processed = ProcessEnvironmentImage(*Imported, Processing);
		if (!Processed)
			return std::unexpected(MakeError(Asset, "Environment preprocessing failed: " + Processed.error().Message));

		try
		{
			EnvironmentLightingTextures Textures;
			auto Environment = CreateCubeTexture(m_Runtime, Processed->Environment, 1, {}, "PulseForge environment cubemap");
			if (!Environment)
				return std::unexpected(MakeError(Asset, "Environment cubemap creation failed: " + Environment.error().Message));
			Textures.Environment = std::move(*Environment);

			auto Irradiance = CreateCubeTexture(m_Runtime, Processed->DiffuseIrradiance, 1, {}, "PulseForge diffuse irradiance cubemap");
			if (!Irradiance)
				return std::unexpected(MakeError(Asset, "Irradiance cubemap creation failed: " + Irradiance.error().Message));
			Textures.DiffuseIrradiance = std::move(*Irradiance);

			auto Specular = CreateCubeTexture(m_Runtime, Processed->PrefilteredSpecular.front(),
				static_cast<uint32_t>(Processed->PrefilteredSpecular.size()), Processed->PrefilteredSpecular,
				"PulseForge prefiltered specular cubemap");
			if (!Specular)
				return std::unexpected(MakeError(Asset, "Specular cubemap creation failed: " + Specular.error().Message));
			Textures.PrefilteredSpecular = std::move(*Specular);

			TextureDesc BrdfDescription;
			BrdfDescription.Width = Processed->BrdfIntegrationLut.Width;
			BrdfDescription.Height = Processed->BrdfIntegrationLut.Height;
			BrdfDescription.Format = TextureFormat::RGBA32_Float;
			BrdfDescription.DebugName = "PulseForge split-sum BRDF integration LUT";
			const std::span<const float> BrdfPixels(Processed->BrdfIntegrationLut.RGBA32FPixels);
			auto Brdf = m_Runtime.CreateTexture(BrdfDescription, std::as_bytes(BrdfPixels));
			if (!Brdf)
				return std::unexpected(MakeError(Asset, "BRDF integration LUT creation failed: " + Brdf.error().Message));
			Textures.BrdfIntegrationLut = std::move(*Brdf);

			auto [It, Inserted] = m_Textures.emplace(Asset, std::move(Textures));
			(void)Inserted;
			return std::cref(It->second);
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(Asset, std::string("Environment resource allocation failed: ") + Exception.what()));
		}
	}
}
