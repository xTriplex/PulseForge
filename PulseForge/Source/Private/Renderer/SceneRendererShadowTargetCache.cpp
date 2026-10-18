#include "Renderer/SceneRendererShadowTargetCache.h"
#include "Renderer/SceneRendererGeneration.h"

#include "Core/Log.h"

#include <utility>

namespace PulseForge
{
	namespace
	{
		SceneRendererError MakeShadowTargetResourceError(std::string Message)
		{
			return { SceneRendererErrorCode::ResourceCreationFailed, {}, {}, std::move(Message) };
		}
	} // namespace

	SceneRenderer::ShadowTargetCache::ShadowTargetCache(RenderDevice& Runtime) noexcept : m_Runtime(Runtime)
	{
	}

	std::expected<void, SceneRendererError> SceneRenderer::ShadowTargetCache::Initialize(const BindingLayoutHandle& DirectionalLayout,
		const BindingLayoutHandle& SpotlightLayout, const BindingLayoutHandle& PointLightLayout, const Sampler& ShadowSampler,
		const Texture& FallbackPointShadowTexture)
	{
		constexpr uint32_t DirectionalShadowResolution = 1024;
		std::array<RenderTargetHandle, 4> DirectionalTargets;
		for (size_t CascadeIndex = 0; CascadeIndex < DirectionalTargets.size(); ++CascadeIndex)
		{
			RenderTargetDesc Description;
			Description.Width = DirectionalShadowResolution;
			Description.Height = DirectionalShadowResolution;
			Description.ColorFormat = ColorTargetFormat::None;
			Description.DepthMode = DepthAttachmentMode::ShaderReadableAttachment;
			Description.DebugName = "PulseForge directional shadow cascade " + std::to_string(CascadeIndex);
			auto Target = m_Runtime.CreateRenderTarget(Description);
			if (!Target)
				return std::unexpected(
					MakeShadowTargetResourceError("Could not create directional shadow target: " + Target.error().Message));
			DirectionalTargets[CascadeIndex] = std::move(*Target);
		}

		BindingSetDesc DirectionalDescription;
		DirectionalDescription.Layout = DirectionalLayout;
		for (uint32_t CascadeIndex = 0; CascadeIndex < DirectionalTargets.size(); ++CascadeIndex)
			DirectionalDescription.Textures.push_back({ CascadeIndex, std::cref(*DirectionalTargets[CascadeIndex]->GetDepthTexture()) });
		DirectionalDescription.Samplers.push_back({ 0, std::cref(ShadowSampler) });
		auto DirectionalBindings = m_Runtime.CreateBindingSet(DirectionalDescription);
		if (!DirectionalBindings)
			return std::unexpected(
				MakeShadowTargetResourceError("Could not create directional shadow bindings: " + DirectionalBindings.error().Message));

		BindingSetDesc SpotlightDescription;
		SpotlightDescription.Layout = SpotlightLayout;
		for (uint32_t ShadowIndex = 0; ShadowIndex < DirectionalTargets.size(); ++ShadowIndex)
			SpotlightDescription.Textures.push_back({ ShadowIndex, std::cref(*DirectionalTargets[ShadowIndex]->GetDepthTexture()) });
		SpotlightDescription.Samplers.push_back({ 0, std::cref(ShadowSampler) });
		auto SpotlightBindings = m_Runtime.CreateBindingSet(SpotlightDescription);
		if (!SpotlightBindings)
			return std::unexpected(MakeShadowTargetResourceError(
				"Could not create safe initial spotlight shadow bindings: " + SpotlightBindings.error().Message));

		BindingSetDesc PointLightDescription;
		PointLightDescription.Layout = PointLightLayout;
		PointLightDescription.Textures = { { 0, std::cref(FallbackPointShadowTexture), BindingResourceType::TextureCube },
			{ 1, std::cref(FallbackPointShadowTexture), BindingResourceType::TextureCube } };
		PointLightDescription.Samplers.push_back({ 0, std::cref(ShadowSampler) });
		auto PointLightBindings = m_Runtime.CreateBindingSet(PointLightDescription);
		if (!PointLightBindings)
			return std::unexpected(
				MakeShadowTargetResourceError("Could not create fallback point-shadow bindings: " + PointLightBindings.error().Message));

		m_DirectionalTargets = std::move(DirectionalTargets);
		m_DirectionalBindings = std::move(*DirectionalBindings);
		m_SpotlightBindings = std::move(*SpotlightBindings);
		m_PointLightBindings = std::move(*PointLightBindings);
		return {};
	}

	std::expected<void, SceneRendererError> SceneRenderer::ShadowTargetCache::EnsureSpotlightMaps(
		const BindingLayoutHandle& Layout, const Sampler& ShadowSampler)
	{
		if (m_SpotlightTargets.front()) return {};

		std::array<RenderTargetHandle, MaxSpotlightShadowMapCount> Targets;
		for (size_t ShadowIndex = 0; ShadowIndex < Targets.size(); ++ShadowIndex)
		{
			RenderTargetDesc Description;
			Description.Width = SpotlightShadowMapResolution;
			Description.Height = SpotlightShadowMapResolution;
			Description.ColorFormat = ColorTargetFormat::None;
			Description.DepthMode = DepthAttachmentMode::ShaderReadableAttachment;
			Description.DebugName = "PulseForge spotlight shadow map " + std::to_string(ShadowIndex);
			auto Target = m_Runtime.CreateRenderTarget(Description);
			if (!Target)
				return std::unexpected(MakeShadowTargetResourceError("Could not create spotlight shadow map: " + Target.error().Message));
			if (!(*Target)->GetDepthTexture() || (*Target)->GetDepthTexture()->GetDescription().Format != TextureFormat::Depth32Float)
				return std::unexpected(
					MakeShadowTargetResourceError("Spotlight shadow target did not provide a shader-readable D32 depth texture"));
			Targets[ShadowIndex] = std::move(*Target);
		}

		BindingSetDesc Description;
		Description.Layout = Layout;
		for (uint32_t ShadowIndex = 0; ShadowIndex < Targets.size(); ++ShadowIndex)
			Description.Textures.push_back({ ShadowIndex, std::cref(*Targets[ShadowIndex]->GetDepthTexture()) });
		Description.Samplers.push_back({ 0, std::cref(ShadowSampler) });
		auto Bindings = m_Runtime.CreateBindingSet(Description);
		if (!Bindings)
			return std::unexpected(
				MakeShadowTargetResourceError("Could not create spotlight shadow bindings: " + Bindings.error().Message));

		m_SpotlightTargets = std::move(Targets);
		m_SpotlightBindings = std::move(*Bindings);
		m_SpotlightResourceGeneration = Detail::AdvanceSceneRendererGeneration(m_SpotlightResourceGeneration);
		PF_CORE_INFO("Created {0} persistent {1}x{1} D32 spotlight shadow maps ({2} MiB depth storage)", MaxSpotlightShadowMapCount,
			SpotlightShadowMapResolution,
			(MaxSpotlightShadowMapCount * SpotlightShadowMapResolution * SpotlightShadowMapResolution * sizeof(float)) / (1024 * 1024));
		return {};
	}

	std::expected<void, SceneRendererError> SceneRenderer::ShadowTargetCache::EnsurePointLightCubemaps(
		const BindingLayoutHandle& Layout, const Sampler& ShadowSampler)
	{
		if (m_PointLightCubemaps.front()) return {};

		std::array<DepthCubemapHandle, MaxPointLightShadowCount> Cubemaps;
		for (size_t Slot = 0; Slot < Cubemaps.size(); ++Slot)
		{
			auto Cubemap = m_Runtime.CreateDepthCubemap(
				PointLightShadowMapResolution, "PulseForge point-shadow depth cubemap " + std::to_string(Slot));
			if (!Cubemap)
				return std::unexpected(
					MakeShadowTargetResourceError("Could not create point-shadow depth cubemap: " + Cubemap.error().Message));
			if ((*Cubemap)->GetTexture().GetDescription().Format != TextureFormat::Depth32Float ||
				(*Cubemap)->GetResolution() != PointLightShadowMapResolution)
				return std::unexpected(
					MakeShadowTargetResourceError("Point-shadow cubemap has an incompatible depth format or resolution"));
			Cubemaps[Slot] = std::move(*Cubemap);
		}

		BindingSetDesc Description;
		Description.Layout = Layout;
		for (uint32_t Slot = 0; Slot < Cubemaps.size(); ++Slot)
			Description.Textures.push_back({ Slot, std::cref(Cubemaps[Slot]->GetTexture()), BindingResourceType::TextureCube });
		Description.Samplers.push_back({ 0, std::cref(ShadowSampler) });
		auto Bindings = m_Runtime.CreateBindingSet(Description);
		if (!Bindings)
			return std::unexpected(
				MakeShadowTargetResourceError("Could not create point-shadow cubemap bindings: " + Bindings.error().Message));

		m_PointLightCubemaps = std::move(Cubemaps);
		m_PointLightBindings = std::move(*Bindings);
		m_PointLightResourceGeneration = Detail::AdvanceSceneRendererGeneration(m_PointLightResourceGeneration);
		PF_CORE_INFO("Created {0} persistent {1}x{1} D32 point-shadow cubemaps ({2} MiB depth storage)", MaxPointLightShadowCount,
			PointLightShadowMapResolution,
			(MaxPointLightShadowCount * 6 * PointLightShadowMapResolution * PointLightShadowMapResolution * sizeof(float)) / (1024 * 1024));
		return {};
	}

	const RenderTarget& SceneRenderer::ShadowTargetCache::DirectionalTarget(size_t CascadeIndex) const noexcept
	{
		return *m_DirectionalTargets[CascadeIndex];
	}

	const Texture* SceneRenderer::ShadowTargetCache::DirectionalShadowTexture(size_t CascadeIndex) const noexcept
	{
		return CascadeIndex < m_DirectionalTargets.size() && m_DirectionalTargets[CascadeIndex]
				   ? m_DirectionalTargets[CascadeIndex]->GetDepthTexture()
				   : nullptr;
	}

	const BindingSet& SceneRenderer::ShadowTargetCache::DirectionalBindings() const noexcept
	{
		return *m_DirectionalBindings;
	}

	const BindingSet& SceneRenderer::ShadowTargetCache::SpotlightBindings() const noexcept
	{
		return *m_SpotlightBindings;
	}

	const BindingSet& SceneRenderer::ShadowTargetCache::PointLightBindings() const noexcept
	{
		return *m_PointLightBindings;
	}

	const RenderTarget* SceneRenderer::ShadowTargetCache::SpotlightTarget(size_t Slot) const noexcept
	{
		return Slot < m_SpotlightTargets.size() ? m_SpotlightTargets[Slot].get() : nullptr;
	}

	const Texture* SceneRenderer::ShadowTargetCache::SpotlightShadowTexture(size_t Slot) const noexcept
	{
		const RenderTarget* Target = SpotlightTarget(Slot);
		return Target ? Target->GetDepthTexture() : nullptr;
	}

	const DepthCubemap* SceneRenderer::ShadowTargetCache::PointLightCubemap(size_t Slot) const noexcept
	{
		return Slot < m_PointLightCubemaps.size() ? m_PointLightCubemaps[Slot].get() : nullptr;
	}

	const Texture* SceneRenderer::ShadowTargetCache::PointShadowFaceTexture(size_t Slot, uint32_t Face) const noexcept
	{
		if (Slot >= m_PointLightCubemaps.size() || Face >= 6 || !m_PointLightCubemaps[Slot]) return nullptr;
		return m_PointLightCubemaps[Slot]->GetFaceTarget(Face).GetDepthTexture();
	}

	bool SceneRenderer::ShadowTargetCache::HasSpotlightMaps() const noexcept
	{
		return static_cast<bool>(m_SpotlightTargets.front());
	}

	bool SceneRenderer::ShadowTargetCache::HasPointLightCubemaps() const noexcept
	{
		return static_cast<bool>(m_PointLightCubemaps.front());
	}

	uint64_t SceneRenderer::ShadowTargetCache::SpotlightResourceGeneration() const noexcept
	{
		return m_SpotlightResourceGeneration;
	}

	uint64_t SceneRenderer::ShadowTargetCache::PointLightResourceGeneration() const noexcept
	{
		return m_PointLightResourceGeneration;
	}
} // namespace PulseForge
