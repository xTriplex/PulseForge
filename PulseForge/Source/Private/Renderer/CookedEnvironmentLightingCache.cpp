#include "Renderer/EnvironmentLightingCache.h"

#include "Core/Diagnostics.h"
#include "Runtime/CookedContentProvider.h"

#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace PulseForge
{
	namespace
	{
		EnvironmentLightingCacheError MakeCookedEnvironmentError(const AssetID& Asset, std::string Message)
		{
			return { Asset, std::move(Message) };
		}

		std::expected<TextureHandle, TextureError> CreateCookedCube(
			RenderDevice& Runtime,
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
					Subresources.push_back({ Mip, Face, std::as_bytes(Pixels),
						static_cast<size_t>(Source.Size) * 4 * sizeof(float) });
				}
			}
			return Runtime.CreateTexture(Description, Subresources);
		}
	}

	EnvironmentLightingCache::EnvironmentLightingCache(
		RenderDevice& Runtime, std::shared_ptr<const CookedContentProvider> Content, DiagnosticRecorder* Diagnostics)
		: m_Runtime(Runtime), m_CookedContent(std::move(Content)), m_Diagnostics(Diagnostics)
	{
		if (!m_CookedContent)
			throw std::invalid_argument("Cooked environment cache requires a content provider");
	}

	EnvironmentLightingCache::~EnvironmentLightingCache() = default;

	std::expected<std::optional<std::reference_wrapper<const EnvironmentLightingTextures>>, EnvironmentLightingCacheError>
	EnvironmentLightingCache::GetOrLoad(const AssetID& Asset)
	{
		return GetOrLoadInternal(Asset, false);
	}

	std::expected<std::optional<std::reference_wrapper<const EnvironmentLightingTextures>>, EnvironmentLightingCacheError>
	EnvironmentLightingCache::GetOrLoadInternal(const AssetID& Asset, bool CollectReadyProduct)
	{
		(void)CollectReadyProduct;
		if (Asset.IsNil())
			return std::unexpected(MakeCookedEnvironmentError(Asset, "Cannot load environment lighting with a nil asset UUID"));

		auto [Iterator, WasInserted] = m_Entries.try_emplace(Asset, std::make_shared<Entry>());
		(void)WasInserted;
		Entry& State = *Iterator->second;
		if (State.GpuTextures)
			return std::optional<std::reference_wrapper<const EnvironmentLightingTextures>>{ std::cref(*State.GpuTextures) };
		if (State.UploadFailed)
			return std::unexpected(MakeCookedEnvironmentError(Asset, State.Error));

		auto Product = m_CookedContent->LoadEnvironmentProduct(Asset);
		if (!Product)
		{
			State.Error = Product.error().Message;
			State.UploadFailed = true;
			return std::unexpected(MakeCookedEnvironmentError(Asset, State.Error));
		}

		ScopedDiagnosticTimer Timer(
			m_Diagnostics, "Cooked environment GPU resource upload submission", "EnvironmentLightingCache");
		try
		{
			EnvironmentLightingTextures Textures;
			const EnvironmentLightingData& Data = Product->Data;
			auto Environment = CreateCookedCube(
				m_Runtime, Data.Environment, 1, {}, "PulseForge cooked environment cubemap");
			if (!Environment)
				throw std::runtime_error("Environment cubemap creation failed: " + Environment.error().Message);
			Textures.Environment = std::move(*Environment);

			auto Irradiance = CreateCookedCube(
				m_Runtime, Data.DiffuseIrradiance, 1, {}, "PulseForge cooked diffuse irradiance cubemap");
			if (!Irradiance)
				throw std::runtime_error("Irradiance cubemap creation failed: " + Irradiance.error().Message);
			Textures.DiffuseIrradiance = std::move(*Irradiance);

			auto Specular = CreateCookedCube(m_Runtime, Data.PrefilteredSpecular.front(),
				static_cast<uint32_t>(Data.PrefilteredSpecular.size()), Data.PrefilteredSpecular,
				"PulseForge cooked prefiltered specular cubemap");
			if (!Specular)
				throw std::runtime_error("Specular cubemap creation failed: " + Specular.error().Message);
			Textures.PrefilteredSpecular = std::move(*Specular);

			TextureDesc BrdfDescription;
			BrdfDescription.Width = Data.BrdfIntegrationLut.Width;
			BrdfDescription.Height = Data.BrdfIntegrationLut.Height;
			BrdfDescription.Format = TextureFormat::RGBA32_Float;
			BrdfDescription.DebugName = "PulseForge cooked split-sum BRDF integration LUT";
			const std::span<const float> BrdfPixels(Data.BrdfIntegrationLut.RGBA32FPixels);
			auto Brdf = m_Runtime.CreateTexture(BrdfDescription, std::as_bytes(BrdfPixels));
			if (!Brdf)
				throw std::runtime_error("BRDF integration LUT creation failed: " + Brdf.error().Message);
			Textures.BrdfIntegrationLut = std::move(*Brdf);

			State.GpuTextures = std::move(Textures);
			State.Error.clear();
			State.UploadFailed = false;
			++State.ResourceRevision;
			if (m_Diagnostics)
				(void)m_Diagnostics->RecordEvent(
					DiagnosticEventKind::Resource, "Cooked environment resources created", "EnvironmentLightingCache", "objects", 4);
			return std::optional<std::reference_wrapper<const EnvironmentLightingTextures>>{ std::cref(*State.GpuTextures) };
		}
		catch (const std::exception& Exception)
		{
			State.Error = Exception.what();
			State.UploadFailed = true;
			return std::unexpected(MakeCookedEnvironmentError(Asset, State.Error));
		}
	}

	bool EnvironmentLightingCache::AdvancePending()
	{
		return false;
	}

	uint64_t EnvironmentLightingCache::GetResourceRevision(const AssetID& Asset) const noexcept
	{
		const auto Found = m_Entries.find(Asset);
		return Found == m_Entries.end() ? 0 : Found->second->ResourceRevision;
	}

	bool EnvironmentLightingCache::HasPendingWork() const noexcept
	{
		return false;
	}

	void EnvironmentLightingCache::SetSessionKey(std::string SessionKey)
	{
		(void)SessionKey;
	}

	void EnvironmentLightingCache::SetRegistrySnapshot(std::shared_ptr<const AssetRegistry> Registry) noexcept
	{
		(void)Registry;
	}

	void EnvironmentLightingCache::Clear() noexcept
	{
		m_Entries.clear();
	}
}
