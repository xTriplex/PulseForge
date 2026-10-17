#include "Renderer/SceneRendererMaterialBindingCache.h"

#include "Core/ScopedProfileTimer.h"

#include <array>
#include <functional>
#include <span>
#include <string_view>

namespace PulseForge
{
	SceneRendererMaterialBindingCache::SceneRendererMaterialBindingCache(RenderDevice& Runtime) noexcept : m_Runtime(Runtime)
	{
	}

	std::expected<void, SceneRendererError> SceneRendererMaterialBindingCache::Initialize(const Resources& Resources)
	{
		if (m_Layout) return std::unexpected(MakeResourceError("Material binding cache was initialized more than once"));
		if (!Resources.Layout || !Resources.ObjectConstants || !Resources.FrameConstants || !Resources.LocalLightingConstants)
			return std::unexpected(MakeResourceError("Material binding cache initialization received incomplete scene resources"));

		SamplerDesc MaterialSamplerDescription;
		MaterialSamplerDescription.Minification = SamplerFilter::Linear;
		MaterialSamplerDescription.Magnification = SamplerFilter::Linear;
		MaterialSamplerDescription.Mip = SamplerMipFilter::Linear;
		MaterialSamplerDescription.AddressU = SamplerAddressMode::Repeat;
		MaterialSamplerDescription.AddressV = SamplerAddressMode::Repeat;
		MaterialSamplerDescription.DebugName = "PulseForge material linear mip sampler";
		auto MaterialSampler = m_Runtime.CreateSampler(MaterialSamplerDescription);
		if (!MaterialSampler)
			return std::unexpected(MakeResourceError("Could not create the scene sampler: " + MaterialSampler.error().Message));

		SamplerDesc EnvironmentSamplerDescription;
		EnvironmentSamplerDescription.Minification = SamplerFilter::Linear;
		EnvironmentSamplerDescription.Magnification = SamplerFilter::Linear;
		EnvironmentSamplerDescription.Mip = SamplerMipFilter::Linear;
		EnvironmentSamplerDescription.AddressU = SamplerAddressMode::ClampToEdge;
		EnvironmentSamplerDescription.AddressV = SamplerAddressMode::ClampToEdge;
		EnvironmentSamplerDescription.DebugName = "PulseForge environment linear mip sampler";
		auto EnvironmentSampler = m_Runtime.CreateSampler(EnvironmentSamplerDescription);
		if (!EnvironmentSampler)
			return std::unexpected(MakeResourceError("Could not create the environment sampler: " + EnvironmentSampler.error().Message));

		const std::array<uint8_t, 4> WhitePixel{ 255, 255, 255, 255 };
		TextureDesc BaseColorDescription;
		BaseColorDescription.Width = 1;
		BaseColorDescription.Height = 1;
		BaseColorDescription.Format = TextureFormat::RGBA8_Srgb;
		BaseColorDescription.DebugName = "PulseForge fallback white base-color texture";
		auto BaseColor = m_Runtime.CreateTexture(BaseColorDescription, std::as_bytes(std::span<const uint8_t>(WhitePixel)));
		if (!BaseColor)
			return std::unexpected(MakeResourceError("Could not create fallback base-color texture: " + BaseColor.error().Message));

		const auto CreateMaterialFallback = [this](const std::array<uint8_t, 4>& Pixel, TextureFormat Format,
												std::string DebugName) -> std::expected<TextureHandle, TextureError>
		{
			TextureDesc Description;
			Description.Width = 1;
			Description.Height = 1;
			Description.Format = Format;
			Description.DebugName = std::move(DebugName);
			return m_Runtime.CreateTexture(Description, std::as_bytes(std::span<const uint8_t>(Pixel)));
		};
		auto MetallicRoughness =
			CreateMaterialFallback(WhitePixel, TextureFormat::RGBA8_UNorm, "PulseForge default metallic-roughness map");
		if (!MetallicRoughness)
			return std::unexpected(MakeResourceError("Could not create metallic-roughness fallback: " + MetallicRoughness.error().Message));

		const std::array<uint8_t, 4> FlatNormalPixel{ 128, 128, 255, 255 };
		auto Normal = CreateMaterialFallback(FlatNormalPixel, TextureFormat::RGBA8_UNorm, "PulseForge flat normal map fallback");
		if (!Normal) return std::unexpected(MakeResourceError("Could not create normal-map fallback: " + Normal.error().Message));

		const std::array<uint8_t, 4> BlackPixel{ 0, 0, 0, 255 };
		auto Emissive = CreateMaterialFallback(BlackPixel, TextureFormat::RGBA8_Srgb, "PulseForge black emissive map fallback");
		if (!Emissive) return std::unexpected(MakeResourceError("Could not create emissive-map fallback: " + Emissive.error().Message));

		const MaterialAssetDesc& DefaultMaterial = m_DefaultMaterialDescription;
		const MaterialConstants FallbackMaterial{ { DefaultMaterial.BaseColorFactor.r, DefaultMaterial.BaseColorFactor.g,
													  DefaultMaterial.BaseColorFactor.b, DefaultMaterial.BaseColorFactor.a },
			{ DefaultMaterial.MetallicFactor, DefaultMaterial.RoughnessFactor, DefaultMaterial.NormalScale,
				DefaultMaterial.OcclusionStrength },
			{ DefaultMaterial.EmissiveFactor.r, DefaultMaterial.EmissiveFactor.g, DefaultMaterial.EmissiveFactor.b, 0.0f },
			{ static_cast<float>(DefaultMaterial.AlphaMode), DefaultMaterial.AlphaCutoff, DefaultMaterial.DoubleSided ? 1.0f : 0.0f,
				0.0f } };
		BufferDesc FallbackMaterialDescription;
		FallbackMaterialDescription.ByteSize = sizeof(FallbackMaterial);
		FallbackMaterialDescription.Usage = BufferUsage::Constant;
		FallbackMaterialDescription.DebugName = "PulseForge fallback material constants";
		auto FallbackMaterialBuffer = m_Runtime.CreateBuffer(FallbackMaterialDescription, std::as_bytes(std::span(&FallbackMaterial, 1)));
		if (!FallbackMaterialBuffer)
			return std::unexpected(
				MakeResourceError("Could not create fallback material constants: " + FallbackMaterialBuffer.error().Message));

		m_Materials = &Resources.Materials;
		m_Textures = &Resources.Textures;
		m_Layout = &Resources.Layout;
		m_OcclusionFallback = &Resources.OcclusionFallback;
		m_ObjectConstants = &Resources.ObjectConstants;
		m_FrameConstants = &Resources.FrameConstants;
		m_LocalLightingConstants = &Resources.LocalLightingConstants;
		m_MaterialSampler = std::move(*MaterialSampler);
		m_EnvironmentSampler = std::move(*EnvironmentSampler);
		m_FallbackBaseColor = std::move(*BaseColor);
		m_FallbackMetallicRoughness = std::move(*MetallicRoughness);
		m_FallbackNormal = std::move(*Normal);
		m_FallbackEmissive = std::move(*Emissive);
		m_FallbackMaterialConstants = std::move(*FallbackMaterialBuffer);
		return {};
	}

	std::expected<void, SceneRendererError> SceneRendererMaterialBindingCache::EnsureDefaultMaterialBindings(
		const EnvironmentLightingTextures& EnvironmentTextures)
	{
		if (auto Initialized = RequireInitialized(); !Initialized) return Initialized;
		if (m_DefaultMaterialBinding) return {};

		BindingSetDesc Description;
		Description.Layout = *m_Layout;
		Description.Textures = { { 0, std::cref(*m_FallbackBaseColor) },
			{ 1, std::cref(*EnvironmentTextures.DiffuseIrradiance), BindingResourceType::TextureCube },
			{ 2, std::cref(*EnvironmentTextures.PrefilteredSpecular), BindingResourceType::TextureCube },
			{ 3, std::cref(*EnvironmentTextures.BrdfIntegrationLut) },
			{ 4, std::cref(*EnvironmentTextures.Environment), BindingResourceType::TextureCube },
			{ 5, std::cref(*m_FallbackMetallicRoughness) }, { 6, std::cref(*m_FallbackNormal) }, { 7, std::cref(*m_OcclusionFallback) },
			{ 8, std::cref(*m_FallbackEmissive) } };
		Description.Samplers = { { 0, std::cref(*m_MaterialSampler) }, { 1, std::cref(*m_EnvironmentSampler) } };
		Description.Buffers = { { 0, std::cref(*m_FallbackMaterialConstants) }, { 1, std::cref(**m_ObjectConstants) },
			{ 2, std::cref(**m_FrameConstants) }, { 3, std::cref(**m_LocalLightingConstants) } };
		auto Binding = m_Runtime.CreateBindingSet(Description);
		if (!Binding) return std::unexpected(MakeResourceError("Could not create default material bindings: " + Binding.error().Message));
		m_DefaultMaterialBinding = std::move(*Binding);
		return {};
	}

	std::expected<void, SceneRendererError> SceneRendererMaterialBindingCache::EnsureMaterialBindings(const AssetID& MaterialAsset,
		const std::optional<AssetID>& EnvironmentAsset, const EnvironmentLightingTextures& EnvironmentTextures)
	{
		if (auto Initialized = RequireInitialized(); !Initialized) return Initialized;
		const std::string Key = MakeMaterialBindingKey(MaterialAsset, EnvironmentAsset);
		if (m_MaterialBindings.contains(Key)) return {};
		Detail::ScopedProfileTimer Timer("PBR material/texture binding first creation");

		auto Material = m_Materials->GetOrLoad(MaterialAsset);
		if (!Material)
			return std::unexpected(SceneRendererError{ SceneRendererErrorCode::AssetLoadFailed, {}, MaterialAsset,
				"Could not load scene material " + MaterialAsset.ToString() + ": " + Material.error().Message });

		const auto LoadMaterialTexture =
			[this, &MaterialAsset](const AssetID& TextureAsset, TextureFormat Format, const Texture& Fallback,
				std::string_view Name) -> std::expected<std::reference_wrapper<const Texture>, SceneRendererError>
		{
			if (TextureAsset.IsNil()) return std::cref(Fallback);
			auto Texture = m_Textures->GetOrLoad(TextureAsset, Format);
			if (!Texture)
				return std::unexpected(SceneRendererError{ SceneRendererErrorCode::AssetLoadFailed, {}, TextureAsset,
					"Could not load " + std::string(Name) + " texture for material " + MaterialAsset.ToString() + ": " +
						Texture.error().Message });
			return std::cref(Texture->get());
		};

		auto BaseColorTexture =
			LoadMaterialTexture(Material->get().BaseColorTexture, TextureFormat::RGBA8_Srgb, *m_FallbackBaseColor, "base-color");
		if (!BaseColorTexture) return std::unexpected(std::move(BaseColorTexture.error()));
		auto MetallicRoughnessTexture = LoadMaterialTexture(
			Material->get().MetallicRoughnessTexture, TextureFormat::RGBA8_UNorm, *m_FallbackMetallicRoughness, "metallic-roughness");
		if (!MetallicRoughnessTexture) return std::unexpected(std::move(MetallicRoughnessTexture.error()));
		auto NormalTexture = LoadMaterialTexture(Material->get().NormalTexture, TextureFormat::RGBA8_UNorm, *m_FallbackNormal, "normal");
		if (!NormalTexture) return std::unexpected(std::move(NormalTexture.error()));
		auto OcclusionTexture =
			LoadMaterialTexture(Material->get().OcclusionTexture, TextureFormat::RGBA8_UNorm, *m_OcclusionFallback, "occlusion");
		if (!OcclusionTexture) return std::unexpected(std::move(OcclusionTexture.error()));
		auto EmissiveTexture =
			LoadMaterialTexture(Material->get().EmissiveTexture, TextureFormat::RGBA8_Srgb, *m_FallbackEmissive, "emissive");
		if (!EmissiveTexture) return std::unexpected(std::move(EmissiveTexture.error()));

		const MaterialConstants Constants{ { Material->get().BaseColorFactor.r, Material->get().BaseColorFactor.g,
											   Material->get().BaseColorFactor.b, Material->get().BaseColorFactor.a },
			{ Material->get().MetallicFactor, Material->get().RoughnessFactor, Material->get().NormalScale,
				Material->get().OcclusionStrength },
			{ Material->get().EmissiveFactor.r, Material->get().EmissiveFactor.g, Material->get().EmissiveFactor.b, 0.0f },
			{ static_cast<float>(Material->get().AlphaMode), Material->get().AlphaCutoff, Material->get().DoubleSided ? 1.0f : 0.0f,
				0.0f } };
		BufferDesc MaterialBufferDescription;
		MaterialBufferDescription.ByteSize = sizeof(Constants);
		MaterialBufferDescription.Usage = BufferUsage::Constant;
		MaterialBufferDescription.DebugName = "PulseForge PBR material constants";
		auto MaterialBuffer = m_Runtime.CreateBuffer(MaterialBufferDescription, std::as_bytes(std::span(&Constants, 1)));
		if (!MaterialBuffer)
			return std::unexpected(MakeResourceError("Could not create material constants: " + MaterialBuffer.error().Message));

		BindingSetDesc BindingSetDescription;
		BindingSetDescription.Layout = *m_Layout;
		BindingSetDescription.Textures = { { 0, *BaseColorTexture },
			{ 1, std::cref(*EnvironmentTextures.DiffuseIrradiance), BindingResourceType::TextureCube },
			{ 2, std::cref(*EnvironmentTextures.PrefilteredSpecular), BindingResourceType::TextureCube },
			{ 3, std::cref(*EnvironmentTextures.BrdfIntegrationLut) },
			{ 4, std::cref(*EnvironmentTextures.Environment), BindingResourceType::TextureCube }, { 5, *MetallicRoughnessTexture },
			{ 6, *NormalTexture }, { 7, *OcclusionTexture }, { 8, *EmissiveTexture } };
		BindingSetDescription.Samplers = { { 0, std::cref(*m_MaterialSampler) }, { 1, std::cref(*m_EnvironmentSampler) } };
		BindingSetDescription.Buffers = { { 0, std::cref(*MaterialBuffer.value()) }, { 1, std::cref(**m_ObjectConstants) },
			{ 2, std::cref(**m_FrameConstants) }, { 3, std::cref(**m_LocalLightingConstants) } };
		auto BindingSet = m_Runtime.CreateBindingSet(BindingSetDescription);
		if (!BindingSet) return std::unexpected(MakeResourceError("Could not create material bindings: " + BindingSet.error().Message));

		m_MaterialBindings.emplace(Key, MaterialBinding{ std::move(*MaterialBuffer), std::move(*BindingSet) });
		return {};
	}

	std::expected<void, SceneRendererError> SceneRendererMaterialBindingCache::EnsureEnvironmentBindings(
		const AssetID& EnvironmentAsset, const EnvironmentLightingTextures& EnvironmentTextures, bool IsReady)
	{
		if (auto Initialized = RequireInitialized(); !Initialized) return Initialized;
		const std::string Key = MakeEnvironmentBindingKey(EnvironmentAsset, IsReady);
		if (m_EnvironmentBindings.contains(Key)) return {};
		Detail::ScopedProfileTimer Timer("Environment GPU binding creation");
		BindingSetDesc Description;
		Description.Layout = *m_Layout;
		Description.Textures = { { 0, std::cref(*m_FallbackBaseColor) },
			{ 1, std::cref(*EnvironmentTextures.DiffuseIrradiance), BindingResourceType::TextureCube },
			{ 2, std::cref(*EnvironmentTextures.PrefilteredSpecular), BindingResourceType::TextureCube },
			{ 3, std::cref(*EnvironmentTextures.BrdfIntegrationLut) },
			{ 4, std::cref(*EnvironmentTextures.Environment), BindingResourceType::TextureCube },
			{ 5, std::cref(*m_FallbackMetallicRoughness) }, { 6, std::cref(*m_FallbackNormal) }, { 7, std::cref(*m_OcclusionFallback) },
			{ 8, std::cref(*m_FallbackEmissive) } };
		Description.Samplers = { { 0, std::cref(*m_MaterialSampler) }, { 1, std::cref(*m_EnvironmentSampler) } };
		Description.Buffers = { { 0, std::cref(*m_FallbackMaterialConstants) }, { 1, std::cref(**m_ObjectConstants) },
			{ 2, std::cref(**m_FrameConstants) }, { 3, std::cref(**m_LocalLightingConstants) } };
		auto BindingSet = m_Runtime.CreateBindingSet(Description);
		if (!BindingSet)
			return std::unexpected(MakeResourceError("Could not create environment binding set: " + BindingSet.error().Message));
		m_EnvironmentBindings.emplace(Key, std::move(*BindingSet));
		return {};
	}

	std::expected<const MaterialAssetDesc*, SceneRendererError> SceneRendererMaterialBindingCache::ResolveMaterialDescription(
		const SceneMeshInstance& Instance)
	{
		if (auto Initialized = RequireInitialized(); !Initialized) return std::unexpected(std::move(Initialized.error()));
		if (!Instance.MaterialAsset) return &m_DefaultMaterialDescription;
		auto Material = m_Materials->GetOrLoad(*Instance.MaterialAsset);
		if (!Material)
			return std::unexpected(SceneRendererError{ SceneRendererErrorCode::AssetLoadFailed, Instance.Entity, *Instance.MaterialAsset,
				"Could not resolve material for mesh instance: " + Material.error().Message });
		return &Material->get();
	}

	std::expected<const BindingSet*, SceneRendererError> SceneRendererMaterialBindingCache::ResolveMaterialBinding(
		const SceneMeshInstance& Instance, const std::optional<AssetID>& EnvironmentAsset) const
	{
		if (auto Initialized = RequireInitialized(); !Initialized) return std::unexpected(std::move(Initialized.error()));
		if (!Instance.MaterialAsset)
		{
			if (EnvironmentAsset)
			{
				const auto EnvironmentBinding = m_EnvironmentBindings.find(MakeEnvironmentBindingKey(*EnvironmentAsset, true));
				if (EnvironmentBinding != m_EnvironmentBindings.end()) return EnvironmentBinding->second.get();
			}
			if (m_DefaultMaterialBinding) return m_DefaultMaterialBinding.get();
			return std::unexpected(SceneRendererError{ SceneRendererErrorCode::ResourceCreationFailed, Instance.Entity, Instance.MeshAsset,
				"Prepared default material binding set is unavailable" });
		}
		const auto Binding = m_MaterialBindings.find(MakeMaterialBindingKey(*Instance.MaterialAsset, EnvironmentAsset));
		if (Binding == m_MaterialBindings.end())
			return std::unexpected(SceneRendererError{ SceneRendererErrorCode::AssetLoadFailed, Instance.Entity, *Instance.MaterialAsset,
				"Prepared material binding set is unavailable" });
		return Binding->second.Set.get();
	}

	const BindingSet* SceneRendererMaterialBindingCache::FindEnvironmentBindings(const AssetID& EnvironmentAsset, bool IsReady) const
	{
		const auto Binding = m_EnvironmentBindings.find(MakeEnvironmentBindingKey(EnvironmentAsset, IsReady));
		return Binding != m_EnvironmentBindings.end() ? Binding->second.get() : nullptr;
	}

	void SceneRendererMaterialBindingCache::RefreshMaterialAssets() noexcept
	{
		m_MaterialBindings.clear();
		if (m_Materials) m_Materials->Clear();
	}

	std::expected<void, SceneRendererError> SceneRendererMaterialBindingCache::RequireInitialized() const
	{
		if (m_Layout) return {};
		return std::unexpected(MakeResourceError("Material binding cache was used before initialization"));
	}

	SceneRendererError SceneRendererMaterialBindingCache::MakeResourceError(std::string Message)
	{
		return { SceneRendererErrorCode::ResourceCreationFailed, {}, {}, std::move(Message) };
	}

	std::string SceneRendererMaterialBindingCache::MakeMaterialBindingKey(
		const AssetID& MaterialAsset, const std::optional<AssetID>& EnvironmentAsset)
	{
		return MaterialAsset.ToString() + ":" + (EnvironmentAsset ? EnvironmentAsset->ToString() : std::string("none"));
	}

	std::string SceneRendererMaterialBindingCache::MakeEnvironmentBindingKey(const AssetID& EnvironmentAsset, bool IsReady)
	{
		return EnvironmentAsset.ToString() + (IsReady ? ":ready" : ":loading");
	}
} // namespace PulseForge
